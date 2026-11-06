/* Actual Ring3 policy + independent kernel preview. No device or effectors. */
#include <stdio.h>
#include <string.h>
#include "../../user/drivers/amd_polaris_plane.h"
#include "../../kernel/polaris_plane.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"plane line%d: %s\n",__LINE__,#x); return 1; } } while (0)
static PolarisDisplayObservation observation;
static PolarisPlaneWitness witness;
static void setup(void) {
    memset(&observation,0,sizeof(observation)); memset(&witness,0,sizeof(witness));
    observation.surface_gpu_address=0xF400200000ULL; observation.linear_span_bytes=0x300000;
    observation.surface_width=1024; observation.surface_height=768; observation.pitch_pixels=1024;
    observation.depth=2; observation.array_mode=1;
    witness.values[0][POLARIS_WITNESS_FRAME_COUNT]=100;
    witness.values[1][POLARIS_WITNESS_FRAME_COUNT]=110;
}
static int init(PolarisPlanePreview *p,unsigned pipe) {
    return polaris_plane_preview_init(p,17,29,0xF400200000ULL,pipe,&witness);
}
int main(void) {
    const unsigned kind[5]={1,1,2,3,4},field[5]={7,6,6,0,2},value[5]={0xF4,0x200000,0x200000,0,110};
    for (unsigned pipe=0;pipe<6;++pipe) {
        setup(); PolarisPlanePreview preview;
        CHECK(init(&preview,pipe));
        for (unsigned step=0;step<5;++step) {
            JcosU64 tag=0; JcosU32 data=0;
            CHECK(polaris_plane_recipe_step(&observation,&witness,pipe,step,&tag,&data));
            CHECK(tag==((1ULL<<56)|((JcosU64)step<<40)|((JcosU64)kind[step]<<32)|
                ((JcosU64)pipe<<16)|field[step]) && data==value[step]);
            CHECK(polaris_plane_preview_accept(&preview,17,29,tag,data));
        }
        CHECK(preview.closed && preview.next==5 &&
            !polaris_plane_preview_accept(&preview,17,29,0,0));
    }
    /* Every altered tag bit and payload bit is refused at every step. */
    for (unsigned step=0;step<5;++step) for (unsigned bit=0;bit<64;++bit) {
        setup(); PolarisPlanePreview a,b;
        CHECK(init(&a,3) && init(&b,3));
        for (unsigned i=0;i<step;++i) {
            JcosU64 tag; JcosU32 data;
            CHECK(polaris_plane_recipe_step(&observation,&witness,3,i,&tag,&data));
            CHECK(polaris_plane_preview_accept(&a,17,29,tag,data) &&
                polaris_plane_preview_accept(&b,17,29,tag,data));
        }
        JcosU64 tag; JcosU32 data;
        CHECK(polaris_plane_recipe_step(&observation,&witness,3,step,&tag,&data));
        CHECK(!polaris_plane_preview_accept(&a,17,29,tag^(1ULL<<bit),data) && a.closed);
        CHECK(!polaris_plane_preview_accept(&b,17,29,tag,(JcosU64)data^(1ULL<<bit)) && b.closed);
    }
    for (unsigned failure=0;failure<16;++failure) {
        setup();
        switch (failure) {
        case 0: witness.values[0][0]=4; break;
        case 1: witness.values[1][0]=1; break;
        case 2: witness.values[0][0]=0x10000; break;
        case 3: witness.values[1][0]=0x100; break;
        case 4: witness.values[0][1]=1; break;
        case 5: witness.values[1][1]=0x10; break;
        case 6: witness.values[1][1]=0x40; break;
        case 7: witness.values[1][2]=100; break;
        case 8: witness.values[1][2]=99; break;
        case 9: witness.values[1][2]=613; break;
        case 10: witness.values[0][0]=~0U; break;
        case 11: witness.values[1][2]=0x100006E; break;
        case 12: witness.values[0][2]=~0U; break;
        case 13: witness.values[1][0]=0x100000; break;
        case 14: witness.values[0][0]=0x1000000; break;
        case 15: witness.values[1][0]=0x10000000; break;
        }
        JcosU64 tag=~0ULL; JcosU32 data=~0U; PolarisPlanePreview p;
        CHECK(polaris_plane_blockers(&witness) &&
            !polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&data) && !tag && !data);
        CHECK(!init(&p,0) && p.closed);
    }
    for (unsigned mode=0;mode<2;++mode) {
        setup(); witness.values[0][1]=witness.values[1][1]=mode ? 0x20 : 0;
        PolarisPlanePreview p;
        CHECK(init(&p,0));
        for (unsigned step=0;step<5;++step) {
            JcosU64 tag; JcosU32 data;
            CHECK(polaris_plane_recipe_step(&observation,&witness,0,step,&tag,&data) &&
                polaris_plane_preview_accept(&p,17,29,tag,data));
        }
        CHECK(p.closed && p.next==5);
        for (unsigned bit=0;bit<32;++bit) {
            if (bit==5) continue;
            setup(); witness.values[0][1]=witness.values[1][1]=(mode ? 0x20 : 0)|(1U<<bit);
            JcosU64 tag; JcosU32 data;
            CHECK(polaris_plane_blockers(&witness)==POLARIS_PLANE_BLOCK_FLIP &&
                !polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&data));
        }
    }
    for (unsigned mode=0;mode<2;++mode) {
        setup(); witness.values[mode][1]=0x20;
        JcosU64 tag; JcosU32 data; PolarisPlanePreview p;
        CHECK(polaris_plane_blockers(&witness)==POLARIS_PLANE_BLOCK_FLIP &&
            !init(&p,0) && !polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&data));
    }
    setup(); witness.values[0][0]=10; witness.values[1][0]=2;
    CHECK(!polaris_plane_blockers(&witness)); /* status-taken flags are not a lock */
    witness.values[0][2]=0xFFFFFE; witness.values[1][2]=1;
    CHECK(!polaris_plane_blockers(&witness)); /* actual 24-bit wrap */
    witness.values[0][2]=0xFFFFFF; witness.values[1][2]=0;
    CHECK(!polaris_plane_blockers(&witness));
    for (unsigned failure=0;failure<12;++failure) {
        setup();
        switch (failure) {
        case 0: observation.restrictions=1; break;
        case 1: observation.depth=1; break;
        case 2: observation.format=1; break;
        case 3: observation.array_mode=2; break;
        case 4: observation.surface_width=1280; break;
        case 5: observation.surface_height=720; break;
        case 6: observation.pitch_pixels=1088; break;
        case 7: observation.linear_span_bytes--; break;
        case 8: observation.surface_gpu_address++; break;
        case 9: observation.surface_gpu_address=1ULL<<40; break;
        case 10: observation.surface_gpu_address=(1ULL<<40)-0x2FFF00; break;
        case 11: observation.surface_gpu_address=~0ULL; break;
        }
        JcosU64 tag=~0ULL; JcosU32 data=~0U;
        CHECK(!polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&data) && !tag && !data);
    }
    setup(); observation.array_mode=0;
    JcosU64 tag=0; JcosU32 data=0; PolarisPlanePreview p;
    CHECK(polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&data) && init(&p,0));
    CHECK(!polaris_plane_preview_accept(&p,18,29,tag,data) && p.closed);
    CHECK(init(&p,0) && !polaris_plane_preview_accept(&p,17,30,tag,data) && p.closed);
    CHECK(init(&p,0) && polaris_plane_preview_accept(&p,17,29,tag,data) &&
        !polaris_plane_preview_accept(&p,17,29,tag,data) && p.closed);
    memset(&p,0,sizeof(p));
    CHECK(!polaris_plane_preview_accept(&p,0,0,tag,0) && p.closed);
    CHECK(!polaris_plane_preview_init(&p,0,29,0xF400200000ULL,0,&witness));
    CHECK(!polaris_plane_preview_init(&p,17,0,0xF400200000ULL,0,&witness));
    CHECK(!init(&p,6));
    CHECK(!polaris_plane_preview_init(&p,17,29,0xF400200001ULL,0,&witness));
    CHECK(!polaris_plane_preview_init(&p,17,29,1ULL<<40,0,&witness));
    CHECK(!polaris_plane_preview_init(&p,17,29,0xF400200000ULL,0,0));
    CHECK(polaris_plane_blockers(0));
    CHECK(!polaris_plane_recipe_step(&observation,&witness,6,0,&tag,&data));
    CHECK(!polaris_plane_recipe_step(&observation,&witness,0,5,&tag,&data));
    CHECK(!polaris_plane_recipe_step(0,&witness,0,0,&tag,&data));
    CHECK(!polaris_plane_recipe_step(&observation,0,0,0,&tag,&data));
    CHECK(!polaris_plane_recipe_step(&observation,&witness,0,0,0,&data));
    CHECK(!polaris_plane_recipe_step(&observation,&witness,0,0,&tag,0));
    CHECK(!jcos_polaris_plane_tag(65536,1,0,7) &&
        !jcos_polaris_plane_tag(0,257,0,7) && !jcos_polaris_plane_tag(0,1,65536,7) &&
        !jcos_polaris_plane_tag(0,1,0,65536));
    puts("PASS: actual Ring3 rebind policy, independent kernel sequence, all6 pipes,640 bit-tamper refusals,stable0/0x20,62 unknown flip-bit refusals,transition refusal,24-bit wrap,stale identities,invalid snapshots and fail-closed data-only preview.");
    return 0;
}
