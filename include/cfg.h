#ifndef _CFG_H_
#define _CFG_H_

#define CFG_PATH    "./conf/cfg.json" //配置文件路径

#define CFG_LOG_MODULE "cgi"
#define CFG_LOG_PROC   "cfg"
#define CFG_VALUE_MAX_LEN 256

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------*/
/**
 * @brief  从配置文件中得到相对应的参数
 *
 * @param profile   配置文件路径
 * @param tile      配置文件title名称[title]
 * @param key       key
 * @param value    (out)  得到的value
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int get_cfg_value_len(const char *profile, const char *title, const char *key,
                      char *value, size_t value_len);
int get_cfg_value(const char *profile, char *tile, char *key, char *value);

#ifdef __cplusplus
}
#endif

#endif
