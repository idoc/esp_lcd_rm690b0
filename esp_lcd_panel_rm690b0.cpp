#include <memory>
#include <thread>
#include <vector>
#include <chrono>


#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "driver/gpio.h"
#include "hal/gpio_types.h"
#include "esp_err.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_rm690b0.h"

constexpr auto TAG = "panel.rm690b0";

constexpr auto LOW = 0;
constexpr auto HIGH = 1;

namespace {
    struct RM690B0Panel {
        esp_lcd_panel_t base{};
        esp_lcd_panel_io_handle_t io = nullptr;
        uint8_t brightness = 0;
        gpio_num_t reset_gpio_num = GPIO_NUM_NC;
        gpio_num_t en_gpio_num = GPIO_NUM_NC;
        uint8_t x_gap = 0;
        uint8_t y_gap = 0;
        uint8_t bits_per_pixel{};
        bool swap_xy = false;
        bool mirror_x = false;
        bool mirror_y = false;
        lcd_rgb_element_order_t rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        bool grayscale = false;
    };

    RM690B0Panel* panel_cast(const esp_lcd_panel_t* panel) {
        return __containerof(panel, RM690B0Panel, base);
    }

    constexpr int32_t command_prefix = 0x02000000UL;
    constexpr int32_t pixel_prefix = 0x32000000UL;

    constexpr uint8_t color_3_bits_per_pixel = 0b00110011;
    constexpr uint8_t grayscale_8_bits_per_pixel = 0b00010001;
    constexpr uint8_t color_8_bits_per_pixel = 0b00100010;
    constexpr uint8_t color_16_bits_per_pixel = 0b01010101;
    constexpr uint8_t color_18_bits_per_pixel = 0b01100110;
    constexpr uint8_t color_24_bits_per_pixel = 0b01110111;

    constexpr uint8_t lcd_cmd_unknown_0x2400 = 0x24;
    constexpr uint8_t lcd_cmd_unknown_0x5B00 = 0x5B;
    constexpr uint8_t lcd_cmd_write_display_brightness = 0x51;
    constexpr uint8_t lcd_cmd_set_disp_mode = 0xC2;
    constexpr uint8_t lcd_cmd_cmd_mode_switch = 0xFE;
    constexpr uint8_t lcd_cmd_interface_pixel_format_option = 0x80;

    constexpr uint8_t rotation_normal = 0;
    constexpr uint8_t rotation_mirror_y = 0x10;
    constexpr uint8_t rotation_swap_xy = 0x20;
    constexpr uint8_t rotation_minus90 = 0x30;
    constexpr uint8_t rotation_plus90 = 0x60;

    constexpr uint8_t rbg_element_order_rgb = 0;
    constexpr uint8_t rbg_element_order_bgr = 0b00001000;

    // This struct is used to convert commands from the structure described in
    // RM690B0's docs to the one used esp_lcd_panel_io_spi. It should work for
    // non-spi interfaces (esp_lcd_panel_io_i2c etc.), but I don't have the means to test it.
    //
    // RM690B0's command structure is four bytes. These need to be combined into
    // a single 32-bit integer for the esp_lcd_panel_io_spi interface. After these
    // four bytes, all other bytes sent in the transaction are assumed to be parameters to the command.
    //
    // Command structure is four bytes, which we combine into
    // an int32_t for the esp_lcd_panel_io_spi interface:
    // 1. Command. A command of 0x02 tells RM690B0 to expect parameters over a single wire, even in quad or octal SPI mode.
    // 2. 0x00 (always)
    // 3. The command address
    // 4. Another 0x00 (there are exceptions to this, but they are not supported by this structure)
    //
    // The docs use two bytes to describe the command address, even though the second byte is always zero.
    // I do the same in the code to keep things consistent.
    //
    // Some commands require a delay before other commands can be processed.
    // NOLINTBEGIN(*-magic-numbers)
    struct LCDCmd {
        LCDCmd(const uint8_t command_addr, const std::vector<uint8_t>& param) noexcept :
            lcd_cmd(command_prefix + (static_cast<int32_t>(command_addr) << 8)),
            param(param) {
            using namespace std::literals;

            switch (command_addr) {
            case LCD_CMD_SLPIN:
                delay = 5ms;
                break;

            case LCD_CMD_SLPOUT:
                delay = 120ms;
                break;

            case LCD_CMD_DISPON:
            case lcd_cmd_set_disp_mode:
                delay = 10ms;
                break;

            default:
                delay = 0ms;
            }
        }

        int32_t lcd_cmd;
        std::vector<uint8_t> param;
        std::chrono::milliseconds delay{};
    };

    // NOLINTEND(*-magic-numbers)

    // This command sequence (and the comments) is based on LilyGo's code by Lewis He:
    // https://github.com/Xinyuan-LilyGO/LilyGo-Display-IDF/blob/master/main/initSequence.c,

    std::vector<LCDCmd> default_init_cmds() {
        return {
            {lcd_cmd_cmd_mode_switch, {0x20}}, // CMD Mode Switch: Manufacture Command Set Page Panel

            // The 0x2400 and 0x5B00 commands  came from the LilyGo code.
            // I cannot find mention of them in the RM9690B0 docs and don't know what they do.
            // All I can say is that the display stays dark if I remove them, so here they stay.
            {lcd_cmd_unknown_0x2400, {0x80}}, // SPI write RAM
            {lcd_cmd_unknown_0x5B00, {0x2E}}, //! 230918:SWIRE FOR BV6804

            {lcd_cmd_cmd_mode_switch, {0x00}}, // CMD Mode Switch: User Command Set (UCS = CMD1)
            {lcd_cmd_set_disp_mode, {0x00}}, // set_DISP Mode: internal timing
            {LCD_CMD_TEON, {0x00}}, // Tearing effect pin on
            {LCD_CMD_SLPOUT, {}}, // Sleep out
            {LCD_CMD_DISPON, {}}, // Display on
        };
    }

    esp_err_t send_command(const esp_lcd_panel_t* panel, const LCDCmd& cmd) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        if (cmd.param.size() == 1) {
            ESP_LOGD(TAG, "Sending command %#010x with parameter 0x%x", cmd.lcd_cmd, cmd.param.at(0));
        } else {
            ESP_LOGD(TAG, "Sending command %#010x with %zu parameters", cmd.lcd_cmd, cmd.param.size());
        }

        const esp_err_t ret = rm690b0->io->tx_param(rm690b0->io, cmd.lcd_cmd, cmd.param.data(), cmd.param.size());

        std::this_thread::sleep_for(cmd.delay);
        return ret;
    }

    esp_err_t send_command(const esp_lcd_panel_t* panel, const uint8_t command_addr,
                           const std::vector<uint8_t>& param = {}) {
        const LCDCmd cmd(command_addr, param);
        return send_command(panel, cmd);
    }

    esp_err_t send_commands(const esp_lcd_panel_t* panel, const std::vector<LCDCmd>& cmds) {
        for (const auto& cmd : cmds) {
            ESP_RETURN_ON_ERROR((send_command(panel, cmd)), TAG, "send command failed"); // NOLINT
        }

        return ESP_OK;
    }

    // NOLINTBEGIN(*-magic-numbers)
    uint8_t get_pixel_format(const esp_lcd_panel_t* panel) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        if (rm690b0->grayscale && rm690b0->bits_per_pixel != 8) {
            ESP_LOGE(TAG, "Grayscale is only supported with 8 bits per pixel");
            return 0;
        }

        switch (rm690b0->bits_per_pixel) {
        case 3:
            return color_3_bits_per_pixel;

        case 8:
            return rm690b0->grayscale ? grayscale_8_bits_per_pixel : color_8_bits_per_pixel;

        case 16:
            return color_16_bits_per_pixel;

        case 18:
            return color_18_bits_per_pixel;

        case 24:
            return color_24_bits_per_pixel;

        default:
            return 0;
        }
    }

    // NOLINTEND(*-magic-numbers)

    // Returns the controller's code for the rotation set up by the user.
    // Note that not all combinations seem to be supported by the RM690B0.
    // If the user tries to mirror both x and y, at the same time, we return a "similar" code known to work.

    // 0x60 = 0b01100000 swap xy + mirror y (rotate 90)
    // 0x50 = 0b01010000 display goes wonky
    // 0x40 = 0b01000000 display goes wonky
    // 0x30 = 0b00110000 swap xy + mirror x (rotate -90)
    // 0x20 = 0b00100000 swap xy
    // 0x10 = 0b00010000 mirror y
    // 0x00 = 0b00000000 default
    uint8_t get_scan_direction(const esp_lcd_panel_t* panel) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        uint8_t rotation = rotation_normal;

        if (rm690b0->swap_xy) {
            if (rm690b0->mirror_x) {
                rotation = rotation_minus90;
            } else if (rm690b0->mirror_y) {
                rotation = rotation_plus90;
            } else {
                rotation = rotation_swap_xy;
            }
        } else if (rm690b0->mirror_y) {
            rotation = rotation_mirror_y;
        }

        return rotation;
    }

    esp_err_t update_screen_orientation(const esp_lcd_panel_t* panel) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        uint8_t param = get_scan_direction(panel);
        const uint8_t element_order = rm690b0->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_RGB
                                          ? rbg_element_order_rgb
                                          : rbg_element_order_bgr;
        param |= element_order;
        ESP_LOGD(TAG, "Applying rotation code: 0x%x", param); // NOLINT

        return send_command(panel, LCD_CMD_MADCTL, {param});
    }

    // ReSharper restore CppRedundantZeroInitializerInAggregateInitialization
    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t init(esp_lcd_panel_t* panel) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        // Power up the AMOLED controller
        if (rm690b0->en_gpio_num != GPIO_NUM_NC) {
            gpio_set_level(rm690b0->en_gpio_num, HIGH);
            // The RM690B0 controller needs time to wake up before it can process commands
            using namespace std::literals;
            std::this_thread::sleep_for(25ms);
        }

        // Send initialization commands
        ESP_RETURN_ON_ERROR(send_commands(panel, default_init_cmds()), TAG, // NOLINT
                            "Failed to send init commands to display");

        // Set up the image
        update_screen_orientation(panel);

        const uint8_t pixel_format = get_pixel_format(panel);
        if (!pixel_format) {
            ESP_LOGE(TAG, "Unsupported pixel format setting: %d bits per pixel, grayscale: %d",
                     rm690b0->bits_per_pixel,
                     rm690b0->grayscale);
            return ESP_ERR_INVALID_ARG;
        }

        ESP_RETURN_ON_ERROR(send_command(panel, LCD_CMD_COLMOD, {pixel_format}), // NOLINT
                            TAG, "Error setting display brightness");

        if (rm690b0->bits_per_pixel == 16) { // NOLINT(*-magic-numbers)
            static constexpr uint32_t swap_rgb565_bytes = 0b00010000;
            send_command(panel, lcd_cmd_interface_pixel_format_option, {swap_rgb565_bytes});
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_rm690b0_set_brightness(panel, 0xFF), TAG, // NOLINT
                            "Error setting display brightness");

        return ESP_OK;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t reset(esp_lcd_panel_t* panel) {
        using namespace std::literals;
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        static constexpr auto delay = 300ms;

        gpio_set_level(rm690b0->reset_gpio_num, HIGH);
        std::this_thread::sleep_for(delay);
        gpio_set_level(rm690b0->reset_gpio_num, LOW);
        std::this_thread::sleep_for(delay);
        gpio_set_level(rm690b0->reset_gpio_num, HIGH);
        std::this_thread::sleep_for(delay);

        return ESP_OK;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    // ReSharper disable CppParameterMayBeConst
    esp_err_t draw_bitmap(esp_lcd_panel_t* panel, int x_start, int y_start, int x_end, int y_end,
                          const void* color_data) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);
        const esp_lcd_panel_io_handle_t io = rm690b0->io; // NOLINT(*-misplaced-const)

        // Set the drawing window
        //
        // The -1 adjustment to x_end and y_end is needed because the esp_lcd API
        // says the drawing windows *excludes* x_end and y_end, but the RM690B0 chip
        // expects they are included.

        x_start += rm690b0->x_gap;
        x_end += rm690b0->x_gap - 1;

        y_start += rm690b0->y_gap;
        y_end += rm690b0->y_gap - 1;

        const std::vector<LCDCmd> cmds = {
            {
                LCD_CMD_CASET,
                {
                    static_cast<uint8_t>(x_start >> 8 & 0xFF),
                    static_cast<uint8_t>(x_start & 0xFF),
                    static_cast<uint8_t>(x_end >> 8 & 0xFF),
                    static_cast<uint8_t>(x_end & 0xFF)
                }
            },
            {
                LCD_CMD_RASET,
                {
                    static_cast<uint8_t>(y_start >> 8 & 0xFF),
                    static_cast<uint8_t>(y_start & 0xFF),
                    static_cast<uint8_t>(y_end >> 8 & 0xFF),
                    static_cast<uint8_t>(y_end & 0xFF)
                }
            },
            {
                LCD_CMD_RAMWR,
                {}
            }
        };

        ESP_RETURN_ON_ERROR(send_commands(panel, cmds), TAG, "set window commands failed"); // NOLINT

        // Send color data
        const size_t area_size = (x_end - x_start + 1) * (y_end - y_start + 1);
        constexpr int lcd_cmd = pixel_prefix + (LCD_CMD_RAMWR << 8);
        static constexpr uint8_t bits_per_byte = 8;
        return io->tx_color(io, lcd_cmd, color_data, area_size * rm690b0->bits_per_pixel / bits_per_byte);
    }

    esp_err_t reset(gpio_num_t pin_num, const char* pin_name) {
        if (pin_num == GPIO_NUM_NC) {
            return ESP_OK;
        }

        const esp_err_t ret = gpio_reset_pin(pin_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset %s pin: %d", pin_name, ret);
        } else {
            ESP_LOGD(TAG, "%s pin reset", pin_name);
        }

        return ret;
    }

    void reset_all_pins(const esp_lcd_panel_t* panel) {
        const RM690B0Panel* rm690b0 = panel_cast(panel);

        reset(rm690b0->reset_gpio_num, "RESET");
        reset(rm690b0->en_gpio_num, "EN");
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t del(esp_lcd_panel_t* panel) {
        const std::unique_ptr<RM690B0Panel> owner(panel_cast(panel));

        reset_all_pins(panel);
        return ESP_OK;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t invert_color(esp_lcd_panel_t* panel, bool invert_color_data) {
        return send_command(panel, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF);
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t swap_xy(esp_lcd_panel_t* panel, bool swap_axes) {
        RM690B0Panel* rm690b0 = panel_cast(panel);

        rm690b0->swap_xy = swap_axes;
        return update_screen_orientation(panel);
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t mirror(esp_lcd_panel_t* panel, bool mirror_x, bool mirror_y) {
        RM690B0Panel* rm690b0 = panel_cast(panel);
        rm690b0->mirror_x = mirror_x;
        rm690b0->mirror_y = mirror_y;

        return update_screen_orientation(panel);
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t set_gap(esp_lcd_panel_t* panel, int x_gap, int y_gap) {
        RM690B0Panel* rm690b0 = panel_cast(panel);

        rm690b0->x_gap = x_gap;
        rm690b0->y_gap = y_gap;

        return ESP_OK;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t disp_on_off(esp_lcd_panel_t* panel, bool on_off) {
        const uint8_t command_code = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
        return send_command(panel, command_code);
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    esp_err_t sleep(esp_lcd_panel_t* panel, bool sleep) {
        const uint8_t command_code = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
        return send_command(panel, command_code);
    }


    // Initialize a GPIO pin for output.
    // If this operation fails, *all* output pins are reset.
    esp_err_t init_out_pin(const esp_lcd_panel_t* panel, gpio_num_t pin, const char* pin_name) {
        if (pin == GPIO_NUM_NC) {
            return ESP_OK;
        }

        const gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_OUTPUT,
        };

        ESP_LOGD(TAG, "Configuring %s pin (GPIO %d) as output", pin_name, pin);
        const esp_err_t ret = gpio_config(&io_conf);

        if (ret != ESP_OK) {
            reset_all_pins(panel);
        }

        return ret;
    }
}

uint8_t esp_lcd_panel_rm690b0_get_brightness(const esp_lcd_panel_t* panel) {
    const RM690B0Panel* rm690b0 = panel_cast(panel);
    return rm690b0->brightness;
}

esp_err_t esp_lcd_panel_rm690b0_set_brightness(const esp_lcd_panel_t* panel, uint8_t brightness) {
    RM690B0Panel* rm690b0 = panel_cast(panel);
    rm690b0->brightness = brightness;

    return send_command(panel, lcd_cmd_write_display_brightness, {brightness});
}

esp_err_t esp_lcd_new_panel_rm690b0(esp_lcd_panel_io_handle_t io, // NOLINT(*-misplaced-const)
                                    const esp_lcd_panel_dev_config_t* panel_dev_config,
                                    esp_lcd_panel_handle_t* ret_panel) {
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "io must not be null");
    ESP_RETURN_ON_FALSE(panel_dev_config, ESP_ERR_INVALID_ARG, TAG, "panel_dev_config must not be null");
    ESP_RETURN_ON_FALSE(ret_panel, ESP_ERR_INVALID_ARG, TAG, "ret_panel must not be null");

    // Populate rm690b0 object
    auto rm690b0 = std::make_unique<RM690B0Panel>();

    rm690b0->base.del = del;
    rm690b0->base.reset = reset;
    rm690b0->base.init = init;
    rm690b0->base.draw_bitmap = draw_bitmap;
    rm690b0->base.invert_color = invert_color;
    rm690b0->base.set_gap = set_gap;
    rm690b0->base.mirror = mirror;
    rm690b0->base.swap_xy = swap_xy;
    rm690b0->base.disp_on_off = disp_on_off;
    rm690b0->base.disp_sleep = sleep;

    rm690b0->io = io;
    rm690b0->bits_per_pixel = panel_dev_config->bits_per_pixel;
    // ReSharper disable once CppDFANullDereference
    rm690b0->rgb_ele_order = panel_dev_config->rgb_ele_order;

    if (panel_dev_config->reset_gpio_num >= 0) {
        rm690b0->reset_gpio_num = static_cast<gpio_num_t>(panel_dev_config->reset_gpio_num);
    }

    auto rm690b0_vendor = static_cast<rm960b0_vendor_config_t*>(panel_dev_config->vendor_config);
    if (!rm690b0_vendor) {
        ESP_LOGW(
            TAG, "No vendor config found. Caller must power up the RM690B0 before calling init on this driver.");
    } else {
        rm690b0->en_gpio_num = rm690b0_vendor->en_gpio_num;
        if (rm690b0->en_gpio_num == GPIO_NUM_NC) {
            ESP_LOGW(
                TAG,
                "No EN pin found in vendor config. Caller must power up the RM690B0 calling init on this driver.");
        }

        rm690b0->grayscale = rm690b0_vendor->grayscale;
    }

    // Initialize gpio pins

    // IMPORTANT: If you add a new pin to this section, you must also add the pin to be released in reset_all_pins()
    ESP_RETURN_ON_ERROR(init_out_pin(&rm690b0->base, rm690b0->reset_gpio_num, "RESET"), TAG, "Failed to init pin"); // NOLINT(*-const-correctness)
    ESP_RETURN_ON_ERROR(init_out_pin(&rm690b0->base, rm690b0->en_gpio_num, "EN"), TAG, "Failed to init pin"); // NOLINT(*-const-correctness)

    // All done
    // ReSharper disable once CppDFANullDereference
    *ret_panel = &rm690b0.release()->base;

    ESP_LOGD(TAG, "new rm690b0 panel @%p", *ret_panel);

    return ESP_OK;
}
