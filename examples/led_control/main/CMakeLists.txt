set(srcs)
set(reqs
    freertos
    json
    phyMCP
)

if(CONFIG_PHYMCP_APP_HOST)
    list(APPEND srcs "host_main.c")
    list(APPEND reqs
        esp_driver_uart
        esp_driver_usb_serial_jtag
    )
else()
    list(APPEND srcs "device_main.c")
    list(APPEND reqs
        esp_driver_gpio
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${reqs}
)
