/* bohun mission (the 4.3" kobzar): hardware bring-up.
 * RGB565 parallel LCD driven by the S3's LCD peripheral (frame buffers in octal
 * PSRAM, bounce buffers in DRAM), GT911 touch over I2C, and the CH422G expander
 * that owns the lines the S3 ran out of: touch reset (IO1), backlight (IO2),
 * LCD reset (IO3). The CH422G is address-as-register: one data byte written to
 * "device" 0x24 sets the mode, to 0x38 sets the IO outputs.
 * All pins per the vendor demos cited in panel.h - not guessed.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "panel.h"

static const char *TAG = "bohun_panel";

/* I2C bus (expander + touch) */
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9

/* RGB interface (LCD.h, 800x480 variant) */
#define PIN_HSYNC 46
#define PIN_VSYNC 3
#define PIN_DE    5
#define PIN_PCLK  7
#define PCLK_HZ   (16 * 1000 * 1000)

/* CH422G: register-as-address writes */
#define CH422G_REG_MODE   0x24
#define CH422G_REG_IO_OUT 0x38
#define CH422G_MODE_IO_OE 0x01
/* IO1 = touch reset, IO2 = backlight, IO3 = LCD reset (CH422G.h) */
#define CH422G_ALL_UP     0x0E

static i2c_master_bus_handle_t s_bus;

/* NOT ESP_ERROR_CHECK: this runs on the touch path, on a bus the GT911
 * clock-stretches - a transient NACK must not panic the dashboard. A missed
 * backlight toggle is a nuisance; init failures are logged loudly and the
 * caller decides. */
static esp_err_t ch422g_write(uint8_t reg_addr, uint8_t val)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = reg_addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G reg 0x%02x: no device handle (%s)", reg_addr,
                 esp_err_to_name(err));
        return err;
    }
    err = i2c_master_transmit(dev, &val, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G reg 0x%02x write failed (%s)", reg_addr,
                 esp_err_to_name(err));
    }
    i2c_master_bus_rm_device(dev);   /* free the handle even on a failed write */
    return err;
}

void panel_backlight(bool on)
{
    ch422g_write(CH422G_REG_IO_OUT, on ? CH422G_ALL_UP : (CH422G_ALL_UP & ~0x04));
}

lv_display_t *panel_init(void)
{
    /* ---- I2C bus ---- */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    /* ---- CH422G: assert resets, then release with backlight on ---- */
    ch422g_write(CH422G_REG_MODE, CH422G_MODE_IO_OE);
    ch422g_write(CH422G_REG_IO_OUT, 0x00);              /* LCD + touch in reset, BL off */
    vTaskDelay(pdMS_TO_TICKS(20));
    ch422g_write(CH422G_REG_IO_OUT, CH422G_ALL_UP);     /* release resets, backlight on */
    vTaskDelay(pdMS_TO_TICKS(120));                     /* GT911 boot time */

    /* ---- RGB panel ---- */
    const esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,        /* v6: replaces bits_per_pixel */
        .num_fbs = 2,                                   /* double FB in PSRAM */
        .bounce_buffer_size_px = 20 * PANEL_H_RES,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            14, 38, 18, 17, 10,     /* B3..B7 */
            39, 0, 45, 48, 47, 21,  /* G2..G7 */
            1, 2, 42, 41, 40,       /* R3..R7 */
        },
        .timings = {
            .pclk_hz = PCLK_HZ,
            .h_res = PANEL_H_RES,
            .v_res = PANEL_V_RES,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* ---- GT911 touch ---- */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_bus, &tp_io_cfg, &tp_io));
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = PANEL_H_RES,
        .y_max = PANEL_V_RES,
        .rst_gpio_num = -1,     /* reset rides the CH422G, already released */
        .int_gpio_num = -1,     /* not wired to the S3; polled */
    };
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t terr = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed (%s) - UI continues without touch", esp_err_to_name(terr));
        tp = NULL;
    }

    /* ---- LVGL ---- */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = PANEL_H_RES * PANEL_V_RES,
        .double_buffer = true,
        .hres = PANEL_H_RES,
        .vres = PANEL_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,          /* buffers are the panel's PSRAM FBs */
            .swap_bytes = false,        /* parallel RGB: native byte order */
            .full_refresh = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_port_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_port_cfg);

    if (tp) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    }

    ESP_LOGI(TAG, "panel up: %dx%d RGB @ %d MHz, touch %s",
             PANEL_H_RES, PANEL_V_RES, PCLK_HZ / 1000000, tp ? "GT911" : "ABSENT");
    return disp;
}
