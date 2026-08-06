#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include "cpp_cgi.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <vector>

extern "C" {
#include "cfg.h"
#include "md5.h"
}

namespace ycc {
namespace {

std::string cfg_value(const char *section, const char *key) {
    std::array<char, 256> value;
    std::array<char, 64> sec;
    std::array<char, 64> k;
    value.fill(0);
    sec.fill(0);
    k.fill(0);

    std::snprintf(sec.data(), sec.size(), "%s", section);
    std::snprintf(k.data(), k.size(), "%s", key);
    if (get_cfg_value_len(CFG_PATH, sec.data(), k.data(), value.data(), value.size()) != 0) {
        return std::string();
    }
    return std::string(value.data());
}

std::string bytes_to_hex(const unsigned char *data, std::size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

} // namespace

JsonDoc::JsonDoc(const std::string &text) : root_(Json::objectValue), valid_(false) {
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(text);

    valid_ = Json::parseFromStream(builder, input, &root_, &errors);
}

JsonDoc::~JsonDoc() {}

bool JsonDoc::valid() const { return valid_ && root_.isObject(); }

bool JsonDoc::string_field(const char *key, std::string *out) const {
    if (!valid() || !key || !out) return false;
    const Json::Value &item = root_[key];
    if (!item.isString()) return false;
    *out = item.asString();
    return true;
}

bool JsonDoc::long_field(const char *key, long *out) const {
    if (!valid() || !key || !out) return false;
    const Json::Value &item = root_[key];
    if (item.isIntegral()) {
        *out = static_cast<long>(item.asInt64());
        return true;
    }
    if (item.isDouble()) {
        *out = static_cast<long>(item.asDouble());
        return true;
    }
    if (item.isString()) {
        *out = std::atol(item.asCString());
        return true;
    }
    return false;
}

bool JsonDoc::int_field(const char *key, int *out) const {
    long value = 0;
    if (!long_field(key, &value)) return false;
    *out = static_cast<int>(value);
    return true;
}

JsonBuilder::JsonBuilder() : root_(Json::objectValue) {}

JsonBuilder::~JsonBuilder() {}

Json::Value &JsonBuilder::value() { return root_; }

const Json::Value &JsonBuilder::value() const { return root_; }

std::string JsonBuilder::print() const {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root_);
}

std::string query_param(const char *query, const char *key) {
    if (!query || !key) return std::string();
    std::string qs(query);
    std::string prefix(key);
    prefix += "=";

    std::size_t pos = 0;
    while (pos <= qs.size()) {
        std::size_t end = qs.find('&', pos);
        std::string part = qs.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (part.compare(0, prefix.size(), prefix) == 0) return part.substr(prefix.size());
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::string();
}
MysqlConn::MysqlConn() : conn_(NULL) {
    std::string host = cfg_value("mysql", "ip");
    std::string user = cfg_value("mysql", "user");
    std::string pwd = cfg_value("mysql", "password");
    std::string db = cfg_value("mysql", "database");
    if (host.empty()) host = "localhost";
    if (user.empty() || db.empty()) return;

    conn_ = mysql_init(NULL);
    if (!conn_) return;

    if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), pwd.c_str(), db.c_str(), 0, NULL, 0)) {
        mysql_close(conn_);
        conn_ = NULL;
        return;
    }
    if (conn_) mysql_query(conn_, "set names utf8mb4");
}

MysqlConn::~MysqlConn() {
    if (conn_) mysql_close(conn_);
}

bool MysqlConn::ok() const { return conn_ != NULL; }

MYSQL *MysqlConn::get() const { return conn_; }

std::string MysqlConn::escape(const std::string &value) const {
    if (!conn_) return std::string();
    std::string out;
    out.resize(value.size() * 2 + 1);
    unsigned long len = mysql_real_escape_string(
        conn_, &out[0], value.data(), static_cast<unsigned long>(value.size()));
    out.resize(len);
    return out;
}

bool MysqlConn::query_one(const std::string &sql, std::string *out) const {
    if (!conn_ || !out || mysql_query(conn_, sql.c_str()) != 0) return false;
    MysqlResult result(mysql_store_result(conn_));
    if (!result.get()) return false;

    MYSQL_ROW row = mysql_fetch_row(result.get());
    bool found = false;
    if (row && row[0]) {
        *out = row[0];
        found = true;
    }
    return found;
}

bool MysqlConn::exists(const std::string &sql) const {
    if (!conn_ || mysql_query(conn_, sql.c_str()) != 0) return false;
    MysqlResult result(mysql_store_result(conn_));
    return result.get() && mysql_num_rows(result.get()) > 0;
}

bool MysqlConn::execute(const std::string &sql) const {
    return conn_ && mysql_query(conn_, sql.c_str()) == 0;
}

MysqlResult::MysqlResult(MYSQL_RES *res) : res_(res) {}

MysqlResult::~MysqlResult() {
    if (res_) mysql_free_result(res_);
}

MYSQL_RES *MysqlResult::get() const { return res_; }

RedisConn::RedisConn() : conn_(NULL) {
    std::string ip = cfg_value("redis", "ip");
    std::string port = cfg_value("redis", "port");
    if (ip.empty() || port.empty()) return;

    conn_ = redisConnect(ip.c_str(), std::atoi(port.c_str()));
    if (conn_ && conn_->err) {
        redisFree(conn_);
        conn_ = NULL;
    }
}

RedisConn::~RedisConn() {
    if (conn_) redisFree(conn_);
}

bool RedisConn::ok() const { return conn_ != NULL; }

bool RedisConn::setex(const std::string &key, unsigned int seconds, const std::string &value) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "SETEX %s %u %s", key.c_str(), seconds, value.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_STATUS &&
              reply->str && std::strcmp(reply->str, "OK") == 0;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::get(const std::string &key, std::string *out) const {
    if (!conn_ || !out) return false;
    redisReply *reply = static_cast<redisReply *>(redisCommand(conn_, "GET %s", key.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_STRING;
    if (ok) out->assign(reply->str, reply->len);
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::key_exists(const std::string &key) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(redisCommand(conn_, "EXISTS %s", key.c_str()));
    if (!reply) return false;
    bool exists = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    freeReplyObject(reply);
    return exists;
}

bool RedisConn::hash_set(const std::string &key, const std::string &field, const std::string &value) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "HSET %s %s %s", key.c_str(), field.c_str(), value.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::hash_get(const std::string &key, const std::string &field, std::string *out) const {
    if (!conn_ || !out) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "HGET %s %s", key.c_str(), field.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_STRING;
    if (ok) out->assign(reply->str, reply->len);
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::hash_del(const std::string &key, const std::string &field) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "HDEL %s %s", key.c_str(), field.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

int RedisConn::zset_exists(const std::string &key, const std::string &member) const {
    if (!conn_) return -1;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "ZSCORE %s %s", key.c_str(), member.c_str()));
    if (!reply) return -1;
    int ret = -1;
    if (reply->type == REDIS_REPLY_STRING) ret = 1;
    else if (reply->type == REDIS_REPLY_NIL) ret = 0;
    freeReplyObject(reply);
    return ret;
}

bool RedisConn::zset_add(const std::string &key, long score, const std::string &member) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "ZADD %s %ld %s", key.c_str(), score, member.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::zset_remove(const std::string &key, const std::string &member) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "ZREM %s %s", key.c_str(), member.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::zset_increment(const std::string &key, const std::string &member) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(
        redisCommand(conn_, "ZINCRBY %s 1 %s", key.c_str(), member.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_STRING;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::expire(const std::string &key, unsigned int seconds) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(redisCommand(conn_, "EXPIRE %s %u", key.c_str(), seconds));
    if (!reply) return false;
    bool ok = reply->type != REDIS_REPLY_ERROR;
    freeReplyObject(reply);
    return ok;
}

bool RedisConn::del(const std::string &key) const {
    if (!conn_) return false;
    redisReply *reply = static_cast<redisReply *>(redisCommand(conn_, "DEL %s", key.c_str()));
    if (!reply) return false;
    bool ok = reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
    freeReplyObject(reply);
    return ok;
}

bool read_stdin_body(std::size_t max_bytes, std::string *out) {
    if (!out) return false;
    const char *content_length = std::getenv("CONTENT_LENGTH");
    if (!content_length) return false;

    char *end = NULL;
    unsigned long len = std::strtoul(content_length, &end, 10);
    if (end == content_length || len == 0 || len > max_bytes) return false;

    out->assign(len, '\0');
    std::size_t got = FCGI_fread(&(*out)[0], 1, len, FCGI_stdin);
    return got == len;
}

std::string md5_hex(const std::string &input) {
    MD5_CTX ctx;
    unsigned char digest[16];
    MD5Init(&ctx);
    MD5Update(&ctx, reinterpret_cast<unsigned char *>(const_cast<char *>(input.data())),
              static_cast<unsigned int>(input.size()));
    MD5Final(&ctx, digest);
    return bytes_to_hex(digest, sizeof(digest));
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> raw(bytes);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.good()) {
        urandom.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
    }
    if (!urandom.good()) {
        std::srand(static_cast<unsigned int>(std::time(NULL)));
        for (std::size_t i = 0; i < raw.size(); ++i) {
            raw[i] = static_cast<unsigned char>(std::rand() % 256);
        }
    }
    return bytes_to_hex(raw.data(), raw.size());
}

std::string config_value(const std::string &section, const std::string &key) {
    return cfg_value(section.c_str(), key.c_str());
}

std::string now_local_string() {
    timeval tv;
    gettimeofday(&tv, NULL);

    std::tm tm_value;
    time_t seconds = static_cast<time_t>(tv.tv_sec);
#ifdef _WIN32
    localtime_s(&tm_value, &seconds);
#else
    localtime_r(&seconds, &tm_value);
#endif

    char out[32] = {0};
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_value);
    return out;
}

bool verify_token(const std::string &user, const std::string &token) {
    RedisConn redis;
    std::string stored;
    return redis.ok() && redis.get(user, &stored) && stored == token;
}

bool ensure_dir(const std::string &path) {
    if (path.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] != '/' && i + 1 != path.size()) continue;
        if (cur.size() == 1 && cur[0] == '/') continue;
#ifdef _WIN32
        if (_mkdir(cur.c_str()) != 0 && errno != EEXIST) return false;
#else
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
    }
    return true;
}

std::string status_json(int code) {
    return std::string("{\"code\":") + std::to_string(code) + "}";
}

std::string login_json(int code, const std::string &token) {
    return std::string("{\"code\":") + std::to_string(code) + ",\"token\":\"" + token + "\"}";
}

std::string chunk_init_json(int code, int chunk_count, const std::string &uploaded_chunks) {
    return std::string("{\"code\":") + std::to_string(code) +
           ",\"chunkCount\":" + std::to_string(chunk_count) +
           ",\"uploadedChunks\":\"" + uploaded_chunks + "\"}";
}

} // namespace ycc


