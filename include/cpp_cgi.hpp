#ifndef YCC_CPP_CGI_HPP
#define YCC_CPP_CGI_HPP

#include <mysql/mysql.h>
#include <hiredis/hiredis.h>
#include <json/json.h>

#include <cstddef>
#include <string>

namespace ycc {

class JsonDoc {
public:
    explicit JsonDoc(const std::string &text);
    ~JsonDoc();

    JsonDoc(const JsonDoc &) = delete;
    JsonDoc &operator=(const JsonDoc &) = delete;

    bool valid() const;
    bool string_field(const char *key, std::string *out) const;
    bool long_field(const char *key, long *out) const;
    bool int_field(const char *key, int *out) const;

private:
    Json::Value root_;
    bool valid_;
};


class JsonBuilder {
public:
    JsonBuilder();
    ~JsonBuilder();

    JsonBuilder(const JsonBuilder &) = delete;
    JsonBuilder &operator=(const JsonBuilder &) = delete;

    Json::Value &value();
    const Json::Value &value() const;
    std::string print() const;

private:
    Json::Value root_;
};
class MysqlConn {
public:
    MysqlConn();
    ~MysqlConn();

    MysqlConn(const MysqlConn &) = delete;
    MysqlConn &operator=(const MysqlConn &) = delete;

    bool ok() const;
    MYSQL *get() const;
    std::string escape(const std::string &value) const;
    bool query_one(const std::string &sql, std::string *out) const;
    bool exists(const std::string &sql) const;
    bool execute(const std::string &sql) const;

private:
    MYSQL *conn_;
};

class MysqlResult {
public:
    explicit MysqlResult(MYSQL_RES *res);
    ~MysqlResult();

    MysqlResult(const MysqlResult &) = delete;
    MysqlResult &operator=(const MysqlResult &) = delete;

    MYSQL_RES *get() const;

private:
    MYSQL_RES *res_;
};

class RedisConn {
public:
    RedisConn();
    ~RedisConn();

    RedisConn(const RedisConn &) = delete;
    RedisConn &operator=(const RedisConn &) = delete;

    bool ok() const;
    bool setex(const std::string &key, unsigned int seconds, const std::string &value) const;
    bool get(const std::string &key, std::string *out) const;
    bool key_exists(const std::string &key) const;
    bool hash_set(const std::string &key, const std::string &field, const std::string &value) const;
    bool hash_get(const std::string &key, const std::string &field, std::string *out) const;
    bool hash_del(const std::string &key, const std::string &field) const;
    int zset_exists(const std::string &key, const std::string &member) const;
    bool zset_add(const std::string &key, long score, const std::string &member) const;
    bool zset_remove(const std::string &key, const std::string &member) const;
    bool zset_increment(const std::string &key, const std::string &member) const;
    bool expire(const std::string &key, unsigned int seconds) const;
    bool del(const std::string &key) const;

private:
    redisContext *conn_;
};

bool read_stdin_body(std::size_t max_bytes, std::string *out);
std::string query_param(const char *query, const char *key);
std::string md5_hex(const std::string &input);
std::string random_hex(std::size_t bytes);
std::string now_local_string();
std::string config_value(const std::string &section, const std::string &key);
bool verify_token(const std::string &user, const std::string &token);
bool ensure_dir(const std::string &path);
std::string status_json(int code);
std::string login_json(int code, const std::string &token);
std::string chunk_init_json(int code, int chunk_count, const std::string &uploaded_chunks);

} // namespace ycc

#endif
