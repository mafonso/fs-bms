// Host test for main/features/bms_proto.c. Run: test/run.sh
// Inline samples are from aiobmsble's tests (Apache-2.0); captures in
// test/fixtures/*.json are checked too.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bms_proto.h"

static const char RT[] =
    "{\"CommVer\":1,\"wifiSN\":\"F075704831426030796\",\"modID\":3,"
    "\"date\":\"20260813120925\",\"DevSN\":\"075704831426030796\",\"Type\":112,"
    "\"SubType\":7353,\"Estate\":9152,\"Bfault\":0,\"Bwarn\":0,\"Bstate\":9152,"
    "\"BBfault\":0,\"BBwarn\":0,\"BTemp\":[[340,330],[256,514]],"
    "\"Batt\":[[54100],[1977],[null]],\"Batsoc\":[[7860,943,1050000]],"
    "\"Templist\":[[340,340],[0,0],[65535,65535],[65535,65535]],"
    "\"BattList\":[[54070,65535],[647,-1]],\"BatsocList\":[[7800,1000,350000]],"
    "\"BatcelList\":[[3378,3380,3379,3380,3379,3380,3380,3382,3389,3381,"
    "3380,3381,3380,3380,3379,3380],[65535,65535,65535,65535,65535,65535,"
    "65535,65535,65535,65535,65535,65535,65535,65535,65535,65535]],"
    "\"EMSpara\":[[3,14]],\"BMaxMin\":[[3389,3378],[8,0]],"
    "\"LVolCur\":[[576,480],[4320,4800]],\"BMSpara\":[[3,14]],"
    "\"BLVolCu\":[[576,480],[1600,1600]],"
    "\"BtemList\":[[340,340,340,340,32767,32767,32767,32767]]}";

static const char BAS[] =
    "{\"CommVer\":1,\"version\":\"2.06\",\"wifiSN\":\"F100011002424470238\",\"COM\":3,\"iotType\":3,"
    "\"modID\":1,\"DevSN\":\"100011002424470238\",\"Type\":112,\"SubType\":7300,\"DSwVer\":65535,"
    "\"M1SwVer\":519,\"M2SwVer\":16,\"DHwVer\":0,\"CtHwVer\":0,\"PwHwVer\":65535}";

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-3)

static bms_frame_t s_frame;

// Feed `s` in `chunk`-byte notifications; returns the parsed frame.
static cJSON *feed(const char *s, size_t chunk)
{
    size_t n = strlen(s);
    cJSON *out = NULL;
    for (size_t i = 0; i < n; i += chunk) {
        size_t len = n - i < chunk ? n - i : chunk;
        bms_frame_result_t r = bms_frame_feed(&s_frame, (const uint8_t *)s + i, len, &out);
        if (i + len < n) {
            assert(r == BMS_FRAME_PENDING);
        } else {
            assert(r == BMS_FRAME_DONE);
        }
    }
    return out;
}

static void test_sample_real(void)
{
    // Notification sizes: 20 = MTU 23, 244 = MTU 247, and odd sizes.
    size_t chunks[] = {35, 20, 244, 1};
    for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        cJSON *msg = feed(RT, chunks[k]);
        bms_sample_t s;
        char err[64];
        assert(bms_decode_real(msg, &s, err, sizeof(err)));
        assert(NEAR(s.voltage, 54.07));
        assert(NEAR(s.current, 64.7));
        assert(NEAR(s.soc, 78.0));
        assert(NEAR(s.remaining_ah, 273.0));
        assert(NEAR(s.capacity_ah, 350.0));
        assert(s.n_cells == 16);
        assert(NEAR(s.cells[0], 3.378) && NEAR(s.cells[8], 3.389) && NEAR(s.cells[15], 3.380));
        assert(NEAR(s.cell_delta, 0.011));
        assert(s.cell_max_idx == 8 && s.cell_min_idx == 0);   // matches BMaxMin [[3389,3378],[8,0]]
        assert(s.n_temps == 4 && NEAR(s.temps[0], 34.0) && NEAR(s.temps[3], 34.0));
        assert(s.warn == 0 && s.fault == 0 && s.problem == 0);
        cJSON_Delete(msg);
    }
}

static void test_basic(void)
{
    cJSON *msg = feed(BAS, 35);
    bms_info_t info;
    bms_decode_basic(msg, &info);
    assert(!strcmp(info.serial, "100011002424470238"));
    assert(!strcmp(info.sw, "2.06"));
    assert(!strcmp(info.fw, "519"));
    assert(!strcmp(info.model, "112"));
    cJSON_Delete(msg);
}

static void test_framing(void)
{
    cJSON *out;
    // A chunk ending in a nested '}' doesn't end the frame.
    const char *a = "{\"CommVer\":1,\"x\":{\"y\":1}", *b = ",\"z\":2}";
    assert(bms_frame_feed(&s_frame, (const uint8_t *)a, strlen(a), &out) == BMS_FRAME_PENDING);
    assert(bms_frame_feed(&s_frame, (const uint8_t *)b, strlen(b), &out) == BMS_FRAME_DONE);
    cJSON_Delete(out);
    // Tail without a start is ignored; a new '{' restarts.
    bms_frame_reset(&s_frame);
    assert(bms_frame_feed(&s_frame, (const uint8_t *)"1}", 2, &out) == BMS_FRAME_PENDING);
    const char *half = "{\"CommVer\":1,\"a\":";
    bms_frame_feed(&s_frame, (const uint8_t *)half, strlen(half), &out);
    const char *full = "{\"CommVer\":1}";
    assert(bms_frame_feed(&s_frame, (const uint8_t *)full, strlen(full), &out) == BMS_FRAME_DONE);
    cJSON_Delete(out);
    // Wrong protocol version is dropped.
    const char *v2 = "{\"CommVer\":2}";
    assert(bms_frame_feed(&s_frame, (const uint8_t *)v2, strlen(v2), &out) == BMS_FRAME_BAD_VER);
    // Trailing NUL padding.
    const uint8_t pad[] = "{\"CommVer\":1}\0\0";
    assert(bms_frame_feed(&s_frame, pad, sizeof(pad), &out) == BMS_FRAME_DONE);
    cJSON_Delete(out);
}

static void test_strings_and_limits(void)
{
    const char *j = "{\"CommVer\":1,\"BattList\":[[\"52100\"],[\"-123\"]],\"BatsocList\":[[5000,1000,100000]],"
                    "\"BatcelList\":[[3300,\"3310\",65535]],\"BtemList\":[[250,32767,-50]],\"Bwarn\":\"4\",\"Bfault\":1}";
    cJSON *msg = cJSON_Parse(j);
    bms_sample_t s;
    char err[64];
    assert(bms_decode_real(msg, &s, err, sizeof(err)));
    assert(NEAR(s.voltage, 52.1) && NEAR(s.current, -12.3));
    assert(s.n_cells == 2 && NEAR(s.cell_delta, 0.010));
    assert(s.n_temps == 2 && NEAR(s.temps[1], -5.0));
    assert(s.warn == 4 && s.fault == 1 && s.problem == 5);
    cJSON_Delete(msg);

    msg = cJSON_Parse("{\"BattList\":[[99999999999],[0]],\"BatsocList\":[[1,1,1]],"
                      "\"BatcelList\":[[1]],\"BtemList\":[[1]]}");
    assert(!bms_decode_real(msg, &s, err, sizeof(err)));
    cJSON_Delete(msg);
    msg = cJSON_Parse("{\"CommVer\":1}");
    assert(!bms_decode_real(msg, &s, err, sizeof(err)));
    cJSON_Delete(msg);
}

// Every *.json in the fixtures dir that is a "real infor" reply must decode.
static void test_capture(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    static char buf[BMS_FRAME_MAX + 1];
    size_t n = fread(buf, 1, BMS_FRAME_MAX, fp);
    fclose(fp);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    cJSON *msg = feed(buf, 20);
    bms_sample_t s;
    char err[64];
    if (!bms_decode_real(msg, &s, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", path, err);
        abort();
    }
    printf("%s: %.3f V %.1f A %.2f %% %d cells (delta %.3f V) %d temps\n", path, s.voltage, s.current,
           s.soc, s.n_cells, s.cell_delta, s.n_temps);
    cJSON_Delete(msg);
}

int main(int argc, char **argv)
{
    test_sample_real();
    test_basic();
    test_framing();
    test_strings_and_limits();
    for (int i = 1; i < argc; i++) {
        test_capture(argv[i]);
    }
    printf("bms_proto: all tests passed\n");
    return 0;
}
