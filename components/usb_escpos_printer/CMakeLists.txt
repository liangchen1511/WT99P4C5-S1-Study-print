idf_component_register(
    SRCS "usb_escpos_printer.c"
         "uart_escpos_printer.c"
         "escpos_jpeg_raster.c"
         "escpos_text_print.c"
         "ble_escpos_printer.c"
    INCLUDE_DIRS "include"
    REQUIRES usb esp_driver_uart esp_driver_gpio espressif__esp_jpeg freertos esp_common nvs_flash bt espressif__esp_hosted
)
