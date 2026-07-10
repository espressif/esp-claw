/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_runtime.h"

#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_ID "openai_compatible"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_CHAT_PATH "/chat/completions"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_AUTH_TYPE "bearer"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_DEFAULT_MAX_TOKENS_FIELD "max_tokens"

const claw_llm_backend_vtable_t *claw_llm_backend_openai_compatible_vtable(void);
const claw_llm_backend_registration_t *claw_llm_backend_openai_compatible_registration(void);
