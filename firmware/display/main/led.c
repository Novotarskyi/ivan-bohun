/* bohun LED rail: 6 SK6812 RGBW pixels, one per swarm member.
 * px0..px3 = N1..N4, px4 = Display (the host itself - lit rail means it lives),
 * px5 = kobzar. Driven from the display DevKitC: GPIO4 -> 74AHCT125 -> 330R ->
 * px0 Din. One UNCUT strip segment: the strip's own copper is the chain.
 *
 * THE COLOUR LAW (matches the website's roster words):
 *   lost      red solid        booting  amber fast blink
 *   release   amber slow blink benched  amber solid (the balancer skips it)
 *   balancing deep green       serving  faint white
 *   pysar     deepest blue     kobzar   deepest purple
 * Every pixel runs under one global brightness cap - a full-brightness rail
 * reads as glare on the rack. Power budget enforced HERE.
 * SK6812 RGBW = 32 bits/px GRBW - not plain WS2812 24-bit; format matters.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include "led.h"

static const char *TAG = "bohun_led";

#define LED_GPIO   4
#define LED_N      6
#define PX_DISPLAY 4          /* the host's own pixel: lit rail = pysar lives */
#define PX_KOBZAR  5
#define BRIGHT_CAP 50         /* /255 = ~20%: one global cap for every pixel */

static led_strip_handle_t s_strip;
static bool s_blackout;   /* maintenance blackout: nothing lights, nothing winks */

static uint8_t s_mirror[LED_N][3];   /* logical colour per pixel, w folded in */
static bool s_dma;
static uint32_t s_last_uptime[LED_N];   /* to spot a reboot: uptime running backwards */

/* Blink states: led_render() (4 Hz) records what each pixel SHOULD do; a
 * 40 ms animation task does the winking.
 *   BOOTING - alive on radio, TLS not listening: rapid amber.
 *   RELEASE - an OTA landing: slow amber, so the two never read the same. */
typedef enum { PXA_SOLID = 0, PXA_AMBER_FAST, PXA_AMBER_SLOW } px_anim_t;
static px_anim_t s_anim[LED_N];
static uint8_t   s_solid[LED_N][4];   /* r,g,b,w to show when the blink is "on" */
static portMUX_TYPE s_anim_lock = portMUX_INITIALIZER_UNLOCKED;

/* THE STRIP LOCK. Two tasks paint this rail: led_render (the render task, 4 Hz)
 * and led_anim_task (its own task, 25 Hz for the winks). Both the pixel buffer
 * and the refresh sit under it: a same-priority interleave can push a
 * half-written frame, and concurrent RMT transmits on one channel are not a
 * documented-safe pattern. Every painter takes this. */
static SemaphoreHandle_t s_paint;

static void px(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    int mr = r + w, mg = g + w, mb = b + w;
    s_mirror[i][0] = mr > 255 ? 255 : mr;
    s_mirror[i][1] = mg > 255 ? 255 : mg;
    s_mirror[i][2] = mb > 255 ? 255 : mb;
    /* scale into the cap; the rail is a status lamp, not a torch */
    led_strip_set_pixel_rgbw(s_strip, i,
        (r * BRIGHT_CAP) / 255, (g * BRIGHT_CAP) / 255,
        (b * BRIGHT_CAP) / 255, (w * BRIGHT_CAP) / 255);
}

/* 40 ms tick. Only pixels in a blink state are touched, and the strip is refreshed
 * only when something actually changed - the rail must not be rewritten 25x a
 * second for six mostly-static lamps. Phase is shared, so all blinking pixels wink
 * together and read as one deliberate signal rather than a fairground. */
static void led_anim_task(void *arg)
{
    const int FAST_TICKS = 3;    /* ~120 ms on, 120 ms off -> brisk, unmistakable */
    const int SLOW_TICKS = 10;   /* ~400 ms on, 400 ms off -> a calm pulse */
    uint32_t t = 0;
    bool last_on_fast = true, last_on_slow = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(40));
        t++;
        bool on_fast = ((t / FAST_TICKS) & 1) == 0;
        bool on_slow = ((t / SLOW_TICKS) & 1) == 0;
        if (on_fast == last_on_fast && on_slow == last_on_slow) {
            continue;
        }
        last_on_fast = on_fast;
        last_on_slow = on_slow;
        if (!s_strip || s_blackout) {
            continue;
        }
        bool touched = false;
        for (int i = 0; i < LED_N; i++) {
            px_anim_t a;
            uint8_t c[4];
            taskENTER_CRITICAL(&s_anim_lock);
            a = s_anim[i];
            memcpy(c, s_solid[i], sizeof(c));
            taskEXIT_CRITICAL(&s_anim_lock);
            if (a == PXA_SOLID) {
                continue;
            }
            bool on = (a == PXA_AMBER_FAST) ? on_fast : on_slow;
            if (on) px(i, c[0], c[1], c[2], c[3]);
            else    px(i, 0, 0, 0, 0);
            touched = true;
        }
        if (touched) {
            xSemaphoreTake(s_paint, portMAX_DELAY);
            if (!s_blackout) {          /* never relight a rail held dark */
                led_strip_refresh(s_strip);
            }
            xSemaphoreGive(s_paint);
        }
    }
}

void led_init(void)
{
    s_paint = xSemaphoreCreateMutex();
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_N,
        .led_model = LED_MODEL_SK6812,
        /* standard SK6812 GRBW. (An undervolted rail browns out the W die
         * first and can masquerade as a different byte order - always judge
         * colour order at the full 5 V.) */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
        .flags.invert_out = false,
    };
    /* DMA keeps the bit-stream flowing when WiFi interrupts stall the CPU -
     * the cause of occasional single-frame blips on later pixels at 1 Hz */
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 512,
        .flags.with_dma = true,
    };
    s_dma = true;
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "RMT DMA unavailable - falling back to non-DMA");
        s_dma = false;
        rmt_cfg.flags.with_dma = false;
        rmt_cfg.mem_block_symbols = 0;
        if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
            ESP_LOGE(TAG, "rail driver failed - continuing dark");
            s_strip = NULL;
            return;
        }
    }
    /* boot parade: white, green, blue then red dots walk the rail - one pass
     * per die family, so the gate proves every joint AND every colour at once */
    static const uint8_t walk[4][4] = {
        {0, 0, 0, 255}, {0, 255, 0, 0}, {0, 0, 255, 0}, {255, 0, 0, 0} };
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < LED_N; i++) {
            led_strip_clear(s_strip);
            px(i, walk[c][0], walk[c][1], walk[c][2], walk[c][3]);
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(160));
        }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    xTaskCreate(led_anim_task, "led_anim", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "rail up: %d px on GPIO%d (SK6812 GRBW, cap %d/255)", LED_N, LED_GPIO, BRIGHT_CAP);
}

/* channel-order probe: LEDs on one reel can be mixed variants with differing
 * byte order. Set to 1, flash, film one 24 s cycle, derive per-pixel maps. */
#define LED_COLOR_PROBE 0

/* maintenance blackout (long-press on the TFT): rail dark and held dark so a
 * flash starts from darkness instead of latched stale colours */
void led_blackout(void)
{
    if (!s_strip) return;
    s_blackout = true;
    taskENTER_CRITICAL(&s_anim_lock);
    for (int i = 0; i < LED_N; i++) s_anim[i] = PXA_SOLID;   /* nothing winks in the dark */
    taskEXIT_CRITICAL(&s_anim_lock);
    xSemaphoreTake(s_paint, portMAX_DELAY);
    memset(s_mirror, 0, sizeof(s_mirror));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    xSemaphoreGive(s_paint);
}

void led_get_status(led_status_t *out)
{
    out->up = (s_strip != NULL);
    out->dma = s_dma;
    out->blackout = s_blackout;
    memcpy(out->rgb, s_mirror, sizeof(out->rgb));
}

void led_wake(void)
{
    s_blackout = false;
}

/* The OTA window's rail voice: every pixel slow-blinks amber, unmistakable
 * from across the room. Applies the amber HERE, immediately - the trigger
 * task calls this at the 4 s threshold and the operator needs the "you can
 * let go" cue now, not on the next ~1 Hz render tick. led_render holds off
 * while active and repaints normally the tick after it clears. */
static bool s_ota_rail;
void led_ota(bool active)
{
    if (active && !s_ota_rail) {
        taskENTER_CRITICAL(&s_anim_lock);
        for (int i = 0; i < LED_N; i++) {
            s_anim[i] = PXA_AMBER_SLOW;
            s_solid[i][0] = 255; s_solid[i][1] = 140;
            s_solid[i][2] = 0;   s_solid[i][3] = 0;
        }
        taskEXIT_CRITICAL(&s_anim_lock);
    }
    s_ota_rail = active;
}

void led_render(const swarm_snap_entry_t *e, int n, uint8_t leader_id)
{
    if (!s_strip) return;
    if (s_blackout) return;   /* re-checked under the lock before the refresh */

    /* a fast OTA can otherwise flash past between samples - latch the release
     * amber so it is always seen (the rail samples at 4 Hz; lost and booting are
     * long enough at that rate to need no latch of their own) */
    static int64_t amber_until_us[LED_N];
    int64_t now = esp_timer_get_time();

#if LED_COLOR_PROBE
    /* forever: R 5s, G 5s, B 5s, W 5s, dark 4s - logical channels, all pixels */
    static const uint8_t seq[5][4] = {
        {255,0,0,0}, {0,255,0,0}, {0,0,255,0}, {0,0,0,255}, {0,0,0,0} };
    static uint8_t tick;
    int ph = (tick++ / 5) % 5;   /* led_render runs ~1 Hz on roster ticks */
    for (int i = 0; i < LED_N; i++)
        px(i, seq[ph][0], seq[ph][1], seq[ph][2], seq[ph][3]);
    led_strip_refresh(s_strip);
    return;
#endif

    if (s_ota_rail) return;   /* the window owns the rail; the anim task blinks it */

    bool heard_any = false;
    for (int i = 0; i < LED_N; i++) px(i, 0, 0, 0, 0);

    /* the balancer's bench travels in the leader's heartbeat - a benched blade
     * is alive and serving-capable but the splicer is skipping it, and the rail
     * should say so rather than show a healthy white */
    swarm_lb_view_t lb;
    swarm_lb_view(&lb);

    for (int i = 0; i < n; i++) {
        const swarm_snap_entry_t *m = &e[i];
        if (!m->self && m->alive) heard_any = true;
        int p = -1;
        if (m->id >= 1 && m->id <= 4) p = m->id - 1;   /* px0..3 = N1..N4 */
        else if (m->id == 6) p = PX_DISPLAY;
        else if (m->id == 7) p = PX_KOBZAR;
        if (p < 0) continue;
        if (m->updating) amber_until_us[p] = now + 6000000;

        /* A release ends when the node comes back, not when a timer says so.
         * The latch is set from the LAST updating heartbeat, so it can outlive
         * the reboot it announced and keep a returned node amber. Uptime running
         * backwards is an unambiguous reboot: the release is over, stop
         * latching. */
        if (m->uptime_s < s_last_uptime[p]) {
            amber_until_us[p] = 0;
        }
        s_last_uptime[p] = m->uptime_s;

        /* A node can be perfectly alive on the radio and serving nothing at
         * all - a failure the rail must not hide. Booting outranks every other
         * colour except death, because a green pixel on a node that cannot
         * answer a request is a lie. */
        bool booting = m->alive && m->eligible && !m->serving;
        bool benched = m->alive && m->eligible && m->id != leader_id && lb.valid
                       && ((lb.benched_mask >> (m->id & 7)) & 1);
        px_anim_t anim = PXA_SOLID;
        uint8_t r = 0, g = 0, b = 0, w = 0;

        if (!m->alive)                    { r = 255; }                       /* lost: red */
        else if (booting)                 { r = 255; g = 140; anim = PXA_AMBER_FAST; }
        else if (now < amber_until_us[p]) { r = 255; g = 140; anim = PXA_AMBER_SLOW; } /* release */
        else if (m->id == 7)              { r = 90; b = 255; }               /* kobzar: deepest purple */
        else if (m->id == 6)              { b = 255; }                       /* pysar peer: deepest blue */
        else if (m->id == leader_id)      { g = 255; }                       /* otaman: deep green */
        else if (benched)                 { r = 255; g = 140; }              /* benched: amber solid */
        else if (m->eligible)             { w = 120; }                       /* kozak: faint white */
        else                              { r = 90; b = 255; }               /* other witness: purple */

        taskENTER_CRITICAL(&s_anim_lock);
        s_anim[p] = anim;
        s_solid[p][0] = r; s_solid[p][1] = g; s_solid[p][2] = b; s_solid[p][3] = w;
        taskEXIT_CRITICAL(&s_anim_lock);
        px(p, r, g, b, w);
    }
    /* the host's own pixel: deepest blue while the air carries heartbeats,
     * red in silence. (The id==6 branch covers the pysar as a PEER on any
     * other rail; this covers our own.) */
    taskENTER_CRITICAL(&s_anim_lock);
    s_anim[PX_DISPLAY] = PXA_SOLID;
    taskEXIT_CRITICAL(&s_anim_lock);
    if (heard_any) px(PX_DISPLAY, 0, 0, 255, 0);
    else           px(PX_DISPLAY, 255, 0, 0, 0);

    /* Blackout can land mid-paint: without this re-check an in-flight render
     * relights the rail and every later call early-returns, leaving stale
     * colours lit for the whole maintenance window. */
    xSemaphoreTake(s_paint, portMAX_DELAY);
    if (!s_blackout) {
        led_strip_refresh(s_strip);
    }
    xSemaphoreGive(s_paint);
}
