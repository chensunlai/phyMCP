#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "phymcp_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t PHYMCP_BROADCAST_MAC[PHYMCP_MAC_LEN];

typedef struct {
    const uint8_t *pmk;
    size_t pmk_len;
    uint8_t channel;
    wifi_interface_t ifidx;
    wifi_mode_t wifi_mode;
    bool init_nvs;
    bool init_netif;
    bool init_wifi;
    uint8_t rx_queue_size;
    uint32_t rx_task_stack;
    UBaseType_t rx_task_priority;
} phymcp_transport_config_t;

typedef struct {
    uint8_t src_mac[PHYMCP_MAC_LEN];
    uint8_t dst_mac[PHYMCP_MAC_LEN];
    int rssi;
    uint8_t channel;
    phymcp_frame_header_t header;
    const char *json;
    size_t json_len;
} phymcp_rx_packet_t;

typedef void (*phymcp_rx_cb_t)(const phymcp_rx_packet_t *packet, void *ctx);

void phymcp_transport_default_config(phymcp_transport_config_t *config);

esp_err_t phymcp_transport_init(const phymcp_transport_config_t *config,
                                phymcp_rx_cb_t rx_cb,
                                void *rx_ctx);

esp_err_t phymcp_transport_deinit(void);

esp_err_t phymcp_transport_add_peer(const uint8_t mac[PHYMCP_MAC_LEN],
                                    bool encrypted,
                                    const uint8_t *lmk,
                                    size_t lmk_len);

esp_err_t phymcp_transport_send_json(const uint8_t mac[PHYMCP_MAC_LEN],
                                     phymcp_msg_type_t type,
                                     uint8_t flags,
                                     uint16_t xid,
                                     const char *json);

bool phymcp_transport_is_started(void);

#ifdef __cplusplus
}
#endif
