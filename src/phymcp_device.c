#include "phymcp_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PHYMCP_DISCOVER_JITTER_MS 300
#define PHYMCP_DUP_CACHE_MAX 16
#define PHYMCP_DUP_CACHE_TTL_US (5 * 1000 * 1000)
#define PHYMCP_DEFAULT_TOOL_LIMIT 8

typedef struct {
    bool used;
    uint8_t src_mac[PHYMCP_MAC_LEN];
    uint8_t type;
    uint16_t xid;
    int64_t expires_at_us;
    char response_json[PHYMCP_MAX_JSON_LEN + 1];
} duplicate_entry_t;

static phymcp_device_config_t s_config;
static bool s_started;
static duplicate_entry_t s_dup_cache[PHYMCP_DUP_CACHE_MAX];

static const char *safe_str(const char *value, const char *fallback)
{
    return value ? value : fallback;
}

static size_t duplicate_cache_size(void)
{
    size_t size = s_config.duplicate_cache_size;
    if (size == 0 || size > PHYMCP_DUP_CACHE_MAX) {
        size = 8;
    }
    return size;
}

static duplicate_entry_t *find_duplicate(const uint8_t src_mac[PHYMCP_MAC_LEN],
                                         uint8_t type,
                                         uint16_t xid)
{
    const int64_t now = esp_timer_get_time();
    const size_t size = duplicate_cache_size();

    for (size_t i = 0; i < size; ++i) {
        duplicate_entry_t *entry = &s_dup_cache[i];
        if (!entry->used || entry->expires_at_us < now) {
            entry->used = false;
            continue;
        }
        if (entry->type == type &&
            entry->xid == xid &&
            memcmp(entry->src_mac, src_mac, PHYMCP_MAC_LEN) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void store_duplicate(const uint8_t src_mac[PHYMCP_MAC_LEN],
                            uint8_t type,
                            uint16_t xid,
                            const char *response_json)
{
    if (!response_json) {
        return;
    }

    duplicate_entry_t *slot = NULL;
    const int64_t now = esp_timer_get_time();
    const size_t size = duplicate_cache_size();

    for (size_t i = 0; i < size; ++i) {
        if (!s_dup_cache[i].used || s_dup_cache[i].expires_at_us < now) {
            slot = &s_dup_cache[i];
            break;
        }
    }
    if (!slot) {
        slot = &s_dup_cache[esp_random() % size];
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->src_mac, src_mac, PHYMCP_MAC_LEN);
    slot->type = type;
    slot->xid = xid;
    slot->expires_at_us = now + PHYMCP_DUP_CACHE_TTL_US;
    strlcpy(slot->response_json, response_json, sizeof(slot->response_json));
}

static esp_err_t send_json_object(const uint8_t mac[PHYMCP_MAC_LEN],
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

static esp_err_t send_error(const uint8_t mac[PHYMCP_MAC_LEN],
                            phymcp_msg_type_t type,
                            uint16_t xid,
                            const char *code,
                            const char *message)
{
    char json[160];
    esp_err_t ret = phymcp_make_error_json(code, message, json, sizeof(json));
    if (ret != ESP_OK) {
        return ret;
    }
    return phymcp_transport_send_json(mac, type, PHYMCP_FLAG_ERR, xid, json);
}

static cJSON *parse_payload(const phymcp_rx_packet_t *packet)
{
    if (!packet->json || packet->json_len == 0) {
        return cJSON_CreateObject();
    }

    char *buf = calloc(1, packet->json_len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, packet->json, packet->json_len);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static bool name_prefix_matches(cJSON *root)
{
    cJSON *prefix = cJSON_GetObjectItem(root, "namePrefix");
    if (!cJSON_IsString(prefix) || !prefix->valuestring || prefix->valuestring[0] == '\0') {
        return true;
    }

    const char *name = safe_str(s_config.name, "");
    return strncmp(name, prefix->valuestring, strlen(prefix->valuestring)) == 0;
}

static void handle_discover(const phymcp_rx_packet_t *packet)
{
    cJSON *request = parse_payload(packet);
    if (!request) {
        send_error(packet->src_mac, PHYMCP_MSG_DISCOVER, packet->header.xid,
                   "badJson", "invalid discover JSON");
        return;
    }

    if (!name_prefix_matches(request)) {
        cJSON_Delete(request);
        return;
    }

    const uint32_t jitter_ms = esp_random() % (PHYMCP_DISCOVER_JITTER_MS + 1);
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(request);
        send_error(packet->src_mac, PHYMCP_MSG_DISCOVER, packet->header.xid,
                   "noMemory", "out of memory");
        return;
    }

    cJSON_AddStringToObject(root, "name", safe_str(s_config.name, "device"));
    cJSON_AddStringToObject(root, "class", safe_str(s_config.class_name, "generic"));
    cJSON_AddStringToObject(root, "model", safe_str(s_config.model, "esp32"));
    cJSON_AddStringToObject(root, "firmware", safe_str(s_config.firmware, "0.1.0"));
    cJSON_AddStringToObject(root, "toolEtag", safe_str(s_config.tool_etag, "tools-v1"));
    cJSON_AddNumberToObject(root, "toolCount", (double)s_config.tool_count);
    cJSON_AddBoolToObject(root, "encryptedRequired", s_config.encrypted_unicast_required);

    cJSON *nonce = cJSON_GetObjectItem(request, "nonce");
    if (cJSON_IsString(nonce)) {
        cJSON_AddStringToObject(root, "nonce", nonce->valuestring);
    }
    cJSON_Delete(request);

    send_json_object(packet->src_mac, PHYMCP_MSG_DISCOVER, packet->header.xid, root);
}

static void add_tool_schema(cJSON *tool_obj, const phymcp_tool_t *tool)
{
    if (tool->input_schema_json) {
        cJSON *schema = cJSON_Parse(tool->input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool_obj, "inputSchema", schema);
            return;
        }
    }
    cJSON_AddItemToObject(tool_obj, "inputSchema", cJSON_CreateObject());
}

static void handle_tools_list(const phymcp_rx_packet_t *packet)
{
    cJSON *request = parse_payload(packet);
    if (!request) {
        send_error(packet->src_mac, PHYMCP_MSG_TOOLS_LIST, packet->header.xid,
                   "badJson", "invalid tools/list JSON");
        return;
    }

    int start = 0;
    cJSON *cursor = cJSON_GetObjectItem(request, "cursor");
    if (cJSON_IsString(cursor) && cursor->valuestring) {
        start = atoi(cursor->valuestring);
    }

    int limit = PHYMCP_DEFAULT_TOOL_LIMIT;
    cJSON *limit_item = cJSON_GetObjectItem(request, "limit");
    if (cJSON_IsNumber(limit_item) && limit_item->valueint > 0) {
        limit = limit_item->valueint;
    }
    if (limit > PHYMCP_DEFAULT_TOOL_LIMIT) {
        limit = PHYMCP_DEFAULT_TOOL_LIMIT;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    if (!root || !tools) {
        cJSON_Delete(request);
        cJSON_Delete(root);
        cJSON_Delete(tools);
        send_error(packet->src_mac, PHYMCP_MSG_TOOLS_LIST, packet->header.xid,
                   "noMemory", "out of memory");
        return;
    }

    cJSON_AddItemToObject(root, "tools", tools);
    cJSON_AddStringToObject(root, "etag", safe_str(s_config.tool_etag, "tools-v1"));

    int emitted = 0;
    for (size_t i = (size_t)start; i < s_config.tool_count && emitted < limit; ++i) {
        const phymcp_tool_t *tool = &s_config.tools[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        cJSON_AddStringToObject(item, "name", safe_str(tool->name, ""));
        cJSON_AddStringToObject(item, "description", safe_str(tool->description, ""));
        cJSON_AddBoolToObject(item, "destructive", tool->destructive);
        add_tool_schema(item, tool);
        cJSON_AddItemToArray(tools, item);
        emitted++;
    }

    const int next = start + emitted;
    if ((size_t)next < s_config.tool_count) {
        char next_cursor[16];
        snprintf(next_cursor, sizeof(next_cursor), "%d", next);
        cJSON_AddStringToObject(root, "nextCursor", next_cursor);
    }

    cJSON_Delete(request);
    send_json_object(packet->src_mac, PHYMCP_MSG_TOOLS_LIST, packet->header.xid, root);
}

static const phymcp_tool_t *find_tool(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < s_config.tool_count; ++i) {
        if (s_config.tools[i].name && strcmp(s_config.tools[i].name, name) == 0) {
            return &s_config.tools[i];
        }
    }
    return NULL;
}

static char *make_arguments_json(cJSON *request)
{
    cJSON *args = cJSON_GetObjectItem(request, "arguments");
    if (!args) {
        return strdup("{}");
    }
    return cJSON_PrintUnformatted(args);
}

static void handle_tool_call(const phymcp_rx_packet_t *packet)
{
    duplicate_entry_t *dup = find_duplicate(packet->src_mac,
                                            PHYMCP_MSG_TOOL_CALL,
                                            packet->header.xid);
    if (dup) {
        phymcp_transport_send_json(packet->src_mac, PHYMCP_MSG_TOOL_CALL,
                                   0, packet->header.xid, dup->response_json);
        return;
    }

    cJSON *request = parse_payload(packet);
    if (!request) {
        send_error(packet->src_mac, PHYMCP_MSG_TOOL_CALL, packet->header.xid,
                   "badJson", "invalid tool call JSON");
        return;
    }

    cJSON *name_item = cJSON_GetObjectItem(request, "name");
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : NULL;
    const phymcp_tool_t *tool = find_tool(name);
    if (!tool || !tool->handler) {
        cJSON_Delete(request);
        send_error(packet->src_mac, PHYMCP_MSG_TOOL_CALL, packet->header.xid,
                   "toolNotFound", "tool not found");
        return;
    }

    char *args = make_arguments_json(request);
    cJSON_Delete(request);
    if (!args) {
        send_error(packet->src_mac, PHYMCP_MSG_TOOL_CALL, packet->header.xid,
                   "noMemory", "out of memory");
        return;
    }

    char response[PHYMCP_MAX_JSON_LEN + 1];
    response[0] = '\0';
    esp_err_t ret = tool->handler(args, response, sizeof(response), tool->ctx);
    free(args);

    if (ret != ESP_OK) {
        send_error(packet->src_mac, PHYMCP_MSG_TOOL_CALL, packet->header.xid,
                   "toolError", "tool handler failed");
        return;
    }
    if (response[0] == '\0') {
        phymcp_make_tool_result_text_json("ok", false, response, sizeof(response));
    }

    store_duplicate(packet->src_mac, PHYMCP_MSG_TOOL_CALL,
                    packet->header.xid, response);
    phymcp_transport_send_json(packet->src_mac, PHYMCP_MSG_TOOL_CALL,
                               0, packet->header.xid, response);
}

static void handle_ping(const phymcp_rx_packet_t *packet)
{
    cJSON *request = parse_payload(packet);
    if (!request) {
        send_error(packet->src_mac, PHYMCP_MSG_PING, packet->header.xid,
                   "badJson", "invalid ping JSON");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(request);
        send_error(packet->src_mac, PHYMCP_MSG_PING, packet->header.xid,
                   "noMemory", "out of memory");
        return;
    }

    cJSON_AddBoolToObject(root, "pong", true);
    cJSON_AddStringToObject(root, "name", safe_str(s_config.name, "device"));
    cJSON_AddStringToObject(root, "toolEtag", safe_str(s_config.tool_etag, "tools-v1"));

    cJSON *nonce = cJSON_GetObjectItem(request, "nonce");
    if (cJSON_IsString(nonce)) {
        cJSON_AddStringToObject(root, "nonce", nonce->valuestring);
    }
    cJSON_Delete(request);

    send_json_object(packet->src_mac, PHYMCP_MSG_PING, packet->header.xid, root);
}

static void rx_cb(const phymcp_rx_packet_t *packet, void *ctx)
{
    (void)ctx;
    if (!packet) {
        return;
    }

    switch (packet->header.type) {
    case PHYMCP_MSG_DISCOVER:
        handle_discover(packet);
        break;
    case PHYMCP_MSG_TOOLS_LIST:
        handle_tools_list(packet);
        break;
    case PHYMCP_MSG_TOOL_CALL:
        handle_tool_call(packet);
        break;
    case PHYMCP_MSG_PING:
        handle_ping(packet);
        break;
    default:
        send_error(packet->src_mac, packet->header.type, packet->header.xid,
                   "badType", "unsupported message type");
        break;
    }
}

void phymcp_device_default_config(phymcp_device_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->name = "phymcp-device";
    config->class_name = "generic";
    config->model = "esp32";
    config->firmware = "0.1.0";
    config->tool_etag = "tools-v1";
    config->duplicate_cache_size = 8;
    phymcp_transport_default_config(&config->transport);
}

esp_err_t phymcp_device_init(const phymcp_device_config_t *config)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    memset(s_dup_cache, 0, sizeof(s_dup_cache));
    esp_err_t ret = phymcp_transport_init(&s_config.transport, rx_cb, NULL);
    if (ret == ESP_OK) {
        s_started = true;
    }
    return ret;
}

esp_err_t phymcp_device_deinit(void)
{
    s_started = false;
    memset(&s_config, 0, sizeof(s_config));
    memset(s_dup_cache, 0, sizeof(s_dup_cache));
    return phymcp_transport_deinit();
}
