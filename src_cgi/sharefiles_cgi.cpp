#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <cstdlib>
#include <string>

extern "C" {
#include "make_log.h"
#include "cgi_constants.h"
}

#define SHAREFILES_LOG_MODULE "cgi"
#define SHAREFILES_LOG_PROC "sharefiles"

namespace {

const std::size_t kMaxJsonBody = 64 * 1024;
const char kPublicCountKey[] = "FILE_PUBLIC_COUNT";

bool parse_page_request(const std::string &body, int *start, int *count) {
    ycc::JsonDoc doc(body);
    return doc.valid() && doc.int_field("start", start) && doc.int_field("count", count);
}

bool get_share_files_count(long *count) {
    if (!count) return false;
    *count = 0;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return false;

    std::string value;
    std::string sql = std::string("select count from user_file_count where user='") + kPublicCountKey + "'";
    if (!mysql.query_one(sql, &value)) {
        *count = 0;
        return true;
    }

    *count = std::atol(value.c_str());
    return true;
}

std::string count_json(long total, int code) {
    ycc::JsonBuilder root;
    root.value()["code"] = code;
    root.value()["total"] = Json::Int64(total);
    return root.print();
}

void add_string_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = row[index];
}

void add_long_if_present(Json::Value *item, const char *key, MYSQL_ROW row, unsigned int index) {
    if (row[index]) (*item)[key] = Json::Int64(std::atol(row[index]));
}

std::string build_list_sql(const std::string &cmd, int start, int count) {
    if (start < 0) start = 0;
    if (count < 0) count = 0;

    std::string order;
    if (cmd == "pvdesc") {
        order = " order by share_file_list.pv desc";
    } else if (cmd == "pvasc") {
        order = " order by share_file_list.pv asc";
    } else if (cmd != "normal") {
        return std::string();
    }

    return "select share_file_list.*, file_info.url, file_info.size, file_info.type "
           "from file_info, share_file_list "
           "where file_info.md5 = share_file_list.md5" +
           order + " limit " + std::to_string(start) + ", " + std::to_string(count);
}

std::string handle_share_list(const std::string &cmd, int start, int count) {
    long total = 0;
    int code = HTTP_RESP_OK;
    int rows = 0;

    ycc::JsonBuilder root;
    Json::Value files(Json::arrayValue);

    if (!get_share_files_count(&total)) {
        code = HTTP_RESP_FAIL;
    } else if (total > 0) {
        ycc::MysqlConn mysql;
        std::string sql = mysql.ok() ? build_list_sql(cmd, start, count) : std::string();
        if (!mysql.ok() || sql.empty()) {
            code = HTTP_RESP_FAIL;
        } else {
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
                        add_string_if_present(&item, "user", row, 1);
                        add_string_if_present(&item, "md5", row, 2);
                        add_string_if_present(&item, "file_name", row, 3);
                        item["share_status"] = 1;
                        add_long_if_present(&item, "pv", row, 4);
                        add_string_if_present(&item, "create_time", row, 5);
                        add_string_if_present(&item, "url", row, 6);
                        add_long_if_present(&item, "size", row, 7);
                        add_string_if_present(&item, "type", row, 8);
                        files.append(item);
                        ++rows;
                    }
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

std::string dispatch_request(const std::string &cmd, const std::string &body) {
    if (cmd == "count") {
        long total = 0;
        return get_share_files_count(&total) ? count_json(total, HTTP_RESP_OK)
                                             : count_json(0, HTTP_RESP_FAIL);
    }

    int start = 0;
    int count = 0;
    if (!parse_page_request(body, &start, &count)) return ycc::status_json(HTTP_RESP_FAIL);
    return handle_share_list(cmd, start, count);
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");

        std::string cmd = ycc::query_param(getenv("QUERY_STRING"), "cmd");
        LOG(SHAREFILES_LOG_MODULE, SHAREFILES_LOG_PROC, "cmd = %s\n", cmd.c_str());

        std::string body;
        if (cmd != "count" && !ycc::read_stdin_body(kMaxJsonBody, &body)) {
            FCGI_printf("No data from standard input.<p>\n");
            continue;
        }

        std::string response = dispatch_request(cmd, body);
        LOG(SHAREFILES_LOG_MODULE, SHAREFILES_LOG_PROC, "%s\n", response.c_str());
        FCGI_printf("%s", response.c_str());
    }
    return 0;
}


