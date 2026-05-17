#include "phymcp_frame.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_random.h"

static uint16_t s_next_seq;
static uint16_t s_next_xid;

static uint16_t load_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void store_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

esp_err_t phymcp_frame_pack(phymcp_msg_type_t type,
                            uint8_t flags,
                            uint16_t seq,
                            uint16_t xid,
                            const char *json,
                            uint8_t *out,
                            size_t out_len,
                            size_t *written)
{
    if (!out || !written) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t payload_len = json ? strlen(json) : 0;
    const size_t frame_len = PHYMCP_FRAME_HEADER_LEN + payload_len;

    if (payload_len > UINT16_MAX || frame_len > PHYMCP_MAX_FRAME_LEN) {
        return ESP_ERR_PHYMCP_TOO_LARGE;
    }
    if (out_len < frame_len) {
        return ESP_ERR_NO_MEM;
    }

    out[0] = PHYMCP_MAGIC0;
    out[1] = PHYMCP_MAGIC1;
    out[2] = PHYMCP_VERSION;
    out[3] = (uint8_t)type;
    out[4] = flags;
    out[5] = PHYMCP_FRAME_HEADER_LEN;
    store_u16_le(&out[6], seq);
    store_u16_le(&out[8], xid);
    store_u16_le(&out[10], (uint16_t)payload_len);

    if (payload_len > 0) {
        memcpy(&out[PHYMCP_FRAME_HEADER_LEN], json, payload_len);
    }

    *written = frame_len;
    return ESP_OK;
}

esp_err_t phymcp_frame_unpack(const uint8_t *data,
                              size_t data_len,
                              phymcp_frame_header_t *header,
                              const char **json,
                              size_t *json_len)
{
    if (!data || !header || !json || !json_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len < PHYMCP_FRAME_HEADER_LEN ||
        data[0] != PHYMCP_MAGIC0 ||
        data[1] != PHYMCP_MAGIC1) {
        return ESP_ERR_PHYMCP_BAD_FRAME;
    }
    if (data[2] != PHYMCP_VERSION) {
        return ESP_ERR_PHYMCP_VERSION;
    }
    if (data[5] != PHYMCP_FRAME_HEADER_LEN) {
        return ESP_ERR_PHYMCP_BAD_FRAME;
    }

    const uint16_t payload_len = load_u16_le(&data[10]);
    if (PHYMCP_FRAME_HEADER_LEN + (size_t)payload_len > data_len) {
        return ESP_ERR_PHYMCP_BAD_FRAME;
    }

    header->version = data[2];
    header->type = (phymcp_msg_type_t)data[3];
    header->flags = data[4];
    header->seq = load_u16_le(&data[6]);
    header->xid = load_u16_le(&data[8]);
    header->payload_len = payload_len;
    *json = payload_len ? (const char *)&data[PHYMCP_FRAME_HEADER_LEN] : NULL;
    *json_len = payload_len;
    return ESP_OK;
}

uint16_t phymcp_next_seq(void)
{
    if (s_next_seq == 0) {
        s_next_seq = (uint16_t)esp_random();
    }
    if (++s_next_seq == 0) {
        ++s_next_seq;
    }
    return s_next_seq;
}

uint16_t phymcp_next_xid(void)
{
    if (s_next_xid == 0) {
        s_next_xid = (uint16_t)esp_random();
    }
    if (++s_next_xid == 0) {
        ++s_next_xid;
    }
    return s_next_xid;
}

void phymcp_mac_to_str(const uint8_t mac[PHYMCP_MAC_LEN],
                       char out[18])
{
    if (!mac || !out) {
        return;
    }
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool phymcp_mac_from_str(const char *str,
                         uint8_t mac[PHYMCP_MAC_LEN])
{
    if (!str || !mac) {
        return false;
    }

    unsigned int v[PHYMCP_MAC_LEN];
    const int n = sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != PHYMCP_MAC_LEN) {
        return false;
    }

    for (size_t i = 0; i < PHYMCP_MAC_LEN; ++i) {
        if (v[i] > 0xff) {
            return false;
        }
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

static esp_err_t print_json(cJSON *root, char *out, size_t out_len)
{
    if (!root || !out || out_len == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }

    const size_t len = strlen(text);
    if (len + 1 > out_len) {
        cJSON_free(text);
        return ESP_ERR_PHYMCP_TOO_LARGE;
    }

    memcpy(out, text, len + 1);
    cJSON_free(text);
    return ESP_OK;
}

esp_err_t phymcp_make_error_json(const char *code,
                                 const char *message,
                                 char *out,
                                 size_t out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (!root || !error) {
        cJSON_Delete(root);
        cJSON_Delete(error);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "error", error);
    cJSON_AddStringToObject(error, "code", code ? code : "error");
    cJSON_AddStringToObject(error, "message", message ? message : "phyMCP error");
    return print_json(root, out, out_len);
}

esp_err_t phymcp_make_tool_result_text_json(const char *text,
                                            bool is_error,
                                            char *out,
                                            size_t out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (!root || !content || !item) {
        cJSON_Delete(root);
        cJSON_Delete(content);
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddItemToArray(content, item);
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddBoolToObject(root, "isError", is_error);
    return print_json(root, out, out_len);
}
