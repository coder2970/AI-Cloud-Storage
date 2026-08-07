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

    int code = ycc::bind_existing_file_to_user(mysql, request.user, request.md5, request.filename);
    return code == -1 ? HTTP_RESP_FAIL : code;
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
