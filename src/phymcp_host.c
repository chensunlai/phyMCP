#include "phymcp_host.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_random.h"

static phymcp_host_event_cb_t s_event_cb;
static void *s_event_ctx;

static esp_err_t send_json_and_free(const uint8_t mac[PHYMCP_MAC_LEN],
                                    phymcp_msg_type_t type,
                                    uint16_t xid,
                                    cJSON *root)
{
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = phymcp_transport_send_json(mac, type, 0, xid, json);
    cJSON_free(json);
    return ret;
}

static phymcp_host_event_type_t map_event_type(const phymcp_rx_packet_t *packet)
{
    if (packet->header.flags & PHYMCP_FLAG_ERR) {
        return PHYMCP_HOST_EVENT_ERROR;
    }

    switch (packet->header.type) {
    case PHYMCP_MSG_DISCOVER:
        return PHYMCP_HOST_EVENT_DEVICE_FOUND;
    case PHYMCP_MSG_TOOLS_LIST:
        return PHYMCP_HOST_EVENT_TOOLS_LIST;
    case PHYMCP_MSG_TOOL_CALL:
        return PHYMCP_HOST_EVENT_TOOL_RESULT;
    case PHYMCP_MSG_PING:
        return PHYMCP_HOST_EVENT_PONG;
    default:
        return PHYMCP_HOST_EVENT_RAW;
    }
}

static void host_rx_cb(const phymcp_rx_packet_t *packet, void *ctx)
{
    (void)ctx;

    if (!packet || !s_event_cb) {
        return;
    }

    phymcp_host_event_t event = {
        .type = map_event_type(packet),
        .rssi = packet->rssi,
        .msg_type = (uint8_t)packet->header.type,
        .flags = packet->header.flags,
        .xid = packet->header.xid,
        .json = packet->json,
        .json_len = packet->json_len,
    };
    memcpy(event.mac, packet->src_mac, PHYMCP_MAC_LEN);
    s_event_cb(&event, s_event_ctx);
}

void phymcp_host_default_config(phymcp_host_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    phymcp_transport_default_config(&config->transport);
}

esp_err_t phymcp_host_init(const phymcp_host_config_t *config)
{
    if (!config || !config->event_cb) {
        return ESP_ERR_INVALID_ARG;
    }

    s_event_cb = config->event_cb;
    s_event_ctx = config->event_ctx;
    return phymcp_transport_init(&config->transport, host_rx_cb, NULL);
}

esp_err_t phymcp_host_deinit(void)
{
    s_event_cb = NULL;
    s_event_ctx = NULL;
    return phymcp_transport_deinit();
}

esp_err_t phymcp_host_discover(const phymcp_discover_options_t *options,
                               uint16_t *xid_out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (options) {
        if (options->name_prefix) {
            cJSON_AddStringToObject(root, "namePrefix", options->name_prefix);
        }
        if (options->nonce) {
            cJSON_AddStringToObject(root, "nonce", options->nonce);
        }
    }

    const uint16_t xid = phymcp_next_xid();
    if (xid_out) {
        *xid_out = xid;
    }
    return send_json_and_free(PHYMCP_BROADCAST_MAC, PHYMCP_MSG_DISCOVER, xid, root);
}

esp_err_t phymcp_host_list_tools(const uint8_t mac[PHYMCP_MAC_LEN],
                                 const phymcp_tools_list_options_t *options,
                                 uint16_t *xid_out)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (options) {
        if (options->cursor) {
            cJSON_AddStringToObject(root, "cursor", options->cursor);
        }
        if (options->if_none_match) {
            cJSON_AddStringToObject(root, "ifNoneMatch", options->if_none_match);
        }
        if (options->limit) {
            cJSON_AddNumberToObject(root, "limit", options->limit);
        }
    }

    const uint16_t xid = phymcp_next_xid();
    if (xid_out) {
        *xid_out = xid;
    }
    return send_json_and_free(mac, PHYMCP_MSG_TOOLS_LIST, xid, root);
}

esp_err_t phymcp_host_call_tool(const uint8_t mac[PHYMCP_MAC_LEN],
                                const char *name,
                                const phymcp_tool_call_options_t *options,
                                uint16_t *xid_out)
{
    if (!mac || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "name", name);
    if (options && options->arguments_json) {
        cJSON *args = cJSON_Parse(options->arguments_json);
        if (args) {
            cJSON_AddItemToObject(root, "arguments", args);
        } else {
            cJSON_AddStringToObject(root, "argumentsRaw", options->arguments_json);
        }
    } else {
        cJSON_AddItemToObject(root, "arguments", cJSON_CreateObject());
    }
    if (options && options->timeout_ms) {
        cJSON_AddNumberToObject(root, "timeoutMs", options->timeout_ms);
    }

    const uint16_t xid = phymcp_next_xid();
    if (xid_out) {
        *xid_out = xid;
    }
    return send_json_and_free(mac, PHYMCP_MSG_TOOL_CALL, xid, root);
}

esp_err_t phymcp_host_ping(const uint8_t mac[PHYMCP_MAC_LEN],
                           const phymcp_ping_options_t *options,
                           uint16_t *xid_out)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (options) {
        if (options->nonce) {
            cJSON_AddStringToObject(root, "nonce", options->nonce);
        }
        if (options->known_tool_etag) {
            cJSON_AddStringToObject(root, "knownToolEtag", options->known_tool_etag);
        }
    }

    const uint16_t xid = phymcp_next_xid();
    if (xid_out) {
        *xid_out = xid;
    }
    return send_json_and_free(mac, PHYMCP_MSG_PING, xid, root);
}
