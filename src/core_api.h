#ifndef DOLLMANMUTE_CORE_API_H
#define DOLLMANMUTE_CORE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOLLMANMUTE_PROXY_VERSION 1u

typedef struct ProxyContext {
    const char *game_root;
    const char *ini_path;
    const char *log_path;
    uint32_t    proxy_version;
} ProxyContext;

__declspec(dllexport) int  core_init(const ProxyContext *ctx);
__declspec(dllexport) void core_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
