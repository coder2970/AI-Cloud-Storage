#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <cctype>
#include <set>
#include <sstream>
#include <string>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

namespace {

const std::size_t kMaxChunkBytes = 64 * 1024 * 1024;
const char *kChunkTempDir = "/tmp/chunks";

bool is_safe_key(const std::string &value) {
    if (value.empty() || value.size() > 256) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_safe_user(const std::string &value) {
    return !value.empty() && value.size() <= 128;
}

int from_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '+' ) {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < value.size()) {
            int hi = from_hex(value[i + 1]);
            int lo = from_hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

bool parse_query_value(const std::string &query, const std::string &key, std::string *out) {
    if (!out) return false;
    std::string prefix = key + "=";
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t next = query.find('&', pos);
        std::size_t end = (next == std::string::npos) ? query.size() : next;
        if (query.compare(pos, prefix.size(), prefix) == 0) {
            *out = url_decode(query.substr(pos + prefix.size(), end - pos - prefix.size()));
            return true;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return false;
}

bool parse_non_negative_int(const std::string &text, int *out) {
    if (!out || text.empty() || text.size() > 10) return false;
    long value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
        if (value > 1000000) return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool write_chunk_file(const std::string &path, const std::string &body) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file.good()) return false;
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return file.good();
}

std::set<int> parse_uploaded_set(const std::string &uploaded) {
    std::set<int> result;
    std::size_t pos = 0;
    while (pos < uploaded.size()) {
        std::size_t comma = uploaded.find(',', pos);
        std::string token = uploaded.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        int index = -1;
        if (parse_non_negative_int(token, &index)) {
            result.insert(index);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

std::string join_uploaded_set(const std::set<int> &uploaded) {
    std::ostringstream out;
    bool first = true;
    for (std::set<int>::const_iterator it = uploaded.begin(); it != uploaded.end(); ++it) {
        if (!first) out << ',';
        out << *it;
        first = false;
    }
    return out.str();
}

bool update_uploaded_chunks(const std::string &file_md5, int chunk_index) {
    ycc::RedisConn redis;
    if (!redis.ok()) return false;

    std::string redis_key = "chunk:" + file_md5;
    std::string uploaded_text;
    redis.hash_get(redis_key, "uploaded", &uploaded_text);

    std::set<int> uploaded = parse_uploaded_set(uploaded_text);
    uploaded.insert(chunk_index);
    return redis.hash_set(redis_key, "uploaded", join_uploaded_set(uploaded));
}

void handle_chunk_upload() {
    const char *query_env = std::getenv("QUERY_STRING");
    if (!query_env) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::string query(query_env);
    std::string file_md5;
    std::string index_text;
    std::string user;
    std::string token;
    int chunk_index = -1;
    if (!parse_query_value(query, "md5", &file_md5) ||
        !parse_query_value(query, "index", &index_text) ||
        !parse_query_value(query, "user", &user) ||
        !parse_query_value(query, "token", &token) ||
        !is_safe_key(file_md5) ||
        !is_safe_user(user) ||
        !parse_non_negative_int(index_text, &chunk_index)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!ycc::verify_token(user, token)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_TOKEN_ERR).c_str());
        return;
    }

    std::string body;
    if (!ycc::read_stdin_body(kMaxChunkBytes, &body)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::string chunk_dir = std::string(kChunkTempDir) + "/" + file_md5;
    if (!ycc::ensure_dir(chunk_dir)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    std::ostringstream path;
    path << chunk_dir << '/' << chunk_index;
    if (!write_chunk_file(path.str(), body)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!update_uploaded_chunks(file_md5, chunk_index)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    FCGI_printf("%s", ycc::status_json(HTTP_RESP_OK).c_str());
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_chunk_upload();
    }
    return 0;
}
