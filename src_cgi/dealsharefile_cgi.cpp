#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <cstdlib>
#include <string>

extern "C" {
#include "make_log.h"
#include "redis_keys.h"
#include "cgi_constants.h"
}

#define DEALSHAREFILE_LOG_MODULE "cgi"
#define DEALSHAREFILE_LOG_PROC "dealsharefile"

namespace {

const std::size_t kMaxJsonBody = 64 * 1024;

struct ShareRequest {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
};

bool parse_request(const std::string &body, ShareRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("user", &req->user) &&
           doc.string_field("token", &req->token) &&
           doc.string_field("md5", &req->md5) &&
           doc.string_field("filename", &req->filename);
}

std::string file_id(const ShareRequest &req) {
    return req.md5 + req.filename;
}

std::string sq(const ycc::MysqlConn &mysql, const std::string &value) {
    return "'" + mysql.escape(value) + "'";
}

bool query_int(const ycc::MysqlConn &mysql, const std::string &sql, int *out) {
    std::string value;
    if (!out || !mysql.query_one(sql, &value)) return false;
    *out = std::atoi(value.c_str());
    return true;
}

bool update_public_count(const ycc::MysqlConn &mysql, int delta) {
    std::string value;
    std::string sql = std::string("select count from user_file_count where user = '") + FILE_PUBLIC_COUNT + "'";
    if (!mysql.query_one(sql, &value)) {
        if (delta > 0) {
            return mysql.execute(std::string("insert into user_file_count (user, count) values('") +
                                 FILE_PUBLIC_COUNT + "', " + std::to_string(delta) + ")");
        }
        return true;
    }

    int count = std::atoi(value.c_str()) + delta;
    if (count <= 0) {
        return mysql.execute(std::string("delete from user_file_count where user = '") + FILE_PUBLIC_COUNT + "'");
    }
    return mysql.execute(std::string("update user_file_count set count = ") + std::to_string(count) +
                         " where user = '" + FILE_PUBLIC_COUNT + "'");
}

bool increment_user_file_count(const ycc::MysqlConn &mysql, const std::string &user) {
    std::string value;
    std::string sql = "select count from user_file_count where user = " + sq(mysql, user);
    if (!mysql.query_one(sql, &value)) {
        return mysql.execute("insert into user_file_count (user, count) values(" + sq(mysql, user) + ", 1)");
    }

    int count = std::atoi(value.c_str()) + 1;
    return mysql.execute("update user_file_count set count = " + std::to_string(count) +
                         " where user = " + sq(mysql, user));
}

int pv_file(const ShareRequest &req) {
    ycc::RedisConn redis;
    if (!redis.ok()) return HTTP_RESP_FAIL;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    int pv = 0;
    std::string sql = "select pv from share_file_list where md5 = " + sq(mysql, req.md5) +
                      " and file_name = " + sq(mysql, req.filename);
    if (!query_int(mysql, sql, &pv)) return HTTP_RESP_FAIL;

    int next_pv = pv + 1;
    if (!mysql.execute("update share_file_list set pv = " + std::to_string(next_pv) +
                       " where md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    std::string fid = file_id(req);
    int in_redis = redis.zset_exists(FILE_PUBLIC_ZSET, fid);
    if (in_redis == 1) {
        if (!redis.zset_increment(FILE_PUBLIC_ZSET, fid)) return HTTP_RESP_FAIL;
    } else if (in_redis == 0) {
        redis.zset_add(FILE_PUBLIC_ZSET, next_pv, fid);
        redis.hash_set(FILE_NAME_HASH, fid, req.filename);
    } else {
        return HTTP_RESP_FAIL;
    }

    return HTTP_RESP_OK;
}

int cancel_share_file(const ShareRequest &req) {
    ycc::RedisConn redis;
    if (!redis.ok()) return HTTP_RESP_FAIL;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    if (!mysql.execute("update user_file_list set shared_status = 0 where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    if (!update_public_count(mysql, -1)) return HTTP_RESP_FAIL;

    if (!mysql.execute("delete from share_file_list where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    std::string fid = file_id(req);
    if (!redis.zset_remove(FILE_PUBLIC_ZSET, fid)) return HTTP_RESP_FAIL;
    if (!redis.hash_del(FILE_NAME_HASH, fid)) return HTTP_RESP_FAIL;
    return HTTP_RESP_OK;
}

int save_file(const ShareRequest &req) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    std::string exists_sql = "select * from user_file_list where user = " + sq(mysql, req.user) +
                             " and md5 = " + sq(mysql, req.md5) +
                             " and file_name = " + sq(mysql, req.filename);
    if (mysql.exists(exists_sql)) return HTTP_RESP_FILE_EXIST;

    int file_ref_count = 0;
    if (!query_int(mysql, "select count from file_info where md5 = " + sq(mysql, req.md5), &file_ref_count)) {
        return HTTP_RESP_FAIL;
    }

    if (!mysql.execute("update file_info set count = " + std::to_string(file_ref_count + 1) +
                       " where md5 = " + sq(mysql, req.md5))) {
        return HTTP_RESP_FAIL;
    }

    std::string now = ycc::now_local_string();
    if (!mysql.execute("insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values (" +
                       sq(mysql, req.user) + ", " + sq(mysql, req.md5) + ", " + sq(mysql, now) + ", " +
                       sq(mysql, req.filename) + ", 0, 0)")) {
        return HTTP_RESP_FAIL;
    }

    if (!increment_user_file_count(mysql, req.user)) return HTTP_RESP_FAIL;
    return HTTP_RESP_OK;
}

int dispatch_request(const std::string &cmd, const ShareRequest &req) {
    if (!ycc::verify_token(req.user, req.token)) return HTTP_RESP_TOKEN_ERR;
    if (cmd == "pv") return pv_file(req);
    if (cmd == "cancel") return cancel_share_file(req);
    if (cmd == "save") return save_file(req);
    return HTTP_RESP_FAIL;
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

        ShareRequest req;
        if (!parse_request(body, &req)) {
            FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
            continue;
        }

        std::string cmd = ycc::query_param(getenv("QUERY_STRING"), "cmd");
        LOG(DEALSHAREFILE_LOG_MODULE, DEALSHAREFILE_LOG_PROC,
            "cmd=%s user=%s md5=%s file_name=%s\n",
            cmd.c_str(), req.user.c_str(), req.md5.c_str(), req.filename.c_str());

        int code = dispatch_request(cmd, req);
        FCGI_printf("%s", ycc::status_json(code).c_str());
    }
    return 0;
}

