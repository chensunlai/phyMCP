#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHYMCP_MAGIC0 'P'
#define PHYMCP_MAGIC1 'M'
#define PHYMCP_VERSION 1
#define PHYMCP_FRAME_HEADER_LEN 12
#define PHYMCP_MAX_FRAME_LEN 1470
#define PHYMCP_MAX_JSON_LEN (PHYMCP_MAX_FRAME_LEN - PHYMCP_FRAME_HEADER_LEN)
#define PHYMCP_MAC_LEN 6

#define ESP_ERR_PHYMCP_BASE 0x7000
#define ESP_ERR_PHYMCP_BAD_FRAME (ESP_ERR_PHYMCP_BASE + 1)
#define ESP_ERR_PHYMCP_VERSION (ESP_ERR_PHYMCP_BASE + 2)
#define ESP_ERR_PHYMCP_TOO_LARGE (ESP_ERR_PHYMCP_BASE + 3)
#define ESP_ERR_PHYMCP_NOT_FOUND (ESP_ERR_PHYMCP_BASE + 4)

typedef enum {
    PHYMCP_MSG_DISCOVER = 1,
    PHYMCP_MSG_TOOLS_LIST = 2,
    PHYMCP_MSG_TOOL_CALL = 3,
    PHYMCP_MSG_PING = 4,
} phymcp_msg_type_t;

typedef enum {
    PHYMCP_FLAG_ACK = 1 << 0,
    PHYMCP_FLAG_ERR = 1 << 1,
} phymcp_frame_flags_t;

typedef struct {
    uint8_t version;
    phymcp_msg_type_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t xid;
    uint16_t payload_len;
} phymcp_frame_header_t;

esp_err_t phymcp_frame_pack(phymcp_msg_type_t type,
                            uint8_t flags,
                            uint16_t seq,
                            uint16_t xid,
                            const char *json,
                            uint8_t *out,
                            size_t out_len,
                            size_t *written);

esp_err_t phymcp_frame_unpack(const uint8_t *data,
                              size_t data_len,
                              phymcp_frame_header_t *header,
                              const char **json,
                              size_t *json_len);

uint16_t phymcp_next_seq(void);
uint16_t phymcp_next_xid(void);

void phymcp_mac_to_str(const uint8_t mac[PHYMCP_MAC_LEN],
                       char out[18]);

bool phymcp_mac_from_str(const char *str,
                         uint8_t mac[PHYMCP_MAC_LEN]);

esp_err_t phymcp_make_error_json(const char *code,
                                 const char *message,
                                 char *out,
                                 size_t out_len);

esp_err_t phymcp_make_tool_result_text_json(const char *text,
                                            bool is_error,
                                            char *out,
                                            size_t out_len);

#ifdef __cplusplus
}
#endif
