/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_runtime.h"

#define CLAW_LLM_BACKEND_OPENAI_RESPONSES_ID "openai_responses"
#define CLAW_LLM_BACKEND_OPENAI_RESPONSES_AUTH_TYPE "bearer"
#define CLAW_LLM_BACKEND_OPENAI_RESPONSES_CHAT_PATH "/responses"
#define CLAW_LLM_BACKEND_OPENAI_RESPONSES_MAX_TOKENS_FIELD "max_output_tokens"
#define CLAW_LLM_BACKEND_OPENAI_RESPONSES_DEFAULT_REASONING_EFFORT "medium"

const claw_llm_backend_registration_t *claw_llm_backend_openai_responses_registration(void);
