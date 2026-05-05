/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_mcp_client.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "esp_console.h"

static struct {
    struct arg_lit *discover;
    struct arg_lit *list_tools;
    struct arg_lit *call_tool;
    struct arg_str *server_url;
    struct arg_str *endpoint;
    struct arg_str *cursor;
    struct arg_str *tool_name;
    struct arg_str *arguments_json;
    struct arg_str *auth_token;
    struct arg_lit *initialize;
    struct arg_str *transport;
    struct arg_int *timeout_ms;
    struct arg_lit *include_self;
    struct arg_end *end;
} mcp_client_args;

static int mcp_client_call_cap(const char *cap_name, cJSON *root)
{
    char *input_json = NULL;
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
    };

    input_json = cJSON_PrintUnformatted(root);
    if (!input_json) {
        printf("Out of memory\n");
        return 1;
    }

    output = calloc(1, 4096);
    if (!output) {
        free(input_json);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(cap_name, input_json, &ctx, output, 4096);
    if (err != ESP_OK) {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    } else {
        printf("%s\n", output);
    }

    free(output);
    free(input_json);
    return err == ESP_OK ? 0 : 1;
}

static int mcp_client_func(int argc, char **argv)
{
    cJSON *root = NULL;
    cJSON *arguments = NULL;
    const char *cap_name = NULL;
    int nerrors = arg_parse(argc, argv, (void **)&mcp_client_args);
    int operation_count;
    int rc;

    if (nerrors != 0) {
        arg_print_errors(stderr, mcp_client_args.end, argv[0]);
        return 1;
    }

    operation_count = mcp_client_args.discover->count + mcp_client_args.list_tools->count +
                      mcp_client_args.call_tool->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    root = cJSON_CreateObject();
    if (!root) {
        printf("Out of memory\n");
        return 1;
    }

    if (mcp_client_args.discover->count) {
        cap_name = "mcp_discover";
        if (mcp_client_args.timeout_ms->count) {
            cJSON_AddNumberToObject(root, "timeout_ms", mcp_client_args.timeout_ms->ival[0]);
        }
        if (mcp_client_args.include_self->count) {
            cJSON_AddBoolToObject(root, "include_self", true);
        }
    } else {
        if (!mcp_client_args.server_url->count) {
            cJSON_Delete(root);
            printf("'--server-url' is required\n");
            return 1;
        }

        cJSON_AddStringToObject(root, "server_url", mcp_client_args.server_url->sval[0]);
        if (mcp_client_args.endpoint->count) {
            cJSON_AddStringToObject(root, "endpoint", mcp_client_args.endpoint->sval[0]);
        }
        if (mcp_client_args.auth_token->count) {
            cJSON_AddStringToObject(root, "auth_token", mcp_client_args.auth_token->sval[0]);
        }
        if (mcp_client_args.transport->count) {
            cJSON_AddStringToObject(root, "transport", mcp_client_args.transport->sval[0]);
        }
        if (mcp_client_args.initialize->count) {
            cJSON_AddBoolToObject(root, "initialize", true);
        }

        if (mcp_client_args.list_tools->count) {
            cap_name = "mcp_list_tools";
            if (mcp_client_args.cursor->count) {
                cJSON_AddStringToObject(root, "cursor", mcp_client_args.cursor->sval[0]);
            }
        } else {
            cap_name = "mcp_call_tool";
            if (!mcp_client_args.tool_name->count) {
                cJSON_Delete(root);
                printf("'--call-tool' requires '--tool-name'\n");
                return 1;
            }
            cJSON_AddStringToObject(root, "tool_name", mcp_client_args.tool_name->sval[0]);

            if (mcp_client_args.arguments_json->count) {
                arguments = cJSON_Parse(mcp_client_args.arguments_json->sval[0]);
                if (!arguments || !cJSON_IsObject(arguments)) {
                    cJSON_Delete(arguments);
                    cJSON_Delete(root);
                    printf("'--arguments-json' must be a JSON object\n");
                    return 1;
                }
                cJSON_AddItemToObject(root, "arguments", arguments);
            }
        }
    }

    rc = mcp_client_call_cap(cap_name, root);
    cJSON_Delete(root);
    return rc;
}

void register_cap_mcp_client(void)
{
    mcp_client_args.discover = arg_lit0(NULL, "discover", "Discover MCP servers on the local network");
    mcp_client_args.list_tools = arg_lit0(NULL, "list-tools", "List tools from one remote MCP server");
    mcp_client_args.call_tool = arg_lit0(NULL, "call-tool", "Call one tool on a remote MCP server");
    mcp_client_args.server_url = arg_str0(NULL, "server-url", "<url>", "Remote MCP server base URL");
    mcp_client_args.endpoint = arg_str0(NULL, "endpoint", "<endpoint>", "Remote MCP endpoint");
    mcp_client_args.cursor = arg_str0(NULL, "cursor", "<cursor>", "Pagination cursor for list-tools");
    mcp_client_args.tool_name = arg_str0(NULL, "tool-name", "<name>", "Remote MCP tool name");
    mcp_client_args.arguments_json = arg_str0(NULL, "arguments-json", "<json>", "Remote MCP tool arguments JSON object");
    mcp_client_args.auth_token = arg_str0(NULL, "auth-token", "<token>", "Bearer token for MCP server auth");
    mcp_client_args.initialize = arg_lit0(NULL, "initialize", "Force MCP initialize handshake before request");
    mcp_client_args.transport = arg_str0(NULL, "transport", "<http|sse>", "Transport mode: http (default) or sse");
    mcp_client_args.timeout_ms = arg_int0(NULL, "timeout-ms", "<ms>", "Discovery timeout in milliseconds");
    mcp_client_args.include_self = arg_lit0(NULL, "include-self", "Include the local device in discovery");
    mcp_client_args.end = arg_end(12);

    const esp_console_cmd_t mcp_client_cmd = {
        .command = "mcp_client",
        .help = "MCP client operations.\n"
        "Examples:\n"
        " mcp_client --discover --timeout-ms 3000\n"
        " mcp_client --list-tools --server-url http://host.local:18791 --endpoint mcp_server\n"
        " mcp_client --call-tool --server-url http://host.local:18791 --tool-name device.describe\n"
        " mcp_client --list-tools --server-url http://host.local:18791 --auth-token secret-token --initialize\n"
        " mcp_client --list-tools --server-url http://cloud.example.com:3000 --endpoint /sse/device-id --transport sse --auth-token secret-token\n",
        .func = mcp_client_func,
        .argtable = &mcp_client_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&mcp_client_cmd));
}
