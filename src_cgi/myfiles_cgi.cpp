#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <cstdlib>
#include <string>

extern "C" {
#include "make_log.h"
#include "cgi_constants.h"
}

#define MYFILES_LOG_MODULE "cgi"
#define MYFILES_LOG_PROC "myfiles"

namespace {

const std::size_t kMaxJsonBody = 64 * 1024;

bool parse_user_token(const std::string &body, std::string *user, std::string *token) {
    ycc::JsonDoc doc(body);
    return doc.valid() && doc.string_field("user", user) && doc.string_field("token", token);
}

bool parse_list_request(const std::string &body, std::string *user, std::string *token,
                        int *start, int *count) {
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("user", user) &&
           doc.string_field("token", token) &&
           doc.int_field("start", start) &&
           doc.int_field("count", count);
}

std::string files_count_json(long total, int code) {
    ycc::JsonBuilder root;
    root.value()["code"] = code;
    root.value()["total"] = Json::Int64(total);
    return root.print();
}

bool get_user_files_count(const std::string &user, long *count) {
    if (!count) return false;
    *count = 0;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return false;

    std::string value;
    std::string sql = "select count from user_file_count where user='" + mysql.escape(user) + "'";
    if (!mysql.query_one(sql, &value)) {
        *count = 0;
        return true;
    }

    *count = std::atol(value.c_str());
    return true;
}

std::string handle_count(const std::string &user) {
    long total = 0;
    if (!get_user_files_count(user, &total)) {
        return files_count_json(0, HTTP_RESP_FAIL);
    }
    return files_count_json(total, HTTP_RESP_OK);
}

std::string list_sql(const std::string &cmd, const std::string &user, int start, int count,
                     const ycc::MysqlConn &mysql) {
    std::string order;
    if (cmd == "pvasc") {
        order = " order by pv asc";
    } else if (cmd == "pvdesc") {
        order = " order by pv desc";
    } else if (cmd != "normal") {
        return std::string();
    }

    if (start < 0) start = 0;
    if (count < 0) count = 0;

    return "select user_file_list.*, file_info.url, file_info.size, file_info.type "
           "from file_info, user_file_list "
           "where user = '" + mysql.escape(user) + "' and file_info.md5 = user_file_list.md5" +
           order + " limit " + std::to_string(start) + ", " + std::to_string(count);
}

void add_string_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = row[index];
}

void add_long_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = Json::Int64(std::atol(row[index]));
}

std::string handle_list(const std::string &cmd, const std::string &user, int start, int count) {
    long total = 0;
    int code = HTTP_RESP_OK;
    int rows = 0;

    ycc::JsonBuilder root;
    Json::Value files(Json::arrayValue);

    if (!get_user_files_count(user, &total)) {
        code = HTTP_RESP_FAIL;
    } else {
        ycc::MysqlConn mysql;
        std::string sql = mysql.ok() ? list_sql(cmd, user, start, count, mysql) : std::string();
        if (!mysql.ok() || sql.empty() || mysql_query(mysql.get(), sql.c_str()) != 0) {
            code = HTTP_RESP_FAIL;
        } else {
            ycc::MysqlResult result(mysql_store_result(mysql.get()));
            if (!result.get()) {
                code = HTTP_RESP_FAIL;
            } else {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(result.get())) != NULL) {
                    Json::Value item(Json::objectValue);
                    add_string_if_present(&item, "user", row, 1);
                    add_string_if_present(&item, "md5", row, 2);
                    add_string_if_present(&item, "create_time", row, 3);
                    add_string_if_present(&item, "file_name", row, 4);
                    add_long_if_present(&item, "share_status", row, 5);
                    add_long_if_present(&item, "pv", row, 6);
                    add_string_if_present(&item, "url", row, 7);
                    add_long_if_present(&item, "size", row, 8);
                    add_string_if_present(&item, "type", row, 9);
                    files.append(item);
                    ++rows;
                }
            }
        }
    }

    root.value()["files"] = files;
    root.value()["code"] = code;
    root.value()["count"] = rows;
    root.value()["total"] = Json::Int64(total);
    return root.print();
}

std::string dispatch_request(const std::string &cmd, const std::string &body) {
    std::string user;
    std::string token;

    if (cmd == "count") {
        if (!parse_user_token(body, &user, &token)) return ycc::status_json(HTTP_RESP_FAIL);
        if (!ycc::verify_token(user, token)) return ycc::status_json(HTTP_RESP_FAIL);
        return handle_count(user);
    }

    int start = 0;
    int count = 0;
    if (!parse_list_request(body, &user, &token, &start, &count)) return ycc::status_json(HTTP_RESP_FAIL);
    if (!ycc::verify_token(user, token)) return ycc::status_json(HTTP_RESP_FAIL);
    return handle_list(cmd, user, start, count);
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
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "cmd = %s\n", cmd.c_str());
        std::string response = dispatch_request(cmd, body);
        FCGI_printf("%s", response.c_str());
    }
    return 0;
}



