/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_agent.h"

#include "claw_agent.h"
#include "claw_cap.h"
#include "claw_event_router.h"

/* Agent runtime exposed through the claw_cap registry. The descriptor is a
 * system entry point (no CLAW_CAP_FLAG_CALLABLE_BY_LLM) so the event router and
 * other subsystems can invoke it via claw_cap_call without it becoming a tool
 * the model can call recursively. The execute pointer is the claw-cabi bridge
 * that schedules a fire-and-forget submit on the agent worker. */
static const claw_cap_descriptor_t s_agent_descriptors[] = {
    {
        .id = CLAW_EVENT_ROUTER_AGENT_CAP_ID,
        .name = CLAW_EVENT_ROUTER_AGENT_CAP_ID,
        .family = "agent",
        .description = "Submit an inbound message to the agent runtime.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}",
        .execute = claw_agent_cap_execute,
    },
};

static const claw_cap_group_t s_agent_group = {
    .group_id = "cap_agent",
    .descriptors = s_agent_descriptors,
    .descriptor_count = sizeof(s_agent_descriptors) / sizeof(s_agent_descriptors[0]),
};

esp_err_t cap_agent_register_group(void)
{
    if (claw_cap_group_exists(s_agent_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_agent_group);
}
