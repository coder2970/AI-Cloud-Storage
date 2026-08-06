/**
 * @file cfg.cpp
 * @brief Read values from the JSON config file.
 */

#include "cfg.h"

#include "make_log.h"

#include <json/json.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

bool read_file(const char *profile, std::string *content)
{
    std::ifstream file(profile, std::ios::in | std::ios::binary);
    std::ostringstream out;

    if (!file.is_open()) {
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "open config failed: %s\n", profile);
        return false;
    }

    out << file.rdbuf();
    if (file.bad()) {
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "read config failed: %s\n", profile);
        return false;
    }

    *content = out.str();
    return true;
}

bool parse_json(const std::string &content, Json::Value *root)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(content);

    return Json::parseFromStream(builder, input, root, &errors);
}

bool value_to_string(const Json::Value &item, std::string *out)
{
    if (out == NULL) {
        return false;
    }

    if (item.isString()) {
        *out = item.asString();
        return true;
    }

    if (item.isBool()) {
        *out = item.asBool() ? "true" : "false";
        return true;
    }

    if (item.isInt64() || item.isUInt64()) {
        *out = item.asString();
        return true;
    }

    if (item.isNumeric()) {
        *out = item.asString();
        return true;
    }

    return false;
}

int copy_value(const char *src, char *dst, size_t dst_len)
{
    size_t len;

    if (src == NULL || dst == NULL || dst_len == 0) {
        return -1;
    }

    len = strlen(src);
    if (len >= dst_len) {
        memcpy(dst, src, dst_len - 1);
        dst[dst_len - 1] = '\0';
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "config value truncated\n");
        return -1;
    }

    memcpy(dst, src, len + 1);
    return 0;
}

} // namespace

int get_cfg_value_len(const char *profile, const char *title, const char *key,
                      char *value, size_t value_len)
{
    std::string content;
    Json::Value root;
    std::string cfg_value;

    if (profile == NULL || title == NULL || key == NULL || value == NULL || value_len == 0) {
        return -1;
    }

    value[0] = '\0';

    if (!read_file(profile, &content)) {
        return -1;
    }

    if (!parse_json(content, &root) || !root.isObject()) {
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "parse config failed\n");
        return -1;
    }

    if (!root.isMember(title) || !root[title].isObject()) {
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "config section not found: %s\n", title);
        return -1;
    }

    const Json::Value &section = root[title];
    if (!section.isMember(key) || !value_to_string(section[key], &cfg_value)) {
        LOG(CFG_LOG_MODULE, CFG_LOG_PROC, "config key not found: %s.%s\n", title, key);
        return -1;
    }

    return copy_value(cfg_value.c_str(), value, value_len);
}

int get_cfg_value(const char *profile, char *title, char *key, char *value)
{
    if (value == NULL) {
        return -1;
    }

    return get_cfg_value_len(profile, title, key, value, CFG_VALUE_MAX_LEN);
}
