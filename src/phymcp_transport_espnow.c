#include "phymcp_transport.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

const uint8_t PHYMCP_BROADCAST_MAC[PHYMCP_MAC_LEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

typedef struct {
    uint8_t src_mac[PHYMCP_MAC_LEN];
    uint8_t dst_mac[PHYMCP_MAC_LEN];
    int rssi;
    uint8_t channel;
    int len;
    uint8_t data[];
} phymcp_rx_msg_t;

static const char *TAG = "phymcp_transport";

static phymcp_transport_config_t s_config;
static phymcp_rx_cb_t s_rx_cb;
static void *s_rx_ctx;
static QueueHandle_t s_rx_queue;
static TaskHandle_t s_rx_task;
static bool s_started;

static bool mac_is_broadcast(const uint8_t mac[PHYMCP_MAC_LEN])
{
    return memcmp(mac, PHYMCP_BROADCAST_MAC, PHYMCP_MAC_LEN) == 0;
}

void phymcp_transport_default_config(phymcp_transport_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
#ifdef CONFIG_PHYMCP_DEFAULT_ESPNOW_CHANNEL
    config->channel = CONFIG_PHYMCP_DEFAULT_ESPNOW_CHANNEL;
#else
    config->channel = 6;
#endif
    config->ifidx = WIFI_IF_STA;
    config->wifi_mode = WIFI_MODE_STA;
    config->init_nvs = true;
    config->init_netif = true;
    config->init_wifi = true;
    config->rx_queue_size = 8;
    config->rx_task_stack = 4096;
    config->rx_task_priority = 4;
}

static esp_err_t init_nvs_if_needed(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_wifi_if_needed(const phymcp_transport_config_t *config)
{
    if (config->init_netif) {
        esp_err_t ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }

        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
    }

    if (!config->init_wifi) {
        return ESP_OK;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(config->wifi_mode), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(config->channel, WIFI_SECOND_CHAN_NONE),
                        TAG, "wifi channel");
    return ESP_OK;
}

static void recv_cb(const esp_now_recv_info_t *info,
                    const uint8_t *data,
                    int len)
{
    if (!info || !data || len <= 0 || !s_rx_queue) {
        return;
    }

    phymcp_rx_msg_t *msg = calloc(1, sizeof(*msg) + len + 1);
    if (!msg) {
        return;
    }

    memcpy(msg->src_mac, info->src_addr, PHYMCP_MAC_LEN);
    if (info->des_addr) {
        memcpy(msg->dst_mac, info->des_addr, PHYMCP_MAC_LEN);
    }
    if (info->rx_ctrl) {
        msg->rssi = info->rx_ctrl->rssi;
        msg->channel = info->rx_ctrl->channel;
    }
    msg->len = len;
    memcpy(msg->data, data, len);
    msg->data[len] = 0;

    if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
        free(msg);
    }
}

static void rx_task(void *arg)
{
    (void)arg;

    while (true) {
        phymcp_rx_msg_t *msg = NULL;
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE || !msg) {
            continue;
        }

        phymcp_rx_packet_t packet = {
            .rssi = msg->rssi,
            .channel = msg->channel,
        };
        memcpy(packet.src_mac, msg->src_mac, PHYMCP_MAC_LEN);
        memcpy(packet.dst_mac, msg->dst_mac, PHYMCP_MAC_LEN);

        esp_err_t ret = phymcp_frame_unpack(msg->data, msg->len,
                                            &packet.header,
                                            &packet.json,
                                            &packet.json_len);
        if (ret == ESP_OK && s_rx_cb) {
            s_rx_cb(&packet, s_rx_ctx);
        }
        free(msg);
    }
}

esp_err_t phymcp_transport_init(const phymcp_transport_config_t *config,
                                phymcp_rx_cb_t rx_cb,
                                void *rx_ctx)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !rx_cb) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_rx_cb = rx_cb;
    s_rx_ctx = rx_ctx;

    if (s_config.init_nvs) {
        ESP_RETURN_ON_ERROR(init_nvs_if_needed(), TAG, "nvs");
    }
    ESP_RETURN_ON_ERROR(init_wifi_if_needed(&s_config), TAG, "wifi");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init");

    if (s_config.pmk && s_config.pmk_len == ESP_NOW_KEY_LEN) {
        ESP_RETURN_ON_ERROR(esp_now_set_pmk(s_config.pmk), TAG, "pmk");
    }

    s_rx_queue = xQueueCreate(s_config.rx_queue_size, sizeof(phymcp_rx_msg_t *));
    if (!s_rx_queue) {
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(rx_task, "phymcp_rx",
                                s_config.rx_task_stack,
                                NULL,
                                s_config.rx_task_priority,
                                &s_rx_task);
    if (ok != pdPASS) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(recv_cb), TAG, "recv cb");
    ESP_RETURN_ON_ERROR(phymcp_transport_add_peer(PHYMCP_BROADCAST_MAC,
                                                  false, NULL, 0),
                        TAG, "broadcast peer");
    s_started = true;
    return ESP_OK;
}

esp_err_t phymcp_transport_deinit(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    esp_now_unregister_recv_cb();
    esp_now_deinit();

    if (s_rx_task) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    if (s_rx_queue) {
        phymcp_rx_msg_t *msg = NULL;
        while (xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
            free(msg);
        }
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }

    s_rx_cb = NULL;
    s_rx_ctx = NULL;
    s_started = false;
    return ESP_OK;
}

esp_err_t phymcp_transport_add_peer(const uint8_t mac[PHYMCP_MAC_LEN],
                                    bool encrypted,
                                    const uint8_t *lmk,
                                    size_t lmk_len)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }

    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, PHYMCP_MAC_LEN);
    peer.channel = s_config.channel;
    peer.ifidx = s_config.ifidx;
    peer.encrypt = encrypted;
    if (encrypted) {
        if (!lmk || lmk_len != ESP_NOW_KEY_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    }

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    return ret;
}

esp_err_t phymcp_transport_send_json(const uint8_t mac[PHYMCP_MAC_LEN],
                                     phymcp_msg_type_t type,
                                     uint8_t flags,
                                     uint16_t xid,
                                     const char *json)
{
    if (!mac || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mac_is_broadcast(mac)) {
        esp_err_t peer_ret = phymcp_transport_add_peer(mac, false, NULL, 0);
        if (peer_ret != ESP_OK) {
            return peer_ret;
        }
    }

    uint8_t frame[PHYMCP_MAX_FRAME_LEN];
    size_t frame_len = 0;
    ESP_RETURN_ON_ERROR(phymcp_frame_pack(type, flags, phymcp_next_seq(), xid,
                                          json, frame, sizeof(frame), &frame_len),
                        TAG, "pack");
    return esp_now_send(mac, frame, frame_len);
}

bool phymcp_transport_is_started(void)
{
    return s_started;
}
