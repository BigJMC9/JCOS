#include <stdio.h>
#include "polaris_observation.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"observation line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static u32 fixture(u32 pipe, u32 field, u32 active) {
    if (field == POLARIS_REG_CRTC_CONTROL) return pipe == active ? 0x10001U : 0U;
    if (field == POLARIS_REG_GRPH_ENABLE) return 1U;
    if (field == POLARIS_REG_PITCH) return 1024U;
    return 0;
}
int main(void) {
    for (u32 active = 0; active < 6U; ++active) {
        PolarisObservationBatch b; polaris_observation_init(&b);
        for (u32 i = 0; i < POLARIS_OBSERVATION_MAX_READS; ++i) {
            u32 pipe, field;
            CHECK(polaris_observation_next(&b,&pipe,&field));
            CHECK(pipe == (i < 6U ? i : active));
            CHECK(field == (i < 6U ? 0U : (i - 6U) % 15U));
            CHECK(polaris_observation_accept(&b,fixture(pipe,field,active)));
        }
        CHECK(b.complete && b.closed && b.reads == 36U && b.pipe == active);
        u32 p,f; CHECK(!polaris_observation_next(&b,&p,&f));
    }
    for (u32 absent = 0; absent < 36U; ++absent) {
        PolarisObservationBatch b; polaris_observation_init(&b);
        for (u32 i = 0; i <= absent; ++i) {
            u32 p,f; CHECK(polaris_observation_next(&b,&p,&f));
            bool accepted = polaris_observation_accept(&b, i == absent ? ~0U : fixture(p,f,3U));
            CHECK(accepted == (i != absent));
        }
        CHECK(b.closed && !b.complete && b.reads == absent + 1U);
    }
    for (u32 changed = 0; changed < 15U; ++changed) {
        PolarisObservationBatch b; polaris_observation_init(&b);
        for (u32 i = 0; i < 21U + changed + 1U; ++i) {
            u32 p,f; CHECK(polaris_observation_next(&b,&p,&f));
            u32 value = fixture(p,f,3U);
            if (i == 21U + changed) value ^= 0x100U;
            CHECK(polaris_observation_accept(&b,value) == (i != 21U + changed));
        }
        CHECK(b.closed && !b.complete);
    }
    for (u32 kind = 0; kind < 6U; ++kind) {
        PolarisObservationBatch b; polaris_observation_init(&b);
        bool rejected = false;
        for (u32 i = 0; i < 36U; ++i) {
            u32 p,f; CHECK(polaris_observation_next(&b,&p,&f));
            u32 value = fixture(p,f,3U);
            if (kind == 0 && i < 6U) value = 0; /* no active pipe */
            if (kind == 1 && i == 4U) value = 0x10001U; /* second active */
            if (kind == 2 && i == 0U) value = 1U; /* transition */
            if (kind == 3 && i == 3U) value |= 0x1000000U; /* reads disabled */
            if (kind == 4 && i == 7U) value = 0x100U; /* blank */
            if (kind == 5 && i == 9U) value = 0U; /* plane disabled */
            if (!polaris_observation_accept(&b,value)) { rejected = true; break; }
        }
        CHECK(rejected && b.closed && !b.complete);
    }
    PolarisObservationBatch b; polaris_observation_init(&b);
    CHECK(!polaris_observation_accept(&b,0U) && b.closed);
    polaris_observation_init(&b);
    u32 p,f; CHECK(polaris_observation_next(&b,&p,&f));
    CHECK(!polaris_observation_next(&b,&p,&f) && b.closed);
    puts("PASS: fixed 36-read plan; six unique pipes; every absent/changed boundary; transition/ambiguous/blank/inactive/order refusal.");
    return 0;
}
