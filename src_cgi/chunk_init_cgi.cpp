#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <sstream>
#include <string>

#include "cpp_cgi.hpp"
#include "cgi_constants.h"

namespace {

const std::size_t kMaxChunkInitBody = 16 * 1024;
const unsigned int kChunkMetaTtlSeconds = 86400;
const char *kChunkTempDir = "/tmp/chunks";

struct ChunkInitRequest {
    std::string user;
    std::string token;
    std::string filename;
    std::string md5;
    long size;
    int chunk_count;
};

std::string to_string_compat(long value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

bool parse_chunk_init(const std::string &body, ChunkInitRequest *request) {
    if (!request) return false;
    ycc::JsonDoc doc(body);
    if (!doc.valid()) return false;

    request->size = 0;
    request->chunk_count = 0;
    return doc.string_field("user", &request->user) &&
           doc.string_field("token", &request->token) &&
           doc.string_field("filename", &request->filename) &&
           doc.string_field("md5", &request->md5) &&
           doc.long_field("size", &request->size) &&
           doc.int_field("chunkCount", &request->chunk_count) &&
           !request->user.empty() && !request->token.empty() &&
           !request->filename.empty() && !request->md5.empty() &&
           request->size > 0 && request->chunk_count > 0;
}

bool init_chunk_meta(const ChunkInitRequest &request, std::string *uploaded_chunks) {
    if (!uploaded_chunks) return false;
    ycc::RedisConn redis;
    if (!redis.ok()) return false;

    std::string redis_key = "chunk:" + request.md5;
    if (redis.key_exists(redis_key)) {
        if (!redis.hash_get(redis_key, "uploaded", uploaded_chunks)) {
            uploaded_chunks->clear();
        }
        return true;
    }

    uploaded_chunks->clear();
    if (!redis.hash_set(redis_key, "filename", request.filename) ||
        !redis.hash_set(redis_key, "filesize", to_string_compat(request.size)) ||
        !redis.hash_set(redis_key, "chunk_count", to_string_compat(request.chunk_count)) ||
        !redis.hash_set(redis_key, "user", request.user) ||
        !redis.hash_set(redis_key, "uploaded", "") ||
        !redis.expire(redis_key, kChunkMetaTtlSeconds)) {
        return false;
    }

    return true;
}

void handle_chunk_init() {
    std::string body;
    ChunkInitRequest request;
    std::string uploaded_chunks;

    if (!ycc::read_stdin_body(kMaxChunkInitBody, &body) || !parse_chunk_init(body, &request)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!ycc::verify_token(request.user, request.token)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_TOKEN_ERR).c_str());
        return;
    }

    if (!ycc::ensure_dir(kChunkTempDir) || !ycc::ensure_dir(std::string(kChunkTempDir) + "/" + request.md5)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    if (!init_chunk_meta(request, &uploaded_chunks)) {
        FCGI_printf("%s", ycc::status_json(HTTP_RESP_FAIL).c_str());
        return;
    }

    FCGI_printf("%s", ycc::chunk_init_json(HTTP_RESP_OK, request.chunk_count, uploaded_chunks).c_str());
}

} // namespace

int main() {
    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-type: text/html\r\n\r\n");
        handle_chunk_init();
    }
    return 0;
}
