#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

extern "C" {
#include "fdfs_client.h"
}

namespace {

const std::size_t kMaxMergeBody = 16 * 1024;
const char *kChunkTempDir = "/tmp/chunks";

struct MergeRequest {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
};

bool is_safe_md5(const std::string &value) {
    if (value.empty() || value.size() > 256) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string chunk_dir_for(const std::string &md5) {
    return std::string(kChunkTempDir) + "/" + md5;
}

std::string chunk_path_for(const std::string &md5, int index) {
    std::ostringstream out;
    out << chunk_dir_for(md5) << '/' << index;
    return out.str();
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

bool parse_merge_request(const std::string &body, MergeRequest *request) {
    if (!request) return false;
    ycc::JsonDoc doc(body);
    if (!doc.valid()) return false;

    return doc.string_field("user", &request->user) &&
           doc.string_field("token", &request->token) &&
           doc.string_field("md5", &request->md5) &&
           doc.string_field("filename", &request->filename) &&
           !request->user.empty() && !request->token.empty() &&
           !request->md5.empty() && !request->filename.empty() &&
           is_safe_md5(request->md5);
}

bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool verify_all_chunks(const std::string &md5, int chunk_count) {
    if (chunk_count <= 0) return false;
    for (int i = 0; i < chunk_count; ++i) {
        if (!file_exists(chunk_path_for(md5, i))) return false;
    }
    return true;
}

void remove_chunk_dir(const std::string &md5) {
    std::string dir_path = chunk_dir_for(md5);
    DIR *dir = opendir(dir_path.c_str());
    if (!dir) return;

    dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        std::string path = dir_path + "/" + entry->d_name;
        unlink(path.c_str());
    }
    closedir(dir);
    rmdir(dir_path.c_str());
}

int parse_int_or_zero(const std::string &text) {
    return std::atoi(text.c_str());
}

long parse_long_or_zero(const std::string &text) {
    return std::strtol(text.c_str(), NULL, 10);
}

std::string make_file_url(const std::string &fileid) {
    std::string host = ycc::config_value("storage_web_server", "ip");
    std::string port = ycc::config_value("storage_web_server", "port");
    return "http://" + host + ":" + port + "/" + fileid;
}

int bind_existing_file_to_user(const MergeRequest &request) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    std::string user = mysql.escape(request.user);
    std::string md5 = mysql.escape(request.md5);
    std::string filename = mysql.escape(request.filename);

    std::string unused;
    if (!mysql.query_one("select count from file_info where md5='" + md5 + "' limit 1", &unused)) {
        return -1;
    }

    if (mysql.exists("select 1 from user_file_list where user='" + user +
                     "' and md5='" + md5 + "' and file_name='" + filename + "' limit 1")) {
        return HTTP_RESP_FILE_EXIST;
    }

    if (!mysql.execute("update file_info set count = count + 1 where md5='" + md5 + "'")) {
        return HTTP_RESP_FAIL;
    }

    std::string create_time = mysql.escape(ycc::now_local_string());
    if (!mysql.execute("insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values ('" +
                       user + "', '" + md5 + "', '" + create_time + "', '" + filename + "', 0, 0)")) {
        return HTTP_RESP_FAIL;
    }

    std::string count;
    if (mysql.query_one("select count from user_file_count where user='" + user + "' limit 1", &count)) {
        if (!mysql.execute("update user_file_count set count = count + 1 where user='" + user + "'")) {
            return HTTP_RESP_FAIL;
        }
    } else {
        if (!mysql.execute("insert into user_file_count(user, count) values('" + user + "', 1)")) {
            return HTTP_RESP_FAIL;
        }
    }

    return HTTP_RESP_OK;
}

bool store_fileinfo_to_mysql(const MergeRequest &request, long size,
                             const std::string &fileid, const std::string &url) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return false;

    std::string user = mysql.escape(request.user);
    std::string md5 = mysql.escape(request.md5);
    std::string filename = mysql.escape(request.filename);
    std::string escaped_fileid = mysql.escape(fileid);
    std::string escaped_url = mysql.escape(url);
    std::string suffix = mysql.escape(file_suffix(request.filename));

    if (mysql.exists("select 1 from user_file_list where user='" + user +
                     "' and md5='" + md5 + "' and file_name='" + filename + "' limit 1")) {
        return true;
    }

    std::string existing_count;
    if (mysql.query_one("select count from file_info where md5='" + md5 + "' limit 1", &existing_count)) {
        if (!mysql.execute("update file_info set count = count + 1 where md5='" + md5 + "'")) return false;
    } else {
        std::ostringstream sql;
        sql << "insert into file_info (md5, file_id, url, size, type, count) values ('"
            << md5 << "', '" << escaped_fileid << "', '" << escaped_url << "', '"
            << size << "', '" << suffix << "', 1)";
        if (!mysql.execute(sql.str())) return false;
    }

    std::string create_time = mysql.escape(ycc::now_local_string());
    if (!mysql.execute("insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values ('" +
                       user + "', '" + md5 + "', '" + create_time + "', '" + filename + "', 0, 0)")) {
        return false;
    }

    std::string count;
    if (mysql.query_one("select count from user_file_count where user='" + user + "' limit 1", &count)) {
        return mysql.execute("update user_file_count set count = count + 1 where user='" + user + "'");
    }
    return mysql.execute("insert into user_file_count(user, count) values('" + user + "', 1)");
}

bool init_fastdfs_once() {
    std::string conf_path = ycc::config_value("dfs_path", "client");
    if (conf_path.empty()) return false;
    ignore_signal_pipe();
    g_log_context.log_level = LOG_ERR;
    return fdfs_client_init(conf_path.c_str()) == 0;
}

bool merge_chunks_to_fastdfs(const MergeRequest &request, int chunk_count, std::string *fileid_out) {
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

    std::string suffix = file_suffix(request.filename);
    std::string first_path = chunk_path_for(request.md5, 0);
    result = storage_upload_appender_by_filename1(
        tracker, &storage, store_path_index, first_path.c_str(), suffix.c_str(),
        NULL, 0, group_name, fileid);
    if (result != 0) {
        tracker_close_connection_ex(tracker, true);
        return false;
    }
    unlink(first_path.c_str());

    for (int i = 1; i < chunk_count; ++i) {
        std::string path = chunk_path_for(request.md5, i);
        result = storage_append_by_filename1(tracker, &storage, path.c_str(), fileid);
        if (result != 0) {
            tracker_close_connection_ex(tracker, true);
            return false;
        }
        unlink(path.c_str());
    }

    tracker_close_connection_ex(tracker, true);
    rmdir(chunk_dir_for(request.md5).c_str());
    *fileid_out = fileid;
    return true;
}

void cleanup_state(const std::string &md5) {
    ycc::RedisConn redis;
    if (redis.ok()) redis.del("chunk:" + md5);
    remove_chunk_dir(md5);
}

void handle_merge() {
    std::string body;
    MergeRequest request;
    if (!ycc::read_stdin_body(kMaxMergeBody, &body) || !parse_merge_request(body, &request)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!ycc::verify_token(request.user, request.token)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_TOKEN_ERR).c_str());
        return;
    }

    int dedupe = bind_existing_file_to_user(request);
    if (dedupe == HTTP_RESP_OK || dedupe == HTTP_RESP_FILE_EXIST) {
        cleanup_state(request.md5);
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_OK).c_str());
        return;
    }
    if (dedupe == HTTP_RESP_FAIL) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    ycc::RedisConn redis;
    std::string chunk_count_text;
    std::string file_size_text;
    if (!redis.ok() ||
        !redis.hash_get("chunk:" + request.md5, "chunk_count", &chunk_count_text) ||
        !redis.hash_get("chunk:" + request.md5, "filesize", &file_size_text)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    int chunk_count = parse_int_or_zero(chunk_count_text);
    long file_size = parse_long_or_zero(file_size_text);
    if (!verify_all_chunks(request.md5, chunk_count)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::string fileid;
    if (!merge_chunks_to_fastdfs(request, chunk_count, &fileid)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::string url = make_file_url(fileid);
    if (!store_fileinfo_to_mysql(request, file_size, fileid, url)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    redis.del("chunk:" + request.md5);
    FCGI_printf("%s", ycc::status_json(HTTP_RESP_OK).c_str());
}

} // namespace

int main() {
    if (!init_fastdfs_once()) {
        return 1;
    }

    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_merge();
    }
    fdfs_client_destroy();
    return 0;
}
