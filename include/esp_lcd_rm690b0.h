#pragma once

#include "esp_lcd_panel_dev.h"
#include "soc/gpio_num.h"
#include "esp_lcd_panel_interface.h"

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  This structure needs to be passed to the `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 *
 */
typedef struct { // NOLINT(*-use-using)
    gpio_num_t en_gpio_num;
    bool grayscale;
} rm960b0_vendor_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a panel for the RM690B0 AMOLED controller
 *
 * @param[in] io panel IO handle
 * @param[in] panel_dev_config general panel device configuration
 * @param[out] ret_panel Returned panel handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if a parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 */
esp_err_t esp_lcd_new_panel_rm690b0(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t* panel_dev_config,
                                    esp_lcd_panel_handle_t* ret_panel);


uint8_t esp_lcd_panel_rm690b0_get_brightness(const esp_lcd_panel_t* panel);
esp_err_t esp_lcd_panel_rm690b0_set_brightness(const esp_lcd_panel_t* panel, uint8_t brightness);

#ifdef __cplusplus
}
#endif
