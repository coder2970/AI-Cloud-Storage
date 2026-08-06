#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "make_log.h"
#include "cgi_constants.h"
}

#define SHAREPIC_LOG_MODULE "cgi"
#define SHAREPICS_LOG_PROC "sharepic"

namespace {

const std::size_t kMaxJsonBody = 64 * 1024;
const char kSharePictureCountSuffix[] = "_share_picture_list_count";

struct SharePictureRequest {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
    std::string urlmd5;
    int start;
    int count;

    SharePictureRequest() : start(0), count(0) {}
};

std::string sq(const ycc::MysqlConn &mysql, const std::string &value) {
    return "'" + mysql.escape(value) + "'";
}

bool parse_share_request(const std::string &body, SharePictureRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("token", &req->token) &&
           doc.string_field("user", &req->user) &&
           doc.string_field("md5", &req->md5) &&
           doc.string_field("filename", &req->filename);
}

bool parse_list_request(const std::string &body, SharePictureRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("token", &req->token) &&
           doc.string_field("user", &req->user) &&
           doc.int_field("start", &req->start) &&
           doc.int_field("count", &req->count);
}

bool parse_cancel_request(const std::string &body, SharePictureRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("user", &req->user) &&
           doc.string_field("token", &req->token) &&
           doc.string_field("urlmd5", &req->urlmd5);
}

bool parse_browse_request(const std::string &body, SharePictureRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() && doc.string_field("urlmd5", &req->urlmd5);
}

std::string share_count_key(const std::string &user) {
    return user + kSharePictureCountSuffix;
}

bool get_share_picture_count(const ycc::MysqlConn &mysql, const std::string &user, long *total) {
    if (!total) return false;
    *total = 0;
    std::string value;
    std::string sql = "select count from user_file_count where user = " + sq(mysql, share_count_key(user));
    if (!mysql.query_one(sql, &value)) {
        *total = 0;
        return true;
    }
    *total = std::atol(value.c_str());
    return true;
}

bool update_share_picture_count(const ycc::MysqlConn &mysql, const std::string &user, int delta) {
    std::string key = share_count_key(user);
    std::string value;
    std::string sql = "select count from user_file_count where user = " + sq(mysql, key);
    if (!mysql.query_one(sql, &value)) {
        if (delta > 0) {
            return mysql.execute("insert into user_file_count (user, count) values(" + sq(mysql, key) +
                                 ", " + std::to_string(delta) + ")");
        }
        return true;
    }

    int count = std::atoi(value.c_str()) + delta;
    if (count < 0) count = 0;
    return mysql.execute("update user_file_count set count = " + std::to_string(count) +
                         " where user = " + sq(mysql, key));
}

bool update_file_ref_count(const ycc::MysqlConn &mysql, const std::string &md5, int delta,
                           std::string *file_id, int *new_count) {
    std::string sql = "select count, file_id from file_info where md5 = " + sq(mysql, md5);
    if (mysql_query(mysql.get(), sql.c_str()) != 0) return false;

    ycc::MysqlResult result(mysql_store_result(mysql.get()));
    if (!result.get()) return false;
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row || !row[0]) return false;

    int count = std::atoi(row[0]) + delta;
    if (count < 0) count = 0;
    if (row[1] && file_id) *file_id = row[1];
    if (new_count) *new_count = count;

    return mysql.execute("update file_info set count = " + std::to_string(count) +
                         " where md5 = " + sq(mysql, md5));
}

int remove_file_from_storage(const std::string &fileid) {
    std::string client_conf = ycc::config_value("dfs_path", "client");
    if (client_conf.empty() || fileid.empty()) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("fdfs_delete_file", "fdfs_delete_file", client_conf.c_str(), fileid.c_str(), (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    int ret = WEXITSTATUS(status);
    LOG(SHAREPIC_LOG_MODULE, SHAREPICS_LOG_PROC, "remove_file_from_storage ret = %d\n", ret);
    return ret;
}

void add_string_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = row[index];
}

void add_long_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = Json::Int64(std::atol(row[index]));
}

std::string list_response(const SharePictureRequest &req) {
    ycc::MysqlConn mysql;
    long total = 0;
    int rows = 0;
    int code = HTTP_RESP_OK;

    ycc::JsonBuilder root;
    Json::Value files(Json::arrayValue);

    if (!mysql.ok() || !get_share_picture_count(mysql, req.user, &total)) {
        code = HTTP_RESP_FAIL;
    } else if (total > 0) {
        int start = req.start < 0 ? 0 : req.start;
        int count = req.count < 0 ? 0 : req.count;
        std::string sql = "select share_picture_list.user, share_picture_list.filemd5, "
                          "share_picture_list.file_name, share_picture_list.urlmd5, "
                          "share_picture_list.pv, share_picture_list.create_time, file_info.size "
                          "from file_info, share_picture_list "
                          "where share_picture_list.user = " + sq(mysql, req.user) +
                          " and file_info.md5 = share_picture_list.filemd5 limit " +
                          std::to_string(start) + ", " + std::to_string(count);

        if (mysql_query(mysql.get(), sql.c_str()) != 0) {
            code = HTTP_RESP_FAIL;
        } else {
            ycc::MysqlResult result(mysql_store_result(mysql.get()));
            if (!result.get()) {
                code = HTTP_RESP_FAIL;
            } else {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(result.get())) != NULL) {
                    Json::Value item(Json::objectValue);
                    add_string_if_present(&item, "user", row, 0);
                    add_string_if_present(&item, "filemd5", row, 1);
                    add_string_if_present(&item, "file_name", row, 2);
                    add_string_if_present(&item, "urlmd5", row, 3);
                    add_long_if_present(&item, "pv", row, 4);
                    add_string_if_present(&item, "create_time", row, 5);
                    add_long_if_present(&item, "size", row, 6);
                    files.append(item);
                    ++rows;
                }
            }
        }
    }

    root.value()["files"] = files;
    root.value()["code"] = code;
    root.value()["total"] = Json::Int64(total);
    root.value()["count"] = rows;
    return root.print();
}

std::string share_response(int code, const std::string &urlmd5) {
    ycc::JsonBuilder root;
    root.value()["code"] = code;
    if (code == HTTP_RESP_OK) root.value()["urlmd5"] = urlmd5;
    return root.print();
}

std::string request_share_picture(const SharePictureRequest &req) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return share_response(HTTP_RESP_FAIL, std::string());

    std::string now = ycc::now_local_string();
    std::string salt = ycc::random_hex(4);
    std::string urlmd5 = ycc::md5_hex(req.md5 + now + salt);
    std::string key = ycc::random_hex(2).substr(0, 4);

    if (!mysql.execute("insert into share_picture_list (user, filemd5, file_name, urlmd5, `key`, pv, create_time) values (" +
                       sq(mysql, req.user) + ", " + sq(mysql, req.md5) + ", " + sq(mysql, req.filename) + ", " +
                       sq(mysql, urlmd5) + ", " + sq(mysql, key) + ", 0, " + sq(mysql, now) + ")")) {
        return share_response(HTTP_RESP_FAIL, std::string());
    }

    if (!update_file_ref_count(mysql, req.md5, 1, NULL, NULL)) {
        return share_response(HTTP_RESP_FAIL, std::string());
    }
    if (!update_share_picture_count(mysql, req.user, 1)) {
        return share_response(HTTP_RESP_FAIL, std::string());
    }

    return share_response(HTTP_RESP_OK, urlmd5);
}

std::string cancel_share_picture(const SharePictureRequest &req) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return ycc::status_json(HTTP_RESP_FAIL);

    std::string filemd5;
    std::string lookup = "select filemd5 from share_picture_list where user = " + sq(mysql, req.user) +
                         " and urlmd5 = " + sq(mysql, req.urlmd5);
    if (!mysql.query_one(lookup, &filemd5)) {
        return ycc::status_json(HTTP_RESP_OK);
    }

    update_share_picture_count(mysql, req.user, -1);

    std::string storage_fileid;
    int new_ref_count = 0;
    if (!update_file_ref_count(mysql, filemd5, -1, &storage_fileid, &new_ref_count)) {
        return ycc::status_json(HTTP_RESP_FAIL);
    }

    if (!mysql.execute("delete from share_picture_list where user = " + sq(mysql, req.user) +
                       " and urlmd5 = " + sq(mysql, req.urlmd5))) {
        return ycc::status_json(HTTP_RESP_FAIL);
    }

    if (new_ref_count == 0 && !storage_fileid.empty()) {
        if (!mysql.execute("delete from file_info where md5 = " + sq(mysql, filemd5))) {
            return ycc::status_json(HTTP_RESP_FAIL);
        }
        remove_file_from_storage(storage_fileid);
    }

    return ycc::status_json(HTTP_RESP_OK);
}

std::string browse_response(int code, long pv, const std::string &url,
                            const std::string &user, const std::string &time) {
    ycc::JsonBuilder root;
    root.value()["code"] = code;
    if (code == HTTP_RESP_OK) {
        root.value()["pv"] = Json::Int64(pv);
        root.value()["url"] = url;
        root.value()["user"] = user;
        root.value()["time"] = time;
    }
    return root.print();
}

std::string browse_picture_url(const SharePictureRequest &req) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());

    std::string sql = "select user, filemd5, file_name, pv, create_time from share_picture_list where urlmd5 = " +
                      sq(mysql, req.urlmd5);
    if (mysql_query(mysql.get(), sql.c_str()) != 0) {
        return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());
    }

    ycc::MysqlResult result(mysql_store_result(mysql.get()));
    if (!result.get()) return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row || !row[0] || !row[1] || !row[3] || !row[4]) {
        return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());
    }

    std::string user = row[0];
    std::string filemd5 = row[1];
    long pv = std::atol(row[3]);
    std::string create_time = row[4];

    std::string picture_url;
    if (!mysql.query_one("select url from file_info where md5 = " + sq(mysql, filemd5), &picture_url)) {
        return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());
    }

    if (!mysql.execute("update share_picture_list set pv = " + std::to_string(pv + 1) +
                       " where urlmd5 = " + sq(mysql, req.urlmd5))) {
        return browse_response(HTTP_RESP_FAIL, 0, std::string(), std::string(), std::string());
    }

    return browse_response(HTTP_RESP_OK, pv, picture_url, user, create_time);
}

std::string dispatch_request(const std::string &cmd, const std::string &body) {
    SharePictureRequest req;

    if (cmd == "share") {
        if (!parse_share_request(body, &req)) return ycc::status_json(HTTP_RESP_FAIL);
        if (!ycc::verify_token(req.user, req.token)) return ycc::status_json(HTTP_RESP_TOKEN_ERR);
        return request_share_picture(req);
    }

    if (cmd == "normal") {
        if (!parse_list_request(body, &req)) return ycc::status_json(HTTP_RESP_FAIL);
        if (!ycc::verify_token(req.user, req.token)) return ycc::status_json(HTTP_RESP_TOKEN_ERR);
        return list_response(req);
    }

    if (cmd == "cancel") {
        if (!parse_cancel_request(body, &req)) return ycc::status_json(HTTP_RESP_FAIL);
        if (!ycc::verify_token(req.user, req.token)) return ycc::status_json(HTTP_RESP_TOKEN_ERR);
        return cancel_share_picture(req);
    }

    if (cmd == "browse") {
        if (!parse_browse_request(body, &req)) return ycc::status_json(HTTP_RESP_FAIL);
        return browse_picture_url(req);
    }

    return ycc::status_json(HTTP_RESP_FAIL);
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");

        std::string body;
        if (!ycc::read_stdin_body(kMaxJsonBody, &body)) {
            FCGI_printf("No data from standard input.<p>\n");
            continue;
        }

        std::string cmd = ycc::query_param(getenv("QUERY_STRING"), "cmd");
        LOG(SHAREPIC_LOG_MODULE, SHAREPICS_LOG_PROC, "cmd = %s\n", cmd.c_str());

        std::string response = dispatch_request(cmd, body);
        LOG(SHAREPIC_LOG_MODULE, SHAREPICS_LOG_PROC, "%s\n", response.c_str());
        FCGI_printf("%s", response.c_str());
    }
    return 0;
}


