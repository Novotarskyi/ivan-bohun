/* bohun: the swarm on glass.
 * ST7789 240x320 over SPI, LVGL via esp_lvgl_port. THREE recolor labels stacked
 * by Y so the header and footer read BIG and only the dense data is small:
 *   s_head - unscii_16 (16px): IVAN BOHUN / Pysar / vN-date  (big)
 *   s_loss - unscii_8: Loss ESP-NOW %, its own small line under the head
 *   s_rows - unscii_8  (8px) : the per-node table, columns lined up (small)
 *   s_foot - unscii_16 (16px): quorum / req / avg rssi / served%        (big)
 * At 16px only ~14 chars fit per 234px row, so the 17-char vMAC and the wide
 * node rows (~27 chars: name+role+rssi+delivery+uptime) ride the 8px font where
 * they fit whole and, being monospace, stay column-aligned.
 * Wrapping is disabled (LONG_CLIP) - a wrapped line breaks LVGL's recolor parser.
 * BRING-UP: panel assumed ST7789; invert/mirror are the usual first-flash fixes.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mask.h"   /* MASK_STATIC_IP - the address this glass prints */
#include "tft.h"
#include "led.h"
#include "ota_window.h"
#include "esp_app_desc.h"

static const char *TAG = "bohun_tft";

#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_CS   10
#define PIN_DC    9
#define PIN_RST   8
#define PIN_BL    7
/* touch loom (wired at assembly, driver still deferred): the controller runs on
 * its own after reset release and pulses INT low on any finger - enough for
 * tap-to-blank without knowing which IC it is (CST816/FT6x36/GT911 all do this). */
#define PIN_TP_INT 42
#define PIN_TP_RST 41
#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_HOST  SPI2_HOST

/* brighter palette (the panel washes muted colours out) */
#define COL_LIME  "d8ff33"
#define COL_ALIVE "43e86a"
#define COL_LOST  "ff5555"
#define COL_DIM   "c2c0cc"
#define COL_WARN  "ffb020"
#define DIVIDER   "------------------------"   /* 24 dashes, fits the 8px row */

static lv_obj_t *s_head;   /* unscii_16 identity + version line */
static lv_obj_t *s_loss;   /* unscii_8: the radio-loss line (16px could not hold it) */
static char s_ver_line[48];   /* "vN-YYYY.MM.DD" (app-desc version is a 32-byte field) */
static lv_obj_t *s_rows;   /* unscii_8  vMAC + node table */
static lv_obj_t *s_foot;   /* unscii_16 footer stats */
static lv_obj_t *s_rail;      /* unscii_8 rail status line */
static lv_obj_t *s_chip[6];   /* live mirror of the six SK6812 pixels */
static lv_obj_t *s_rail_nm;   /* unscii_8 initials under the chips */
static lv_obj_t *s_boot_shaft, *s_boot_sole, *s_boot_lbl;  /* the booting boot, bottom-right */

/* ---- tap-to-blank: any touch toggles the backlight ---- */
static volatile bool s_tp_flag;
static bool s_blanked;

static void IRAM_ATTR tp_int_isr(void *arg)
{
    (void)arg;
    s_tp_flag = true;
}

static void tp_blank_task(void *arg)
{
    (void)arg;
    int64_t last_action = 0;
    for (;;) {
        if (s_tp_flag) {
            s_tp_flag = false;
            int64_t t0 = esp_timer_get_time();
            if (t0 - last_action < 500000) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            /* held = INT still low, or the controller keeps pulsing events */
            int64_t last_seen = t0;
            bool long_press = false;
            bool ota_hold = false;
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(30));
                int64_t now = esp_timer_get_time();
                if (s_tp_flag) { s_tp_flag = false; last_seen = now; }
                if (gpio_get_level(PIN_TP_INT) == 0) last_seen = now;
                if (now - t0 >= 1200000) long_press = true;   /* keep timing */
                if (now - t0 >= 4000000) { ota_hold = true; break; }   /* the marathon */
                if (now - last_seen > 250000) break;   /* finger gone */
            }
            if (ota_hold) {
                /* 4 s of finger = open the OTA window. Amber the rail from
                 * right here - the render tick is up to 1 s late and the
                 * operator needs the "let go now" cue at the threshold. */
                s_blanked = false;
                gpio_set_level(PIN_BL, 1);
                led_wake();
                led_ota(true);
                ota_window_open();
                /* The finger is still down. Drain the REST of this press
                 * before re-arming: the outer loop would otherwise read the
                 * held finger as a fresh gesture and its release as a tap -
                 * and a tap while the window is open means cancel. */
                int64_t seen = esp_timer_get_time();
                for (;;) {
                    vTaskDelay(pdMS_TO_TICKS(30));
                    int64_t now = esp_timer_get_time();
                    if (s_tp_flag) { s_tp_flag = false; seen = now; }
                    if (gpio_get_level(PIN_TP_INT) == 0) seen = now;
                    if (now - seen > 300000) break;   /* truly gone */
                }
            } else if (long_press) {
                /* maintenance blackout: screen AND rail dark - flash from darkness */
                s_blanked = true;
                gpio_set_level(PIN_BL, 0);
                led_blackout();
            } else if (ota_window_status(NULL, NULL, NULL) != OTAW_IDLE) {
                /* a tap while the window is open cancels it */
                ota_window_close();
            } else {
                s_blanked = !s_blanked;
                gpio_set_level(PIN_BL, s_blanked ? 0 : 1);
                if (!s_blanked) led_wake();   /* waking the screen wakes the rail */
            }
            last_action = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void tft_init(void)
{
    gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_BL };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS, .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40, .double_buffer = true,
        .hres = LCD_H_RES, .vres = LCD_V_RES, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    (void)disp;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_head = lv_label_create(scr);
    lv_label_set_recolor(s_head, true);
    lv_label_set_long_mode(s_head, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_head, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_head, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_line_space(s_head, 2, 0);
    lv_obj_set_pos(s_head, 4, 4);
    lv_obj_set_width(s_head, LCD_H_RES - 6);
    lv_label_set_text(s_head, "#" COL_LIME " IVAN BOHUN#\nwaiting...");

    {
        /* app-desc date is "Mmm dd yyyy"; the glass wants numbers */
        const esp_app_desc_t *ad = esp_app_get_description();
        char mon[4] = "";
        int d = 0, y = 0;
        sscanf(ad->date, "%3s %d %d", mon, &d, &y);
        static const char *MM = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char *q = strstr(MM, mon);
        int mo = q ? (int)(q - MM) / 3 + 1 : 0;
        snprintf(s_ver_line, sizeof(s_ver_line), "v%.10s-%04u.%02u.%02u",
                 ad->version, (unsigned)y % 10000, (unsigned)mo % 100, (unsigned)d % 100);
    }
    s_loss = lv_label_create(scr);
    lv_label_set_recolor(s_loss, true);
    lv_obj_set_style_text_font(s_loss, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_loss, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(s_loss, 4, 64);
    lv_obj_set_width(s_loss, LCD_H_RES - 6);
    lv_label_set_text(s_loss, "");

    s_rows = lv_label_create(scr);
    lv_label_set_recolor(s_rows, true);
    lv_label_set_long_mode(s_rows, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_rows, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_rows, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_line_space(s_rows, 3, 0);
    lv_obj_set_pos(s_rows, 4, 78);
    lv_obj_set_width(s_rows, LCD_H_RES - 6);
    lv_label_set_text(s_rows, "");

    s_foot = lv_label_create(scr);
    lv_label_set_recolor(s_foot, true);
    lv_label_set_long_mode(s_foot, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_foot, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_line_space(s_foot, 2, 0);
    lv_obj_set_pos(s_foot, 4, 178);
    lv_obj_set_width(s_foot, LCD_H_RES - 6);
    lv_label_set_text(s_foot, "");

    /* ---- the rail row: status + a live mirror of the six pixels ---- */
    s_rail = lv_label_create(scr);
    lv_label_set_recolor(s_rail, true);
    lv_obj_set_style_text_font(s_rail, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_rail, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(s_rail, 4, 258);
    lv_obj_set_width(s_rail, LCD_H_RES - 6);
    lv_label_set_text(s_rail, "");
    for (int i = 0; i < 6; i++) {
        s_chip[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_chip[i]);
        lv_obj_set_size(s_chip[i], 16, 16);
        lv_obj_set_pos(s_chip[i], 6 + i * 22, 274);
        lv_obj_set_style_radius(s_chip[i], 3, 0);
        lv_obj_set_style_bg_opa(s_chip[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_chip[i], lv_color_hex(0x22262e), 0);
        lv_obj_set_style_border_width(s_chip[i], 1, 0);
        lv_obj_set_style_border_color(s_chip[i], lv_color_hex(0x2a2a33), 0);
    }
    /* The booting indicator: a boot, drawn as a shaft plus a sole, roughly twice a
     * rail chip (16 px) so it reads at a glance without shouting. Bottom-right, in
     * the one corner nothing else occupies: the rail status line ends around x=204
     * at y=258, the chips stop at x=132, and the name row is short. Everything here
     * stays inside 240x320 with room to spare. Hidden unless something is booting. */
    s_boot_shaft = lv_obj_create(scr);
    lv_obj_remove_style_all(s_boot_shaft);
    lv_obj_set_size(s_boot_shaft, 12, 22);
    lv_obj_set_pos(s_boot_shaft, 200, 272);
    lv_obj_set_style_radius(s_boot_shaft, 2, 0);
    lv_obj_set_style_bg_opa(s_boot_shaft, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_boot_shaft, lv_color_hex(0xffb020), 0);
    lv_obj_add_flag(s_boot_shaft, LV_OBJ_FLAG_HIDDEN);

    s_boot_sole = lv_obj_create(scr);
    lv_obj_remove_style_all(s_boot_sole);
    lv_obj_set_size(s_boot_sole, 30, 10);
    lv_obj_set_pos(s_boot_sole, 200, 292);
    lv_obj_set_style_radius(s_boot_sole, 3, 0);
    lv_obj_set_style_bg_opa(s_boot_sole, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_boot_sole, lv_color_hex(0xffb020), 0);
    lv_obj_add_flag(s_boot_sole, LV_OBJ_FLAG_HIDDEN);

    s_boot_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_boot_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_boot_lbl, lv_color_hex(0xffb020), 0);
    lv_obj_set_pos(s_boot_lbl, 160, 304);
    lv_obj_set_width(s_boot_lbl, 74);
    lv_obj_set_style_text_align(s_boot_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_boot_lbl, "");
    lv_obj_add_flag(s_boot_lbl, LV_OBJ_FLAG_HIDDEN);

    s_rail_nm = lv_label_create(scr);
    lv_label_set_recolor(s_rail_nm, true);
    lv_obj_set_style_text_font(s_rail_nm, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_rail_nm, lv_color_hex(0x8a8894), 0);
    lv_obj_set_pos(s_rail_nm, 6, 294);
    lv_label_set_text(s_rail_nm, "N1 N2 N3 N4 Dp MC");
    lvgl_port_unlock();

    /* touch controller out of reset; INT edge = tap-to-blank */
    gpio_config_t tr = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_TP_RST };
    gpio_config(&tr);
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1);
    gpio_config_t ti = {
        .mode = GPIO_MODE_INPUT, .pin_bit_mask = 1ULL << PIN_TP_INT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&ti);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_TP_INT, tp_int_isr, NULL);
    xTaskCreate(tp_blank_task, "tp_blank", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "TFT up: ST7789 %dx%d, LVGL ready, tap-to-blank armed", LCD_H_RES, LCD_V_RES);
}

/* requests in <=4 chars for the 14-char foot line: 999, 1.0k, 42k, 1.2M.
 * The foot budget is real - at unscii_16 the glass fits ~14 chars per row,
 * and 2000+ served pushed the p50 straight off the right edge. */
static void fmt_kreq(char *out, size_t n, uint32_t v)
{
    if (v < 1000)          snprintf(out, n, "%u", (unsigned)v);
    else if (v < 10000)    snprintf(out, n, "%u.%uk", (unsigned)(v / 1000),
                                    (unsigned)((v / 100) % 10));
    else if (v < 1000000)  snprintf(out, n, "%uk", (unsigned)(v / 1000));
    else                   snprintf(out, n, "%u.%uM", (unsigned)(v / 1000000) % 10,
                                    (unsigned)((v / 100000) % 10));
}

/* compact uptime, always <=3 chars so the column stays aligned */
static void fmt_uptime(char *out, size_t n, uint32_t s)
{
    if (s < 60)         snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 3600)  snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, n, "%luh", (unsigned long)(s / 3600));
    else if (s < 86400UL * 100) snprintf(out, n, "%lud", (unsigned long)(s / 86400));
    else                snprintf(out, n, "%luD", (unsigned long)(s / 86400 / 10));  /* 100d+ in tens, keeps the column */
}

void tft_render(const swarm_snap_entry_t *src, int n, uint8_t leader_id)
{
    if (!s_head) return;

    /* sort a local copy by id so rows read n1..n4, display.
     * static: called from the single render task, and the entry struct is now
     * large enough that two of these on the stack overflowed it. */
    static swarm_snap_entry_t e[8];
    if (n > 8) n = 8;
    memcpy(e, src, n * sizeof(e[0]));
    for (int i = 1; i < n; i++) {
        swarm_snap_entry_t k = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].id > k.id) { e[j + 1] = e[j]; j--; }
        e[j + 1] = k;
    }

    uint32_t generation = 0;
    int booting = 0;       /* nodes up on the radio but not yet serving anything */
    int alive = 0, updating = 0, loss_sum = 0, loss_n = 0, rssi_sum = 0, rssi_n = 0, upd_pct = 0;
    /* FLEET sums - the leader balances and serves nothing, so its own counters
     * stopped being the story. Same arithmetic as the kobzar and the website. */
    uint32_t fleet_req = 0, p50_sum = 0;
    uint64_t fleet_asked = 0, fleet_bad = 0;
    int lat_n = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].alive) alive++;
        if (e[i].alive && e[i].eligible && !e[i].serving) booting++;
        if (e[i].updating) { updating++; if (e[i].update_pct > upd_pct) upd_pct = e[i].update_pct; }
        if (!e[i].self && e[i].alive) {
            loss_sum += e[i].loss_pct; loss_n++;
            if (e[i].rssi != 0) { rssi_sum += e[i].rssi; rssi_n++; }
        }
        if (e[i].eligible) {
            fleet_req   += e[i].req_count;
            fleet_asked += (uint64_t)e[i].req_count + e[i].req_err;
            fleet_bad   += e[i].req_err_srv;
            if (e[i].alive && e[i].serving && e[i].id != leader_id && e[i].p50_us) {
                p50_sum += e[i].p50_us / 1000;
                lat_n++;
            }
        }
        if (e[i].id == leader_id) {
            generation = e[i].generation;
        }
    }
    int fleet_avail = fleet_asked
        ? (int)(((fleet_asked - fleet_bad) * 1000) / fleet_asked) : -1;
    uint32_t fleet_p50 = lat_n ? p50_sum / lat_n : 0;
    int swarm_loss = loss_n ? loss_sum / loss_n : 0;
    int avg_rssi   = rssi_n ? rssi_sum / rssi_n : 0;
    const char *lcol = swarm_loss < 5 ? COL_ALIVE : (swarm_loss < 20 ? COL_WARN : COL_LOST);

    /* ---- the OTA window owns the head while it is open ---- */
    char otaw_ip[16];
    int otaw_left = 0, otaw_pct = 0;
    ota_window_state_t ow = ota_window_status(otaw_ip, &otaw_left, &otaw_pct);
    led_ota(ow != OTAW_IDLE && ow != OTAW_NOCREDS);
    if (ow != OTAW_IDLE) {
        char hb[320];
        int hq = 0;
        hq += snprintf(hb + hq, sizeof(hb) - hq, "#%s OTA WINDOW#\n", COL_WARN);
        switch (ow) {
        case OTAW_NOCREDS:
            hq += snprintf(hb + hq, sizeof(hb) - hq, "#%s no wifi creds#\n#%s in secrets.h#", COL_LOST, COL_LOST);
            break;
        case OTAW_JOINING:
            hq += snprintf(hb + hq, sizeof(hb) - hq, "joining wifi...\n#%s %ds left#", COL_DIM, otaw_left);
            break;
        case OTAW_RECEIVING:
            hq += snprintf(hb + hq, sizeof(hb) - hq, "#%s receiving %d%%#\n%s", COL_ALIVE, otaw_pct, otaw_ip);
            break;
        case OTAW_FAILED:
            hq += snprintf(hb + hq, sizeof(hb) - hq, "#%s wifi join FAILED#", COL_LOST);
            break;
        default:
            hq += snprintf(hb + hq, sizeof(hb) - hq, "%s\n:8443 #%s %ds#\n#%s tap = cancel#",
                           otaw_ip, COL_DIM, otaw_left, COL_DIM);
        }
        lvgl_port_lock(0);
        lv_label_set_text(s_head, hb);
        lvgl_port_unlock();
    }

    /* ---- head (big): identity + address + swarm loss ---- */
    char h[320];
    int hp = 0;
    hp += snprintf(h + hp, sizeof(h) - hp, "#%s IVAN BOHUN#\n", COL_LIME);
    hp += snprintf(h + hp, sizeof(h) - hp, "#%s Pysar#\n", COL_DIM);
    hp += snprintf(h + hp, sizeof(h) - hp, "#%s %s#", COL_LIME, s_ver_line);
    char lossln[48];
    snprintf(lossln, sizeof(lossln), "Loss ESP-NOW: #%s %d%%#", lcol, swarm_loss);
    if (ow == OTAW_IDLE) {
        lvgl_port_lock(0);
        lv_label_set_text(s_head, h);
        lv_label_set_text(s_loss, lossln);
        lvgl_port_unlock();
    }

    /* ---- rows (small): vMAC + the aligned per-node table (+ OTA banner) ----
     * columns: dot | name(12) | rssi(4) | SERVED(5) | uptime(3)
     * The delivery%% column became SERVED in the balancer era: radio delivery
     * was pinned at 100 and said nothing, while per-blade 2xx counts are the
     * work itself. Name colour carries role (lime otaman / white kozak / grey
     * witness); an amber name is a benched kozak - the balancer is skipping it. */
    /* HARD LINE BUDGET: y=78 to the footer at y=178 is exactly nine 11 px
     * lines. The RELEASE banner line exists only while a release is landing
     * (the vMAC that once sat here retired to make room for the version in
     * the head); rows are clamped so a seventh member cannot overflow. */
    #define ROSTER_MAX_ROWS 6
    char r[1024];
    int rp = 0;
    rp += snprintf(r + rp, sizeof(r) - rp, "#%s %s#\n", COL_DIM, DIVIDER);
    if (updating > 0)
        rp += snprintf(r + rp, sizeof(r) - rp, "#%s ! RELEASE ONGOING %d%%#\n", COL_WARN, upd_pct);
    /* The line budget is real (doc: 4 lines of foot + 6 of roster fills the
     * 2.8"), but silently dropping member 7 while the foot still counts it in
     * "N/N up" made the glass lie by omission. Show the count instead. */
    int shown = n > ROSTER_MAX_ROWS ? ROSTER_MAX_ROWS : n;
    int hidden = n - shown;
    swarm_lb_view_t lb;
    swarm_lb_view(&lb);
    for (int i = 0; i < shown && rp < (int)sizeof(r) - 90; i++) {
        const swarm_snap_entry_t *m = &e[i];
        char up[8];
        fmt_uptime(up, sizeof(up), m->uptime_s);
        const char *dot  = m->alive ? COL_ALIVE : COL_LOST;
        const char *nm   = m->shortname ? m->shortname : m->name;
        bool benched = m->eligible && m->id != leader_id && lb.valid
                       && ((lb.benched_mask >> (m->id & 7)) & 1);
        const char *ncol = (m->id == leader_id) ? COL_LIME
                         : benched ? COL_WARN
                         : (m->eligible ? "ffffff" : COL_DIM);
        /* served count, compact to <=5 chars so the column holds. The k form
         * is clamped to 99999k so the compiler can prove it fits the buffer. */
        char sv[8];
        if (!m->eligible) {
            snprintf(sv, sizeof(sv), "--");
        } else if (m->req_count < 100000) {
            snprintf(sv, sizeof(sv), "%u", (unsigned)m->req_count);
        } else {
            unsigned k = m->req_count / 1000;
            snprintf(sv, sizeof(sv), "%uk", k > 99999 ? 99999 : k);
        }
        if (m->self)
            rp += snprintf(r + rp, sizeof(r) - rp, "#%s o# #%s %-12s# %4s %5s %-3s\n",
                          dot, ncol, nm, "--", sv, up);
        else
            rp += snprintf(r + rp, sizeof(r) - rp, "#%s o# #%s %-12s# %4d %5s %-3s\n",
                          dot, ncol, nm, m->rssi, sv, up);
    }
    if (hidden > 0)
        rp += snprintf(r + rp, sizeof(r) - rp, "#%s   +%d more#\n", COL_DIM, hidden);
    rp += snprintf(r + rp, sizeof(r) - rp, "#%s %s#", COL_DIM, DIVIDER);
    lvgl_port_lock(0);
    lv_label_set_text(s_rows, r);
    lvgl_port_unlock();

    /* ---- foot (big): quorum + leader load + avg rssi + failover age ---- */
    char f[320];
    int fp = 0;
    fp += snprintf(f + fp, sizeof(f) - fp, "#%s %d/%d up#  gen %lu\n",
                  (alive == n) ? COL_ALIVE : COL_LOST, alive, n, (unsigned long)generation);
    {
        /* "2.1k p50 12ms" - 14 chars worst case, exactly the budget */
        char kr[8];
        fmt_kreq(kr, sizeof(kr), fleet_req);
        fp += snprintf(f + fp, sizeof(f) - fp, "%s p50 %lums\n",
                      kr, (unsigned long)(fleet_p50 > 999 ? 999 : fleet_p50));
    }
    /* availability: what fraction of everything asked for actually got served,
     * fleet-wide. Coloured, because a dip here means visitors suffered. */
    if (fleet_avail >= 0) {
        fp += snprintf(f + fp, sizeof(f) - fp, "#%s served %d.%d%%#\n",
                      fleet_avail >= 999 ? COL_ALIVE : (fleet_avail >= 950 ? COL_WARN : COL_LOST),
                      fleet_avail / 10, fleet_avail % 10);
    } else {
        fp += snprintf(f + fp, sizeof(f) - fp, "#%s served --#\n", COL_DIM);
    }
    /* EXACTLY four lines, always. The foot starts at y=178 and the rail row begins
     * at y=258 - at 18 px a line that is a hard budget of four. A fifth line put
     * text at y=268, straight through the rail. Booting moved out to its own icon
     * in the corner precisely so this block can never grow. */
    fp += snprintf(f + fp, sizeof(f) - fp, "avg rssi %d", avg_rssi);
    lvgl_port_lock(0);
    lv_label_set_text(s_foot, f);
    if (booting > 0) {
        lv_label_set_text_fmt(s_boot_lbl, "%d booting", booting);
        lv_obj_remove_flag(s_boot_shaft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_boot_sole,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_boot_lbl,   LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_boot_shaft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_boot_sole,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_boot_lbl,   LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();

    /* ---- rail row: driver status + the pixel mirror ---- */
    led_status_t st;
    led_get_status(&st);
    const char *mode = !st.up ? "DARK" : st.blackout ? "BLACKOUT" : st.dma ? "DMA" : "no-DMA";
    const char *mcol = !st.up ? COL_LOST : st.blackout ? COL_WARN : COL_ALIVE;
    char rl[96];
    snprintf(rl, sizeof(rl), "#%s RAIL#  #%s %s#  #%s 6px cap20%% io4#",
             COL_DIM, mcol, mode, COL_DIM);
    lvgl_port_lock(0);
    lv_label_set_text(s_rail, rl);
    for (int i = 0; i < 6; i++)
        lv_obj_set_style_bg_color(s_chip[i],
            (st.up && !st.blackout)
                ? lv_color_make(st.rgb[i][0], st.rgb[i][1], st.rgb[i][2])
                : lv_color_hex(0x22262e), 0);
    lvgl_port_unlock();
}
