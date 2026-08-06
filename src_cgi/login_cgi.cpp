#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <string>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

namespace {

const std::size_t kMaxLoginBody = 16 * 1024;
const unsigned int kTokenTtlSeconds = 86400;

struct LoginRequest {
    std::string user;
    std::string pwd;
};

bool parse_login(const std::string &body, LoginRequest *request) {
    if (!request) return false;
    ycc::JsonDoc doc(body);
    if (!doc.valid()) return false;

    return doc.string_field("user", &request->user) &&
           doc.string_field("pwd", &request->pwd) &&
           !request->user.empty() && !request->pwd.empty();
}

bool check_password(const LoginRequest &request) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return false;

    std::string user = mysql.escape(request.user);
    std::string stored_hash;
    std::string salt;
    if (!mysql.query_one("select password from user_info where user_name='" + user + "' limit 1", &stored_hash)) {
        return false;
    }
    if (!mysql.query_one("select salt from user_info where user_name='" + user + "' limit 1", &salt)) {
        return false;
    }

    return ycc::md5_hex(salt + request.pwd) == stored_hash;
}

bool issue_token(const std::string &user, std::string *token) {
    if (!token) return false;
    ycc::RedisConn redis;
    if (!redis.ok()) return false;

    *token = ycc::md5_hex(user + ":" + ycc::random_hex(32) + ":" + ycc::now_local_string());
    return redis.setex(user, kTokenTtlSeconds, *token);
}

void handle_login() {
    std::string body;
    LoginRequest request;
    std::string token;

    if (!ycc::read_stdin_body(kMaxLoginBody, &body) ||
        !parse_login(body, &request) ||
        !check_password(request) ||
        !issue_token(request.user, &token)) {
        FCGI_printf("%s", ycc::login_json(HTTP_RESP_FAIL, "fail").c_str());
        return;
    }

    FCGI_printf("%s", ycc::login_json(HTTP_RESP_OK, token).c_str());
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_login();
    }
    return 0;
}
