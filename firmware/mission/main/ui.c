/* bohun mission (the kobzar, 4.3"): the mission-control dashboard.
 * 800x480, LVGL 9, the site's palette, FIVE tiles (swipe right through them):
 *   tile 0 - mission control: node rows with status + served columns, the
 *            CHRONICLE, fleet-wide traffic chart (requests + p50), quorum,
 *            fleet availability, reign records, RELEASE progress on rows.
 *   tile 1 - THE LEDGER: per-blade HTTP 200 counts above the refusal table,
 *            fleet 200-OK total in the corner.
 *   tile 2 - CONVERSIONS: the click table per blade, fleet sum, and the
 *            kobzar's own NVS archive of what wiped blades forgot.
 *   tile 3 - THE RADIO: the gossip tail, every heartbeat heard in the last
 *            45 s with rssi, seq and missed-frame markers.
 *   tile 4 - "what the world sees": the served page baked to RGB565 at build
 *            time (render_page_asset.py).
 * Footer tap blanks the backlight (CH422G on/off); any touch wakes.
 * Longest mask reign persists in NVS - the hall of fame survives reboots.
 * NOTE: LVGL's Montserrat builds carry ASCII only - no U+00B7 middle dot
 * (renders as tofu). Separators are plain dashes on glass.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_reset_reason - the kobzar states its own boot cause */
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include "vitals.h"   /* VITALS_TEMP_NONE - "this node never said" */
#include "panel.h"
#include "ui.h"
#include "ota_window.h"
#include "esp_app_desc.h"

#define C_BG     0x0a0a0c
#define C_CARD   0x111115
#define C_CARD2  0x16161c
#define C_BORDER 0x1e1e25
#define C_TEXT   0xe8e6e3
#define C_DIM    0x7a7880
#define C_MUTED  0x4a4850
#define C_LIME   0xc4f04d
#define C_ALIVE  0x43e86a
#define C_WARN   0xffb020
#define C_LOST   0xff5555

#define MAX_ROWS   8
#define ROW_Y      72
#define ROW_PITCH  44
#define ROW_H      40
#define FOOT_Y     454

typedef struct {
    lv_obj_t *card, *dot, *name, *role, *rssi_bar, *temp, *served, *status, *up;
} node_row_t;

static node_row_t s_rows[MAX_ROWS];
/* ---- the refusal ledger (tile 1) ----
 * One column per serving blade, one row per HTTP error code. Indices match
 * httpd_err_code_t exactly, so the array the heartbeat carries drops straight in. */
#define ERR_COLS  4
#define ERR_COL_W 188
/* One name per httpd error kind. Growing SERVE_ERR_KINDS without adding a name
 * here would feed NULL to %s further down; make that a compile error. */
static const char *const ERR_NAME[SERVE_ERR_KINDS] = {
    "500 server", "501 no method", "505 version", "400 bad req", "401 unauth",
    "403 refused", "404 not found", "405 bad method", "408 timeout", "411 length",
    "413 too big", "414 uri long", "431 hdrs big",
};
_Static_assert(sizeof(ERR_NAME) / sizeof(ERR_NAME[0]) == SERVE_ERR_KINDS,
               "ERR_NAME must name every httpd error kind");
static lv_obj_t *s_err_vals[ERR_COLS];   /* one multi-line label of counts per node */
static lv_obj_t *s_err_node[ERR_COLS];
static lv_obj_t *s_col_served[ERR_COLS]; /* the 200-OK line above each refusal column */
static lv_obj_t *s_ok_total;             /* fleet 200-OK, corner of the ledger tile */
/* THE CONVERSIONS tile: the admin /clicks view on glass, plus the archive.
 * The kobzar folds any count a blade LOSES (wipe, retable, replacement) into
 * an NVS-backed base, so ALL TIME outlives every blade's flash - the kobzar
 * is the fleet's long memory, same as its reign records. */
static lv_obj_t *s_conv_lbl;
static uint32_t s_conv_base[SERVE_CLICK_KINDS];
static uint16_t s_conv_last[8][SERVE_CLICK_KINDS];
static nvs_handle_t s_conv_nvs;
static lv_obj_t *s_avail_lbl;
static lv_obj_t *s_radio_sum, *s_radio_lbl;   /* tile 2: the gossip tail */

static lv_obj_t *s_tile0, *s_release, *s_badge, *s_badge_lbl, *s_chart, *s_quorum, *s_leader, *s_gen,
                *s_stable, *s_record_lbl, *s_arc_lbl, *s_temp_lbl, *s_req_now, *s_lat_now;
static lv_obj_t *s_chron_card, *s_chron_lbl;
static lv_obj_t *s_chron_modal, *s_chron_modal_lbl;   /* tap the card -> the full book */
static lv_obj_t *s_modal, *s_modal_title, *s_modal_body, *s_modal_chart, *s_blank;
static lv_chart_series_t *s_ser_req, *s_ser_lat, *s_ser_mrssi, *s_ser_mlat;
static swarm_snap_entry_t s_snap[MAX_ROWS];
static int s_snap_n;
static uint8_t s_leader_cache;
static uint32_t s_last_req;
static bool s_have_last_req;

/* the baked page (little-endian RGB565, render_page_asset.py), mmapped from the
 * dedicated "page" flash partition - kept out of rodata so XIP-to-PSRAM stays cheap */
#include "page_strip.h"
#include "esp_partition.h"
static lv_image_dsc_t s_page_dsc;

/* the arms of the Zaporozhian Host, baked small (render_arms_asset.py) */
extern const uint8_t arms_bin_start[] asm("_binary_arms_84x104_bin_start");
static lv_image_dsc_t s_arms_dsc;

/* ---------- per-node history for the modal sparkline ---------- */

#define ID_MAX  16
#define HIST_N  60
static int8_t   s_hist_rssi[ID_MAX][HIST_N];
static uint16_t s_hist_lat[ID_MAX][HIST_N];
static uint16_t s_hist_pos;
static uint16_t s_hist_fill;

/* ---------- the chronicle: ring of recolored event lines ---------- */

/* 40, not 10. The card still shows five - but the whole point of the tap-through
 * modal is to be able to look BACK, and a ten-deep ring is emptied by a single
 * noisy minute. 40 x 96 = 3.8 KB of static, which the kobzar has. */
#define CHRON_N   40
#define CHRON_LEN 96
static char s_chron[CHRON_N][CHRON_LEN];
static int s_chron_head;
static int s_chron_count;

static bool s_seen[ID_MAX], s_was_alive[ID_MAX], s_was_updating[ID_MAX];
static uint32_t s_last_up[ID_MAX];    /* to catch a reboot: uptime running backwards */
static uint8_t s_heap_tier[ID_MAX];   /* 0 = healthy, 1 = amber, 2 = red - latched */
static uint8_t s_prev_leader;

/* Heap tiers, chosen from what a connection actually costs rather than a round
 * number: one TLS session pins ~20.5 KB of record buffers, so below 32 KB a
 * handshake has nowhere to land and the node is already failing, it just has not
 * noticed. 64 KB is the "two connections from the edge" warning. */
#define HEAP_RED_B    (32 * 1024)
#define HEAP_AMBER_B  (64 * 1024)

/* reign records (persisted) */
static int64_t s_reign_start_us;
static uint32_t s_record_s;
static char s_record_holder[12];

static void chron_stamp(char *out, size_t n)
{
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s < 3600) snprintf(out, n, "+%lum%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else          snprintf(out, n, "+%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
}

static bool is_hex6(const char *s)
{
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void chron_push(const char *fmt, ...)
{
    char stamp[12];
    chron_stamp(stamp, sizeof(stamp));
    char *slot = s_chron[s_chron_head];
    int p = snprintf(slot, CHRON_LEN, "#4a4850 %s#  ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot + p, CHRON_LEN - p, fmt, ap);
    va_end(ap);
    /* UPPERCASE the whole line for glanceability - but step
     * over recolor codes: "#c4f04d" must stay lowercase because LVGL's recolor
     * parser is not guaranteed case-tolerant, and a broken code renders as
     * literal text. A '#' followed by 6 hex chars is a code; any other '#' is
     * a terminator and needs no special care. */
    for (char *c = slot; *c; c++) {
        if (*c == '#' && is_hex6(c + 1)) { c += 6; continue; }
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    s_chron_head = (s_chron_head + 1) % CHRON_N;
    if (s_chron_count < CHRON_N) s_chron_count++;
}

/* ---------- helpers ---------- */

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* Abbreviate from the ledger's own `shortname`, never a strcmp on a literal
 * name - renaming a member in the ledger must not silently break the
 * abbreviation. */
static const char *short_name(const swarm_snap_entry_t *m)
{
    return (m->shortname && m->shortname[0]) ? m->shortname : m->name;
}

/* served counts fit a 52 px column: 999 plain, thousands as 3.7k, then k / M */
static void fmt_count(char *out, size_t n, uint32_t v)
{
    if (v < 1000)         snprintf(out, n, "%lu", (unsigned long)v);
    else if (v < 10000)   snprintf(out, n, "%lu.%luk", (unsigned long)(v / 1000),
                                   (unsigned long)((v / 100) % 10));
    else if (v < 1000000) snprintf(out, n, "%luk", (unsigned long)(v / 1000));
    else                  snprintf(out, n, "%lu.%luM", (unsigned long)(v / 1000000),
                                   (unsigned long)((v / 100000) % 10));
}

static void fmt_uptime(char *out, size_t n, uint32_t s)
{
    if (s < 60)         snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 3600)  snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
    else                snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}

/* ---------- reign records: NVS-backed hall of fame ---------- */

static void record_load(void)
{
    nvs_handle_t h;
    if (nvs_open("rada", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "reign_s", &s_record_s);
    size_t len = sizeof(s_record_holder);
    nvs_get_str(h, "holder", s_record_holder, &len);
    nvs_close(h);
}

static void record_save(void)
{
    nvs_handle_t h;
    if (nvs_open("rada", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "reign_s", s_record_s);
    nvs_set_str(h, "holder", s_record_holder);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- touch: node detail overlay ---------- */

static void modal_close_cb(lv_event_t *ev)
{
    (void)ev;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void chron_modal_close_cb(lv_event_t *ev)
{
    (void)ev;
    lv_obj_add_flag(s_chron_modal, LV_OBJ_FLAG_HIDDEN);
}

/* Tap the chronicle card -> the whole ring, newest first, in a scrollable page.
 * The card itself only ever shows the last five lines; the ring holds 40. */
static void chron_card_click_cb(lv_event_t *ev)
{
    (void)ev;
    static char full[CHRON_N * CHRON_LEN];   /* 3.8 KB - static, never on a task stack */
    int p = 0;
    for (int i = 0; i < s_chron_count; i++) {
        /* snprintf returns what it WOULD have written, so p can run past the
         * buffer and `sizeof(full) - p` then underflows (size_t) into a huge
         * length. Today the maximum content is exactly sizeof(full) - safe by
         * zero bytes. One clamp buys permanent safety. */
        if (p >= (int)sizeof(full) - 1) {
            break;
        }
        int idx = (s_chron_head - 1 - i + CHRON_N * 2) % CHRON_N;
        p += snprintf(full + p, sizeof(full) - p, "%s%s", i ? "\n" : "", s_chron[idx]);
    }
    if (p > (int)sizeof(full) - 1) {
        p = (int)sizeof(full) - 1;
    }
    lv_label_set_text(s_chron_modal_lbl, p ? full : "-");
    lv_obj_scroll_to_y(lv_obj_get_parent(s_chron_modal_lbl), 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_chron_modal, LV_OBJ_FLAG_HIDDEN);
}

static void row_click_cb(lv_event_t *ev)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(ev);
    if (idx >= s_snap_n) return;
    const swarm_snap_entry_t *m = &s_snap[idx];

    char ip[20] = "-";
    if (m->ip) {
        ip4_addr_t a = { .addr = m->ip };
        snprintf(ip, sizeof(ip), "%s", ip4addr_ntoa(&a));
    }
    char up[12];
    fmt_uptime(up, sizeof(up), m->uptime_s);
    char rq[12];
    fmt_count(rq, sizeof(rq), m->req_count);
    /* fw version travels in the heartbeat since v15; a peer still on an older
     * image says nothing, and "-" is the honest rendering of that */
    char fv[8];
    if (m->fw_ver) snprintf(fv, sizeof(fv), "v%u", m->fw_ver);
    else           snprintf(fv, sizeof(fv), "v-");

    lv_label_set_text_fmt(s_modal_title, "%s  #%s %s#",
                          short_name(m),
                          (m->id == s_leader_cache) ? "c4f04d" : "7a7880", m->role);
    /* die temperature (heartbeat v9). The sentinel is not a reading: a node on
     * an older image, or one whose sensor would not install, says nothing - and
     * "-" is the honest rendering of that, never a plausible 0.0. */
    char temp[16];
    if (m->temp_c10 == VITALS_TEMP_NONE) {
        snprintf(temp, sizeof(temp), "-");
    } else {
        int t10 = m->temp_c10;
        snprintf(temp, sizeof(temp), "%d.%d C", t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
    }
    lv_label_set_text_fmt(s_modal_body,
        "state    %s%s\n"
        "ip       %s      up %s\n"
        "rssi     %d dBm   dlv %d%%\n"
        "temperature  %s\n"
        "fw %s   req %s\n"
        "p50 %lu ms   p95 %lu ms",
        m->alive ? "alive" : "lost", m->updating ? " - UPDATING" : "",
        ip, up, (int)m->rssi, 100 - m->loss_pct, temp,
        fv, rq,
        (unsigned long)(m->p50_us / 1000), (unsigned long)(m->p95_us / 1000));

    /* fill the sparkline from this node's history ring (oldest -> newest) */
    int fill = s_hist_fill;
    lv_chart_set_point_count(s_modal_chart, HIST_N);
    lv_chart_set_all_value(s_modal_chart, s_ser_mrssi, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_modal_chart, s_ser_mlat, LV_CHART_POINT_NONE);
    for (int i = 0; i < fill; i++) {
        int slot = (s_hist_pos - fill + i + 2 * HIST_N) % HIST_N;
        lv_chart_set_next_value(s_modal_chart, s_ser_mrssi,
                                m->self ? LV_CHART_POINT_NONE : s_hist_rssi[m->id][slot]);
        lv_chart_set_next_value(s_modal_chart, s_ser_mlat, s_hist_lat[m->id][slot]);
    }
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- touch: blank / wake / the OTA hold ---------- */

/* The flag + arms are the OTA trigger: hold either for a full 4 s (far past
 * any accidental tap) and the window opens. Neither has a tap action, so the
 * release that ends the hold falls on deaf ears - nothing to swallow. */
static lv_obj_t *s_ota_modal, *s_ota_body;
static int64_t   s_hold_press_us = -1;

static void ota_hold_press_cb(lv_event_t *ev)
{
    lv_event_code_t c = lv_event_get_code(ev);
    if (c == LV_EVENT_PRESSED) {
        s_hold_press_us = esp_timer_get_time();
    } else {   /* RELEASED or PRESS_LOST */
        s_hold_press_us = -1;
    }
}

/* 500 ms LVGL timer: watches the hold, then narrates the open window */
static void ota_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hold_press_us > 0 &&
        esp_timer_get_time() - s_hold_press_us >= 4000000LL) {
        s_hold_press_us = -1;
        ota_window_open();
        lv_obj_remove_flag(s_ota_modal, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_has_flag(s_ota_modal, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    char ip[16];
    int left = 0, pct = 0;
    ota_window_state_t st = ota_window_status(ip, &left, &pct);
    char b[192];
    switch (st) {
    case OTAW_NOCREDS:
        snprintf(b, sizeof(b), "REFUSED\n\nno wifi credentials\nin secrets.h");
        break;
    case OTAW_JOINING:
        snprintf(b, sizeof(b), "joining wifi...\n\nwindow closes in %d s", left);
        break;
    case OTAW_READY:
        snprintf(b, sizeof(b), "OPEN  https://%s:8443\n\n/ota   app image\n/page  the strip\n\ncloses in %d s", ip, left);
        break;
    case OTAW_RECEIVING:
        snprintf(b, sizeof(b), "RECEIVING  %d%%\n\n%s\nreboots when complete", pct, ip);
        break;
    case OTAW_FAILED:
        snprintf(b, sizeof(b), "WIFI JOIN FAILED\n\ncheck secrets.h");
        break;
    default:   /* window expired or was cancelled */
        lv_obj_add_flag(s_ota_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_ota_body, b);
}

static void ota_modal_close_cb(lv_event_t *ev)
{
    (void)ev;
    ota_window_close();
    lv_obj_add_flag(s_ota_modal, LV_OBJ_FLAG_HIDDEN);
}

static void blank_cb(lv_event_t *ev)
{
    (void)ev;
    panel_backlight(false);
    lv_obj_remove_flag(s_blank, LV_OBJ_FLAG_HIDDEN);
}

static void wake_cb(lv_event_t *ev)
{
    (void)ev;
    panel_backlight(true);
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- build ---------- */

void ui_init(lv_display_t *disp)
{
    (void)disp;
    record_load();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ---- three tiles: mission control | what we refused | the served page ---- */
    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, 800, 480);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    s_tile0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *tile_err   = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *tile_conv  = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *tile_radio = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *tile1      = lv_tileview_add_tile(tv, 4, 0, LV_DIR_LEFT);

    /* ---- tile 1: the refusal ledger --------------------------------------
     * Availability says how much we turned away; this says WHAT. Deliberately
     * LAN-only - it never appears on the served page, because the shape of your
     * 404s is a map of what people are probing you for. */
    mk_label(tile_err, 16, 12, &lv_font_montserrat_28, C_LIME, "THE LEDGER");
    mk_label(tile_err, 16, 46, &lv_font_montserrat_14, C_DIM,
             "what each blade served, and every non-2xx it answered - kept off the public page");
    /* the number the whole shelf exists to grow: HTTP 200s, fleet total */
    s_ok_total = mk_label(tile_err, 500, 8, &lv_font_montserrat_28, C_ALIVE, "-");
    lv_obj_set_width(s_ok_total, 284);
    lv_obj_set_style_text_align(s_ok_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *okcap = mk_label(tile_err, 500, 42, &lv_font_montserrat_14, C_DIM,
                               "HTTP 200 OK - fleet total");
    lv_obj_set_width(okcap, 284);
    lv_obj_set_style_text_align(okcap, LV_TEXT_ALIGN_RIGHT, 0);
    {
        /* Three labels per column, not one per cell. The first cut created 108
         * labels in a row and starved IDLE0 long enough to trip the task watchdog
         * during ui_init - LVGL object construction is not free. The names are a
         * single static multi-line label; only the counts label is rewritten. */
        char names[256];
        int np = 0;
        for (int k = 0; k < SERVE_ERR_KINDS; k++) {
            np += snprintf(names + np, sizeof(names) - np, "%s%s",
                           k ? "\n" : "", ERR_NAME[k]);
        }
        int x = 16;
        for (int c = 0; c < ERR_COLS; c++) {
            lv_obj_t *card = mk_card(tile_err, x, 76, ERR_COL_W, 356);
            mk_label(card, 10, 8, &lv_font_montserrat_16, C_TEXT,
                     c == 0 ? "N1" : c == 1 ? "N2" : c == 2 ? "N3" : "N4");
            s_col_served[c] = mk_label(card, 60, 10, &lv_font_montserrat_14, C_ALIVE, "-");
            s_err_node[c] = mk_label(card, 10, 32, &lv_font_montserrat_14, C_MUTED, "-");
            lv_obj_t *nm = mk_label(card, 10, 58, &lv_font_montserrat_14, C_DIM, names);
            lv_obj_set_style_text_line_space(nm, 6, 0);
            s_err_vals[c] = mk_label(card, ERR_COL_W - 62, 58, &lv_font_montserrat_14, C_MUTED, "");
            lv_obj_set_width(s_err_vals[c], 50);
            lv_obj_set_style_text_line_space(s_err_vals[c], 6, 0);
            lv_obj_set_style_text_align(s_err_vals[c], LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_recolor(s_err_vals[c], true);
            x += ERR_COL_W + 8;
            vTaskDelay(1);   /* let IDLE breathe between columns */
        }
    }
    mk_label(tile_err, 16, 444, &lv_font_montserrat_14, C_MUTED,
             "swipe right for the radio, then the page - left for mission control");

    /* ---- tile 2: THE RADIO - the gossip tail ---------------------------- */
    mk_label(tile_radio, 16, 12, &lv_font_montserrat_28, C_LIME, "THE RADIO");
    mk_label(tile_radio, 16, 46, &lv_font_montserrat_14, C_DIM,
             "every heartbeat heard - esp-now, encrypted, 1 Hz per member");
    s_radio_sum = mk_label(tile_radio, 400, 22, &lv_font_montserrat_14, C_DIM, "-");
    lv_obj_set_width(s_radio_sum, 384);
    lv_obj_set_style_text_align(s_radio_sum, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *rcard = mk_card(tile_radio, 12, 72, 776, 364);
    s_radio_lbl = mk_label(rcard, 14, 10, &lv_font_unscii_16, C_DIM, "listening...");
    lv_label_set_recolor(s_radio_lbl, true);
    lv_obj_set_style_text_line_space(s_radio_lbl, 2, 0);
    lv_label_set_long_mode(s_radio_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_radio_lbl, 748);
    mk_label(tile_radio, 16, 444, &lv_font_montserrat_14, C_MUTED,
             "swipe right for the page the world sees - left for the ledger");

    /* tile 1: the whole served page, vertically scrollable (mmapped from flash) */
    const void *page_px = NULL;
    const esp_partition_t *pp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "page");
    if (pp) {
        esp_partition_mmap_handle_t mh;
        if (esp_partition_mmap(pp, 0, (size_t)PAGE_STRIP_W * PAGE_STRIP_H * 2,
                               ESP_PARTITION_MMAP_DATA, &page_px, &mh) != ESP_OK) {
            page_px = NULL;
        }
    }
    s_page_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_page_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_page_dsc.header.w = PAGE_STRIP_W;
    s_page_dsc.header.h = PAGE_STRIP_H;
    s_page_dsc.header.stride = PAGE_STRIP_W * 2;
    s_page_dsc.data_size = (uint32_t)PAGE_STRIP_W * PAGE_STRIP_H * 2;
    s_page_dsc.data = page_px;
    /* The image is ~7200 px tall. Parented straight to the tile, it made the TILE
     * that tall, which made the TILEVIEW that tall - so the tileview's own scroll
     * and the tile's fought each other and the render came apart mid-swipe. Give
     * it a viewport instead: a container fixed at panel size that scrolls its own
     * oversized child, with chaining off so a vertical drag never leaks out into
     * the horizontal tile navigation. */
    lv_obj_set_scroll_dir(tile1, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tile1, LV_SCROLLBAR_MODE_OFF);
    if (page_px) {
        lv_obj_t *vp = lv_obj_create(tile1);
        lv_obj_remove_style_all(vp);
        lv_obj_set_pos(vp, 0, 0);
        lv_obj_set_size(vp, 800, 480);
        lv_obj_set_style_bg_color(vp, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(vp, LV_OPA_COVER, 0);
        lv_obj_set_scroll_dir(vp, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(vp, LV_SCROLLBAR_MODE_ACTIVE);
        /* Vertical chaining OFF so a drag on the page never leaks into tile
         * navigation. Horizontal chaining stays ON - without it a sideways swipe
         * inside the viewport has nowhere to go and you are stranded on this tile. */
        lv_obj_remove_flag(vp, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_flag(vp, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        /* momentum on: a flick should carry, or 7000 px of page is a long drag */
        lv_obj_add_flag(vp, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_remove_flag(vp, LV_OBJ_FLAG_SCROLL_ELASTIC);

        lv_obj_t *img = lv_image_create(vp);
        lv_image_set_src(img, &s_page_dsc);
        lv_obj_set_pos(img, 0, 0);
        mk_label(vp, 12, PAGE_STRIP_H + 8, &lv_font_montserrat_14, C_MUTED,
                 "what the world sees - swipe left to return");
    } else {
        mk_label(tile1, 260, 220, &lv_font_montserrat_16, C_DIM,
                 "page partition is empty - run idf.py flash");
    }

    /* ---- tile 3 in swipe order: the conversions ledger ---- */
    mk_label(tile_conv, 16, 12, &lv_font_montserrat_28, C_LIME, "CONVERSIONS");
    mk_label(tile_conv, 16, 46, &lv_font_montserrat_14, C_DIM,
             "who clicked to give - per blade, fleet sum, and the kobzar's own archive of what wiped blades forgot");
    {
        nvs_handle_t h;
        if (nvs_open("clicks", NVS_READWRITE, &h) == ESP_OK) {
            s_conv_nvs = h;
            char key[4] = "c0";
            for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
                key[1] = '0' + k;
                nvs_get_u32(h, key, &s_conv_base[k]);
            }
        }
    }
    s_conv_lbl = mk_label(tile_conv, 8, 92, &lv_font_unscii_16, 0xffffff, "listening...");
    lv_label_set_recolor(s_conv_lbl, true);
    lv_obj_set_width(s_conv_lbl, 784);
    lv_obj_set_style_text_line_space(s_conv_lbl, 10, 0);

    /* ---- tile 0: header ---- */
    mk_label(s_tile0, 16, 10, &lv_font_montserrat_28, C_LIME, "IVAN BOHUN");
    {
        static char sub[64];   /* app-desc version is a 32-byte field */
        snprintf(sub, sizeof(sub), "Kobzar - Mission Control - v%s",
                 esp_app_get_description()->version);
        mk_label(s_tile0, 16, 42, &lv_font_montserrat_16, C_DIM, sub);
    }
    /* ip + vMAC live on the 2.8" pysar; this header carries the VERDICT instead */

    /* the flag: canonical 2:3, blue over gold, right of the name */
    lv_obj_t *flag = lv_obj_create(s_tile0);
    lv_obj_set_pos(flag, 316, 8);   /* clear of the longer kobzar subtitle */
    lv_obj_set_size(flag, 90, 60);   /* 2:3 canonical, sized to fill the header band */
    lv_obj_set_style_border_color(flag, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(flag, 1, 0);
    lv_obj_set_style_radius(flag, 2, 0);
    lv_obj_set_style_pad_all(flag, 0, 0);
    lv_obj_set_style_bg_color(flag, lv_color_hex(0x0057b7), 0);   /* blue field */
    lv_obj_remove_flag(flag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fy = lv_obj_create(flag);
    lv_obj_set_pos(fy, 0, 29);
    lv_obj_set_size(fy, 88, 29);
    lv_obj_set_style_bg_color(fy, lv_color_hex(0xffd700), 0);     /* gold half */
    lv_obj_set_style_border_width(fy, 0, 0);
    lv_obj_set_style_radius(fy, 0, 0);
    lv_obj_remove_flag(fy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fy, LV_OBJ_FLAG_CLICKABLE);   /* presses fall through to the flag */
    /* the flag is the OTA hold surface - the footer strip is too thin a
     * target for a 4 s press at the panel's edge */
    lv_obj_add_event_cb(flag, ota_hold_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(flag, ota_hold_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(flag, ota_hold_press_cb, LV_EVENT_PRESS_LOST, NULL);

    /* the arms of the Host fill the gap between the table and the verdict */
    s_arms_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_arms_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_arms_dsc.header.w = 84;
    s_arms_dsc.header.h = 104;
    s_arms_dsc.header.stride = 84 * 2;
    s_arms_dsc.data_size = 84 * 104 * 2;
    s_arms_dsc.data = arms_bin_start;
    lv_obj_t *arms = lv_image_create(s_tile0);
    lv_image_set_src(arms, &s_arms_dsc);
    lv_obj_set_pos(arms, 534, 6);
    lv_obj_add_flag(arms, LV_OBJ_FLAG_CLICKABLE);   /* images are inert by default */
    lv_obj_add_event_cb(arms, ota_hold_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(arms, ota_hold_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(arms, ota_hold_press_cb, LV_EVENT_PRESS_LOST, NULL);

    s_badge = lv_obj_create(s_tile0);
    lv_obj_set_pos(s_badge, 644, 8);
    lv_obj_set_size(s_badge, 144, 38);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x0d2012), 0);
    lv_obj_set_style_border_color(s_badge, lv_color_hex(C_ALIVE), 0);
    lv_obj_set_style_border_width(s_badge, 1, 0);
    lv_obj_set_style_radius(s_badge, 4, 0);
    lv_obj_set_style_pad_all(s_badge, 0, 0);
    lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_SCROLLABLE);
    s_badge_lbl = mk_label(s_badge, 0, 0, &lv_font_montserrat_20, C_ALIVE, "HEALTHY");
    lv_obj_center(s_badge_lbl);

    s_release = lv_obj_create(s_tile0);
    lv_obj_set_pos(s_release, 644, 52);
    lv_obj_set_size(s_release, 144, 58);
    lv_obj_set_style_bg_color(s_release, lv_color_hex(0x2a1f06), 0);
    lv_obj_set_style_border_color(s_release, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_border_width(s_release, 1, 0);
    lv_obj_set_style_radius(s_release, 4, 0);
    lv_obj_set_style_pad_all(s_release, 0, 0);
    lv_obj_remove_flag(s_release, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rl = mk_label(s_release, 0, 0, &lv_font_montserrat_20, C_WARN, "RELEASE\nONGOING");
    lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(rl);
    lv_obj_add_flag(s_release, LV_OBJ_FLAG_HIDDEN);

    /* ---- tile 0: node rows ---- */
    for (int i = 0; i < MAX_ROWS; i++) {
        node_row_t *r = &s_rows[i];
        r->card = mk_card(s_tile0, 12, ROW_Y + i * ROW_PITCH, 492, ROW_H);
        lv_obj_add_flag(r->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->card, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        r->dot = lv_obj_create(r->card);
        lv_obj_set_pos(r->dot, 14, 14);
        lv_obj_set_size(r->dot, 12, 12);
        lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(r->dot, 0, 0);
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(C_ALIVE), 0);

        r->name = mk_label(r->card, 40, 8, &lv_font_montserrat_20, C_TEXT, "-");
        r->role = mk_label(r->card, 132, 12, &lv_font_montserrat_14, C_DIM, "");

        /* columns, left to right: dot - name - role - rssi bar - temp -
         * served - status - uptime. The rssi NUMBER moved into the tap modal;
         * the bar carries the signal at a glance and the freed width pays for
         * the served and status columns. */
        r->rssi_bar = lv_bar_create(r->card);
        lv_obj_set_pos(r->rssi_bar, 222, 16);
        lv_obj_set_size(r->rssi_bar, 44, 8);
        lv_bar_set_range(r->rssi_bar, 0, 100);
        lv_obj_set_style_bg_color(r->rssi_bar, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_bg_color(r->rssi_bar, lv_color_hex(C_ALIVE), LV_PART_INDICATOR);
        lv_obj_set_style_radius(r->rssi_bar, 2, 0);
        lv_obj_set_style_radius(r->rssi_bar, 2, LV_PART_INDICATOR);

        r->temp   = mk_label(r->card, 274, 12, &lv_font_montserrat_14, C_MUTED, "");
        r->served = mk_label(r->card, 318, 12, &lv_font_montserrat_14, C_DIM, "");
        r->status = mk_label(r->card, 372, 12, &lv_font_montserrat_14, C_DIM, "");
        r->up     = mk_label(r->card, 448, 12, &lv_font_montserrat_14, C_MUTED, "");
        lv_obj_add_flag(r->card, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- tile 0: the chronicle (repositioned per fleet size) ---- */
    s_chron_card = mk_card(s_tile0, 12, ROW_Y + 6 * ROW_PITCH + 4, 492, 110);
    /* the card is a doorway: tap it and the scribe opens the whole book */
    lv_obj_add_flag(s_chron_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chron_card, chron_card_click_cb, LV_EVENT_CLICKED, NULL);
    mk_label(s_chron_card, 12, 6, &lv_font_montserrat_14, C_MUTED, "CHRONICLE - tap for all");
    s_chron_lbl = mk_label(s_chron_card, 12, 26, &lv_font_montserrat_14, C_DIM, "-");
    lv_label_set_recolor(s_chron_lbl, true);
    lv_obj_set_width(s_chron_lbl, 468);
    lv_label_set_long_mode(s_chron_lbl, LV_LABEL_LONG_CLIP);

    /* ---- tile 0: traffic chart ---- */
    lv_obj_t *tc = mk_card(s_tile0, 512, 116, 276, 160);
    mk_label(tc, 12, 8, &lv_font_montserrat_14, C_MUTED, "TRAFFIC  req/s + ms");
    s_req_now = mk_label(tc, 190, 8, &lv_font_montserrat_14, C_LIME, "");
    s_lat_now = mk_label(tc, 234, 8, &lv_font_montserrat_14, C_ALIVE, "");
    s_chart = lv_chart_create(tc);
    lv_obj_set_pos(s_chart, 8, 28);
    lv_obj_set_size(s_chart, 258, 124);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 60);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 20);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 50);
    lv_chart_set_div_line_count(s_chart, 4, 6);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
    s_ser_req = lv_chart_add_series(s_chart, lv_color_hex(C_LIME), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_lat = lv_chart_add_series(s_chart, lv_color_hex(C_ALIVE), LV_CHART_AXIS_SECONDARY_Y);

    /* ---- tile 0: swarm health ---- */
    lv_obj_t *sc = mk_card(s_tile0, 512, 284, 276, 150);
    mk_label(sc, 12, 8, &lv_font_montserrat_14, C_MUTED, "SWARM");
    s_quorum = mk_label(sc, 12, 26, &lv_font_montserrat_28, C_TEXT, "-/-");
    s_leader = mk_label(sc, 12, 62, &lv_font_montserrat_16, C_LIME, "-");
    s_gen    = mk_label(sc, 12, 86, &lv_font_montserrat_16, C_DIM, "gen -");
    s_stable = mk_label(sc, 12, 108, &lv_font_montserrat_16, C_MUTED, "reign -");
    s_record_lbl = mk_label(sc, 12, 130, &lv_font_montserrat_14, C_MUTED, "");

    /* Availability, not a radio-delivery ring: delivery is per-peer and
     * already shown in every node row, while availability is the number that
     * actually moves when visitors are turned away. */
    mk_label(sc, 150, 8, &lv_font_montserrat_14, C_MUTED, "AVAILABILITY");
    s_avail_lbl = mk_label(sc, 150, 26, &lv_font_montserrat_28, C_TEXT, "-");
    s_arc_lbl = mk_label(sc, 150, 62, &lv_font_montserrat_16, C_DIM, "delivery -");
    mk_label(sc, 150, 88, &lv_font_montserrat_14, C_MUTED, "TEMPERATURE");
    s_temp_lbl = mk_label(sc, 150, 104, &lv_font_montserrat_28, C_TEXT, "-");

    /* ---- tile 0: footer = the blank switch ---- */
    lv_obj_t *foot = mk_label(s_tile0, 12, FOOT_Y + 4, &lv_font_montserrat_14, C_MUTED,
                              "tap - screen off    hold the flag 4s - ota    swipe left - ledger, clicks, radio, the page    " __DATE__);
    lv_obj_set_width(foot, 776);
    lv_obj_add_flag(foot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(foot, 10);
    lv_obj_add_event_cb(foot, blank_cb, LV_EVENT_CLICKED, NULL);

    /* ---- top layer: node detail overlay ---- */
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_size(s_modal, 800, 480);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, modal_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mc = mk_card(s_modal, 160, 64, 480, 342);
    lv_obj_set_style_bg_color(mc, lv_color_hex(C_CARD2), 0);
    s_modal_title = mk_label(mc, 20, 14, &lv_font_montserrat_20, C_TEXT, "-");
    lv_label_set_recolor(s_modal_title, true);
    s_modal_body = mk_label(mc, 20, 48, &lv_font_unscii_16, C_DIM, "-");
    mk_label(mc, 20, 150, &lv_font_montserrat_14, C_MUTED, "last 60 s - rssi + latency");
    s_modal_chart = lv_chart_create(mc);
    lv_obj_set_pos(s_modal_chart, 16, 170);
    lv_obj_set_size(s_modal_chart, 448, 130);
    lv_chart_set_type(s_modal_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_modal_chart, HIST_N);
    lv_chart_set_range(s_modal_chart, LV_CHART_AXIS_PRIMARY_Y, -90, -10);
    lv_chart_set_range(s_modal_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 50);
    lv_chart_set_div_line_count(s_modal_chart, 4, 6);
    lv_obj_set_style_bg_color(s_modal_chart, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(s_modal_chart, 0, 0);
    lv_obj_set_style_line_color(s_modal_chart, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_size(s_modal_chart, 0, 0, LV_PART_INDICATOR);
    s_ser_mrssi = lv_chart_add_series(s_modal_chart, lv_color_hex(C_ALIVE), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_mlat  = lv_chart_add_series(s_modal_chart, lv_color_hex(C_LIME), LV_CHART_AXIS_SECONDARY_Y);
    mk_label(mc, 20, 312, &lv_font_montserrat_14, C_MUTED, "tap anywhere to close");
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    /* ---- top layer: the chronicle modal (tap the card -> the full book) ---- */
    s_chron_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_chron_modal, 0, 0);
    lv_obj_set_size(s_chron_modal, 800, 480);
    lv_obj_set_style_bg_color(s_chron_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_chron_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_chron_modal, 0, 0);
    lv_obj_remove_flag(s_chron_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chron_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chron_modal, chron_modal_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cc = mk_card(s_chron_modal, 80, 32, 640, 416);
    mk_label(cc, 20, 12, &lv_font_montserrat_20, C_TEXT, "CHRONICLE");
    mk_label(cc, 150, 17, &lv_font_montserrat_14, C_MUTED, "newest first - tap outside to close");
    /* the page: a scrolling window over one tall label. Momentum + a visible
     * scrollbar, so 40 lines feel like a page and not a well. */
    lv_obj_t *cw = lv_obj_create(cc);
    lv_obj_set_pos(cw, 12, 44);
    lv_obj_set_size(cw, 616, 360);
    lv_obj_set_style_bg_color(cw, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_border_width(cw, 0, 0);
    lv_obj_set_style_radius(cw, 3, 0);
    lv_obj_set_style_pad_all(cw, 10, 0);
    lv_obj_set_scroll_dir(cw, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cw, LV_SCROLLBAR_MODE_AUTO);
    s_chron_modal_lbl = lv_label_create(cw);
    lv_obj_set_pos(s_chron_modal_lbl, 0, 0);
    lv_obj_set_width(s_chron_modal_lbl, 590);
    lv_obj_set_style_text_font(s_chron_modal_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_chron_modal_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_line_space(s_chron_modal_lbl, 5, 0);
    lv_label_set_recolor(s_chron_modal_lbl, true);
    lv_label_set_long_mode(s_chron_modal_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_chron_modal_lbl, "-");
    lv_obj_add_flag(s_chron_modal, LV_OBJ_FLAG_HIDDEN);

    /* ---- top layer: the OTA window modal (amber = unmistakably deliberate) ---- */
    s_ota_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_ota_modal, 0, 0);
    lv_obj_set_size(s_ota_modal, 800, 480);
    lv_obj_set_style_bg_color(s_ota_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ota_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_ota_modal, 0, 0);
    lv_obj_remove_flag(s_ota_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ota_modal, ota_modal_close_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *oc = mk_card(s_ota_modal, 160, 120, 480, 240);
        lv_obj_set_style_bg_color(oc, lv_color_hex(C_CARD2), 0);
        lv_obj_set_style_border_color(oc, lv_color_hex(C_WARN), 0);
        lv_obj_set_style_border_width(oc, 2, 0);
        mk_label(oc, 20, 14, &lv_font_montserrat_20, C_WARN, "OTA WINDOW");
        s_ota_body = mk_label(oc, 20, 52, &lv_font_unscii_16, C_TEXT, "-");
        mk_label(oc, 20, 208, &lv_font_montserrat_14, C_MUTED, "tap anywhere to cancel");
    }
    lv_obj_add_flag(s_ota_modal, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(ota_poll_cb, 500, NULL);

    /* ---- top layer: the blank sheet (wake on touch) ---- */
    s_blank = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_blank, 0, 0);
    lv_obj_set_size(s_blank, 800, 480);
    lv_obj_set_style_bg_color(s_blank, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_blank, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_blank, 0, 0);
    lv_obj_remove_flag(s_blank, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_blank, wake_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();

    s_reign_start_us = esp_timer_get_time();
    /* the scribe opens the book by stating its OWN cause of death - the kobzar
     * watches everyone else, so its reboots would otherwise go unexplained */
    const char *own = swarm_reset_reason_str((uint8_t)esp_reset_reason());
    if (s_record_s) {
        char rec[16];
        fmt_uptime(rec, sizeof(rec), s_record_s);   /* was the literal "(loaded)" */
        chron_push("Kobzar watching (boot: %s) - record %s held by %s",
                   own, rec, s_record_holder[0] ? s_record_holder : "?");
    }
    else {
        chron_push("Kobzar watching (boot: %s)", own);
    }
}

/* ---------- update ---------- */

void ui_update(const swarm_snap_entry_t *e, int n, uint8_t leader_id)
{
    /* THE RINGS ARE SHARED. chron_card_click_cb and row_click_cb read s_chron
     * and s_hist_* from the LVGL task while holding the LVGL lock; everything
     * below writes them. With a full ring the modal's oldest line is exactly the
     * slot chron_push is overwriting, so a tap during a push rendered a torn
     * line. Take the lock for the whole update, not just the widget writes. */
    lvgl_port_lock(0);
    if (n > MAX_ROWS) n = MAX_ROWS;

    /* sort by id so rows read n1..n4, display, kobzar */
    swarm_snap_entry_t s[MAX_ROWS];
    memcpy(s, e, n * sizeof(s[0]));
    for (int i = 1; i < n; i++) {
        swarm_snap_entry_t k = s[i];
        int j = i - 1;
        while (j >= 0 && s[j].id > k.id) { s[j + 1] = s[j]; j--; }
        s[j + 1] = k;
    }

    uint32_t lead_req = 0, generation = 0;
    int16_t lead_temp = VITALS_TEMP_NONE;
    int alive = 0, updating = 0, loss_sum = 0, loss_n = 0;
    const char *leader_name = "-";
    /* FLEET-WIDE aggregates. Since the balancer split, the leader serves
     * nothing - every number worth graphing is a sum or average over the
     * kozaky. The leader's own counters cover /roster only. */
    uint32_t fleet_req = 0;
    uint64_t fleet_asked = 0, fleet_bad = 0;
    uint32_t p50_sum = 0, p95_sum = 0;
    int lat_n = 0;
    for (int i = 0; i < n; i++) {
        if (s[i].alive) alive++;
        if (s[i].updating) updating++;
        if (!s[i].self && s[i].alive) { loss_sum += s[i].loss_pct; loss_n++; }
        if (s[i].eligible) {
            fleet_req   += s[i].req_count;
            fleet_asked += (uint64_t)s[i].req_count + s[i].req_err;
            fleet_bad   += s[i].req_err_srv;
            if (s[i].alive && s[i].serving && s[i].id != leader_id && s[i].p50_us) {
                p50_sum += s[i].p50_us / 1000;
                p95_sum += s[i].p95_us / 1000;
                lat_n++;
            }
        }
        if (s[i].id == leader_id) {
            lead_req = s[i].req_count;
            generation = s[i].generation;
            lead_temp = s[i].temp_c10;
            leader_name = short_name(&s[i]);
        }
    }
    (void)lead_req;
    uint32_t fleet_p50 = lat_n ? p50_sum / lat_n : 0;
    uint32_t fleet_p95 = lat_n ? p95_sum / lat_n : 0;
    /* availability the way the blades themselves score it: only OUR 5xx count
     * against the fleet, summed over every serving blade rather than read off
     * the one node that no longer serves pages */
    int fleet_avail = fleet_asked
        ? (int)(((fleet_asked - fleet_bad) * 1000) / fleet_asked) : -1;
    int delivery = loss_n ? 100 - (loss_sum / loss_n) : 100;
    swarm_lb_view_t lb;
    swarm_lb_view(&lb);

    /* ---- history rings (for the modal sparkline) ---- */
    for (int i = 0; i < n; i++) {
        if (s[i].id >= ID_MAX) continue;
        s_hist_rssi[s[i].id][s_hist_pos] = s[i].self ? 0 : s[i].rssi;
        uint32_t lm = s[i].req_lat_us / 1000;
        s_hist_lat[s[i].id][s_hist_pos] = lm > 50 ? 50 : (uint16_t)lm;
    }
    s_hist_pos = (s_hist_pos + 1) % HIST_N;
    if (s_hist_fill < HIST_N) s_hist_fill++;

    /* ---- the scribe writes: transitions since the last look ---- */
    for (int i = 0; i < n; i++) {
        const swarm_snap_entry_t *m = &s[i];
        if (m->self || m->id >= ID_MAX) continue;
        const char *nm = short_name(m);
        if (!s_seen[m->id]) {
            s_seen[m->id] = true;
            s_was_alive[m->id] = m->alive;
            s_was_updating[m->id] = m->updating;
            s_last_up[m->id] = m->uptime_s;
            if (m->alive) chron_push("%s joined the roster", nm);
            continue;
        }
        if (m->alive != s_was_alive[m->id]) {
            if (m->alive) chron_push("#43e86a %s returned#", nm);
            else          chron_push("#ff5555 %s LOST#", nm);
            s_was_alive[m->id] = m->alive;
        }
        /* THE CORONER'S LINE: uptime running backwards is the unfakeable
         * reboot sign - fires once per death even when a fast reboot slips
         * inside the 3 s lost window - and v8 carries the cause. */
        if (m->alive && m->uptime_s < s_last_up[m->id]) {
            char pu[12];
            fmt_uptime(pu, sizeof(pu), s_last_up[m->id]);
            const char *why = swarm_reset_reason_str(m->reset_reason);
            /* SOFTWARE = an OTA or a commanded restart - routine, dim it.
             * Everything else is nature telling us something - shout it. */
            if (m->reset_reason == (uint8_t)ESP_RST_SW) {
                chron_push("%s rebooted: %s - lived %s", nm, why, pu);
            } else {
                /* amber, not red: red means GONE; a named
                 * reboot is a node back with its story - caution, not alarm */
                chron_push("#ffb020 %s rebooted: %s# - lived %s", nm, why, pu);
            }
            s_heap_tier[m->id] = 0;   /* fresh heap, fresh ledger */
        }
        s_last_up[m->id] = m->uptime_s;
        /* THE HEAP WATCH. min_free_heap == 0 means a v7 node that cannot say -
         * skip those rather than invent a crisis. Tiers latch so a node sitting
         * at 60 KB writes one line, not one per second; recovery unlatches with
         * 8 KB of hysteresis so a boundary-hugger cannot flap the chronicle. */
        if (m->alive && m->min_free_heap > 0) {
            uint8_t tier = m->free_heap < HEAP_RED_B ? 2
                         : m->free_heap < HEAP_AMBER_B ? 1 : 0;
            if (tier > s_heap_tier[m->id]) {
                if (tier == 2) chron_push("#ff5555 %s heap RED - %lu KB free# (floor %lu KB)",
                                          nm, (unsigned long)(m->free_heap / 1024),
                                          (unsigned long)(m->min_free_heap / 1024));
                else           chron_push("#ffb020 %s heap amber - %lu KB free# (floor %lu KB)",
                                          nm, (unsigned long)(m->free_heap / 1024),
                                          (unsigned long)(m->min_free_heap / 1024));
                s_heap_tier[m->id] = tier;
            } else if (tier < s_heap_tier[m->id]
                       && m->free_heap > HEAP_AMBER_B + 8 * 1024) {
                chron_push("#43e86a %s heap recovered - %lu KB free#",
                           nm, (unsigned long)(m->free_heap / 1024));
                s_heap_tier[m->id] = 0;
            }
        }
        if (m->updating != s_was_updating[m->id]) {
            if (m->updating) chron_push("#ffb020 release flowing onto %s#", nm);
            else             chron_push("release done - %s reborn", nm);
            s_was_updating[m->id] = m->updating;
        }
    }
    if (leader_id != s_prev_leader) {
        int64_t now = esp_timer_get_time();
        if (s_prev_leader != 0) {
            uint32_t reign_s = (uint32_t)((now - s_reign_start_us) / 1000000);
            if (reign_s > s_record_s) {
                s_record_s = reign_s;
                /* the FALLEN leader held the record reign */
                const char *prev_nm = "?";
                for (int i = 0; i < n; i++)
                    if (s[i].id == s_prev_leader) prev_nm = short_name(&s[i]);
                snprintf(s_record_holder, sizeof(s_record_holder), "%s", prev_nm);
                record_save();
                char rt[12];
                fmt_uptime(rt, sizeof(rt), reign_s);
                chron_push("#c4f04d new record - %s held the mask %s#", prev_nm, rt);
            }
        }
        s_reign_start_us = now;
        if (leader_id) chron_push("#c4f04d %s wears the mask# (gen %lu)",
                                  leader_name, (unsigned long)generation);
        else           chron_push("#ff5555 no otaman - the mask lies unclaimed#");
        s_prev_leader = leader_id;
    }

    /* the CURRENT reign can itself be the record - track it live, persist gently */
    {
        uint32_t cur_reign = (uint32_t)((esp_timer_get_time() - s_reign_start_us) / 1000000);
        static int64_t last_save_us;
        if (leader_id && cur_reign > s_record_s) {
            s_record_s = cur_reign;
            snprintf(s_record_holder, sizeof(s_record_holder), "%s", leader_name);
            if (esp_timer_get_time() - last_save_us > 300LL * 1000000) {
                record_save();
                last_save_us = esp_timer_get_time();
            }
        }
    }

    /* fleet requests per tick. A blade rebooting zeroes its counter and the
     * SUM dips - treat any decrease as "no delta this tick" and rebase. */
    uint32_t req_delta = 0;
    if (s_have_last_req && fleet_req >= s_last_req) req_delta = fleet_req - s_last_req;
    s_last_req = fleet_req;
    s_have_last_req = true;

    /* (lock taken at function entry - the rings need it too) */
    memcpy(s_snap, s, n * sizeof(s[0]));
    s_snap_n = n;
    s_leader_cache = leader_id;

    if (updating > 0) lv_obj_remove_flag(s_release, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(s_release, LV_OBJ_FLAG_HIDDEN);

    if (leader_id) {
        lv_label_set_text(s_badge_lbl, "HEALTHY");
        lv_obj_set_style_text_color(s_badge_lbl, lv_color_hex(C_ALIVE), 0);
        lv_obj_set_style_border_color(s_badge, lv_color_hex(C_ALIVE), 0);
        lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x0d2012), 0);
    } else {
        lv_label_set_text(s_badge_lbl, "DOWN");
        lv_obj_set_style_text_color(s_badge_lbl, lv_color_hex(C_LOST), 0);
        lv_obj_set_style_border_color(s_badge, lv_color_hex(C_LOST), 0);
        lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x2a0d0d), 0);
    }

    for (int i = 0; i < MAX_ROWS; i++) {
        node_row_t *r = &s_rows[i];
        if (i >= n) { lv_obj_add_flag(r->card, LV_OBJ_FLAG_HIDDEN); continue; }
        const swarm_snap_entry_t *m = &s[i];
        lv_obj_remove_flag(r->card, LV_OBJ_FLAG_HIDDEN);

        bool lead = (m->id == leader_id);
        lv_obj_set_style_bg_color(r->card, lv_color_hex(lead ? 0x161a0d : C_CARD), 0);
        lv_obj_set_style_border_color(r->card, lv_color_hex(lead ? 0x3a4418 : C_BORDER), 0);
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(m->alive ? (m->updating ? C_WARN : C_ALIVE) : C_LOST), 0);
        lv_label_set_text(r->name, short_name(m));
        lv_obj_set_style_text_color(r->name, lv_color_hex(lead ? C_LIME : (m->eligible ? C_TEXT : C_DIM)), 0);
        lv_label_set_text(r->role, m->role);
        lv_obj_set_style_text_color(r->role, lv_color_hex(lead ? C_LIME : C_DIM), 0);

        if (m->updating) {
            /* the rssi bar moonlights as the release progress bar */
            lv_obj_set_style_bg_color(r->rssi_bar, lv_color_hex(C_WARN), LV_PART_INDICATOR);
            lv_bar_set_value(r->rssi_bar, m->update_pct, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_color(r->rssi_bar, lv_color_hex(C_ALIVE), LV_PART_INDICATOR);
            if (m->self || m->rssi == 0) {
                lv_bar_set_value(r->rssi_bar, 0, LV_ANIM_OFF);
            } else {
                int q = ((int)m->rssi + 85) * 100 / 55;   /* -85..-30 dBm -> 0..100 */
                if (q < 5) q = 5;
                if (q > 100) q = 100;
                lv_bar_set_value(r->rssi_bar, q, LV_ANIM_OFF);
            }
        }

        /* SERVED: this blade's own 2xx/3xx tally. Observers never serve - dash. */
        if (m->eligible) {
            char sc[12];
            fmt_count(sc, sizeof(sc), m->req_count);
            lv_label_set_text(r->served, sc);
            lv_obj_set_style_text_color(r->served, lv_color_hex(C_DIM), 0);
        } else {
            lv_label_set_text(r->served, "-");
            lv_obj_set_style_text_color(r->served, lv_color_hex(C_MUTED), 0);
        }

        /* STATUS, the website's words: the leader balances, kozaky serve, a
         * benched blade says so, observers watch. Booting outranks serving the
         * same way the rail's amber does. */
        {
            const char *word; uint32_t col;
            bool benched = m->eligible && !lead && lb.valid
                           && ((lb.benched_mask >> (m->id & 7)) & 1);
            if (!m->alive)            { word = "lost";      col = C_LOST; }
            else if (m->updating)     { word = "release";   col = C_WARN; }
            else if (!m->eligible)    { word = "watching";  col = C_MUTED; }
            else if (lead)            { word = "balancing"; col = C_LIME; }
            else if (benched)         { word = "benched";   col = C_WARN; }
            else if (m->serving)      { word = "serving";   col = C_ALIVE; }
            else                      { word = "booting";   col = C_WARN; }
            lv_label_set_text(r->status, word);
            lv_obj_set_style_text_color(r->status, lv_color_hex(col), 0);
        }
        if (m->temp_c10 == VITALS_TEMP_NONE) {
            lv_label_set_text(r->temp, "-");
            lv_obj_set_style_text_color(r->temp, lv_color_hex(C_MUTED), 0);
        } else {
            int tw = m->temp_c10 / 10;   /* whole degrees read faster at a glance */
            lv_label_set_text_fmt(r->temp, "%dC", tw);
            lv_obj_set_style_text_color(r->temp,
                lv_color_hex(tw >= TEMP_HOT_C ? C_LOST : (tw >= TEMP_WARN_C ? C_WARN : C_ALIVE)), 0);
        }
        char up[12];
        fmt_uptime(up, sizeof(up), m->uptime_s);
        lv_label_set_text(r->up, up);
    }

    /* chronicle rides just under the last row and takes the leftover height */
    int cy = ROW_Y + n * ROW_PITCH + 4;
    int ch = (FOOT_Y - 4) - cy;
    if (ch < 46) ch = 46;
    /* Geometry only when it actually changes. panel.c runs the RGB panel with
     * full_refresh (required by the tearing-avoidance bounce-buffer mode), so
     * ANY invalidation repaints the whole 800x480 PSRAM framebuffer - 768 KB.
     * This card was resized on every 1 Hz tick whether or not the chronicle had
     * grown, guaranteeing that full repaint even on a completely idle second. */
    static int last_cy = -1, last_ch = -1;
    if (cy != last_cy || ch != last_ch) {
        last_cy = cy;
        last_ch = ch;
        lv_obj_set_pos(s_chron_card, 12, cy);
        lv_obj_set_size(s_chron_card, 492, ch);
    }
    int lines = (ch - 30) / 19;
    if (lines > s_chron_count) lines = s_chron_count;
    if (lines > 5) lines = 5;
    /* static, NOT stack: at CHRON_N 40 this buffer is 3.8 KB, and a 3.8 KB local
     * in the render task is exactly how the display build once blew its stack.
     * One render task, one buffer - same rule as the snap arrays. */
    static char text[CHRON_N * CHRON_LEN];
    int p = 0;
    for (int i = 0; i < lines; i++) {   /* newest first */
        int idx = (s_chron_head - 1 - i + CHRON_N * 2) % CHRON_N;
        p += snprintf(text + p, sizeof(text) - p, "%s%s", i ? "\n" : "", s_chron[idx]);
    }
    lv_label_set_text(s_chron_lbl, p ? text : "-");

    lv_chart_set_next_value(s_chart, s_ser_req, (int32_t)(req_delta > 20 ? 20 : req_delta));
    lv_chart_set_next_value(s_chart, s_ser_lat, (int32_t)(fleet_p50 > 50 ? 50 : fleet_p50));
    lv_label_set_text_fmt(s_req_now, "%lu", (unsigned long)req_delta);
    if (lat_n) lv_label_set_text_fmt(s_lat_now, "%lu-%lu", (unsigned long)fleet_p50,
                                     (unsigned long)fleet_p95);
    else       lv_label_set_text(s_lat_now, "-");

    lv_label_set_text_fmt(s_quorum, "%d/%d", alive, n);
    lv_obj_set_style_text_color(s_quorum, lv_color_hex(alive == n ? C_TEXT : C_LOST), 0);
    lv_label_set_text_fmt(s_leader, "%s leads", leader_name);
    char fr[12];
    fmt_count(fr, sizeof(fr), fleet_req);
    lv_label_set_text_fmt(s_gen, "gen %lu - %s served", (unsigned long)generation, fr);
    char st[16];
    fmt_uptime(st, sizeof(st), (uint32_t)((esp_timer_get_time() - s_reign_start_us) / 1000000));
    lv_label_set_text_fmt(s_stable, "reign %s", st);
    if (s_record_s) {
        char rt[12];
        fmt_uptime(rt, sizeof(rt), s_record_s);
        lv_label_set_text_fmt(s_record_lbl, "record %s - %s", rt,
                              s_record_holder[0] ? s_record_holder : "?");
    } else {
        lv_label_set_text(s_record_lbl, "record - forming");
    }
    lv_label_set_text_fmt(s_arc_lbl, "delivery %d%%", delivery);
    lv_obj_set_style_text_color(s_arc_lbl,
        lv_color_hex(delivery >= 95 ? C_DIM : (delivery >= 80 ? C_WARN : C_LOST)), 0);

    /* the otaman's die temperature, coloured by the recorder's own norms
     * (vitals.h): green below TEMP_WARN_C, amber at it, red at TEMP_HOT_C */
    if (lead_temp == VITALS_TEMP_NONE) {
        lv_label_set_text(s_temp_lbl, "-");
        lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(C_MUTED), 0);
    } else {
        lv_label_set_text_fmt(s_temp_lbl, "%d.%d C",
                              lead_temp / 10, (lead_temp < 0 ? -lead_temp : lead_temp) % 10);
        uint32_t tc = (lead_temp >= TEMP_HOT_C * 10)  ? C_LOST
                    : (lead_temp >= TEMP_WARN_C * 10) ? C_WARN : C_ALIVE;
        lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(tc), 0);
    }

    /* FLEET availability, in quorum-sized type: whether visitors actually got
     * what they asked for, summed over every blade that serves. The leader-only
     * figure this replaces described a node that now serves nothing but /roster. */
    {
        int av = fleet_avail;
        if (av >= 0) {
            if (av >= 1000) lv_label_set_text(s_avail_lbl, "100%");
            else            lv_label_set_text_fmt(s_avail_lbl, "%d.%d%%", av / 10, av % 10);
            lv_obj_set_style_text_color(s_avail_lbl,
                lv_color_hex(av >= 999 ? C_LIME : (av >= 950 ? C_WARN : C_LOST)), 0);
        } else {
            lv_label_set_text(s_avail_lbl, "-");
        }
    }

    /* the ledger: one column per serving blade, 200s above the refusals */
    {
        char okt[16];
        fmt_count(okt, sizeof(okt), fleet_req);
        lv_label_set_text(s_ok_total, okt);
    }
    for (int c = 0; c < ERR_COLS; c++) {
        const swarm_snap_entry_t *m = NULL;
        for (int i = 0; i < n; i++) {
            if (s[i].id == c + 1) { m = &s[i]; break; }
        }
        if (!m) {
            lv_label_set_text(s_col_served[c], "-");
            lv_label_set_text(s_err_node[c], "-");
            lv_label_set_text(s_err_vals[c], "");
            continue;
        }
        {
            char sv[16];
            fmt_count(sv, sizeof(sv), m->req_count);
            lv_label_set_text_fmt(s_col_served[c], "%s ok", sv);
        }
        uint32_t tot = 0;
        for (int k = 0; k < SERVE_ERR_KINDS; k++) tot += m->err_kind[k];
        lv_label_set_text_fmt(s_err_node[c], "%lu refused", (unsigned long)tot);
        lv_obj_set_style_text_color(s_err_node[c],
            lv_color_hex(tot ? C_WARN : C_MUTED), 0);
        /* one string, one set_text - a zero stays grey so the eye finds the non-zeros */
        char vals[256];
        int vp = 0;
        for (int k = 0; k < SERVE_ERR_KINDS; k++) {
            vp += snprintf(vals + vp, sizeof(vals) - vp, "%s#%06x %u#",
                           k ? "\n" : "", m->err_kind[k] ? C_TEXT : C_MUTED, m->err_kind[k]);
        }
        lv_label_set_text(s_err_vals[c], vals);
    }

    /* ---- tile 2: the gossip tail, newest first, 45 s window ---- */
    {
        #define RADIO_SHOW 19   /* 19 x 18 px = 342, inside the 364 px card */
        static swarm_radio_evt_t tail[64];
        int tn = swarm_radio_tail(tail, 64);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int shown = 0, missed = 0, heard = 0;
        static char rt[RADIO_SHOW * 64];
        int rp = 0;
        for (int i = 0; i < tn; i++) {
            uint32_t age_s = (now_ms - tail[i].ms) / 1000;
            if (age_s > 45) break;               /* the cut-off tail time */
            heard++;
            missed += tail[i].gap;
            if (shown >= RADIO_SHOW || rp > (int)sizeof(rt) - 72) continue;
            /* name + role colour from the roster snapshot */
            const char *nm = "?";
            uint32_t col = C_TEXT;
            for (int k = 0; k < n; k++) {
                if (s[k].id == tail[i].id) {
                    nm = short_name(&s[k]);
                    col = (tail[i].id == leader_id) ? C_LIME
                        : s[k].eligible ? C_TEXT : C_DIM;
                    break;
                }
            }
            rp += snprintf(rt + rp, sizeof(rt) - rp,
                           "%s#4a4850 +%2lus#  #%06lx %-4.4s#  %4d dBm  seq %-7lu %s",
                           shown ? "\n" : "", (unsigned long)age_s,
                           (unsigned long)col, nm, (int)tail[i].rssi,
                           (unsigned long)tail[i].seq,
                           tail[i].serving ? "#43e86a serving#" : "#4a4850 -#");
            if (tail[i].gap) {
                rp += snprintf(rt + rp, sizeof(rt) - rp,
                               "  #ffb020 !%u missed#", tail[i].gap);
            }
            shown++;
        }
        lv_label_set_text(s_radio_lbl, shown ? rt : "listening...");
        lv_label_set_text_fmt(s_radio_sum, "45 s window - %d heard - %d missed",
                              heard, missed);
        lv_obj_set_style_text_color(s_radio_sum,
            lv_color_hex(missed == 0 ? C_DIM : C_WARN), 0);
    }

    /* ---- the conversions table: slugs as rows, blades as columns ----
     * unscii_16 glyphs are 16 px wide, so a 768 px label holds 48 characters
     * a line - the first cut put blades on rows and slugs on columns and
     * wrapped. Transposed: 12 (slug) + up to 6 numeric columns of 6 = 48. */
    {
        static const char *CN[SERVE_CLICK_KINDS] =
            { "savelife", "prytula", "hospitallers", "u24", "trust", "nomenclature" };
        swarm_snap_entry_t ce[8];
        int cn = swarm_snapshot(ce, 8, NULL);
        int bi[4], nb = 0;
        uint32_t live[SERVE_CLICK_KINDS] = {0};
        bool dirty = false;
        for (int i = 0; i < cn && nb < 4; i++) {
            if (!ce[i].eligible || ce[i].id >= 8) continue;
            bi[nb++] = i;
            for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
                uint16_t v = ce[i].clicks[k];
                /* fold into the archive ONLY on a wipe (counter back to 0):
                 * that blade's NVS is gone, its history now lives here alone.
                 * A small regression is just a reboot losing the <1 min
                 * commit-throttle tail - fold on that and every persisted
                 * click would be counted twice. */
                if (ce[i].alive) {
                    if (v == 0 && s_conv_last[ce[i].id][k] > 0) {
                        s_conv_base[k] += s_conv_last[ce[i].id][k];
                        dirty = true;
                    }
                    s_conv_last[ce[i].id][k] = v;
                }
                live[k] += v;
            }
        }
        char ct[1024];
        int cp = snprintf(ct, sizeof(ct), "#4a4850 %-12s#", "");
        for (int b = 0; b < nb; b++) {
            cp += snprintf(ct + cp, sizeof(ct) - cp, "#4a4850  %5.5s#",
                           ce[bi[b]].shortname ? ce[bi[b]].shortname : "?");
        }
        cp += snprintf(ct + cp, sizeof(ct) - cp, "#c4f04d  FLEET##43e86a   EVER#");
        for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
            cp += snprintf(ct + cp, sizeof(ct) - cp, "\n#4a4850 %-12s#", CN[k]);
            for (int b = 0; b < nb; b++) {
                cp += snprintf(ct + cp, sizeof(ct) - cp, " %5u",
                               (unsigned)ce[bi[b]].clicks[k]);
            }
            cp += snprintf(ct + cp, sizeof(ct) - cp, "#c4f04d  %5lu##43e86a  %5lu#",
                           (unsigned long)live[k],
                           (unsigned long)(s_conv_base[k] + live[k]));
        }
        lv_label_set_text(s_conv_lbl, ct);
        if (dirty && s_conv_nvs) {
            char key[4] = "c0";
            for (int k = 0; k < SERVE_CLICK_KINDS; k++) {
                key[1] = '0' + k;
                nvs_set_u32(s_conv_nvs, key, s_conv_base[k]);
            }
            nvs_commit(s_conv_nvs);
        }
    }

    lvgl_port_unlock();
}
