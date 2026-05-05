/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_mcp_server.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "cap_mcp_server.h"
#include "claw_cap.h"
#include "esp_console.h"

static struct {
    struct arg_lit *status;
    struct arg_lit *enable;
    struct arg_lit *disable;
    struct arg_lit *set_config;
    struct arg_str *hostname;
    struct arg_str *instance_name;
    struct arg_str *endpoint;
    struct arg_int *server_port;
    struct arg_int *ctrl_port;
    struct arg_str *auth_token;
    struct arg_end *end;
} mcp_server_args;

static int mcp_server_print_status(void)
{
    claw_cap_state_t group_state = CLAW_CAP_STATE_REGISTERED;
    claw_cap_descriptor_info_t descriptor = {0};
    esp_err_t err;

    err = claw_cap_get_group_state("cap_mcp_server", &group_state);
    if (err != ESP_OK) {
        printf("mcp_server status failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = claw_cap_get_descriptor_state("mcp_server", &descriptor);
    if (err != ESP_OK) {
        printf("mcp_server descriptor status failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("group_id=%s state=%s\n", descriptor.group_id ? descriptor.group_id : "cap_mcp_server",
           claw_cap_state_to_string(group_state));
    printf("descriptor=%s state=%s active_calls=%u\n",
           descriptor.name ? descriptor.name : "mcp_server",
           claw_cap_state_to_string(descriptor.state),
           (unsigned)descriptor.active_calls);
    return 0;
}

static int mcp_server_func(int argc, char **argv)
{
    cap_mcp_server_config_t config = {0};
    esp_err_t err;
    int nerrors = arg_parse(argc, argv, (void **)&mcp_server_args);
    int operation_count;

    if (nerrors != 0) {
        arg_print_errors(stderr, mcp_server_args.end, argv[0]);
        return 1;
    }

    operation_count = mcp_server_args.status->count + mcp_server_args.enable->count +
                      mcp_server_args.disable->count + mcp_server_args.set_config->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (mcp_server_args.status->count) {
        return mcp_server_print_status();
    }

    if (mcp_server_args.enable->count) {
        err = claw_cap_enable_group("cap_mcp_server");
        if (err != ESP_OK) {
            printf("mcp_server enable failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("cap_mcp_server enabled\n");
        return 0;
    }

    if (mcp_server_args.disable->count) {
        err = claw_cap_disable_group("cap_mcp_server");
        if (err != ESP_OK) {
            printf("mcp_server disable failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("cap_mcp_server disabled\n");
        return 0;
    }

    config.hostname = mcp_server_args.hostname->count ? mcp_server_args.hostname->sval[0] : NULL;
    config.instance_name =
        mcp_server_args.instance_name->count ? mcp_server_args.instance_name->sval[0] : NULL;
    config.endpoint = mcp_server_args.endpoint->count ? mcp_server_args.endpoint->sval[0] : NULL;
    config.server_port = mcp_server_args.server_port->count ?
                         (uint16_t)mcp_server_args.server_port->ival[0] : 0;
    config.ctrl_port = mcp_server_args.ctrl_port->count ?
                       (uint16_t)mcp_server_args.ctrl_port->ival[0] : 0;
    config.auth_token = mcp_server_args.auth_token->count ? mcp_server_args.auth_token->sval[0] : NULL;

    err = cap_mcp_server_set_config(&config);
    if (err != ESP_OK) {
        printf("mcp_server set-config failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("MCP server config updated\n");
    return 0;
}

void register_cap_mcp_server(void)
{
    mcp_server_args.status = arg_lit0(NULL, "status", "Show MCP server group and descriptor state");
    mcp_server_args.enable = arg_lit0(NULL, "enable", "Enable and start the MCP server group");
    mcp_server_args.disable = arg_lit0(NULL, "disable", "Disable and stop the MCP server group");
    mcp_server_args.set_config = arg_lit0(NULL, "set-config", "Update MCP server config before start");
    mcp_server_args.hostname = arg_str0(NULL, "hostname", "<hostname>", "mDNS hostname");
    mcp_server_args.instance_name = arg_str0(NULL, "instance-name", "<name>", "mDNS instance name");
    mcp_server_args.endpoint = arg_str0(NULL, "endpoint", "<endpoint>", "HTTP endpoint path");
    mcp_server_args.server_port = arg_int0(NULL, "server-port", "<port>", "HTTP server port");
    mcp_server_args.ctrl_port = arg_int0(NULL, "ctrl-port", "<port>", "HTTP control port");
    mcp_server_args.auth_token = arg_str0(NULL, "auth-token", "<token>", "Bearer token required for MCP requests");
    mcp_server_args.end = arg_end(9);

    const esp_console_cmd_t mcp_server_cmd = {
        .command = "mcp_server",
        .help = "MCP server operations.\n"
        "Examples:\n"
        " mcp_server --status\n"
        " mcp_server --set-config --hostname "CAP_MCP_SERVER_DEFAULT_HOSTNAME" --endpoint mcp_server\n"
        " mcp_server --set-config --auth-token secret-token\n"
        " mcp_server --disable\n"
        " mcp_server --enable\n",
        .func = mcp_server_func,
        .argtable = &mcp_server_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&mcp_server_cmd));
}
