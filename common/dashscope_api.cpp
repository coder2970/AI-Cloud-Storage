/**
 * @file dashscope_api.cpp
 * @brief  封装 libcurl 调用阿里百炼 DashScope API
 */

#include "dashscope_api.h"
#include "make_log.h"

#include <curl/curl.h>
#include <json/json.h>

#include <stdio.h>
#include <string.h>

#include <sstream>
#include <string>

#define DS_LOG_MODULE "cgi"
#define DS_LOG_PROC   "dashscope"

#define DASHSCOPE_VL_URL   "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
#define DASHSCOPE_EMB_URL  "https://dashscope.aliyuncs.com/api/v1/services/embeddings/text-embedding/text-embedding"

namespace {

size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    std::string *response = static_cast<std::string *>(userdata);

    response->append(static_cast<const char *>(ptr), total);
    return total;
}

std::string json_to_string(const Json::Value &root)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

bool parse_json(const std::string &text, Json::Value *root)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(text);

    return Json::parseFromStream(builder, input, root, &errors);
}

bool append_json_headers(struct curl_slist **headers, const char *api_key, char *auth_header, size_t auth_len)
{
    if (headers == NULL || api_key == NULL || auth_header == NULL || auth_len == 0) {
        return false;
    }

    snprintf(auth_header, auth_len, "Authorization: Bearer %s", api_key);
    *headers = curl_slist_append(*headers, "Content-Type: application/json");
    *headers = curl_slist_append(*headers, auth_header);
    return *headers != NULL;
}

const Json::Value *object_member(const Json::Value &value, const char *key)
{
    if (!value.isObject() || !value.isMember(key)) {
        return NULL;
    }

    return &value[key];
}

void log_api_error(const Json::Value &resp, const char *prefix)
{
    const Json::Value *code = object_member(resp, "code");
    if (code != NULL && code->isString()) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "%s error code: %s\n", prefix, code->asCString());
    }

    const Json::Value *message = object_member(resp, "message");
    if (message != NULL && message->isString()) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "%s error message: %s\n", prefix, message->asCString());
    }
}

bool response_has_api_error(const Json::Value &resp)
{
    const Json::Value *code = object_member(resp, "code");
    return code != NULL && code->isString();
}

const char *extract_description_text(const Json::Value &resp)
{
    const Json::Value *output = object_member(resp, "output");
    if (output == NULL) return NULL;

    const Json::Value *choices = object_member(*output, "choices");
    if (choices == NULL || !choices->isArray() || choices->empty()) return NULL;

    const Json::Value &choice0 = (*choices)[0];
    const Json::Value *message = object_member(choice0, "message");
    if (message == NULL) return NULL;

    const Json::Value *content = object_member(*message, "content");
    if (content == NULL) return NULL;

    if (content->isArray() && !content->empty()) {
        const Json::Value &first = (*content)[0];
        const Json::Value *text = object_member(first, "text");
        if (text != NULL && text->isString()) {
            return text->asCString();
        }
    }

    if (content->isString()) {
        return content->asCString();
    }

    return NULL;
}

const Json::Value *extract_embedding_array(const Json::Value &resp)
{
    const Json::Value *output = object_member(resp, "output");
    if (output == NULL) return NULL;

    const Json::Value *embeddings = object_member(*output, "embeddings");
    if (embeddings == NULL || !embeddings->isArray() || embeddings->empty()) return NULL;

    const Json::Value &emb0 = (*embeddings)[0];
    const Json::Value *embedding = object_member(emb0, "embedding");
    if (embedding == NULL || !embedding->isArray()) return NULL;

    return embedding;
}

} // namespace

/**
 * 调用 Qwen-VL 多模态模型描述图片
 */
int dashscope_describe_image(const char *api_key, const char *image_url,
                              char *out_desc, int max_len)
{
    int ret = -1;
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    std::string response;
    char auth_header[512] = {0};
    CURLcode res;

    if (api_key == NULL || image_url == NULL || out_desc == NULL || max_len <= 0) {
        return -1;
    }

    Json::Value root(Json::objectValue);
    root["model"] = "qwen-vl-plus";
    root["input"]["messages"] = Json::Value(Json::arrayValue);

    Json::Value message(Json::objectValue);
    message["role"] = "user";
    message["content"] = Json::Value(Json::arrayValue);

    Json::Value image_item(Json::objectValue);
    image_item["image"] = image_url;
    message["content"].append(image_item);

    Json::Value text_item(Json::objectValue);
    text_item["text"] = "请用中文详细描述这张图片的内容，包括主要物体、场景、颜色、文字等信息。";
    message["content"].append(text_item);
    root["input"]["messages"].append(message);

    std::string json_str = json_to_string(root);
    LOG(DS_LOG_MODULE, DS_LOG_PROC, "describe_image request: %s\n", json_str.c_str());

    curl = curl_easy_init();
    if (!curl) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "curl_easy_init failed\n");
        goto END;
    }

    if (!append_json_headers(&headers, api_key, auth_header, sizeof(auth_header))) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "append headers failed\n");
        goto END;
    }

    curl_easy_setopt(curl, CURLOPT_URL, DASHSCOPE_VL_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        goto END;
    }

    LOG(DS_LOG_MODULE, DS_LOG_PROC, "describe_image response: %.500s\n", response.c_str());

    {
        Json::Value resp;
        if (!parse_json(response, &resp)) {
            LOG(DS_LOG_MODULE, DS_LOG_PROC, "parse response failed\n");
            goto END;
        }

        if (response_has_api_error(resp)) {
            log_api_error(resp, "API");
            goto END;
        }

        const char *desc_text = extract_description_text(resp);
        if (desc_text != NULL) {
            snprintf(out_desc, static_cast<size_t>(max_len), "%s", desc_text);
            ret = 0;
        }
    }

END:
    if (curl) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return ret;
}

/**
 * 调用 text-embedding-v3 获取文本向量
 */
int dashscope_get_embedding(const char *api_key, const char *model,
                             const char *text,
                             float *out_vector, int dimension)
{
    int ret = -1;
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    std::string response;
    char auth_header[512] = {0};
    CURLcode res;

    if (api_key == NULL || model == NULL || text == NULL || out_vector == NULL || dimension <= 0) {
        return -1;
    }

    Json::Value root(Json::objectValue);
    root["model"] = model;
    root["input"]["texts"] = Json::Value(Json::arrayValue);
    root["input"]["texts"].append(text);
    root["parameters"]["dimension"] = dimension;

    std::string json_str = json_to_string(root);
    LOG(DS_LOG_MODULE, DS_LOG_PROC, "embedding request: %.200s\n", json_str.c_str());

    curl = curl_easy_init();
    if (!curl) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "curl_easy_init failed\n");
        goto END;
    }

    if (!append_json_headers(&headers, api_key, auth_header, sizeof(auth_header))) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "append headers failed\n");
        goto END;
    }

    curl_easy_setopt(curl, CURLOPT_URL, DASHSCOPE_EMB_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG(DS_LOG_MODULE, DS_LOG_PROC, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        goto END;
    }

    LOG(DS_LOG_MODULE, DS_LOG_PROC, "embedding response: %.500s\n", response.c_str());

    {
        Json::Value resp;
        if (!parse_json(response, &resp)) {
            LOG(DS_LOG_MODULE, DS_LOG_PROC, "parse embedding response failed\n");
            goto END;
        }

        if (response_has_api_error(resp)) {
            log_api_error(resp, "Embedding API");
            goto END;
        }

        const Json::Value *embedding = extract_embedding_array(resp);
        if (embedding == NULL) {
            goto END;
        }

        if (embedding->size() < static_cast<Json::ArrayIndex>(dimension)) {
            LOG(DS_LOG_MODULE, DS_LOG_PROC, "embedding dim mismatch: got %u, expected %d\n",
                embedding->size(), dimension);
            goto END;
        }

        for (int i = 0; i < dimension; i++) {
            const Json::Value &val = (*embedding)[static_cast<Json::ArrayIndex>(i)];
            if (!val.isNumeric()) {
                goto END;
            }
            out_vector[i] = static_cast<float>(val.asDouble());
        }

        ret = 0;
    }

END:
    if (curl) curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return ret;
}
