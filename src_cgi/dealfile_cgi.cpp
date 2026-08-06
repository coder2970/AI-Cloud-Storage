#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "make_log.h"
#include "redis_keys.h"
#include "cgi_constants.h"
}

#define DEALFILE_LOG_MODULE "cgi"
#define DEALFILE_LOG_PROC "dealfile"

namespace {

const std::size_t kMaxJsonBody = 64 * 1024;
const char kFaissLockDir[] = "/tmp/faiss_locks";

struct DealFileRequest {
    std::string user;
    std::string token;
    std::string md5;
    std::string filename;
};

bool parse_request(const std::string &body, DealFileRequest *req) {
    if (!req) return false;
    ycc::JsonDoc doc(body);
    return doc.valid() &&
           doc.string_field("user", &req->user) &&
           doc.string_field("token", &req->token) &&
           doc.string_field("md5", &req->md5) &&
           doc.string_field("filename", &req->filename);
}

std::string file_id(const DealFileRequest &req) {
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
    int count = 0;
    std::string count_sql = std::string("select count from user_file_count where user = '") + FILE_PUBLIC_COUNT + "'";
    std::string value;
    if (!mysql.query_one(count_sql, &value)) {
        if (delta > 0) {
            return mysql.execute(std::string("insert into user_file_count (user, count) values('") +
                                 FILE_PUBLIC_COUNT + "', " + std::to_string(delta) + ")");
        }
        return true;
    }

    count = std::atoi(value.c_str()) + delta;
    if (count < 0) count = 0;
    return mysql.execute(std::string("update user_file_count set count = ") + std::to_string(count) +
                         " where user = '" + FILE_PUBLIC_COUNT + "'");
}

bool decrement_user_file_count(const ycc::MysqlConn &mysql, const std::string &user) {
    int count = 0;
    std::string sql = "select count from user_file_count where user = " + sq(mysql, user);
    if (!query_int(mysql, sql, &count)) return false;
    if (count <= 0) return true;
    return mysql.execute("update user_file_count set count = " + std::to_string(count - 1) +
                         " where user = " + sq(mysql, user));
}

void mark_user_index_dirty(const std::string &user) {
    if (!ycc::ensure_dir(kFaissLockDir)) return;
    std::string path = std::string(kFaissLockDir) + "/" + ycc::md5_hex(user) + ".dirty";
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
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
    LOG(DEALFILE_LOG_MODULE, DEALFILE_LOG_PROC, "remove_file_from_storage ret = %d\n", ret);
    return ret;
}

int share_file(const DealFileRequest &req) {
    ycc::RedisConn redis;
    if (!redis.ok()) return HTTP_RESP_FAIL;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    const std::string fid = file_id(req);
    int exists_in_redis = redis.zset_exists(FILE_PUBLIC_ZSET, fid);
    if (exists_in_redis == 1) return HTTP_RESP_DEALFILE_EXIST;
    if (exists_in_redis < 0) return HTTP_RESP_FAIL;

    std::string exists_sql = "select * from share_file_list where md5 = " + sq(mysql, req.md5) +
                             " and file_name = " + sq(mysql, req.filename);
    if (mysql.exists(exists_sql)) {
        redis.zset_add(FILE_PUBLIC_ZSET, 0, fid);
        redis.hash_set(FILE_NAME_HASH, fid, req.filename);
        return HTTP_RESP_DEALFILE_EXIST;
    }

    if (!mysql.execute("update user_file_list set shared_status = 1 where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    std::string now = ycc::now_local_string();
    if (!mysql.execute("insert into share_file_list (user, md5, create_time, file_name, pv) values (" +
                       sq(mysql, req.user) + ", " + sq(mysql, req.md5) + ", " + sq(mysql, now) + ", " +
                       sq(mysql, req.filename) + ", 0)")) {
        return HTTP_RESP_FAIL;
    }

    if (!update_public_count(mysql, 1)) return HTTP_RESP_FAIL;
    redis.zset_add(FILE_PUBLIC_ZSET, 0, fid);
    redis.hash_set(FILE_NAME_HASH, fid, req.filename);
    return HTTP_RESP_OK;
}

int delete_file(const DealFileRequest &req) {
    ycc::RedisConn redis;
    if (!redis.ok()) return HTTP_RESP_FAIL;

    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    const std::string fid = file_id(req);
    bool redis_has_share_record = false;
    int shared_status = 0;

    int exists_in_redis = redis.zset_exists(FILE_PUBLIC_ZSET, fid);
    if (exists_in_redis == 1) {
        shared_status = 1;
        redis_has_share_record = true;
    } else if (exists_in_redis == 0) {
        std::string sql = "select shared_status from user_file_list where user = " + sq(mysql, req.user) +
                          " and md5 = " + sq(mysql, req.md5) +
                          " and file_name = " + sq(mysql, req.filename);
        if (!query_int(mysql, sql, &shared_status)) return HTTP_RESP_FAIL;
    } else {
        return HTTP_RESP_FAIL;
    }

    if (shared_status == 1) {
        if (!mysql.execute("delete from share_file_list where user = " + sq(mysql, req.user) +
                           " and md5 = " + sq(mysql, req.md5) +
                           " and file_name = " + sq(mysql, req.filename))) {
            return HTTP_RESP_FAIL;
        }
        if (!update_public_count(mysql, -1)) return HTTP_RESP_FAIL;
        if (redis_has_share_record) {
            redis.zset_remove(FILE_PUBLIC_ZSET, fid);
            redis.hash_del(FILE_NAME_HASH, fid);
        }
    }

    if (!decrement_user_file_count(mysql, req.user)) return HTTP_RESP_FAIL;

    if (!mysql.execute("delete from user_file_list where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    if (!mysql.execute("delete from user_file_ai_desc where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5))) {
        return HTTP_RESP_FAIL;
    }
    if (mysql_affected_rows(mysql.get()) > 0) mark_user_index_dirty(req.user);

    int file_ref_count = 0;
    if (!query_int(mysql, "select count from file_info where md5 = " + sq(mysql, req.md5), &file_ref_count)) {
        return HTTP_RESP_FAIL;
    }
    if (file_ref_count > 0) --file_ref_count;

    if (!mysql.execute("update file_info set count = " + std::to_string(file_ref_count) +
                       " where md5 = " + sq(mysql, req.md5))) {
        return HTTP_RESP_FAIL;
    }

    if (file_ref_count == 0) {
        std::string storage_fileid;
        if (!mysql.query_one("select file_id from file_info where md5 = " + sq(mysql, req.md5), &storage_fileid)) {
            return HTTP_RESP_FAIL;
        }
        if (!mysql.execute("delete from file_info where md5 = " + sq(mysql, req.md5))) {
            return HTTP_RESP_FAIL;
        }
        if (remove_file_from_storage(storage_fileid) != 0) return HTTP_RESP_FAIL;
    }

    return HTTP_RESP_OK;
}

int pv_file(const DealFileRequest &req) {
    ycc::MysqlConn mysql;
    if (!mysql.ok()) return HTTP_RESP_FAIL;

    std::string sql = "select pv, shared_status from user_file_list where user = " + sq(mysql, req.user) +
                      " and md5 = " + sq(mysql, req.md5) +
                      " and file_name = " + sq(mysql, req.filename);
    if (mysql_query(mysql.get(), sql.c_str()) != 0) return HTTP_RESP_FAIL;

    ycc::MysqlResult result(mysql_store_result(mysql.get()));
    if (!result.get()) return HTTP_RESP_FAIL;
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row || !row[0] || !row[1]) return HTTP_RESP_FAIL;

    int pv = std::atoi(row[0]);
    int shared_status = std::atoi(row[1]);
    int next_pv = pv + 1;

    if (!mysql.execute("update user_file_list set pv = " + std::to_string(next_pv) +
                       " where user = " + sq(mysql, req.user) +
                       " and md5 = " + sq(mysql, req.md5) +
                       " and file_name = " + sq(mysql, req.filename))) {
        return HTTP_RESP_FAIL;
    }

    if (shared_status == 1) {
        if (!mysql.execute("update share_file_list set pv = " + std::to_string(next_pv) +
                           " where user = " + sq(mysql, req.user) +
                           " and md5 = " + sq(mysql, req.md5) +
                           " and file_name = " + sq(mysql, req.filename))) {
            return HTTP_RESP_FAIL;
        }
        ycc::RedisConn redis;
        if (redis.ok()) redis.zset_increment(FILE_PUBLIC_ZSET, file_id(req));
    }

    return HTTP_RESP_OK;
}

int dispatch_request(const std::string &cmd, const DealFileRequest &req) {
    if (!ycc::verify_token(req.user, req.token)) return HTTP_RESP_TOKEN_ERR;
    if (cmd == "share") return share_file(req);
    if (cmd == "del") return delete_file(req);
    if (cmd == "pv") return pv_file(req);
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

        DealFileRequest req;
        if (!parse_request(body, &req)) {
            FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
            continue;
        }

        std::string cmd = ycc::query_param(getenv("QUERY_STRING"), "cmd");
        LOG(DEALFILE_LOG_MODULE, DEALFILE_LOG_PROC,
            "cmd=%s user=%s md5=%s file_name=%s\n",
            cmd.c_str(), req.user.c_str(), req.md5.c_str(), req.filename.c_str());

        int code = dispatch_request(cmd, req);
        FCGI_printf("%s", ycc::status_json(code).c_str());
    }
    return 0;
}

