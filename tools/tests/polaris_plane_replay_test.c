/* Replay recorded data through production code. No device access or authority. */
#include <stdio.h>
#include <string.h>
#include "../../user/drivers/amd_polaris_plane.h"
#include "../../kernel/polaris_plane.h"
#include "../../kernel/polaris_vram.h"
#include "../../kernel/polaris_observation.h"
#include "polaris_replay_fixture.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"replay line%d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc,char **argv) {
    PolarisObservationBatch batch;
    polaris_observation_init(&batch);
    while (!batch.complete) {
        u32 pipe,field;
        CHECK(polaris_observation_next(&batch,&pipe,&field));
        CHECK(polaris_observation_accept(&batch,batch.reads<6 ? replay_controls[pipe] : replay_values[field]));
    }
    CHECK(batch.pipe==replay_pipe && batch.reads==36);
    PolarisDisplaySnapshot snapshot;
    memset(&snapshot,0,sizeof(snapshot));
    snapshot.version=POLARIS_DISPLAY_SNAPSHOT_VERSION; snapshot.pipe=replay_pipe;
    memcpy(snapshot.values,replay_values,sizeof(replay_values));
    PolarisDisplayObservation observation;
    CHECK(polaris_display_decode(&snapshot,&observation) && !observation.restrictions);
    BootInfo boot; BootDisplayBar aperture; PolarisVramObservation vram;
    memset(&boot,0,sizeof(boot)); memset(&aperture,0,sizeof(aperture));
    boot.framebuffer_base=0x50000000ULL; boot.framebuffer_size=0x300000ULL;
    boot.framebuffer_width=boot.framebuffer_pixels_per_scanline=1024;
    boot.framebuffer_height=768; boot.framebuffer_pixel_format=1;
    aperture.base=0x50000000ULL; aperture.size=0x10000000ULL; aperture.flags=15;
    CHECK(polaris_vram_observe(&boot,&aperture,replay_values,replay_location[0],replay_location[1],&vram));
    CHECK(observation.surface_gpu_address==vram.surface_gpu_address && observation.linear_span_bytes==vram.surface_bytes);
    PolarisPlanePreview preview; JcosU64 tag=0; JcosU32 value=0;
    if (argc==2 && !strcmp(argv[1],"--expect-refusal")) {
        CHECK(polaris_plane_blockers(&replay_witness)==POLARIS_PLANE_BLOCK_FLIP &&
            !polaris_plane_preview_init(&preview,17,29,vram.surface_gpu_address,replay_pipe,&replay_witness) &&
            !polaris_plane_recipe_step(&observation,&replay_witness,replay_pipe,0,&tag,&value));
        puts("PASS: original zero-flip policy reproduces measured0x20 refusal; decode/correlation valid; no GPU access.");
        return 0;
    }
    CHECK(argc==1 && polaris_plane_preview_init(&preview,17,29,vram.surface_gpu_address,replay_pipe,&replay_witness));
    for (unsigned i=0;i<5;++i) {
        CHECK(polaris_plane_recipe_step(&observation,&replay_witness,replay_pipe,i,&tag,&value));
        CHECK(polaris_plane_preview_accept(&preview,17,29,tag,value));
    }
    CHECK(preview.closed && preview.next==5);
    puts("PASS: recorded physical data through actual Ring3 decoder/planner, kernel VRAM correlation and independent five-step preview; replay only, no GPU write or physical recipe execution.");
    return 0;
}
