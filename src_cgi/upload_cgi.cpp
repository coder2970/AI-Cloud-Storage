#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

extern "C" {
#include "fdfs_client.h"
}

namespace {

const std::size_t kMaxUploadBody = 20 * 1024 * 1024;
const char *kUploadTempDir = "/tmp/ycc_uploads";

struct UploadRequest {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
    std::string temp_path;
    long size;
};

bool is_safe_md5(const std::string &value) {
    if (value.empty() || value.size() > 256) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string basename_only(const std::string &name) {
    std::size_t pos = name.find_last_of("/\\");
    std::string base = (pos == std::string::npos) ? name : name.substr(pos + 1);
    return base.empty() ? "upload.bin" : base;
}

std::string file_suffix(const std::string &filename) {
    std::size_t slash = filename.find_last_of("/\\");
    std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 >= filename.size()) {
        return "null";
    }
    std::string suffix = filename.substr(dot + 1);
    if (suffix.size() > 7) suffix.resize(7);
    return suffix;
}

bool extract_header_value(const std::string &header, const std::string &key, std::string *out) {
    if (!out) return false;
    std::string token = key + "=\"";
    std::size_t start = header.find(token);
    if (start == std::string::npos) return false;
    start += token.size();
    std::size_t end = header.find('"', start);
    if (end == std::string::npos) return false;
    *out = header.substr(start, end - start);
    return true;
}

bool write_file(const std::string &path, const char *data, std::size_t len) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    out.write(data, static_cast<std::streamsize>(len));
    return out.good();
}

bool parse_part(const std::string &body, std::size_t part_start, std::size_t part_end,
                UploadRequest *request, bool *file_seen) {
    if (!request || !file_seen || part_end <= part_start) return false;

    if (body.compare(part_start, 2, "\r\n") == 0) part_start += 2;
    std::size_t header_end = body.find("\r\n\r\n", part_start);
    if (header_end == std::string::npos || header_end >= part_end) return false;

    std::string header = body.substr(part_start, header_end - part_start);
    std::size_t data_start = header_end + 4;
    std::size_t data_end = part_end;
    if (data_end >= 2 && body.compare(data_end - 2, 2, "\r\n") == 0) data_end -= 2;
    if (data_end < data_start) return false;

    std::string field_name;
    if (!extract_header_value(header, "name", &field_name)) return false;

    if (field_name == "file") {
        std::string original_name;
        extract_header_value(header, "filename", &original_name);
        request->filename = basename_only(original_name);
        request->temp_path = std::string(kUploadTempDir) + "/" + request->md5 + ".upload";
        if (request->md5.empty()) {
            request->temp_path = std::string(kUploadTempDir) + "/" + ycc::random_hex(16) + ".upload";
        }
        if (!write_file(request->temp_path, body.data() + data_start, data_end - data_start)) return false;
        *file_seen = true;
        return true;
    }

    std::string value = body.substr(data_start, data_end - data_start);
    if (field_name == "user") {
        request->user = value;
    } else if (field_name == "token") {
        request->token = value;
    } else if (field_name == "md5") {
        request->md5 = value;
        if (*file_seen && request->temp_path.find(".upload") != std::string::npos) {
            std::string new_path = std::string(kUploadTempDir) + "/" + request->md5 + ".upload";
            if (request->temp_path != new_path) {
                rename(request->temp_path.c_str(), new_path.c_str());
                request->temp_path = new_path;
            }
        }
    } else if (field_name == "size") {
        request->size = std::strtol(value.c_str(), NULL, 10);
    }
    return true;
}

bool parse_multipart_upload(const std::string &body, UploadRequest *request) {
    if (!request) return false;
    request->size = 0;

    std::size_t first_line_end = body.find("\r\n");
    if (first_line_end == std::string::npos) return false;
    std::string boundary = body.substr(0, first_line_end);
    if (boundary.empty()) return false;

    bool file_seen = false;
    std::size_t search_pos = 0;
    while (true) {
        std::size_t boundary_pos = body.find(boundary, search_pos);
        if (boundary_pos == std::string::npos) break;
        std::size_t part_start = boundary_pos + boundary.size();
        if (body.compare(part_start, 2, "--") == 0) break;

        std::size_t next_boundary = body.find(boundary, part_start);
        if (next_boundary == std::string::npos) break;
        if (!parse_part(body, part_start, next_boundary, request, &file_seen)) return false;
        search_pos = next_boundary;
    }

    return file_seen && !request->user.empty() && !request->token.empty() && !request->md5.empty() &&
           !request->filename.empty() && request->size > 0 && is_safe_md5(request->md5);
}

bool init_fastdfs_once() {
    std::string conf_path = ycc::config_value("dfs_path", "client");
    if (conf_path.empty()) return false;
    ignore_signal_pipe();
    g_log_context.log_level = LOG_ERR;
    return fdfs_client_init(conf_path.c_str()) == 0;
}

bool upload_to_fastdfs(const std::string &path, std::string *fileid_out) {
    if (!fileid_out) return false;
    char group_name[FDFS_GROUP_NAME_MAX_LEN + 1] = {0};
    char fileid[TEMP_BUF_MAX_LEN] = {0};
    ConnectionInfo *tracker = tracker_get_connection();
    if (!tracker) return false;

    ConnectionInfo storage;
    int store_path_index = 0;
    int result = tracker_query_storage_store(tracker, &storage, group_name, &store_path_index);
    if (result != 0) {
        tracker_close_connection_ex(tracker, true);
        return false;
    }

    result = storage_upload_by_filename1(tracker, &storage, store_path_index,
                                         path.c_str(), NULL, NULL, 0, group_name, fileid);
    tracker_close_connection_ex(tracker, true);
    if (result != 0) return false;
    *fileid_out = fileid;
    return true;
}

std::string make_file_url(const std::string &fileid) {
    return "http://" + ycc::config_value("storage_web_server", "ip") + ":" +
           ycc::config_value("storage_web_server", "port") + "/" + fileid;
}

void cleanup_temp(const UploadRequest &request) {
    if (!request.temp_path.empty()) unlink(request.temp_path.c_str());
}

void handle_upload() {
    std::string body;
    UploadRequest request;
    int code = HTTP_RESP_FAIL;

    if (!ycc::read_stdin_body(kMaxUploadBody, &body) || !ycc::ensure_dir(kUploadTempDir) ||
        !parse_multipart_upload(body, &request)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        cleanup_temp(request);
        return;
    }

    if (!ycc::verify_token(request.user, request.token)) {
        cleanup_temp(request);
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_TOKEN_ERR).c_str());
        return;
    }

    ycc::MysqlConn mysql;
    int dedupe = ycc::bind_existing_file_to_user(mysql, request.user, request.md5, request.filename);
    if (dedupe == HTTP_RESP_OK || dedupe == HTTP_RESP_FILE_EXIST) {
        cleanup_temp(request);
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_OK).c_str());
        return;
    }
    if (dedupe == HTTP_RESP_FAIL) {
        cleanup_temp(request);
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::string fileid;
    if (upload_to_fastdfs(request.temp_path, &fileid)) {
        std::string url = make_file_url(fileid);
        if (ycc::store_uploaded_file_for_user(mysql, request.user, request.md5, request.filename,
                                              request.size, fileid, url, file_suffix(request.filename))) {
            code = HTTP_RESP_OK;
        }
    }

    cleanup_temp(request);
    FCGI_printf("%s", ycc::status_json(code).c_str());
}

} // namespace

int main() {
    if (!init_fastdfs_once()) return 1;
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_upload();
    }
    fdfs_client_destroy();
    return 0;
}
