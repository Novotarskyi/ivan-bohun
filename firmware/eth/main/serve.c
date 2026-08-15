/* serve.c - the page, TLS 1.3, everything embedded in flash.
 * After editing the page, refresh BOTH encodings or brotli browsers keep
 * the old bytes while fleet_check reports consistency:
 *   gzip -9 -n -c assets/personal.html > firmware/eth/main/personal.html.gz
 *   brotli -f -q 11 assets/personal.html -o firmware/eth/main/personal.html.br
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp for the redirect host allowlist */
#include "esp_https_server.h"
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "swarm.h"
#include "blackbox.h"
#include "cpustat.h"
#include "logic.h"   /* header parsing + policy kernels, host-tested */
#include "serve.h"
#include "ota.h"

/* err_kind[] rides mid-heartbeat sized by SERVE_ERR_KINDS; if the IDF ever
 * grows httpd_err_code_t, tally() below would silently drop the new codes.
 * Surface that as a compile error instead. */
_Static_assert(SERVE_ERR_KINDS == HTTPD_ERR_CODE_MAX,
               "httpd error enum changed - revisit SERVE_ERR_KINDS and the heartbeat");

extern const uint8_t page_gz_start[]    asm("_binary_personal_html_gz_start");
extern const uint8_t page_gz_end[]      asm("_binary_personal_html_gz_end");
static void tally(httpd_err_code_t err);   /* defined with the counters below */

extern const uint8_t page_br_start[]    asm("_binary_personal_html_br_start");
extern const uint8_t page_br_end[]      asm("_binary_personal_html_br_end");
extern const uint8_t favicon_start[]    asm("_binary_favicon_ico_start");
extern const uint8_t favicon_end[]      asm("_binary_favicon_ico_end");
extern const uint8_t favicon_gz_start[] asm("_binary_favicon_ico_gz_start");
extern const uint8_t favicon_gz_end[]   asm("_binary_favicon_ico_gz_end");
extern const uint8_t og_start[]         asm("_binary_og_png_start");
extern const uint8_t og_end[]           asm("_binary_og_png_end");
extern const uint8_t llms_start[]       asm("_binary_llms_txt_start");
extern const uint8_t llms_end[]         asm("_binary_llms_txt_end");
extern const uint8_t robots_start[]     asm("_binary_robots_txt_start");
extern const uint8_t robots_end[]       asm("_binary_robots_txt_end");
extern const uint8_t favicon_br_start[] asm("_binary_favicon_ico_br_start");
extern const uint8_t favicon_br_end[]   asm("_binary_favicon_ico_br_end");

/* An ETag lets a returning visitor spend ~200 bytes on a 304 instead of pulling
 * the whole body again. On a server measured at roughly one request per second
 * that is not a nicety - it is capacity. The tag is size-crc32 of the exact bytes
 * we would send, and it carries the encoding, because gzip and brotli of the same
 * page are different entities and must never share a validator. */
/* Memoized ETags: assets are immutable per image; recomputing the CRC per
 * hit taxed even the 304 path. A racing miss just recomputes - harmless. */
#define ETAG_CACHE_N 8
static struct { const uint8_t *body; const char *enc; char tag[64]; } s_etags[ETAG_CACHE_N];
static uint8_t s_etag_n;

static const char *etag_for(const uint8_t *body, size_t len, const char *enc)
{
    for (uint8_t i = 0; i < s_etag_n; i++) {
        if (s_etags[i].body == body && s_etags[i].enc == enc) {
            return s_etags[i].tag;
        }
    }
    uint8_t i = s_etag_n < ETAG_CACHE_N ? s_etag_n : (uint8_t)(ETAG_CACHE_N - 1);
    snprintf(s_etags[i].tag, sizeof(s_etags[i].tag), "\"%u-%08x-%s\"", (unsigned)len,
             (unsigned)esp_rom_crc32_le(0, body, (uint32_t)len), enc);
    s_etags[i].body = body;
    s_etags[i].enc = enc;
    if (s_etag_n < ETAG_CACHE_N) {
        s_etag_n++;
    }
    return s_etags[i].tag;
}

static const char *set_etag(httpd_req_t *req, const uint8_t *body, size_t len,
                            const char *enc)
{
    const char *tag = etag_for(body, len, enc);
    httpd_resp_set_hdr(req, "ETag", tag);
    return tag;
}

static bool etag_matches(httpd_req_t *req, const char *etag)
{
    char inm[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) != ESP_OK) {
        return false;
    }
    return strstr(inm, etag) != NULL;   /* handles a list, and W/ prefixes */
}
extern const uint8_t servercert_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_end[]      asm("_binary_prvtkey_pem_end");

static const char *TAG = "bohun_serve";

/* DRILL STANDBY: scriptable "act dead" for failover drills. Reports NOT
 * READY - the one lever the election, heartbeat, splicers and mask all read.
 * Admin-plane only, key-gated, and it ALWAYS expires: a blade cannot be
 * switched off permanently by accident. */
static int64_t s_standby_until_us;
#define STANDBY_MAX_S 300

/* ── THE SELF-PROBE ─────────────────────────────────────────────────────────
 * Catches the wedge where httpd tasks block behind an undrained accept queue
 * while the node keeps gossiping serving=1. The only question that
 * matters, asked of ourselves: can anything still connect to the public port?
 * Non-blocking loopback connect with a hard deadline, so the probe cannot
 * itself hang. Two failures: report not-serving (election drops us, splicers
 * bench us in ~2 s). Six: abort() so the coredump names what the server was
 * stuck in, then reboot. */
#define PROBE_PERIOD_MS   7000
#define PROBE_FAIL_SICK   2      /* stop advertising ourselves */
#define PROBE_FAIL_ABORT  6      /* ~42 s of a dead server: capture and reboot */
static volatile int s_probe_fails;
static volatile int64_t s_probe_ran_us;

static bool probe_once(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        ok = true;                       /* loopback can complete immediately */
    } else if (errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        if (select(fd + 1, NULL, &w, NULL, &tv) > 0) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = (err == 0);
        }
    }
    close(fd);
    return ok;
}

static void probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));    /* let the servers finish coming up */
    for (;;) {
        bool ok = probe_once(CONFIG_BOHUN_PUBLIC_PORT);
        s_probe_ran_us = esp_timer_get_time();
        if (ok) {
            if (s_probe_fails) {
                ESP_LOGW(TAG, "self-probe recovered after %d failures", s_probe_fails);
            }
            s_probe_fails = 0;
        } else {
            s_probe_fails++;
            ESP_LOGE(TAG, "SELF-PROBE FAILED (%d): nothing can open :%d on this node",
                     s_probe_fails, CONFIG_BOHUN_PUBLIC_PORT);
            if (s_probe_fails == PROBE_FAIL_SICK) {
                blackbox_event(BBX_ERR5XX, 0, "self-probe: own port dead, standing down");
            }
            if (s_probe_fails >= PROBE_FAIL_ABORT) {
                blackbox_event(BBX_BOOT, (uint16_t)s_probe_fails,
                               "own port dead %d probes - abort for coredump", s_probe_fails);
                ESP_LOGE(TAG, "own server unreachable from itself - aborting to capture why");
                abort();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }
}


/* Build identity: __DATE__/__TIME__ only change when THIS file recompiles
 * and once fooled deploy gate 3. The ELF SHA changes when anything does.
 * Date for humans, SHA for truth. */
static const char *build_id(void)
{
    static char s_id[48];
    if (!s_id[0]) {
        const esp_app_desc_t *d = esp_app_get_description();
        snprintf(s_id, sizeof(s_id), "%s %s %02x%02x%02x%02x",
                 __DATE__, __TIME__,
                 d->app_elf_sha256[0], d->app_elf_sha256[1],
                 d->app_elf_sha256[2], d->app_elf_sha256[3]);
    }
    return s_id;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    char ae[96] = "";
    bool br = httpd_req_get_hdr_value_str(req, "Accept-Encoding", ae, sizeof(ae)) == ESP_OK
              && bohun_accepts_br(ae);   /* truncated/absent header falls back to gzip - always safe */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", br ? "br" : "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    httpd_resp_set_hdr(req, "X-Bohun-Build", build_id());
    /* content tag: size-crc of the embedded page, so fleet_check catches page-only
     * drift that a serve.c compile stamp cannot (idempotent lazy init - races are benign) */
    /* over BOTH encodings: a gz-only refresh must not read as an unchanged
     * page while brotli clients get stale bytes - fleet_check compares exactly
     * this string */
    static char page_tag[40];
    if (!page_tag[0]) {
        uint32_t gzl = (uint32_t)(page_gz_end - page_gz_start);
        uint32_t brl = (uint32_t)(page_br_end - page_br_start);
        snprintf(page_tag, sizeof(page_tag), "%u-%08x-%u-%08x",
                 (unsigned)gzl, (unsigned)esp_rom_crc32_le(0, page_gz_start, gzl),
                 (unsigned)brl, (unsigned)esp_rom_crc32_le(0, page_br_start, brl));
    }
    httpd_resp_set_hdr(req, "X-Bohun-Page", page_tag);

    const uint8_t *body = br ? page_br_start : page_gz_start;
    size_t blen = br ? (size_t)(page_br_end - page_br_start)
                     : (size_t)(page_gz_end - page_gz_start);
    const char *tag = set_etag(req, body, blen, br ? "br" : "gz");
    /* short max-age + revalidation: the page changes only on a release, but when it
     * does we want it seen quickly, so we buy cheap 304s rather than long staleness */
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=600, must-revalidate");
    /* tell the client the connection is worth keeping - a reused connection
     * costs this node ~120 ms instead of ~620 */
    httpd_resp_set_hdr(req, "Keep-Alive", "timeout=5, max=20");   /* the server's idle window, not recv_wait_timeout */

    esp_err_t r;
    if (etag_matches(req, tag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        r = httpd_resp_send(req, NULL, 0);
    } else {
        r = httpd_resp_send(req, (const char *)body, blen);
    }
    swarm_record_request((uint32_t)(esp_timer_get_time() - t0));
    return r;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    char ae[96] = "";
    bool has_ae = httpd_req_get_hdr_value_str(req, "Accept-Encoding", ae, sizeof(ae)) == ESP_OK;
    bool br = has_ae && bohun_accepts_br(ae);
    bool gz = has_ae && strstr(ae, "gzip") != NULL;

    /* a 32x32 32bpp icon is almost entirely redundancy: 4286 B raw, 136 gzipped,
     * 115 brotli'd. Serving it uncompressed was 30x the bytes for no reason. */
    const uint8_t *body; size_t blen; const char *enc;
    if (br)      { body = favicon_br_start; blen = favicon_br_end - favicon_br_start; enc = "br"; }
    else if (gz) { body = favicon_gz_start; blen = favicon_gz_end - favicon_gz_start; enc = "gz"; }
    else         { body = favicon_start;    blen = favicon_end - favicon_start;       enc = "id"; }

    httpd_resp_set_type(req, "image/x-icon");
    if (br || gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", br ? "br" : "gzip");
    }
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    /* the icon outlives the page by a long way - cache it for a week */
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    const char *tag = set_etag(req, body, blen, enc);

    esp_err_t r;
    if (etag_matches(req, tag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        r = httpd_resp_send(req, NULL, 0);
    } else {
        r = httpd_resp_send(req, (const char *)body, blen);
    }
    swarm_record_request((uint32_t)(esp_timer_get_time() - t0));
    return r;
}

extern esp_err_t ota_post_handler(httpd_req_t *req);

/* cross-origin guard for the data endpoint: our own page's fetch('/roster') is
 * same-origin and sends no Origin header; a request carrying one from a different
 * host is another site's JavaScript. Refuse it, and never emit
 * Access-Control-Allow-Origin - the browser wall stays up. (Non-browser spam
 * doesn't care about CORS; that fight is rate limiting, later.) */
static bool origin_is_self(httpd_req_t *req)
{
    char origin[96];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        char sfs[24];
        if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", sfs, sizeof(sfs)) == ESP_OK
            && strcmp(sfs, "cross-site") == 0) {
            return false;             /* no Origin, but the browser admits cross-site */
        }
        return true;                  /* same-origin fetches and plain clients */
    }
    if (strcmp(origin, "null") == 0) {
        return false;
    }
    char host[96];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    return bohun_origin_host_ok(origin, host);   /* full-label match: 1.2.3.4.evil.com must not pass */
}

static esp_err_t roster_get_handler(httpd_req_t *req)
{
    if (req->content_len) {          /* a GET with a body: refuse before reading a byte */
        tally(HTTPD_400_BAD_REQUEST);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "");
        return ESP_FAIL;
    }
    if (!origin_is_self(req)) {
        tally(HTTPD_403_FORBIDDEN);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"err\":\"cross-origin refused\"}");
        return ESP_FAIL;   /* close it: ESP_OK would make httpd drain their body 32B at a time */
    }
    int64_t t0 = esp_timer_get_time();
    /* 3 KB: the payload runs ~300 B/member. swarm_roster_json_ex guarantees
     * a valid tail when clamped; keep headroom ahead of growth. */
    /* STATIC, not malloc'd: the page polls this from every open tab, and this
     * handler runs in exactly one task (esp_http_server is single-threaded per
     * instance), so a per-request 3 KB alloc/free - and its OOM branch - bought
     * nothing but hot-path work and heap churn. The admin instance is a
     * different server object with its own task, hence its own buffer below. */
    static char buf[3072];
    int n = swarm_roster_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");   /* live truth, never cached */
    esp_err_t r = httpd_resp_send(req, buf, n);
    swarm_record_request((uint32_t)(esp_timer_get_time() - t0));
    return r;
}

static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
static const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler };
static const httpd_uri_t roster = { .uri = "/roster", .method = HTTP_GET, .handler = roster_get_handler };

/* llms.txt and robots.txt: both under 3 KB, so compression would cost more in
 * headers than it saves. Cached hard - neither changes between releases.
 * robots.txt asks crawlers to leave /roster alone: it is live telemetry with no
 * archival value, and polling it spends capacity a human visitor needs. */
static esp_err_t text_get_handler(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    bool llms = strcmp(req->uri, "/llms.txt") == 0;
    const uint8_t *body = llms ? llms_start : robots_start;
    size_t blen = llms ? (size_t)(llms_end - llms_start)
                       : (size_t)(robots_end - robots_start);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    const char *tag = set_etag(req, body, blen, "id");
    esp_err_t r;
    if (etag_matches(req, tag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        r = httpd_resp_send(req, NULL, 0);
    } else {
        r = httpd_resp_send(req, (const char *)body, blen);
    }
    swarm_record_request((uint32_t)(esp_timer_get_time() - t0));
    return r;
}
/* The link-preview card. Already PNG-compressed, so no gzip/brotli - re-compressing
 * a PNG costs CPU to gain nothing. Cached for a week: scrapers fetch it once and
 * every messenger that shows the link afterwards serves it from their own cache. */
static esp_err_t og_get_handler(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    size_t blen = (size_t)(og_end - og_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    const char *tag = set_etag(req, og_start, blen, "id");
    esp_err_t r;
    if (etag_matches(req, tag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        r = httpd_resp_send(req, NULL, 0);
    } else {
        r = httpd_resp_send(req, (const char *)og_start, blen);
    }
    swarm_record_request((uint32_t)(esp_timer_get_time() - t0));
    return r;
}
static const httpd_uri_t og_uri     = { .uri = "/og.png",     .method = HTTP_GET, .handler = og_get_handler };
static const httpd_uri_t llms_uri   = { .uri = "/llms.txt",   .method = HTTP_GET, .handler = text_get_handler };
static const httpd_uri_t robots_uri = { .uri = "/robots.txt", .method = HTTP_GET, .handler = text_get_handler };

/* ---- THE CONVERSION LEDGER ----------------------------------------------
 * Six links on the page matter more than the page: four foundation buttons,
 * the direct-trust Monzo link, the public nomenclature sheet. Each carries
 * data-c="<slug>" and a click fires sendBeacon POST /c?k=<slug> at the same
 * origin - counted here, kept in NVS, summed across the fleet by
 * tools/conversions.sh. No third party ever sees a visitor.
 *
 * /c is public and unauthenticated (it must be - browsers call it), so its
 * blast radius is bounded: six fixed slugs, counters saturate, and NVS is
 * committed at most once per slug per minute - a hostile click loop can lie
 * about generosity but cannot wear out the flash. RAM is the truth between
 * commits; a power cut forfeits at most one minute of one slug's clicks. */
#define CLICK_KINDS SERVE_CLICK_KINDS
static const char *CLICK_SLUG[CLICK_KINDS] =
    { "savelife", "prytula", "hospitallers", "u24", "trust", "nomenclature" };
static uint32_t s_clicks[CLICK_KINDS];
static int64_t  s_click_commit_us[CLICK_KINDS];
static nvs_handle_t s_click_nvs;

const uint32_t *serve_click_counts(void) { return s_clicks; }

static void clicks_load(void)
{
    if (nvs_open("clicks", NVS_READWRITE, &s_click_nvs) != ESP_OK) {
        s_click_nvs = 0;
        return;
    }
    for (int i = 0; i < CLICK_KINDS; i++) {
        nvs_get_u32(s_click_nvs, CLICK_SLUG[i], &s_clicks[i]);
    }
}

static esp_err_t click_post_handler(httpd_req_t *req)
{
    char q[48], k[24] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "k", k, sizeof(k));
    }
    for (int i = 0; i < CLICK_KINDS; i++) {
        if (strcmp(k, CLICK_SLUG[i]) == 0) {
            if (s_clicks[i] != UINT32_MAX) s_clicks[i]++;
            int64_t now = esp_timer_get_time();
            if (s_click_nvs && now - s_click_commit_us[i] > 60 * 1000000LL) {
                s_click_commit_us[i] = now;
                nvs_set_u32(s_click_nvs, CLICK_SLUG[i], s_clicks[i]);
                nvs_commit(s_click_nvs);
            }
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
static const httpd_uri_t click_beacon = { .uri = "/c", .method = HTTP_POST, .handler = click_post_handler };

/* The whole fleet's tallies from any one blade, admin plane. Peers arrive by
 * heartbeat (v16), self is live - the splicer scatters each visitor's beacons
 * across backends, so per-blade rows AND the sum are both told here. */
static esp_err_t clicks_admin_handler(httpd_req_t *req)
{
    static char b[1024];   /* admin instance = its own task, same as roster */
    swarm_snap_entry_t e[8];
    int n = swarm_snapshot(e, 8, NULL);
    uint32_t sum[CLICK_KINDS] = {0};
    int p = snprintf(b, sizeof(b), "{\"blades\":{");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!e[i].eligible) continue;   /* observers never count clicks */
        p += snprintf(b + p, sizeof(b) - p, "%s\"%s\":{",
                      first ? "" : ",", e[i].shortname ? e[i].shortname : e[i].name);
        first = false;
        for (int k = 0; k < CLICK_KINDS; k++) {
            uint32_t v = e[i].self ? s_clicks[k] : e[i].clicks[k];
            sum[k] += v;
            p += snprintf(b + p, sizeof(b) - p, "%s\"%s\":%lu", k ? "," : "",
                          CLICK_SLUG[k], (unsigned long)v);
        }
        p += snprintf(b + p, sizeof(b) - p, "}");
    }
    p += snprintf(b + p, sizeof(b) - p, "},\"fleet\":{");
    for (int k = 0; k < CLICK_KINDS; k++) {
        p += snprintf(b + p, sizeof(b) - p, "%s\"%s\":%lu", k ? "," : "",
                      CLICK_SLUG[k], (unsigned long)sum[k]);
    }
    p += snprintf(b + p, sizeof(b) - p, "}}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, b, p);
    return ESP_OK;
}
static const httpd_uri_t clicks_admin = { .uri = "/clicks", .method = HTTP_GET, .handler = clicks_admin_handler };

/* The same view, plus each node's address - admin plane only. Port 8443 is never
 * forwarded, so this stays on the LAN by construction; the public /roster on 443
 * remains scrubbed. Lets tooling ask the fleet where its members are. */
static esp_err_t roster_admin_handler(httpd_req_t *req)
{
    /* 4 KB, not 3: the admin view carries ip + boot cause + two heap figures per
     * member on top of everything the public one has, and RAPP truncates silently -
     * an undersized buffer here emits JSON that parses nowhere. Transient, and the
     * admin plane is queried by hand, not by every open browser tab. */
    static char buf[4096];   /* admin instance = its own task; see the note above */
    int n = swarm_roster_json_ex(buf, sizeof(buf), true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t r = httpd_resp_send(req, buf, n);
    return r;
}
static const httpd_uri_t roster_admin = { .uri = "/roster", .method = HTTP_GET, .handler = roster_admin_handler };
static esp_err_t bbx_dump_handler(httpd_req_t *req)   { return blackbox_http_dump((struct httpd_req *)req); }
static esp_err_t bbx_core_handler(httpd_req_t *req)   { return blackbox_http_coredump((struct httpd_req *)req); }
static const httpd_uri_t bbx_uri  = { .uri = "/blackbox", .method = HTTP_GET, .handler = bbx_dump_handler };
static const httpd_uri_t core_uri = { .uri = "/coredump", .method = HTTP_GET, .handler = bbx_core_handler };
static const httpd_uri_t cpu_uri  = { .uri = "/cpu", .method = HTTP_GET, .handler = cpustat_http };

/* Zero every counter. Admin plane only (8443, never forwarded) and key-gated like
 * /ota - this rewrites the node's history, so it wants the same door. */
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    char key[BOHUN_KEY_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Bohun-Key", key, sizeof(key)) != ESP_OK
        || !ota_key_ok(key)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "bad key\n");
        return ESP_FAIL;
    }
    swarm_reset_stats();
    httpd_resp_sendstr(req, "stats wiped\n");
    return ESP_OK;
}
static const httpd_uri_t reset_uri = { .uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler };

/* POST /standby?s=NN - stand this blade down for NN seconds (see the note on
 * s_standby_until_us). Same door and same key as /ota: it can move the mask. */
static esp_err_t standby_post_handler(httpd_req_t *req)
{
    char key[BOHUN_KEY_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Bohun-Key", key, sizeof(key)) != ESP_OK
        || !ota_key_ok(key)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "bad key\n");
        return ESP_FAIL;
    }
    int secs = 30;
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) {
            secs = atoi(v);
        }
    }
    if (secs < 0)              secs = 0;              /* 0 = come back now */
    if (secs > STANDBY_MAX_S)  secs = STANDBY_MAX_S;
    s_standby_until_us = secs ? esp_timer_get_time() + (int64_t)secs * 1000000 : 0;
    ESP_LOGW(TAG, "STANDBY for %d s - reporting not-ready, dropping the mask if held", secs);
    blackbox_event(BBX_SPLICE, (uint16_t)secs, "drill: standby %d s", secs);
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "standby %d s\n", secs);
    httpd_resp_send(req, msg, n);
    return ESP_OK;
}
static const httpd_uri_t standby_uri = { .uri = "/standby", .method = HTTP_POST, .handler = standby_post_handler };

/* ---- port 80: nothing lives here but a signpost to TLS -------------------- */
/* The only hosts we will ever redirect to. A reflected Host header would make this
 * an open redirect, and a hardcoded LAN fallback would leak the private address to
 * any Host-less HTTP/1.0 probe - so the destination is ours or the request is refused. */
static const char *const REDIRECT_HOSTS[] = {
    "kyrylonovotarskyi.com",
    "www.kyrylonovotarskyi.com",
};

static esp_err_t redirect_handler(httpd_req_t *req)
{
    char host[128] = "";
    (void)httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';        /* drop :80 - https uses its own default */

    const char *dest = NULL;
    for (size_t i = 0; i < sizeof(REDIRECT_HOSTS) / sizeof(REDIRECT_HOSTS[0]); i++) {
        if (strcasecmp(host, REDIRECT_HOSTS[i]) == 0) {
            dest = REDIRECT_HOSTS[i];
            break;
        }
    }
    /* Also honour a private-range literal the client typed itself. Echoing
     * back an address the requester already knows discloses nothing; a
     * hardcoded fallback answering a Host-less probe would. This keeps a typed
     * private literal working on the bench without opening that hole. */
    if (!dest && bohun_is_private_literal(host)) {
        dest = host;
    }
    if (!dest) {
        /* unknown or absent Host: say nothing about who or where we are */
        tally(HTTPD_400_BAD_REQUEST);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "bad host\n");
        return ESP_FAIL;   /* close it rather than reading whatever they want to send */
    }

    char loc[704];   /* "https://" + host[128] + uri (URI cap is 512) + NUL */
    snprintf(loc, sizeof(loc), "https://%s%s", dest, req->uri);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}
/* every error httpd raises on its own (404, 405, 408, 414, 431, 500...) is a request
 * we failed to serve - availability must count them, not just the ones we refuse by hand */
static uint16_t s_err_kind[SERVE_ERR_KINDS];   /* which refusals, not just how many */
static bool s_ready;                           /* the public TLS server is listening */


bool serve_is_ready(void)
{
    if (s_standby_until_us && esp_timer_get_time() < s_standby_until_us) {
        return false;
    }
    /* The self-probe overrides the boot latch. A blade whose own port refuses
     * its own loopback has no business being elected or handed traffic, however
     * cleanly it started. This is the line that stops the fleet trusting a
     * corpse's word for it. */
    if (s_probe_fails >= PROBE_FAIL_SICK) {
        return false;
    }
    return s_ready;
}
bool serve_is_real(void) { return true; }   /* this build has a real server */
const uint16_t *serve_err_counts(void) { return s_err_kind; }
void serve_reset_errors(void) { memset(s_err_kind, 0, sizeof(s_err_kind)); }

/* Which of these are OUR fault? Only the 5xx family. A 404 or a 405 is the server
 * doing its job correctly - telling a scanner that /wp-login.php does not exist is
 * a success, not an outage. Availability must count our failures, not their bad
 * manners, or internet background noise makes a healthy fleet look broken. */
static bool err_is_ours(httpd_err_code_t e)
{
    return e == HTTPD_500_INTERNAL_SERVER_ERROR
        || e == HTTPD_501_METHOD_NOT_IMPLEMENTED
        || e == HTTPD_505_VERSION_NOT_SUPPORTED;
}

/* Three httpd instances on two cores raise errors here concurrently; the
 * short critical section keeps the availability ledger from dropping
 * increments. */
static portMUX_TYPE s_tally_mux = portMUX_INITIALIZER_UNLOCKED;

static void tally(httpd_err_code_t err)
{
    bool ours = err_is_ours(err);
    portENTER_CRITICAL(&s_tally_mux);
    if ((int)err >= 0 && (int)err < SERVE_ERR_KINDS && s_err_kind[err] < 0xffff) {
        s_err_kind[err]++;
    }
    portEXIT_CRITICAL(&s_tally_mux);
    swarm_record_refusal(ours);
}

static esp_err_t counted_err(httpd_req_t *req, httpd_err_code_t err)
{
    tally(err);
    if (err_is_ours(err)) {
        /* a 5xx is OUR failure - remember the request it happened to, and
         * the heap truth of the moment. This is what makes a lone 500 in a
         * day of clean serving explainable. */
        blackbox_event(BBX_ERR5XX, (uint16_t)(err == HTTPD_500_INTERNAL_SERVER_ERROR ? 500 :
                                              err == HTTPD_501_METHOD_NOT_IMPLEMENTED ? 501 : 505),
                       "%s", req->uri);
    }
    return httpd_resp_send_err(req, err, NULL);
}

static void register_err_counters(httpd_handle_t h)
{
    for (int e = 0; e < HTTPD_ERR_CODE_MAX; e++) {
        httpd_register_err_handler(h, (httpd_err_code_t)e, counted_err);
    }
}

static const httpd_uri_t redirect_all = { .uri = "*", .method = HTTP_GET, .handler = redirect_handler };
static const httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler };

/* THE PORT SPLIT: 443 is the public face and the only forwarded port - /ota
 * is not registered there, so from the WAN the update door is ABSENT, not
 * 403. 8443 is the admin face, never forwarded. Reachability reduction beats
 * request inspection. */
#define OTA_ADMIN_PORT 8443

/* set on the raw socket before the TLS handshake begins (https_server
 * forwards its config's open_fn to the underlying httpd session-open) */
static esp_err_t nodelay_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

static void ssl_common(httpd_ssl_config_t *c)
{
    c->servercert = servercert_start;
    c->servercert_len = servercert_end - servercert_start;
    c->prvtkey_pem = prvtkey_start;
    c->prvtkey_len = prvtkey_end - prvtkey_start;
    /* hardening v0 (F2): don't let a slow client squat a connection */
    c->httpd.recv_wait_timeout = 5;
    c->httpd.send_wait_timeout = 5;
    c->httpd.lru_purge_enable = true;
}

void serve_start(void)
{
    /* ---- 443: the public face, no /ota ---- */
    httpd_ssl_config_t pub = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_common(&pub);
    /* configurable so the splicer can own :443 while every blade's TLS
     * server listens on the backend port (see Kconfig: the port split) */
    pub.port_secure = CONFIG_BOHUN_PUBLIC_PORT;
    /* TCP_NODELAY: sender Nagle x an lwIP peer's 250 ms delayed ACK
     * quantizes every spliced flight. Mandatory behind the
     * splicer, harmless for direct visitors. */
    pub.httpd.open_fn = nodelay_open;
    /* Handshakes run inline in this task: one silent connection stalls every
     * new visitor for the whole timeout, so it is 1 s here, floored by
     * recv_wait_timeout (both must move together). Admin keeps 5 s - ota.c
     * treats a recv timeout as fatal mid-release. */
    /* 10 s, NOT 1 s: behind the splicer a queued handshake exceeds 1 s of
     * wall clock at near-zero CPU, and killing it mid-flight wastes the
     * visitor AND the splice slot. The squat defence moved to the
     * splicer; this port is LAN-only. */
    pub.tls_handshake_timeout_ms = 10000;
    /* PIN THE SERVER TO CORE 1. Unpinned it lands on core 0 beside WiFi,
     * lwIP, the event loop and the system tasks - /cpu under load shows it at
     * 89.8% of one core with the other 69.9% idle. This does not make the
     * server parallel - esp_http_server is one task by design - it stops the
     * one task we have from fighting the system for a core. */
    pub.httpd.core_id = 1;

    /* Keep-alive is worth 3.6x (a handshake dwarfs serving). Measured traps:
     * raising recv_wait_timeout buys keep-alive NOTHING (idle reuse rides the
     * select loop) and regresses the squat defence - it stays at 1 s. The
     * real lever is socket count (PSRAM made 8 affordable). Re-run the squat
     * test after changing either number. */
    pub.httpd.recv_wait_timeout  = 5;   /* internal port now - see the timeout note above */
    pub.httpd.max_open_sockets   = 8;
    /* TLS SESSION TICKETS. HTTPD_SSL_CONFIG_DEFAULT ships this false, and
     * without it EVERY visitor - returning ones included - pays the full
     * ~500 ms P-256 handshake (measured: session_reused=false, always). */
    pub.session_tickets = true;

    httpd_handle_t server = NULL;
    if (httpd_ssl_start(&server, &pub) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &favicon);
        httpd_register_uri_handler(server, &roster);
        httpd_register_uri_handler(server, &og_uri);
        httpd_register_uri_handler(server, &llms_uri);
        httpd_register_uri_handler(server, &robots_uri);
        clicks_load();
        httpd_register_uri_handler(server, &click_beacon);
        register_err_counters(server);
        s_ready = true;   /* only now is claiming the mask honest */
        ESP_LOGI(TAG, "public: / /roster over TLS on port %d (no /ota here)", pub.port_secure);
    } else {
        ESP_LOGE(TAG, "httpd_ssl_start (public) failed");
    }

    /* ---- 8443: the admin face, /ota only, LAN-only by never being forwarded ---- */
    httpd_ssl_config_t adm = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_common(&adm);
    adm.port_secure = OTA_ADMIN_PORT;
    adm.httpd.server_port = OTA_ADMIN_PORT;
    /* the ctrl socket must NOT collide with the public server's: the SSL
     * default is already ESP_HTTPD_DEF_CTRL_PORT+1, so the admin instance
     * takes +2. Getting this wrong costs the node its OTA door. */
    adm.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2;
    adm.httpd.max_open_sockets = 2;

    httpd_handle_t admin = NULL;
    if (httpd_ssl_start(&admin, &adm) == ESP_OK) {
        httpd_register_uri_handler(admin, &ota);
        httpd_register_uri_handler(admin, &roster_admin);
        httpd_register_uri_handler(admin, &reset_uri);
        httpd_register_uri_handler(admin, &standby_uri);
        xTaskCreate(probe_task, "serve_probe", 3072, NULL, 4, NULL);
        httpd_register_uri_handler(admin, &bbx_uri);    /* the flight recorder */
        httpd_register_uri_handler(admin, &core_uri);   /* raw panic core, if any */
        httpd_register_uri_handler(admin, &cpu_uri);    /* where the CPU actually goes */
        httpd_register_uri_handler(admin, &clicks_admin); /* this blade's conversion tallies */
        register_err_counters(admin);
        cpustat_start();
        ESP_LOGI(TAG, "admin: /ota over TLS on port %d - NEVER forward this port", OTA_ADMIN_PORT);
    } else {
        ESP_LOGE(TAG, "httpd_ssl_start (admin/ota) failed - fleet is un-updatable, fix before reboot");
    }

    /* port 80 exists only to say "use 443". ctrl ports: 443 uses +1, admin +2 -
     * this one gets +3, because two servers sharing a ctrl port die silently (ask N4) */
    httpd_handle_t plain = NULL;
    httpd_config_t red = HTTPD_DEFAULT_CONFIG();
    red.server_port = 80;
    red.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 3;
    red.uri_match_fn = httpd_uri_match_wildcard;
    red.max_uri_handlers = 1;
    red.max_open_sockets = 2;         /* a redirect is one-shot; the default 7 blew the
                                       * LWIP socket pool and wedged the mask wearer's TLS */
    if (httpd_start(&plain, &red) == ESP_OK) {
        httpd_register_uri_handler(plain, &redirect_all);
        register_err_counters(plain);
        ESP_LOGI(TAG, "port 80: 301 to https, nothing else served");
    } else {
        ESP_LOGW(TAG, "port 80 redirect failed to start (non-fatal)");
    }
}
