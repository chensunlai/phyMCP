#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "phymcp_frame.h"
#include "phymcp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHYMCP_HOST_EVENT_DEVICE_FOUND,
    PHYMCP_HOST_EVENT_TOOLS_LIST,
    PHYMCP_HOST_EVENT_TOOL_RESULT,
    PHYMCP_HOST_EVENT_PONG,
    PHYMCP_HOST_EVENT_ERROR,
    PHYMCP_HOST_EVENT_RAW,
} phymcp_host_event_type_t;

typedef struct {
    phymcp_host_event_type_t type;
    uint8_t mac[PHYMCP_MAC_LEN];
    int rssi;
    uint8_t msg_type;
    uint8_t flags;
    uint16_t xid;
    const char *json;
    size_t json_len;
} phymcp_host_event_t;

typedef void (*phymcp_host_event_cb_t)(const phymcp_host_event_t *event,
                                       void *ctx);

typedef struct {
    phymcp_transport_config_t transport;
    phymcp_host_event_cb_t event_cb;
    void *event_ctx;
} phymcp_host_config_t;

typedef struct {
    const char *name_prefix;
    const char *nonce;
} phymcp_discover_options_t;

typedef struct {
    const char *cursor;
    const char *if_none_match;
    uint8_t limit;
} phymcp_tools_list_options_t;

typedef struct {
    const char *arguments_json;
    uint32_t timeout_ms;
} phymcp_tool_call_options_t;

typedef struct {
    const char *nonce;
    const char *known_tool_etag;
} phymcp_ping_options_t;

void phymcp_host_default_config(phymcp_host_config_t *config);
esp_err_t phymcp_host_init(const phymcp_host_config_t *config);
esp_err_t phymcp_host_deinit(void);

esp_err_t phymcp_host_discover(const phymcp_discover_options_t *options,
                               uint16_t *xid_out);

esp_err_t phymcp_host_list_tools(const uint8_t mac[PHYMCP_MAC_LEN],
                                 const phymcp_tools_list_options_t *options,
                                 uint16_t *xid_out);

esp_err_t phymcp_host_call_tool(const uint8_t mac[PHYMCP_MAC_LEN],
                                const char *name,
                                const phymcp_tool_call_options_t *options,
                                uint16_t *xid_out);

esp_err_t phymcp_host_ping(const uint8_t mac[PHYMCP_MAC_LEN],
                           const phymcp_ping_options_t *options,
                           uint16_t *xid_out);

#ifdef __cplusplus
}
#endif
