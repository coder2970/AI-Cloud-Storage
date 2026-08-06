#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

namespace {

const std::size_t kMaxMd5Body = 16 * 1024;

struct Md5Request {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
};

bool parse_md5_request(const std::string &body, Md5Request *request) {
    if (!request) return false;
    ycc::JsonDoc doc(body);
    if (!doc.valid()) return false;

    return doc.string_field("user", &request->user) &&
           doc.string_field("token", &request->token) &&
           doc.string_field("md5", &request->md5) &&
           doc.string_field("fileName", &request->filename) &&
           !request->user.empty() && !request->token.empty() &&
           !request->md5.empty() && !request->filename.empty();
}

int deal_md5(const Md5Request &request) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    std::string user = mysql.escape(request.user);
    std::string md5 = mysql.escape(request.md5);
    std::string filename = mysql.escape(request.filename);

    std::string file_count;
    if (!mysql.query_one("select count from file_info where md5='" + md5 + "' limit 1", &file_count)) {
        return HTTP_RESP_FAIL;
    }

    if (mysql.exists("select 1 from user_file_list where user='" + user +
                     "' and md5='" + md5 + "' and file_name='" + filename + "' limit 1")) {
        return HTTP_RESP_FILE_EXIST;
    }

    if (!mysql.execute("update file_info set count = count + 1 where md5='" + md5 + "'")) {
        return HTTP_RESP_FAIL;
    }

    std::string create_time = mysql.escape(ycc::now_local_string());
    std::string insert_user_file =
        "insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values ('" +
        user + "', '" + md5 + "', '" + create_time + "', '" + filename + "', 0, 0)";
    if (!mysql.execute(insert_user_file)) {
        return HTTP_RESP_FAIL;
    }

    std::string current_count;
    if (mysql.query_one("select count from user_file_count where user='" + user + "' limit 1", &current_count)) {
        if (!mysql.execute("update user_file_count set count = count + 1 where user='" + user + "'")) {
            return HTTP_RESP_FAIL;
        }
    } else {
        if (!mysql.execute("insert into user_file_count (user, count) values('" + user + "', 1)")) {
            return HTTP_RESP_FAIL;
        }
    }

    return HTTP_RESP_OK;
}

void handle_md5() {
    std::string body;
    Md5Request request;
    int code = HTTP_RESP_FAIL;

    if (!ycc::read_stdin_body(kMaxMd5Body, &body) || !parse_md5_request(body, &request)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!ycc::verify_token(request.user, request.token)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_TOKEN_ERR).c_str());
        return;
    }

    code = deal_md5(request);
    FCGI_printf("%s", ycc::status_json(code).c_str());
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_md5();
    }
    return 0;
}
