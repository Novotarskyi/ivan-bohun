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
    return UNITY_END();
}
