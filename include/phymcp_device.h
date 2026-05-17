#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "phymcp_frame.h"
#include "phymcp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*phymcp_tool_handler_t)(const char *arguments_json,
                                           char *response_json,
                                           size_t response_json_len,
                                           void *ctx);

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    bool destructive;
    phymcp_tool_handler_t handler;
    void *ctx;
} phymcp_tool_t;

typedef struct {
    const char *name;
    const char *class_name;
    const char *model;
    const char *firmware;
    const char *tool_etag;
    const phymcp_tool_t *tools;
    size_t tool_count;
    bool encrypted_unicast_required;
    uint8_t duplicate_cache_size;
    phymcp_transport_config_t transport;
} phymcp_device_config_t;

void phymcp_device_default_config(phymcp_device_config_t *config);
esp_err_t phymcp_device_init(const phymcp_device_config_t *config);
esp_err_t phymcp_device_deinit(void);

#ifdef __cplusplus
}
#endif
