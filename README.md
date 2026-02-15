# ESP LCD RM690B0

[![Component Registry](https://components.espressif.com/components/idoc/esp_lcd_rm690b0/badge.svg)](https://components.espressif.com/components/idoc/esp_lcd_rm690b0)

An esp_lcd driver for the RM690B0 AMOLED controller.

This is the controller used in the LilyGo T4S3. It's only been tested with the QSPI connection used on this board.

Information (and bug fixes) related to other boards and connection are welcome.

| LCD controller | Communication interface | Component name  |                                                    Link to datasheet                                                    |
|:--------------:|:-----------------------:|:---------------:|:-----------------------------------------------------------------------------------------------------------------------:|
|    RM690B0     |  SPI/QSPI/I80/MIPI-DSI  | esp_lcd_rm690b0 | [Data Sheet](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series/blob/master/datasheet/RM690B0%20DataSheet_V0.2.pdf) |

## Add to project

Packages from this repository are uploaded to [Espressif's component service](https://components.espressif.com/).
You can add them to your project via `idf.py add-dependency`, e.g.

```bash
idf.py add-dependency "espressif/esp_lcd_rm690b0^1.0.0"
```

Alternatively, you can create `idf_component.yml`. More is
in [Espressif's documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html).

## Initialization Code

### QSPI interface
GPIO numbers are for the LilyGo T4 S3. Yours might be different.

```c
    ESP_LOGI(TAG, "Initializing QSPI bus");
    const spi_bus_config_t bus_config = {
        .data0_io_num = GPIO_NUM_14,
        .data1_io_num = GPIO_NUM_10,
        .sclk_io_num = GPIO_NUM_15,
        .data2_io_num = GPIO_NUM_16,
        .data3_io_num = GPIO_NUM_12,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGI(TAG, "Initializing panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = -1,
        .cs_gpio_num = GPIO_NUM_11,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .trans_queue_depth = 10,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        }
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle), err, TAG,
                      "New panel IO failed");

    ESP_LOGI(TAG, "Initializing panel");
    rm960b0_vendor_config_t vendor_config = {
        .en_gpio_num = GPIO_NUM_9,
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_rm690b0(*ret_io, &panel_config, &panel), err, TAG, "New panel failed");
```
