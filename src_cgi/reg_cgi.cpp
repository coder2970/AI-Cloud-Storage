#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <string>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

namespace {

const std::size_t kMaxRegisterBody = 16 * 1024;

struct RegisterRequest {
    std::string user;
    std::string nick;
    std::string pwd;
    std::string phone;
    std::string email;
};

bool parse_register(const std::string &body, RegisterRequest *request) {
    if (!request) return false;
    ycc::JsonDoc doc(body);
    if (!doc.valid()) return false;

    return doc.string_field("userName", &request->user) &&
           doc.string_field("nickName", &request->nick) &&
           doc.string_field("firstPwd", &request->pwd) &&
           doc.string_field("phone", &request->phone) &&
           doc.string_field("email", &request->email) &&
           !request->user.empty() && !request->nick.empty() && !request->pwd.empty();
}

int register_user(const RegisterRequest &request) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    std::string user = mysql.escape(request.user);
    std::string nick = mysql.escape(request.nick);
    std::string pwd = mysql.escape(request.pwd);
    std::string phone = mysql.escape(request.phone);
    std::string email = mysql.escape(request.email);

    if (mysql.exists("select 1 from user_info where user_name='" + user + "' limit 1")) {
        return HTTP_RESP_USER_EXIST;
    }
    if (mysql.exists("select 1 from user_info where nick_name='" + nick + "' limit 1")) {
        return HTTP_RESP_NICK_EXIST;
    }

    std::string salt = ycc::random_hex(8);
    std::string salted_hash = ycc::md5_hex(salt + pwd);
    std::string create_time = mysql.escape(ycc::now_local_string());

    std::string sql =
        "insert into user_info (user_name, nick_name, password, salt, phone, email, create_time) values ('" +
        user + "', '" + nick + "', '" + salted_hash + "', '" + salt + "', '" +
        phone + "', '" + email + "', '" + create_time + "')";

    return mysql.execute(sql) ? HTTP_RESP_OK : HTTP_RESP_FAIL;
}

void handle_register() {
    std::string body;
    RegisterRequest request;
    int code = HTTP_RESP_FAIL;

    if (ycc::read_stdin_body(kMaxRegisterBody, &body) && parse_register(body, &request)) {
        code = register_user(request);
    }

    FCGI_printf("%s", ycc::status_json(code).c_str());
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_register();
    }
    return 0;
}
