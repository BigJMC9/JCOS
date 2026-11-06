/* Test the actual userspace backend, independently supplied register fixtures.
 * No physical GPU, register mapping, port I/O or firmware execution is involved. */
#include <stdio.h>
#include <string.h>
#include "../../user/drivers/amd_polaris_display.h"
#include "../../user/tests/amd_polaris_display_test.h"
#include "../../include/display_service_protocol.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Polaris line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static const JcosU32 byte_offsets[6][15] = {
    {0x6E70,0x6E74,0x6E78,0x6800,0x6804,0x680C,0x6810,0x681C,0x6818,0x6824,0x6828,0x682C,0x6830,0x6834,0x6838},
    {0x7670,0x7674,0x7678,0x7000,0x7004,0x700C,0x7010,0x701C,0x7018,0x7024,0x7028,0x702C,0x7030,0x7034,0x7038},
    {0x7E70,0x7E74,0x7E78,0x7800,0x7804,0x780C,0x7810,0x781C,0x7818,0x7824,0x7828,0x782C,0x7830,0x7834,0x7838},
    {0x10670,0x10674,0x10678,0x10000,0x10004,0x1000C,0x10010,0x1001C,0x10018,0x10024,0x10028,0x1002C,0x10030,0x10034,0x10038},
    {0x10E70,0x10E74,0x10E78,0x10800,0x10804,0x1080C,0x10810,0x1081C,0x10818,0x10824,0x10828,0x1082C,0x10830,0x10834,0x10838},
    {0x11670,0x11674,0x11678,0x11000,0x11004,0x1100C,0x11010,0x1101C,0x11018,0x11024,0x11028,0x1102C,0x11030,0x11034,0x11038}
};
typedef struct {
    PolarisDisplaySnapshot fixture;
    unsigned reads, fail_at, change_at, ones_at;
    int omit_output, bad_offset;
} Reader;
static int read_fixture(void *context, JcosU32 offset, JcosU32 *out) {
    Reader *r = context;
    ++r->reads;
    if (r->reads == r->fail_at) return 0;
    if (r->omit_output) return 1;
    if (r->reads == r->ones_at) { *out = ~0U; return 1; }
    for (unsigned i = 0; i < 15; ++i) {
        if (byte_offsets[r->fixture.pipe][i] != offset) continue;
        *out = r->fixture.values[i] ^ (r->reads == r->change_at ? 1U : 0U);
        return 1;
    }
    r->bad_offset = 1;
    return 0;
}
static void setup(Reader *r, unsigned pipe) {
    memset(r, 0, sizeof(*r));
    r->fixture.version = 1;
    r->fixture.pipe = pipe;
    r->fixture.values[POLARIS_REG_CRTC_CONTROL] = 0x10001;
    r->fixture.values[POLARIS_REG_GRPH_ENABLE] = 1;
    r->fixture.values[POLARIS_REG_GRPH_CONTROL] = 2;
    r->fixture.values[POLARIS_REG_PRIMARY_LOW] = 0x200000;
    r->fixture.values[POLARIS_REG_PRIMARY_HIGH] = 1;
    r->fixture.values[POLARIS_REG_PITCH] = 1024;
    r->fixture.values[POLARIS_REG_X_END] = 1024;
    r->fixture.values[POLARIS_REG_Y_END] = 768;
}
static int zero(const void *p, size_t n) {
    const unsigned char *bytes = p;
    while (n--) if (*bytes++) return 0;
    return 1;
}
int main(void) {
    Reader r;
    struct { JcosU64 before; PolarisDisplaySnapshot value; JcosU64 after; } guarded;
    guarded.before = 0xAABBCCDDEEFF1122ULL;
    guarded.after = 0x123456789ABCDEF0ULL;
    PolarisDisplaySnapshot *s = &guarded.value;
    PolarisDisplayObservation o;
    for (unsigned pipe = 0; pipe < 6; ++pipe) {
        setup(&r, pipe);
        for (unsigned field = 0; field < 15; ++field) {
            JcosU32 offset = 0;
            CHECK(polaris_display_register_offset(pipe, (PolarisDisplayRegister)field, 0x40000, &offset));
            CHECK(offset == byte_offsets[pipe][field]);
            CHECK(polaris_display_register_offset(pipe, (PolarisDisplayRegister)field, offset+4ULL, &offset));
            CHECK(offset == byte_offsets[pipe][field]);
            CHECK(!polaris_display_register_offset(pipe, (PolarisDisplayRegister)field, offset+3ULL, &offset));
            CHECK(!offset);
        }
        CHECK(polaris_display_capture(pipe, 0x40000, read_fixture, &r, s) == POLARIS_CAPTURE_OK);
        CHECK(r.reads == 30 && !r.bad_offset && !memcmp(s, &r.fixture, sizeof(*s)));
        CHECK(polaris_display_decode(s, &o) && !o.restrictions);
        CHECK(o.surface_gpu_address == 0x100200000ULL && o.surface_width == 1024 &&
            o.surface_height == 768 && o.pitch_pixels == 1024 && o.linear_span_bytes == 0x300000);
    }
    setup(&r, 5);
    CHECK(polaris_display_capture(5, 0x1167B, read_fixture, &r, s) == POLARIS_CAPTURE_INVALID);
    CHECK(!r.reads && zero(s, sizeof(*s)));
    CHECK(polaris_display_capture(6, ~0ULL, read_fixture, &r, s) == POLARIS_CAPTURE_INVALID);
    CHECK(polaris_display_capture(~0U, ~0ULL, read_fixture, &r, s) == POLARIS_CAPTURE_INVALID);
    CHECK(polaris_display_capture(0, ~0ULL, 0, &r, s) == POLARIS_CAPTURE_INVALID);
    CHECK(polaris_display_capture(0, ~0ULL, read_fixture, &r, 0) == POLARIS_CAPTURE_INVALID);
    CHECK(!r.reads && zero(s, sizeof(*s)));
    JcosU32 offset = ~0U;
    CHECK(!polaris_display_register_offset(0, (PolarisDisplayRegister)-1, ~0ULL, &offset) && !offset);
    CHECK(!polaris_display_register_offset(0, POLARIS_DISPLAY_REGISTER_COUNT, ~0ULL, &offset));
    CHECK(!polaris_display_register_offset(0, POLARIS_REG_PITCH, 0, &offset));
    CHECK(!polaris_display_register_offset(0, POLARIS_REG_PITCH, ~0ULL, 0));

    for (unsigned at = 1; at <= 30; ++at) {
        setup(&r, 3); r.fail_at = at;
        memset(s, 0xAA, sizeof(*s));
        CHECK(polaris_display_capture(3, 0x40000, read_fixture, &r, s) == POLARIS_CAPTURE_READ_FAILED);
        CHECK(r.reads == at && zero(s, sizeof(*s)));
        setup(&r, 3); r.ones_at = at;
        CHECK(polaris_display_capture(3, 0x40000, read_fixture, &r, s) == POLARIS_CAPTURE_READ_FAILED);
        CHECK(r.reads == at && zero(s, sizeof(*s)));
    }
    for (unsigned at = 16; at <= 30; ++at) {
        setup(&r, 3); r.change_at = at;
        CHECK(polaris_display_capture(3, 0x40000, read_fixture, &r, s) == POLARIS_CAPTURE_CHANGED);
        CHECK(r.reads == 30 && zero(s, sizeof(*s)));
    }
    setup(&r, 0); r.omit_output = 1;
    CHECK(polaris_display_capture(0, 0x40000, read_fixture, &r, s) == POLARIS_CAPTURE_READ_FAILED);
    CHECK(r.reads == 1 && zero(s, sizeof(*s)));

    static const struct { unsigned field, value, restriction; } unsupported[] = {
        {POLARIS_REG_CRTC_CONTROL, 1, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_CRTC_CONTROL, 0x10000, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_CRTC_CONTROL, 0x1010001, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_GRPH_ENABLE, 0, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_CRTC_BLANK, 1, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_CRTC_BLANK, 0x100, POLARIS_OBSERVATION_INACTIVE},
        {POLARIS_REG_CRTC_INTERLACE, 1, POLARIS_OBSERVATION_INTERLACED},
        {POLARIS_REG_GRPH_CONTROL, 0x10002, POLARIS_OBSERVATION_TRANSLATED},
        {POLARIS_REG_GRPH_CONTROL, 0x80000002, POLARIS_OBSERVATION_FORMAT},
        {POLARIS_REG_GRPH_ENABLE, 3, POLARIS_OBSERVATION_FORMAT},
        {POLARIS_REG_GRPH_SWAP, 0x10, POLARIS_OBSERVATION_FORMAT},
        {POLARIS_REG_GRPH_CONTROL, 0x12, POLARIS_OBSERVATION_CONTROL},
        {POLARIS_REG_GRPH_ENABLE, 0x81, POLARIS_OBSERVATION_RESERVED},
        {POLARIS_REG_OFFSET_X, 1, POLARIS_OBSERVATION_OFFSET},
        {POLARIS_REG_OFFSET_Y, 1, POLARIS_OBSERVATION_OFFSET},
        {POLARIS_REG_X_START, 1, POLARIS_OBSERVATION_OFFSET},
        {POLARIS_REG_Y_START, 1, POLARIS_OBSERVATION_OFFSET},
        {POLARIS_REG_X_START, 2048, POLARIS_OBSERVATION_GEOMETRY},
        {POLARIS_REG_Y_START, 2048, POLARIS_OBSERVATION_GEOMETRY},
        {POLARIS_REG_X_END, 0, POLARIS_OBSERVATION_GEOMETRY},
        {POLARIS_REG_Y_END, 0, POLARIS_OBSERVATION_GEOMETRY},
        {POLARIS_REG_PITCH, 1023, POLARIS_OBSERVATION_GEOMETRY},
        {POLARIS_REG_PITCH, 0x80000400, POLARIS_OBSERVATION_RESERVED},
        {POLARIS_REG_X_END, 0x80000400, POLARIS_OBSERVATION_RESERVED},
        {POLARIS_REG_PRIMARY_HIGH, 0x100, POLARIS_OBSERVATION_ADDRESS},
        {POLARIS_REG_PRIMARY_LOW, 0x200001, POLARIS_OBSERVATION_ADDRESS}
    };
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
        setup(&r, 0);
        r.fixture.values[unsupported[i].field] = unsupported[i].value;
        CHECK(polaris_display_decode(&r.fixture, &o));
        CHECK((o.restrictions & unsupported[i].restriction) && !o.linear_span_bytes);
    }
    for (unsigned layout = 2; layout < 16; ++layout) {
        setup(&r, 0);
        r.fixture.values[POLARIS_REG_GRPH_CONTROL] |= layout << 20;
        CHECK(polaris_display_decode(&r.fixture, &o));
        CHECK((o.restrictions & POLARIS_OBSERVATION_TILED) && !o.linear_span_bytes);
    }
    /* Exact first physical RX570 observation, replayed without device access.
     * GPU F400000000 remains distinct from CPU aperture 50000000. */
    setup(&r, 0);
    r.fixture.values[POLARIS_REG_CRTC_CONTROL] = 0x80410311U;
    r.fixture.values[POLARIS_REG_GRPH_CONTROL] = 0x0A10800AU;
    r.fixture.values[POLARIS_REG_PRIMARY_HIGH] = 0xF4U;
    r.fixture.values[POLARIS_REG_PRIMARY_LOW] = 0;
    CHECK(polaris_display_decode(&r.fixture, &o) && !o.restrictions &&
        o.array_mode == POLARIS_ARRAY_LINEAR_ALIGNED && o.linear_span_bytes == 0x300000ULL &&
        o.surface_gpu_address == 0xF400000000ULL);
    r.fixture.values[POLARIS_REG_PITCH] = 1087;
    CHECK(polaris_display_decode(&r.fixture, &o) &&
        (o.restrictions & POLARIS_OBSERVATION_GEOMETRY) && !o.linear_span_bytes);
    r.fixture.values[POLARIS_REG_PITCH] = 1088;
    CHECK(polaris_display_decode(&r.fixture, &o) && !o.restrictions);
    const unsigned forbidden_aligned[] = {0x10U, 0x20000U, 0x20000000U, 0x40000000U};
    for (unsigned i=0; i<sizeof(forbidden_aligned)/sizeof(forbidden_aligned[0]); ++i) {
        r.fixture.values[POLARIS_REG_GRPH_CONTROL] = 0x0A10800AU | forbidden_aligned[i];
        CHECK(polaris_display_decode(&r.fixture, &o) &&
            (o.restrictions & POLARIS_OBSERVATION_CONTROL) && !o.linear_span_bytes);
    }
    r.fixture.values[POLARIS_REG_GRPH_CONTROL] = 0x0A11800AU;
    CHECK(polaris_display_decode(&r.fixture, &o) &&
        (o.restrictions & POLARIS_OBSERVATION_TRANSLATED) && !o.linear_span_bytes);
    for (unsigned format = 1; format < 8; ++format) {
        setup(&r, 0);
        r.fixture.values[POLARIS_REG_GRPH_CONTROL] |= format << 8;
        CHECK(polaris_display_decode(&r.fixture, &o) &&
            (o.restrictions & POLARIS_OBSERVATION_FORMAT) && !o.linear_span_bytes);
    }
    for (unsigned depth = 0; depth < 4; ++depth) {
        if (depth == 2) continue;
        setup(&r, 0);
        r.fixture.values[POLARIS_REG_GRPH_CONTROL] = depth;
        CHECK(polaris_display_decode(&r.fixture, &o) &&
            (o.restrictions & POLARIS_OBSERVATION_FORMAT) && !o.linear_span_bytes);
    }
    setup(&r, 0);
    r.fixture.values[POLARIS_REG_PRIMARY_LOW] = 0xFFFFFF00;
    r.fixture.values[POLARIS_REG_PRIMARY_HIGH] = 0xFF;
    CHECK(polaris_display_decode(&r.fixture, &o) &&
        (o.restrictions & POLARIS_OBSERVATION_ADDRESS) && !o.linear_span_bytes);
    setup(&r, 0);
    r.fixture.values[POLARIS_REG_PRIMARY_LOW] = 0;
    r.fixture.values[POLARIS_REG_PRIMARY_HIGH] = 0;
    r.fixture.values[POLARIS_REG_X_END] = 1023;
    CHECK(polaris_display_decode(&r.fixture, &o) && !o.restrictions &&
        o.surface_gpu_address == 0 && o.linear_span_bytes == 0x2FFFFC);
    r.fixture.version = 2;
    CHECK(!polaris_display_decode(&r.fixture, &o) && zero(&o, sizeof(o)));
    r.fixture.version = 1; r.fixture.pipe = 6;
    CHECK(!polaris_display_decode(&r.fixture, &o) && zero(&o, sizeof(o)));
    CHECK(!polaris_display_decode(0, &o) && zero(&o, sizeof(o)));
    CHECK(!polaris_display_decode(&r.fixture, 0));

    /* Bounded decoder fuzz: malformed register words cannot authorize spans. */
    JcosU32 random = 0xA51C5EED;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        setup(&r, trial % 6);
        for (unsigned field = 0; field < 15; ++field) {
            random = random * 1664525U + 1013904223U;
            r.fixture.values[field] = random;
        }
        int decoded = polaris_display_decode(&r.fixture, &o);
        CHECK(!decoded || !o.restrictions || !o.linear_span_bytes);
        CHECK(!decoded || o.surface_gpu_address < (1ULL << 40));
    }
    CHECK(guarded.before == 0xAABBCCDDEEFF1122ULL && guarded.after == 0x123456789ABCDEF0ULL);
    CHECK(POLARIS_DISPLAY_MODEL_VERSION == JCOS_DISPLAY_POLARIS_MODEL_VERSION);
    CHECK(polaris_display_model_test() == JCOS_DISPLAY_POLARIS_MODEL_EXPECTED);
    puts("Polaris userspace backend: PASS (6 pipe maps, 30 failure/absent boundaries, 15 changed samples, unsupported formats/layouts, geometry/address limits, 10000 malformed snapshots, Ring3 model vectors)");
    return 0;
}
