#pragma once

#include "claw_llm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t claw_llm_register_auth_resolver(const char *auth_type,
                                          claw_llm_auth_resolver_fn resolver,
                                          void *user_ctx);

esp_err_t claw_llm_resolve_auth(const char *auth_type,
                                bool force_refresh,
                                char **out_token);

#ifdef __cplusplus
}
#endif
