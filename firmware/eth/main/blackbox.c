/* blackbox.c - see blackbox.h for why this exists.
 *
 * Layout: the "blackbox" data partition is a ring of 64-byte records. A record
 * is valid iff its magic matches; the write head is wherever the highest
 * (boot, seq) pair lives, found by one full scan at init (256 KB read, ~tens of
 * ms, once per boot). Sectors are erased just before first use, so the ring
 * needs no header, no index and no repair path: torn writes lose at most one
 * record, and a corrupt sector is simply erased when the head next enters it.
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#ifdef BBX_HAS_HTTPD
#include "esp_http_server.h"
#endif
#include "nvs.h"
#include "swarm.h"      /* swarm_reset_reason_str - one naming for one fact */
#include "blackbox.h"

#define BBX_MAGIC   0x31584242u      /* "BBX1" */
#define REC_SZ      64
#define SECTOR      4096
#define RECS_PER_SECTOR (SECTOR / REC_SZ)

/* the amber line, same value the kobzar watches remotely; edge-triggered with
 * hysteresis so one hovering value cannot spam the ring */
#define HEAP_AMBER      (64 * 1024)
#define HEAP_REARM      (72 * 1024)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t boot;       /* NVS-persisted boot counter - total order across reboots */
    uint32_t seq;        /* per-boot sequence */
    uint32_t up_s;       /* uptime at the event */
    uint8_t  kind;       /* BBX_* */
    uint8_t  reserved;
    uint16_t code;       /* HTTP code, reset reason, percent - kind-dependent */
    uint32_t heap;       /* free internal heap at the event */
    uint32_t lfb;        /* largest free block - the fragmentation truth */
    char     text[36];   /* NUL-terminated detail (URI, note) */
} bbx_rec_t;
_Static_assert(sizeof(bbx_rec_t) == REC_SZ, "record must be exactly 64 bytes");

static const char *TAG = "bohun_bbx";
static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static uint32_t s_boot;
static uint32_t s_seq;
static uint32_t s_head;      /* next slot index to write */
static uint32_t s_slots;

static const char *kind_name(uint8_t k)
{
    switch (k) {
    case BBX_BOOT:     return "BOOT";
    case BBX_ERR5XX:   return "ERR5XX";
    case BBX_OOM:      return "OOM";
    case BBX_HEAPLOW:  return "HEAPLOW";
    case BBX_MASK_ON:  return "MASK_ON";
    case BBX_MASK_OFF: return "MASK_OFF";
    case BBX_OTA_IN:   return "OTA_IN";
    case BBX_OTA_OK:   return "OTA_OK";
    case BBX_CHRON:    return "CHRON";
    case BBX_TRIAL:    return "TRIAL";
    case BBX_VALID:    return "VALID";
    case BBX_VARIANT:  return "VARIANT";
    case BBX_TEMPHI:   return "TEMPHI";
    default:           return "?";
    }
}

static void write_record(const bbx_rec_t *r)
{
    /* entering a fresh sector: erase it first (the ring's only maintenance) */
    if (s_head % RECS_PER_SECTOR == 0) {
        esp_partition_erase_range(s_part, (size_t)s_head * REC_SZ, SECTOR);
    }
    esp_partition_write(s_part, (size_t)s_head * REC_SZ, r, REC_SZ);
    s_head = (s_head + 1) % s_slots;
}

void blackbox_event(uint8_t kind, uint16_t code, const char *fmt, ...)
{
    if (!s_part) {
        return;                       /* partition missing: degrade to silence */
    }
    bbx_rec_t r = {
        .magic = BBX_MAGIC,
        .boot  = s_boot,
        .up_s  = (uint32_t)(esp_timer_get_time() / 1000000),
        .kind  = kind,
        .code  = code,
        .heap  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .lfb   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.text, sizeof(r.text), fmt ? fmt : "", ap);
    va_end(ap);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
        r.seq = ++s_seq;
        write_record(&r);
        xSemaphoreGive(s_lock);
    }
    /* a full ring beats a blocked request path: on lock timeout the event is
     * dropped, and the counters (which never block) still count it */
}

/* ---- the 1 Hz heap watch: catches the moment the counters can't ---- */
static void heap_watch_cb(void *arg)
{
    static bool armed = true;
    size_t free_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (armed && free_i < HEAP_AMBER) {
        armed = false;
        blackbox_event(BBX_HEAPLOW, (uint16_t)(free_i / 1024), "amber crossed");
    } else if (!armed && free_i > HEAP_REARM) {
        armed = true;
    }
}

void blackbox_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "blackbox");
    if (!s_part) {
        ESP_LOGW(TAG, "no blackbox partition - flying without a recorder");
        return;
    }
    s_slots = s_part->size / REC_SZ;
    s_lock = xSemaphoreCreateMutex();

    /* boot counter: total order for records across reboots */
    nvs_handle_t nvs;
    if (nvs_open("bbx", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boot", &s_boot);
        s_boot++;
        nvs_set_u32(nvs, "boot", s_boot);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* find the head: the slot after the highest (boot, seq) seen */
    uint32_t best_boot = 0, best_seq = 0, best_slot = 0;
    bool any = false;
    /* 4 KB, heap-borrowed for the scan and given back - a static buffer
     * would sit resident forever for one use per boot. If the allocation
     * fails, start the ring at slot 0 - losing history, never the recorder. */
    bbx_rec_t *buf = malloc(SECTOR);
    if (!buf) {
        ESP_LOGW(TAG, "no memory to scan the ring - starting at slot 0");
        s_head = 0;
        goto scanned;
    }
    for (uint32_t s = 0; s < s_slots; s += RECS_PER_SECTOR) {
        esp_partition_read(s_part, (size_t)s * REC_SZ, buf, SECTOR);
        for (uint32_t i = 0; i < RECS_PER_SECTOR; i++) {
            if (buf[i].magic != BBX_MAGIC) {
                continue;
            }
            if (!any || buf[i].boot > best_boot ||
                (buf[i].boot == best_boot && buf[i].seq > best_seq)) {
                any = true;
                best_boot = buf[i].boot;
                best_seq = buf[i].seq;
                best_slot = s + i;
            }
        }
    }
    s_head = any ? (best_slot + 1) % s_slots : 0;
    free(buf);
scanned:;
    /* resuming mid-sector onto non-erased flash would corrupt: if the landing
     * slot is not blank, step to the next sector boundary (costs the tail of
     * one sector of history, keeps every write clean) */
    if (s_head % RECS_PER_SECTOR != 0) {
        bbx_rec_t probe;
        esp_partition_read(s_part, (size_t)s_head * REC_SZ, &probe, sizeof(probe));
        if (probe.magic != 0xffffffffu) {
            s_head = ((s_head / RECS_PER_SECTOR) + 1) % (s_slots / RECS_PER_SECTOR)
                     * RECS_PER_SECTOR;
        }
    }

    /* replay the last few records to the console: a board on the bench answers
     * "why did you reboot" the moment idf.py monitor attaches, no tooling */
    for (int back = 12; back >= 1; back--) {
        uint32_t slot = (s_head + s_slots - (uint32_t)back) % s_slots;
        bbx_rec_t old;
        esp_partition_read(s_part, (size_t)slot * REC_SZ, &old, sizeof(old));
        if (old.magic != BBX_MAGIC) {
            continue;
        }
        old.text[sizeof(old.text) - 1] = '\0';
        ESP_LOGI(TAG, "  #%" PRIu32 ".%" PRIu32 " +%" PRIu32 "s %s code=%u heap=%" PRIu32 " lfb=%" PRIu32 " %s",
                 old.boot, old.seq, old.up_s, kind_name(old.kind), old.code,
                 old.heap, old.lfb, old.text);
    }

    uint8_t rr = (uint8_t)esp_reset_reason();
    blackbox_event(BBX_BOOT, rr, "%s", swarm_reset_reason_str(rr));
    ESP_LOGI(TAG, "ring up: boot #%" PRIu32 ", head slot %" PRIu32 "/%" PRIu32,
             s_boot, s_head, s_slots);

    const esp_timer_create_args_t targs = {
        .callback = heap_watch_cb, .name = "bbx_heap"
    };
    esp_timer_handle_t th;
    if (esp_timer_create(&targs, &th) == ESP_OK) {
        esp_timer_start_periodic(th, 1000000);
    }
}

/* ---- admin-plane readback (blades only - observers have no server) ---- */
#ifdef BBX_HAS_HTTPD

int blackbox_http_dump(struct httpd_req *hr)
{
    httpd_req_t *req = (httpd_req_t *)hr;
    if (!s_part) {
        httpd_resp_sendstr(req, "no blackbox partition\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char line[140];
    int emitted = 0;
    /* newest first, up to 256 lines - one small read per record, chunked out */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_sendstr(req, "busy\n");
        return ESP_OK;
    }
    uint32_t head = s_head;
    xSemaphoreGive(s_lock);
    for (uint32_t back = 1; back <= s_slots && emitted < 256; back++) {
        uint32_t slot = (head + s_slots - back) % s_slots;
        bbx_rec_t r;
        esp_partition_read(s_part, (size_t)slot * REC_SZ, &r, sizeof(r));
        if (r.magic != BBX_MAGIC) {
            break;                    /* ran off the recorded history */
        }
        r.text[sizeof(r.text) - 1] = '\0';
        int n = snprintf(line, sizeof(line),
                         "#%" PRIu32 ".%-4" PRIu32 " +%6" PRIu32 "s  %-8s code=%-3u "
                         "heap=%-6" PRIu32 " lfb=%-6" PRIu32 " %s\n",
                         r.boot, r.seq, r.up_s, kind_name(r.kind), r.code,
                         r.heap, r.lfb, r.text);
        if (httpd_resp_send_chunk(req, line, n) != ESP_OK) {
            return ESP_FAIL;
        }
        emitted++;
    }
    if (!emitted) {
        httpd_resp_send_chunk(req, "ring is empty\n", 14);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

int blackbox_http_coredump(struct httpd_req *hr)
{
    httpd_req_t *req = (httpd_req_t *)hr;
    const esp_partition_t *cd = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!cd) {
        httpd_resp_sendstr(req, "no coredump partition\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    static char buf[1024];
    for (size_t off = 0; off < cd->size; off += sizeof(buf)) {
        esp_partition_read(cd, off, buf, sizeof(buf));
        if (httpd_resp_send_chunk(req, buf, sizeof(buf)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
#endif /* BBX_HAS_HTTPD */
