/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_runtime.h"

#define CLAW_LLM_BACKEND_ANTHROPIC_ID "anthropic_compatible"
#define CLAW_LLM_BACKEND_ANTHROPIC_AUTH_TYPE "none"
#define CLAW_LLM_BACKEND_ANTHROPIC_CHAT_PATH "/messages"
#define CLAW_LLM_BACKEND_ANTHROPIC_DEFAULT_MAX_TOKENS_FIELD "max_tokens"

const claw_llm_backend_vtable_t *claw_llm_backend_anthropic_vtable(void);
const claw_llm_backend_registration_t *claw_llm_backend_anthropic_registration(void);
