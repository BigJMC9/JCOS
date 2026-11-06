/* Address arithmetic replay only: no GPU/MMIO mapping or authority. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/polaris_vram.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"VRAM line%d: %s\\n",__LINE__,#x); return 1; } } while (0)
static BootInfo boot;
static BootDisplayBar aperture;
static u32 values[15], before[2], after[2];
static PolarisVramObservation out;
static void setup(void) {
    memset(&boot,0,sizeof(boot)); memset(values,0,sizeof(values)); memset(&aperture,0,sizeof(aperture));
    boot.framebuffer_base=0x50000000ULL; boot.framebuffer_size=0x300000ULL;
    boot.framebuffer_width=1024; boot.framebuffer_height=768;
    boot.framebuffer_pixels_per_scanline=1024; boot.framebuffer_pixel_format=1;
    aperture.base=0x50000000ULL; aperture.size=0x10000000ULL; aperture.flags=15;
    values[POLARIS_REG_CRTC_CONTROL]=0x80410311U;
    values[POLARIS_REG_GRPH_ENABLE]=1; values[POLARIS_REG_GRPH_CONTROL]=0x0A10800AU;
    values[POLARIS_REG_PRIMARY_HIGH]=0xF4U; values[POLARIS_REG_PITCH]=1024;
    values[POLARIS_REG_X_END]=1024; values[POLARIS_REG_Y_END]=768;
    before[0]=after[0]=0xF4FFF400U; before[1]=after[1]=0xF4000000U;
}
static int observe(void) { return polaris_vram_observe(&boot,&aperture,values,before,after,&out); }
static int cleared(void) {
    const unsigned char *p=(const unsigned char *)&out;
    for (unsigned i=0;i<sizeof(out);++i) if (p[i]) return 0;
    return 1;
}
int main(void) {
    setup();
    CHECK(observe() && out.vram_gpu_start==0xF400000000ULL && out.vram_gpu_end==0xF500000000ULL &&
        out.hdp_gpu_start==out.vram_gpu_start && out.surface_gpu_address==0xF400000000ULL &&
        out.surface_cpu_address==0x50000000ULL && out.surface_bytes==0x300000ULL);
    /* Nonzero aperture offsets must correlate in both address spaces. */
    boot.framebuffer_base+=0x200000; values[POLARIS_REG_PRIMARY_LOW]=0x200000;
    CHECK(observe() && out.surface_gpu_address==0xF400200000ULL && out.surface_cpu_address==0x50200000ULL);
    /* End-exact aperture is allowed; one-byte excess is not. */
    boot.framebuffer_base=aperture.base+aperture.size-0x300000;
    values[POLARIS_REG_PRIMARY_LOW]=0xFD00000;
    CHECK(observe());
    boot.framebuffer_size++;
    CHECK(!observe() && cleared());
    for (unsigned test=0;test<34;++test) {
        setup();
        switch (test) {
        case 0: after[0]^=1; break;
        case 1: after[1]^=1; break;
        case 2: before[0]=after[0]=~0U; break;
        case 3: before[1]=after[1]=~0U; break;
        case 4: boot.framebuffer_width=1280; break;
        case 5: boot.framebuffer_height=720; break;
        case 6: boot.framebuffer_pixels_per_scanline=1088; break;
        case 7: boot.framebuffer_pixel_format=0; break;
        case 8: values[POLARIS_REG_GRPH_CONTROL]=2; break;
        case 9: values[POLARIS_REG_GRPH_ENABLE]=3; break;
        case 10: values[POLARIS_REG_GRPH_SWAP]=1; break;
        case 11: values[POLARIS_REG_CRTC_BLANK]=0x100; break;
        case 12: values[POLARIS_REG_CRTC_INTERLACE]=1; break;
        case 13: values[POLARIS_REG_CRTC_CONTROL]=1; break;
        case 14: values[POLARIS_REG_CRTC_CONTROL]|=0x1000000; break;
        case 15: values[POLARIS_REG_PRIMARY_HIGH]=0x100; break;
        case 16: values[POLARIS_REG_PRIMARY_LOW]=1; break;
        case 17: aperture.flags=3; break;
        case 18: aperture.reserved=1; break;
        case 19: aperture.base=0; break;
        case 20: aperture.base++; break;
        case 21: aperture.size=0; break;
        case 22: aperture.base=1ULL<<40; break;
        case 23: aperture.size=~0ULL; break;
        case 24: boot.framebuffer_base=aperture.base-1; break;
        case 25: boot.framebuffer_size--; break;
        case 26: boot.framebuffer_base=aperture.base+aperture.size; break;
        case 27: boot.framebuffer_size=~0ULL; break;
        case 28: before[0]=after[0]=0xF300F400; break;
        case 29: before[1]=after[1]=0xF4000100; break;
        case 30: before[0]=after[0]=0xF400F400; break;
        case 31: values[POLARIS_REG_PRIMARY_LOW]=0x100; break;
        case 32: before[0]=after[0]=0xFFFFFFFF; break;
        case 33: values[POLARIS_REG_PITCH]=~0U; break;
        }
        memset(&out,0xAA,sizeof(out));
        CHECK(!observe() && cleared());
    }
    for (unsigned field=POLARIS_REG_PITCH;field<15;++field) {
        setup(); values[field]^=1;
        CHECK(!observe() && cleared());
    }
    setup();
    CHECK(!polaris_vram_observe(0,&aperture,values,before,after,&out) && cleared());
    CHECK(!polaris_vram_observe(&boot,0,values,before,after,&out) && cleared());
    CHECK(!polaris_vram_observe(&boot,&aperture,0,before,after,&out) && cleared());
    CHECK(!polaris_vram_observe(&boot,&aperture,values,0,after,&out) && cleared());
    CHECK(!polaris_vram_observe(&boot,&aperture,values,before,0,&out) && cleared());
    CHECK(!polaris_vram_observe(&boot,&aperture,values,before,after,0));
    puts("PASS: fixed inherited-surface MC/HDP/GOP correlation, exact edges and malformed/changed/overflow refusals; no device access or authority.");
    return 0;
}
