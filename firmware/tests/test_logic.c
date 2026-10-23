/* test_logic.c - host tests for the swarm's pure decision kernels (logic.h).
 *
 * Runs on the development machine, not on a board: tools/run_tests.sh compiles
 * this against ESP-IDF's vendored Unity with ASan/UBSan on and executes it.
 * Everything probabilistic about the fleet reduces to these rules - who leads,
 * who counts as alive, what availability means, when a backend is benched, how
 * a clamped JSON document is repaired - so this is where regressions in the
 * rules are caught before an image ever reaches the rack. The serving path
 * itself is proven on-device: the D8 boot trial and tools/fleet_check.sh.
 */
#include "unity.h"
#include "logic.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- liveness ---------------------------------------------------------- */

static void test_alive_never_heard(void)
{
    TEST_ASSERT_FALSE(bohun_peer_alive_ms(0, 123456, 3000));
}

static void test_alive_fresh_and_stale(void)
{
    TEST_ASSERT_TRUE(bohun_peer_alive_ms(10000, 12999, 3000));   /* 2999 ms ago */
    TEST_ASSERT_FALSE(bohun_peer_alive_ms(10000, 13000, 3000));  /* exactly the window */
    TEST_ASSERT_FALSE(bohun_peer_alive_ms(10000, 99999, 3000));
}

static void test_alive_survives_49_day_wrap(void)
{
    /* last heard just before the uint32 ms counter wrapped; now just after */
    uint32_t last = 0xFFFFFF00u;
    uint32_t now  = 0x00000100u;   /* 512 ms later through the wrap */
    TEST_ASSERT_TRUE(bohun_peer_alive_ms(last, now, 3000));
}

/* ---- availability ------------------------------------------------------ */

static void test_avail_no_traffic_is_100(void)
{
    TEST_ASSERT_EQUAL_UINT16(1000, bohun_avail_x10(0, 0, 0));
}

static void test_avail_scanner_noise_does_not_count(void)
{
    /* 9 served, 1 refused with a 404 - their bad question, our correct answer */
    TEST_ASSERT_EQUAL_UINT16(1000, bohun_avail_x10(9, 1, 0));
}

static void test_avail_our_5xx_counts(void)
{
    /* 9 served, 1 refused with a 500 - one in ten failed on our account */
    TEST_ASSERT_EQUAL_UINT16(900, bohun_avail_x10(9, 1, 1));
}

static void test_avail_64bit_intermediate(void)
{
    /* (total - ours) * 1000 overflows 32 bits at ~4.3M requests - prove the
     * arithmetic is done wide */
    TEST_ASSERT_EQUAL_UINT16(1000, bohun_avail_x10(10000000, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(900, bohun_avail_x10(9000000, 1000000, 1000000));
}

static void test_avail_skewed_gossip_clamps(void)
{
    /* counters gossiped in separate frames can skew: 5xx > total must clamp
     * to 0.0%, never wrap */
    TEST_ASSERT_EQUAL_UINT16(0, bohun_avail_x10(1, 0, 5));
}

/* ---- heartbeat delivery ------------------------------------------------ */

static void test_delivery_perfect(void)
{
    TEST_ASSERT_EQUAL_UINT32(100, bohun_delivery_pct(1, 10, 10));
}

static void test_delivery_half(void)
{
    TEST_ASSERT_EQUAL_UINT32(50, bohun_delivery_pct(1, 10, 5));
}

static void test_delivery_reboot_resets_window(void)
{
    /* last < first = the sender rebooted; a fresh window has nothing to hold
     * against it */
    TEST_ASSERT_EQUAL_UINT32(100, bohun_delivery_pct(500, 3, 1));
}

static void test_delivery_never_over_100(void)
{
    TEST_ASSERT_EQUAL_UINT32(100, bohun_delivery_pct(5, 6, 99));
}

/* ---- seq gap ----------------------------------------------------------- */

static void test_gap_first_frame_and_consecutive(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, bohun_seq_gap(0, 7));     /* first ever heard */
    TEST_ASSERT_EQUAL_UINT8(0, bohun_seq_gap(7, 8));     /* consecutive */
}

static void test_gap_counts_missed(void)
{
    TEST_ASSERT_EQUAL_UINT8(5, bohun_seq_gap(10, 16));
}

static void test_gap_caps_at_99(void)
{
    TEST_ASSERT_EQUAL_UINT8(99, bohun_seq_gap(1, 5000));
}

static void test_gap_reboot_is_not_a_gap(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, bohun_seq_gap(5000, 3));  /* seq went backwards */
}

/* ---- percentiles ------------------------------------------------------- */

static void test_pctl_empty_and_single(void)
{
    uint32_t one[1] = { 42 };
    TEST_ASSERT_EQUAL_UINT32(0, bohun_pctl(one, 0, 50));
    TEST_ASSERT_EQUAL_UINT32(42, bohun_pctl(one, 1, 95));
}

static void test_pctl_nearest_rank(void)
{
    uint32_t w[5] = { 50, 10, 40, 20, 30 };   /* unsorted on purpose */
    TEST_ASSERT_EQUAL_UINT32(30, bohun_pctl(w, 5, 50));
    uint32_t w2[5] = { 50, 10, 40, 20, 30 };
    TEST_ASSERT_EQUAL_UINT32(10, bohun_pctl(w2, 5, 0));
    uint32_t w3[5] = { 50, 10, 40, 20, 30 };
    TEST_ASSERT_EQUAL_UINT32(50, bohun_pctl(w3, 5, 100));
}

static void test_pctl_p95_of_100(void)
{
    uint32_t w[100];
    for (uint32_t i = 0; i < 100; i++) {
        w[i] = 99 - i;                        /* descending 99..0 */
    }
    /* nearest rank over 0..99: index (95 * 99) / 100 = 94 */
    TEST_ASSERT_EQUAL_UINT32(94, bohun_pctl(w, 100, 95));
}

/* ---- the election ------------------------------------------------------ */

static void test_elect_nobody(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, bohun_elect(3, false, NULL, 0));
}

static void test_elect_self_alone(void)
{
    TEST_ASSERT_EQUAL_UINT8(3, bohun_elect(3, true, NULL, 0));
}

static void test_elect_lowest_id_wins(void)
{
    bohun_cand_t peers[] = {
        { .id = 2, .eligible = true, .alive = true, .able = true },
        { .id = 1, .eligible = true, .alive = true, .able = true },
    };
    TEST_ASSERT_EQUAL_UINT8(1, bohun_elect(3, true, peers, 2));
}

static void test_elect_witness_never_wins(void)
{
    bohun_cand_t peers[] = {
        { .id = 6, .eligible = false, .alive = true, .able = true },
    };
    TEST_ASSERT_EQUAL_UINT8(3, bohun_elect(3, true, peers, 1));
    TEST_ASSERT_EQUAL_UINT8(0, bohun_elect(3, false, peers, 1));
}

static void test_elect_dead_or_unable_peers_skipped(void)
{
    bohun_cand_t peers[] = {
        { .id = 1, .eligible = true, .alive = false, .able = true },   /* radio-silent */
        { .id = 2, .eligible = true, .alive = true,  .able = false },  /* link down or not serving */
    };
    TEST_ASSERT_EQUAL_UINT8(4, bohun_elect(4, true, peers, 2));
}

static void test_elect_preemption(void)
{
    /* a returned lower id takes the mask back - deliberately no stickiness */
    bohun_cand_t peers[] = {
        { .id = 1, .eligible = true, .alive = true, .able = true },
    };
    TEST_ASSERT_EQUAL_UINT8(1, bohun_elect(2, true, peers, 1));
}

static void test_elect_self_unfit_peer_leads(void)
{
    bohun_cand_t peers[] = {
        { .id = 4, .eligible = true, .alive = true, .able = true },
    };
    TEST_ASSERT_EQUAL_UINT8(4, bohun_elect(1, false, peers, 1));
}

/* ---- the bench --------------------------------------------------------- */

static void test_bench_fresh_offender_needs_streak(void)
{
    TEST_ASSERT_EQUAL_UINT8(3, bohun_bench_threshold(0, 3));
}

static void test_bench_repeat_offender_rebenches_on_one(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, bohun_bench_threshold(15000, 3));
}

static void test_bench_backoff_doubles_and_caps(void)
{
    TEST_ASSERT_EQUAL_UINT32(15000, bohun_bench_next_ms(0, 15000, 240000));
    TEST_ASSERT_EQUAL_UINT32(30000, bohun_bench_next_ms(15000, 15000, 240000));
    TEST_ASSERT_EQUAL_UINT32(240000, bohun_bench_next_ms(200000, 15000, 240000));
}

/* ---- Accept-Encoding parsing ------------------------------------------- */

static void test_br_token_present(void)
{
    TEST_ASSERT_TRUE(bohun_accepts_br("br"));
    TEST_ASSERT_TRUE(bohun_accepts_br("gzip, br"));
    TEST_ASSERT_TRUE(bohun_accepts_br("gzip,br;q=1.0, deflate"));
    TEST_ASSERT_TRUE(bohun_accepts_br("br;q=0"));   /* q-values ignored by design */
}

static void test_br_token_absent(void)
{
    TEST_ASSERT_FALSE(bohun_accepts_br(""));
    TEST_ASSERT_FALSE(bohun_accepts_br("gzip, deflate"));
    TEST_ASSERT_FALSE(bohun_accepts_br("brotli"));       /* not the "br" token */
    TEST_ASSERT_FALSE(bohun_accepts_br("abr, sbr"));     /* substrings never match */
}

/* ---- private-literal detection ----------------------------------------- */

static void test_private_ranges(void)
{
    TEST_ASSERT_TRUE(bohun_is_private_literal("10.7.13.42"));
    TEST_ASSERT_TRUE(bohun_is_private_literal("172.16.200.9"));
    TEST_ASSERT_TRUE(bohun_is_private_literal("172.31.255.255"));
    TEST_ASSERT_TRUE(bohun_is_private_literal("192.168.77.204"));
}

static void test_public_and_malformed_refused(void)
{
    TEST_ASSERT_FALSE(bohun_is_private_literal("8.8.8.8"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("172.15.200.9"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("172.32.200.9"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("192.169.77.204"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("256.168.77.204"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("192.168.77.204x"));  /* trailing junk */
    TEST_ASSERT_FALSE(bohun_is_private_literal("10.7.13"));
    TEST_ASSERT_FALSE(bohun_is_private_literal("evil.example"));
}

/* ---- Origin matching --------------------------------------------------- */

static void test_origin_matches_own_host(void)
{
    TEST_ASSERT_TRUE(bohun_origin_host_ok("https://example.org", "example.org"));
    TEST_ASSERT_TRUE(bohun_origin_host_ok("https://example.org:8443", "example.org"));
    TEST_ASSERT_TRUE(bohun_origin_host_ok("example.org/path", "example.org"));
}

static void test_origin_prefix_attack_refused(void)
{
    /* "example.org.evil.com" begins with our host but is another site */
    TEST_ASSERT_FALSE(bohun_origin_host_ok("https://example.org.evil.com", "example.org"));
    TEST_ASSERT_FALSE(bohun_origin_host_ok("https://other.example", "example.org"));
    TEST_ASSERT_FALSE(bohun_origin_host_ok("https://example.org", ""));
}

/* ---- clamped-JSON repair ----------------------------------------------- */

static void test_json_repair_mid_member(void)
{
    char b[] = "{\"m\":[{\"a\":1},{\"b\":";      /* clamp landed mid-object */
    size_t n = bohun_json_close_clamped(b, sizeof(b) - 1);
    TEST_ASSERT_EQUAL_size_t(15, n);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"m\":[{\"a\":1}]}", b, n);
    TEST_ASSERT_EQUAL_CHAR('\n', b[n]);          /* trailing newline + NUL follow */
    TEST_ASSERT_EQUAL_CHAR('\0', b[n + 1]);
}

static void test_json_repair_boundary_refuses(void)
{
    /* the last '}' sits at len-4: the 4-byte "]}\n"+NUL would land one past
     * the buffer - the repair must refuse, never write out of bounds */
    char b[] = "xx}yyy";
    TEST_ASSERT_EQUAL_size_t(0, bohun_json_close_clamped(b, 6));
    TEST_ASSERT_EQUAL_STRING("xx}yyy", b);       /* untouched */
}

static void test_json_repair_boundary_exact_fit(void)
{
    /* '}' at len-5 is the tightest repair that fits */
    char b[] = "x}yyyy";
    size_t n = bohun_json_close_clamped(b, 6);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING_LEN("x}]}", b, n);
}

static void test_json_repair_no_object_refuses(void)
{
    char b[] = "abcdefgh";
    TEST_ASSERT_EQUAL_size_t(0, bohun_json_close_clamped(b, 8));
}

/* ---- self-probe --------------------------------------------------------- */

static void test_probe_verdict_busy_is_not_dead(void)
{
    TEST_ASSERT_EQUAL(BOHUN_PROBE_OK, bohun_probe_verdict(true, 0));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_BUSY, bohun_probe_verdict(false, ENOBUFS));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_BUSY, bohun_probe_verdict(false, ENOMEM));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_BUSY, bohun_probe_verdict(false, ENFILE));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_BUSY, bohun_probe_verdict(false, EMFILE));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_BUSY, bohun_probe_verdict(false, EAGAIN));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_DEAD, bohun_probe_verdict(false, ECONNREFUSED));
    TEST_ASSERT_EQUAL(BOHUN_PROBE_DEAD, bohun_probe_verdict(false, ETIMEDOUT));
}

static void test_probe_streak_busy_holds_dead_grows_ok_clears(void)
{
    uint8_t f = 0;
    f = bohun_probe_streak(f, BOHUN_PROBE_DEAD);
    TEST_ASSERT_EQUAL_UINT8(1, f);
    f = bohun_probe_streak(f, BOHUN_PROBE_BUSY);   /* a full pool is not a verdict */
    TEST_ASSERT_EQUAL_UINT8(1, f);
    f = bohun_probe_streak(f, BOHUN_PROBE_DEAD);
    TEST_ASSERT_EQUAL_UINT8(2, f);
    f = bohun_probe_streak(f, BOHUN_PROBE_OK);
    TEST_ASSERT_EQUAL_UINT8(0, f);
    TEST_ASSERT_EQUAL_UINT8(255, bohun_probe_streak(255, BOHUN_PROBE_DEAD));
}

static void test_probe_fit_hold_down_after_stand_down(void)
{
    TEST_ASSERT_TRUE(bohun_probe_fit(0, 2, 0, 100000, 60000));       /* never stood down */
    TEST_ASSERT_TRUE(bohun_probe_fit(1, 2, 0, 100000, 60000));       /* one blip is not sick */
    TEST_ASSERT_FALSE(bohun_probe_fit(2, 2, 100000, 100000, 60000)); /* sick right now */
    /* recovered 7 s after standing down - the old flap - still held */
    TEST_ASSERT_FALSE(bohun_probe_fit(0, 2, 100000, 107000, 60000));
    TEST_ASSERT_FALSE(bohun_probe_fit(0, 2, 100000, 159999, 60000));
    TEST_ASSERT_TRUE(bohun_probe_fit(0, 2, 100000, 160000, 60000));  /* the hold is up */
}

static void test_probe_fit_survives_49_day_wrap(void)
{
    uint32_t stood = 0xFFFFF000u;
    TEST_ASSERT_FALSE(bohun_probe_fit(0, 2, stood, 0x00000100u, 60000)); /* 4.3 s later, through the wrap */
    TEST_ASSERT_TRUE(bohun_probe_fit(0, 2, stood, 0x0000F000u, 60000));  /* 65.5 s later */
}

static void test_probe_room_leaves_two_blocks_spare(void)
{
    TEST_ASSERT_TRUE(bohun_probe_room(0, 16));
    TEST_ASSERT_TRUE(bohun_probe_room(6, 16));    /* 12 + the probe's 2 + 2 spare */
    TEST_ASSERT_FALSE(bohun_probe_room(7, 16));   /* 14: the probe would take the last block */
    TEST_ASSERT_FALSE(bohun_probe_room(8, 16));   /* 16: socket() itself fails */
}

static void test_pair_silent_client_never_spoke(void)
{
    TEST_ASSERT_FALSE(bohun_pair_silent(false, 1000, 8999, 8000));  /* still inside the window */
    TEST_ASSERT_TRUE(bohun_pair_silent(false, 1000, 9001, 8000));   /* a scanner holding a slot */
    TEST_ASSERT_FALSE(bohun_pair_silent(true, 1000, 999999, 8000)); /* it spoke: the idle rule owns it */
}

/* ---- refusal sampler ---------------------------------------------------- */

static void test_refusal_clip_terminates_and_fills_blanks(void)
{
    char d[8];
    bohun_refusal_clip(d, sizeof(d), NULL);
    TEST_ASSERT_EQUAL_STRING("-", d);
    bohun_refusal_clip(d, sizeof(d), "");
    TEST_ASSERT_EQUAL_STRING("-", d);
    bohun_refusal_clip(d, sizeof(d), "abcdefg");       /* exact fit */
    TEST_ASSERT_EQUAL_STRING("abcdefg", d);
    bohun_refusal_clip(d, sizeof(d), "abcdefghijkl");  /* clipped to cap-1 */
    TEST_ASSERT_EQUAL_STRING("abcdefg", d);
}

static void test_refusal_record_clips_every_field(void)
{
    static bohun_refusals_t r;
    memset(&r, 0, sizeof(r));
    char longuri[101];
    memset(longuri, 'a', 100);
    longuri[100] = '\0';
    bohun_refusal_record(&r, 10, 0, 80, 404, false, "", longuri, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, r.total);
    TEST_ASSERT_EQUAL_STRING("-", r.rec[0].method);
    TEST_ASSERT_EQUAL_STRING("-", r.rec[0].host);
    TEST_ASSERT_EQUAL_size_t(BOHUN_REFUSAL_URI - 1, strlen(r.rec[0].uri));
}

static void test_refusal_dump_line_format(void)
{
    static bohun_refusals_t r;
    memset(&r, 0, sizeof(r));
    bohun_refusal_record(&r, 3661234, 0xCB007107u /* 203.0.113.7 */, 80, 400, true,
                         "GET", "/", "203.0.113.9");
    char out[512];
    size_t n = bohun_refusal_dump(&r, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
    TEST_ASSERT_EQUAL_STRING_LEN("refusals total=1 showing=1 (", out, 28);
    TEST_ASSERT_NOT_NULL(strstr(out, "\n#1 +3661.234s :80 203.0.113.7 400 bad-host GET / host=203.0.113.9\n"));
}

static void test_refusal_dump_empty_is_header_only(void)
{
    static bohun_refusals_t r;
    memset(&r, 0, sizeof(r));
    char out[256];
    size_t n = bohun_refusal_dump(&r, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("refusals total=0 showing=0 (", out, 28);
    TEST_ASSERT_NULL(strchr(out, '#'));
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
}

static void test_refusal_ring_wraps_newest_first(void)
{
    static bohun_refusals_t r;
    memset(&r, 0, sizeof(r));
    for (unsigned i = 1; i <= BOHUN_REFUSAL_N + 1; i++) {
        bohun_refusal_record(&r, i, 0x0A000001u, 8080, 404, false, "GET", "/x", "h");
    }
    TEST_ASSERT_EQUAL_UINT32(BOHUN_REFUSAL_N + 1, r.total);
    char out[4096];
    bohun_refusal_dump(&r, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "showing=32 ("));
    TEST_ASSERT_NOT_NULL(strstr(out, "\n#33 +"));          /* newest, right after the header */
    TEST_ASSERT_NOT_NULL(strstr(out, "\n#2 +"));           /* the oldest survivor */
    TEST_ASSERT_NULL(strstr(out, "\n#1 +"));               /* overwritten by #33 */
    TEST_ASSERT_TRUE(strstr(out, "\n#33 +") < strstr(out, "\n#2 +"));
}

static void test_refusal_dump_truncates_on_a_line_boundary(void)
{
    static bohun_refusals_t r;
    memset(&r, 0, sizeof(r));
    bohun_refusal_record(&r, 1000, 0x0A000001u, 80, 404, false, "GET", "/aa", "h");
    bohun_refusal_record(&r, 2000, 0x0A000001u, 80, 404, false, "GET", "/bb", "h");
    char full[1024];
    size_t f = bohun_refusal_dump(&r, full, sizeof(full));
    char cut[1024];
    size_t c = bohun_refusal_dump(&r, cut, f);          /* one byte short of the whole text */
    TEST_ASSERT_TRUE(c < f);
    TEST_ASSERT_EQUAL_size_t(strlen(cut), c);
    TEST_ASSERT_EQUAL_CHAR('\n', cut[c - 1]);           /* ends on a whole line */
    TEST_ASSERT_NOT_NULL(strstr(cut, "\n#2 +"));        /* the newest survived */
    TEST_ASSERT_NULL(strstr(cut, "\n#1 +"));            /* the oldest was dropped whole */
}

/* ---- rate ring + counter deltas + the status table ---------------------- */

static void test_err_status_table(void)
{
    TEST_ASSERT_EQUAL_UINT16(500, bohun_err_status(0));
    TEST_ASSERT_EQUAL_UINT16(505, bohun_err_status(2));
    TEST_ASSERT_EQUAL_UINT16(400, bohun_err_status(3));
    TEST_ASSERT_EQUAL_UINT16(404, bohun_err_status(6));
    TEST_ASSERT_EQUAL_UINT16(431, bohun_err_status(12));
    TEST_ASSERT_EQUAL_UINT16(0, bohun_err_status(13));
}

static void test_delta_treats_reset_as_fresh_start(void)
{
    TEST_ASSERT_EQUAL_UINT32(7, bohun_delta(10, 3, true));
    TEST_ASSERT_EQUAL_UINT32(0, bohun_delta(2, 9, true));    /* ran backwards: a reboot */
    TEST_ASSERT_EQUAL_UINT32(0, bohun_delta(5, 0, false));   /* first reading: no rate yet */
}

static void test_rate_ring_rolls_once_per_minute(void)
{
    static bohun_rate_ring_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 59; i++) {
        TEST_ASSERT_FALSE(bohun_rate_add(&r, 2));
    }
    TEST_ASSERT_EQUAL_UINT16(0, bohun_rate_last_min(&r));
    TEST_ASSERT_TRUE(bohun_rate_add(&r, 2));
    TEST_ASSERT_EQUAL_UINT8(1, r.min_n);
    TEST_ASSERT_EQUAL_UINT16(120, bohun_rate_last_min(&r));
    TEST_ASSERT_EQUAL_UINT32(120, bohun_rate_last60(&r));
    TEST_ASSERT_EQUAL_UINT32(120, bohun_rate_hour(&r));
}

static void test_rate_ring_last60_slides(void)
{
    static bohun_rate_ring_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 60; i++) bohun_rate_add(&r, 1);
    TEST_ASSERT_EQUAL_UINT32(60, bohun_rate_last60(&r));
    for (int i = 0; i < 30; i++) bohun_rate_add(&r, 0);
    TEST_ASSERT_EQUAL_UINT32(30, bohun_rate_last60(&r));   /* half the window has aged out */
    TEST_ASSERT_EQUAL_UINT32(60, bohun_rate_hour(&r));     /* the hour still remembers all 60 */
}

static void test_rate_ring_saturates_instead_of_wrapping(void)
{
    static bohun_rate_ring_t r;
    memset(&r, 0, sizeof(r));
    bohun_rate_add(&r, 70000);
    TEST_ASSERT_EQUAL_UINT16(0xffff, r.sec[0]);
    for (int i = 0; i < 59; i++) bohun_rate_add(&r, 0);
    TEST_ASSERT_EQUAL_UINT16(0xffff, bohun_rate_last_min(&r));
    TEST_ASSERT_EQUAL_UINT32(0xffff, bohun_rate_hour(&r));
}

static void test_rate_ring_peak_forgets_after_an_hour(void)
{
    static bohun_rate_ring_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 60; i++) bohun_rate_add(&r, 5);     /* minute 1: 300, the peak */
    for (int m = 0; m < 59; m++) {
        for (int i = 0; i < 60; i++) bohun_rate_add(&r, 1); /* minutes 2..60: 60 each */
    }
    TEST_ASSERT_EQUAL_UINT8(60, r.min_n);
    TEST_ASSERT_EQUAL_UINT16(300, bohun_rate_peak(&r));
    for (int i = 0; i < 60; i++) bohun_rate_add(&r, 1);     /* minute 61 overwrites minute 1 */
    TEST_ASSERT_EQUAL_UINT8(60, r.min_n);
    TEST_ASSERT_EQUAL_UINT16(60, bohun_rate_peak(&r));
    TEST_ASSERT_EQUAL_UINT32(3600, bohun_rate_hour(&r));
}

/* ---- the fleet's clock ------------------------------------------------- */

static void test_time_sane_boundary(void)
{
    TEST_ASSERT_FALSE(bohun_time_sane(0));
    TEST_ASSERT_FALSE(bohun_time_sane(86400u * 365u));       /* an ESP32 a year after its 1970 boot */
    TEST_ASSERT_TRUE(bohun_time_sane(BOHUN_TIME_SANE_S));
    TEST_ASSERT_TRUE(bohun_time_sane(1760000000u));           /* 2025-10 */
}

static void test_time_adopt_unset_local_takes_a_sane_beacon(void)
{
    TEST_ASSERT_TRUE(bohun_time_adopt(1234, 1760000000u, 3));
    TEST_ASSERT_FALSE(bohun_time_adopt(1234, 5678, 3));       /* the sender's clock is unset too */
}

static void test_time_adopt_holds_jitter_corrects_drift(void)
{
    TEST_ASSERT_FALSE(bohun_time_adopt(1760000000u, 1760000003u, 3));   /* inside the tolerance */
    TEST_ASSERT_FALSE(bohun_time_adopt(1760000003u, 1760000000u, 3));
    TEST_ASSERT_TRUE(bohun_time_adopt(1760000000u, 1760000004u, 3));    /* one past it */
    TEST_ASSERT_TRUE(bohun_time_adopt(1760000400u, 1760000000u, 3));    /* either direction */
}

/* ---- the thermal history ----------------------------------------------- */

static bohun_temp_hist_t s_hist;   /* 22 KB - static, never on the test's stack */

static void test_temp_record_refuses_an_unset_clock_and_a_bad_node(void)
{
    memset(&s_hist, 0, sizeof(s_hist));
    TEST_ASSERT_FALSE(bohun_temp_record(&s_hist, 0, 30, 550));
    TEST_ASSERT_FALSE(bohun_temp_record(&s_hist, BOHUN_TEMP_NODES, 1760000000u, 550));
    TEST_ASSERT_EQUAL_INT16(0, bohun_temp_value(&s_hist, 0, bohun_temp_bucket(30), BOHUN_TEMP_AVG));
    TEST_ASSERT_EQUAL_INT16(0, bohun_temp_value(&s_hist, BOHUN_TEMP_NODES, bohun_temp_bucket(1760000000u), BOHUN_TEMP_AVG));
}

static void test_temp_bucket_aggregates_max_min_avg(void)
{
    memset(&s_hist, 0, sizeof(s_hist));
    uint32_t t = 1760000000u;   /* three readings, one second apart, same bucket */
    TEST_ASSERT_TRUE(bohun_temp_record(&s_hist, 2, t,     600));
    TEST_ASSERT_TRUE(bohun_temp_record(&s_hist, 2, t + 1, 650));
    TEST_ASSERT_TRUE(bohun_temp_record(&s_hist, 2, t + 2, 610));
    uint32_t b = bohun_temp_bucket(t);
    TEST_ASSERT_EQUAL_INT16(650, bohun_temp_value(&s_hist, 2, b, BOHUN_TEMP_MAX));
    TEST_ASSERT_EQUAL_INT16(600, bohun_temp_value(&s_hist, 2, b, BOHUN_TEMP_MIN));
    TEST_ASSERT_EQUAL_INT16(620, bohun_temp_value(&s_hist, 2, b, BOHUN_TEMP_AVG));
    /* another node in that bucket, and the same node one bucket on: nobody heard */
    TEST_ASSERT_EQUAL_INT16(0, bohun_temp_value(&s_hist, 3, b, BOHUN_TEMP_MAX));
    TEST_ASSERT_EQUAL_INT16(0, bohun_temp_value(&s_hist, 2, b + 1, BOHUN_TEMP_MAX));
}

static void test_temp_slot_reused_a_day_later_forgets_the_old_day(void)
{
    memset(&s_hist, 0, sizeof(s_hist));
    uint32_t t = 1760000000u;
    uint32_t b = bohun_temp_bucket(t);
    bohun_temp_record(&s_hist, 0, t, 500);
    bohun_temp_record(&s_hist, 1, t, 510);
    /* 24 h later the same slot comes round, and only node 0 speaks */
    uint32_t t2 = t + BOHUN_TEMP_BUCKETS * BOHUN_TEMP_BUCKET_S;
    bohun_temp_record(&s_hist, 0, t2, 420);
    TEST_ASSERT_EQUAL_INT16(0,   bohun_temp_value(&s_hist, 0, b, BOHUN_TEMP_AVG));   /* yesterday is gone */
    TEST_ASSERT_EQUAL_INT16(0,   bohun_temp_value(&s_hist, 1, b, BOHUN_TEMP_AVG));   /* for every node */
    TEST_ASSERT_EQUAL_INT16(0,   bohun_temp_value(&s_hist, 1, b + BOHUN_TEMP_BUCKETS, BOHUN_TEMP_AVG));   /* silent today */
    TEST_ASSERT_EQUAL_INT16(420, bohun_temp_value(&s_hist, 0, b + BOHUN_TEMP_BUCKETS, BOHUN_TEMP_AVG));
}

static void test_temp_ticks_every_three_hours_on_the_round_hour(void)
{
    uint16_t idx[16];
    uint8_t hour[16];
    /* now = 1700000000 (2023-11-14 22:13:20 UTC). The window starts at bucket
     * 5666379 = 1699913700; the first 3-hour mark inside it is 1699920000 =
     * 2023-11-14 00:00 UTC, 21 buckets in, then one every 36 buckets. */
    int n = bohun_temp_ticks(1700000000u, 3, idx, hour, 16);
    TEST_ASSERT_EQUAL_INT(8, n);
    static const uint16_t want_idx[8]  = { 21, 57, 93, 129, 165, 201, 237, 273 };
    static const uint8_t  want_hour[8] = { 0, 3, 6, 9, 12, 15, 18, 21 };
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want_idx, idx, 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_hour, hour, 8);
    TEST_ASSERT_EQUAL_INT(3, bohun_temp_ticks(1700000000u, 3, idx, hour, 3));   /* the cap holds */
}

static void test_hour_label_twelve_hour_clock(void)
{
    char s[8];
    bohun_hour_label(0, s, sizeof(s));  TEST_ASSERT_EQUAL_STRING("12am", s);
    bohun_hour_label(1, s, sizeof(s));  TEST_ASSERT_EQUAL_STRING("1am", s);
    bohun_hour_label(12, s, sizeof(s)); TEST_ASSERT_EQUAL_STRING("12pm", s);
    bohun_hour_label(15, s, sizeof(s)); TEST_ASSERT_EQUAL_STRING("3pm", s);
    bohun_hour_label(23, s, sizeof(s)); TEST_ASSERT_EQUAL_STRING("11pm", s);
}

static void test_temp_slots_are_the_racks_six(void)
{
    TEST_ASSERT_EQUAL_INT(0, bohun_temp_slot(1));
    TEST_ASSERT_EQUAL_INT(3, bohun_temp_slot(4));
    TEST_ASSERT_EQUAL_INT(4, bohun_temp_slot(6));
    TEST_ASSERT_EQUAL_INT(5, bohun_temp_slot(7));
    TEST_ASSERT_EQUAL_INT(-1, bohun_temp_slot(5));    /* the shelf spare has no line */
    TEST_ASSERT_EQUAL_INT(-1, bohun_temp_slot(0));
    for (unsigned s = 0; s < BOHUN_TEMP_NODES; s++) {
        TEST_ASSERT_EQUAL_INT((int)s, bohun_temp_slot(bohun_temp_slot_id(s)));   /* round trip */
    }
    TEST_ASSERT_EQUAL_UINT8(0, bohun_temp_slot_id(BOHUN_TEMP_NODES));
}

static void test_temp_json_series_nulls_gaps_and_fits(void)
{
    memset(&s_hist, 0, sizeof(s_hist));
    uint32_t now = 1760000000u;
    /* the newest bucket has three readings; the one before it has one; the rest were never heard */
    bohun_temp_record(&s_hist, 1, now,     600);
    bohun_temp_record(&s_hist, 1, now + 1, 620);
    bohun_temp_record(&s_hist, 1, now + 2, 610);
    bohun_temp_record(&s_hist, 1, now - BOHUN_TEMP_BUCKET_S, 555);
    static char out[1600];
    size_t len = bohun_temp_json_series(&s_hist, 1, now + 2, BOHUN_TEMP_AVG, out, sizeof(out));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_UINT(len, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('[', out[0]);
    TEST_ASSERT_EQUAL_CHAR(']', out[len - 1]);
    TEST_ASSERT_EQUAL_STRING_LEN("[null,null,", out, 11);
    TEST_ASSERT_NOT_NULL(strstr(out, ",555,610]"));           /* ...the two heard buckets end the window */
    /* 286 nulls + two numbers = 288 elements */
    int commas = 0;
    for (const char *c = out; *c; c++) if (*c == ',') commas++;
    TEST_ASSERT_EQUAL_INT(BOHUN_TEMP_BUCKETS - 1, commas);
    /* another node heard nothing at all: all null, and still a valid array */
    len = bohun_temp_json_series(&s_hist, 0, now + 2, BOHUN_TEMP_MAX, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(2 + 4 * BOHUN_TEMP_BUCKETS + (BOHUN_TEMP_BUCKETS - 1), len);
    /* max and min of the newest bucket */
    len = bohun_temp_json_series(&s_hist, 1, now + 2, BOHUN_TEMP_MAX, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, ",555,620]"));
    len = bohun_temp_json_series(&s_hist, 1, now + 2, BOHUN_TEMP_MIN, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, ",555,600]"));
    /* a buffer too small refuses whole, rather than handing back a truncated array */
    TEST_ASSERT_EQUAL_UINT(0, bohun_temp_json_series(&s_hist, 1, now + 2, BOHUN_TEMP_AVG, out, 100));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_alive_never_heard);
    RUN_TEST(test_alive_fresh_and_stale);
    RUN_TEST(test_alive_survives_49_day_wrap);
    RUN_TEST(test_avail_no_traffic_is_100);
    RUN_TEST(test_avail_scanner_noise_does_not_count);
    RUN_TEST(test_avail_our_5xx_counts);
    RUN_TEST(test_avail_64bit_intermediate);
    RUN_TEST(test_avail_skewed_gossip_clamps);
    RUN_TEST(test_delivery_perfect);
    RUN_TEST(test_delivery_half);
    RUN_TEST(test_delivery_reboot_resets_window);
    RUN_TEST(test_delivery_never_over_100);
    RUN_TEST(test_gap_first_frame_and_consecutive);
    RUN_TEST(test_gap_counts_missed);
    RUN_TEST(test_gap_caps_at_99);
    RUN_TEST(test_gap_reboot_is_not_a_gap);
    RUN_TEST(test_pctl_empty_and_single);
    RUN_TEST(test_pctl_nearest_rank);
    RUN_TEST(test_pctl_p95_of_100);
    RUN_TEST(test_elect_nobody);
    RUN_TEST(test_elect_self_alone);
    RUN_TEST(test_elect_lowest_id_wins);
    RUN_TEST(test_elect_witness_never_wins);
    RUN_TEST(test_elect_dead_or_unable_peers_skipped);
    RUN_TEST(test_elect_preemption);
    RUN_TEST(test_elect_self_unfit_peer_leads);
    RUN_TEST(test_bench_fresh_offender_needs_streak);
    RUN_TEST(test_bench_repeat_offender_rebenches_on_one);
    RUN_TEST(test_bench_backoff_doubles_and_caps);
    RUN_TEST(test_br_token_present);
    RUN_TEST(test_br_token_absent);
    RUN_TEST(test_private_ranges);
    RUN_TEST(test_public_and_malformed_refused);
    RUN_TEST(test_origin_matches_own_host);
    RUN_TEST(test_origin_prefix_attack_refused);
    RUN_TEST(test_json_repair_mid_member);
    RUN_TEST(test_json_repair_boundary_refuses);
    RUN_TEST(test_json_repair_boundary_exact_fit);
    RUN_TEST(test_json_repair_no_object_refuses);
    RUN_TEST(test_probe_verdict_busy_is_not_dead);
    RUN_TEST(test_probe_streak_busy_holds_dead_grows_ok_clears);
    RUN_TEST(test_probe_fit_hold_down_after_stand_down);
    RUN_TEST(test_probe_fit_survives_49_day_wrap);
    RUN_TEST(test_probe_room_leaves_two_blocks_spare);
    RUN_TEST(test_pair_silent_client_never_spoke);
    RUN_TEST(test_refusal_clip_terminates_and_fills_blanks);
    RUN_TEST(test_refusal_record_clips_every_field);
    RUN_TEST(test_refusal_dump_line_format);
    RUN_TEST(test_refusal_dump_empty_is_header_only);
    RUN_TEST(test_refusal_ring_wraps_newest_first);
    RUN_TEST(test_refusal_dump_truncates_on_a_line_boundary);
    RUN_TEST(test_err_status_table);
    RUN_TEST(test_delta_treats_reset_as_fresh_start);
    RUN_TEST(test_rate_ring_rolls_once_per_minute);
    RUN_TEST(test_rate_ring_last60_slides);
    RUN_TEST(test_rate_ring_saturates_instead_of_wrapping);
    RUN_TEST(test_rate_ring_peak_forgets_after_an_hour);
    RUN_TEST(test_time_sane_boundary);
    RUN_TEST(test_time_adopt_unset_local_takes_a_sane_beacon);
    RUN_TEST(test_time_adopt_holds_jitter_corrects_drift);
    RUN_TEST(test_temp_record_refuses_an_unset_clock_and_a_bad_node);
    RUN_TEST(test_temp_bucket_aggregates_max_min_avg);
    RUN_TEST(test_temp_slot_reused_a_day_later_forgets_the_old_day);
    RUN_TEST(test_temp_ticks_every_three_hours_on_the_round_hour);
    RUN_TEST(test_hour_label_twelve_hour_clock);
    RUN_TEST(test_temp_slots_are_the_racks_six);
    RUN_TEST(test_temp_json_series_nulls_gaps_and_fits);
    return UNITY_END();
}
