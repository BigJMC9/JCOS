/* Production bring-up controller + fixed batch, CPU/PCI/service/VM doubles.
 * No UART/device handles and no MMIO load primitive are linked. Each process
 * runs one failure boundary because production admission is once per boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "polaris_live_capture.h"
#include "polaris_read_profile.h"
#include "polaris_observation.h"
#include "device_register_reader.h"
#include "managed_service.h"
#include "display.h"
#include "register_probe_protocol.h"
static int fault;
static u32 location_loads, inventory_reads, loads, grants, release_calls, metadata_calls, tick_calls, frequency_calls;
static bool binding_live, held, reader_live, assignment_live, attempted_message;
static u64 owner_id;
static PciAssignmentState parent_state;
static ManagedServiceState service_state;
static DeviceRegisterReader *reader;
static BootInfo boot;
static PciDevice selected;
static bool complete_message, retained_message, cleanup_failure;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"controller case%d line%d: %s\n",fault,__LINE__,#x); return 1; } } while (0)
void terminal_write(const char *text) { (void)text; }
void terminal_putchar(char c) { (void)c; }
void terminal_write_u64(u64 v) { (void)v; }
void terminal_write_hex(u64 v) { (void)v; }
void terminal_writeln(const char *text) {
    if (!strcmp(text,"POLARIS PHYSICAL OBSERVATION: COMPLETE")) complete_message = true;
    if (!strcmp(text,"CPU MAPPINGS/READER/SERVICE DRAINED; PARENT QUARANTINE RETAINED.")) retained_message = true;
    if (strstr(text,"CLEANUP INCOMPLETE")) cleanup_failure = true;
    if (strstr(text,"ONE ATTEMPT PER BOOT")) attempted_message = true;
}
bool polaris_read_profile_inspect(const BootInfo *b, PolarisReadReport *r) {
    (void)b; memset(r,0,sizeof(*r)); r->blockers=POLARIS_READ_UNQUALIFIED;
    return fault != 1;
}
bool serial_available(void) { return true; }
bool scheduler_preemption_enabled(void) { return true; }
bool timer_initialized(void) { return true; }
bool device_mmio_interrupts_enabled(void) { return fault != 54; }
bool framebuffer_writers_idle(void) { return true; }
bool framebuffer_firmware_valid(void) { return fault != 57; }
DisplayLeaseState display_direct_state(void) { return fault == 53 ? DISPLAY_LEASE_USER : DISPLAY_LEASE_KERNEL; }
u32 timer_frequency(void) { return fault == 56 && frequency_calls++ ? 200U : 100U; }
u64 timer_ticks(void) { return (u64)++tick_calls * (fault == 55 ? 200ULL : 1ULL); }
u32 pci_device_count(void) { return 1; }
const PciDevice *pci_device(u32 index) { return index ? 0 : &selected; }
bool display_probe_matches_gop(const BootInfo *b,const PciDevice *d) { (void)b; return d==&selected; }
bool display_probe_read(const PciDevice *d,DisplayProbeInfo *i) {
    (void)d; memset(i,0,sizeof(*i)); ++inventory_reads;
    return fault!=70 && !(fault==71 && inventory_reads==2);
}
bool display_probe_firmware_bar(const BootInfo *b,const PciDevice *d,
    const DisplayProbeInfo *i,u32 index,BootDisplayBar *out) {
    (void)b; (void)d; (void)i;
    if (index || fault==66) return false;
    memset(out,0,sizeof(*out)); out->base=0x50000000; out->size=0x10000000;
    out->flags=fault==72 ? 3 : 15; return true;
}
bool polaris_mmio_prepare_location(PolarisMmioBinding *b,const BootInfo *info,Process *p,u64 id) {
    (void)b; (void)info; (void)p; return binding_live && id==19 && fault!=58;
}
bool polaris_mmio_read_location_held(PolarisMmioHold *h,u64 id,Process *p,
    PolarisLocationRegister field,u32 *out) {
    (void)h; (void)p; ++location_loads;
    if (!held || id!=23 || field>=POLARIS_LOCATION_COUNT ||
        (location_loads<=2 ? loads!=6 : loads!=36) ||
        (fault>=59 && fault<=62 && location_loads==(u32)(fault-58))) return false;
    *out=field==POLARIS_LOCATION_MC_FB ? 0xF4FFF400U : 0xF4000000U;
    if ((fault==63 && location_loads==3) || (fault==64 && location_loads==4)) *out^=1;
    if (fault==65) *out=~0U;
    return true;
}
bool managed_service_start(ManagedService *s,const ManagedServiceSpec *spec) {
    (void)spec;
    memset(s,0,sizeof(*s)); s->program.process.id=11; s->program.process.initialized=true;
    service_state=fault==2 ? MANAGED_SERVICE_FAILED : MANAGED_SERVICE_RUNNING;
    return fault!=2;
}
bool managed_service_connect_kernel(ManagedService *s,ManagedServiceConnection *c) {
    (void)s; memset(c,0,sizeof(*c)); c->incarnation=17; return fault!=3;
}
ManagedServiceState managed_service_state(ManagedService *s) { (void)s; return service_state; }
ManagedServiceStopResult managed_service_stop_bounded(ManagedService *s) {
    (void)s;
    if (fault==50) return MANAGED_SERVICE_STOP_FAILED;
    service_state=MANAGED_SERVICE_STOPPED; return MANAGED_SERVICE_STOP_GRACEFUL;
}
bool managed_service_recover(ManagedService *s) { (void)s; service_state=MANAGED_SERVICE_STOPPED; return true; }
bool pci_assignment_create(PciAssignment *a,const PciDevice *d) {
    (void)a; (void)d; assignment_live=fault!=4; parent_state=PCI_ASSIGNMENT_AVAILABLE; return assignment_live;
}
bool pci_assignment_claim(PciAssignment *a,Process *p,CapabilityHandle *cap) {
    (void)a; if (fault==5) return false;
    owner_id=p->id; parent_state=PCI_ASSIGNMENT_ASSIGNED; *cap=3; return true;
}
bool pci_assignment_valid(const PciAssignment *a) { (void)a; return assignment_live; }
PciAssignmentState pci_assignment_state(const PciAssignment *a) { (void)a; return parent_state; }
bool pci_assignment_destroy(PciAssignment *a) { (void)a; if (owner_id) return false; assignment_live=false; return true; }
bool pci_assignment_force_quarantine(PciAssignment *a,u64 id) {
    (void)a; if (id!=owner_id) return false; parent_state=PCI_ASSIGNMENT_QUARANTINED; return true;
}
bool polaris_mmio_prepare(PolarisMmioBinding *b,const BootInfo *info,PciAssignment *a,
                         Process *p,CapabilityHandle cap) {
    (void)b; (void)info; (void)a; (void)p; (void)cap; binding_live=true; return fault!=6;
}
bool polaris_mmio_binding_valid(const PolarisMmioBinding *b) { (void)b; return binding_live; }
u64 polaris_mmio_binding_identity(const PolarisMmioBinding *b) { (void)b; return 19; }
bool polaris_mmio_close(PolarisMmioBinding *b) { (void)b; return binding_live; }
bool polaris_mmio_hold_acquire(PolarisMmioHold *h,PolarisMmioBinding *b,u64 id,Process *p,u64 *out) {
    (void)h; (void)b; (void)p;
    if (fault==7 || id!=19) return false;
    *out=23; held=true; return true;
}
bool polaris_mmio_hold_release(PolarisMmioHold *h,u64 id) {
    (void)h; ++release_calls;
    if (fault==52 || id!=23 || !held) return false;
    held=false; return true;
}
bool polaris_mmio_cleanup(PolarisMmioBinding *b) {
    (void)b; if (held || fault==45) return false; binding_live=false; return true;
}
bool polaris_mmio_read_held(PolarisMmioHold *h,u64 id,Process *p,u32 pipe,PolarisDisplayRegister field,u32 *out) {
    (void)h; (void)p; ++loads;
    if (!held || id!=23 || (fault>=8 && fault<=43 && loads==(u32)(fault-7))) return false;
    if (fault==44) { *out=~0U; return true; }
    *out=0;
    if (field==POLARIS_REG_CRTC_CONTROL) *out=pipe==3U ? 0x10001U : 0;
    if (field==POLARIS_REG_GRPH_ENABLE) *out=1;
    if (field==POLARIS_REG_GRPH_CONTROL) *out=fault==67 ? 2 : 0x0A10800A;
    if (field==POLARIS_REG_PRIMARY_HIGH) *out=0xF4;
    if (field==POLARIS_REG_PITCH || field==POLARIS_REG_X_END) *out=1024;
    if (field==POLARIS_REG_Y_END) *out=768;
    return true;
}
bool device_register_reader_create_observation(DeviceRegisterReader *r,Process *p,u32 pipe,
                                               const u32 *values,u64 duration,CapabilityHandle *out) {
    (void)p; (void)duration; ++grants;
    if (fault==46 || held || binding_live || pipe!=3 || values[POLARIS_REG_PITCH]!=1024) return false;
    reader=r; r->resource.id=29; reader_live=true; *out=31; return true;
}
bool device_register_reader_valid(const DeviceRegisterReader *r) { return reader_live && r==reader; }
bool device_register_reader_revoke(DeviceRegisterReader *r) { r->revoked=true; return true; }
bool device_register_reader_destroy(DeviceRegisterReader *r) {
    (void)r; if (fault==51 || service_state!=MANAGED_SERVICE_STOPPED) return false;
    reader_live=false; return true;
}
bool managed_service_call(ManagedService *s,const ManagedServiceConnection *c,const IpcMessage *q,IpcMessage *r) {
    (void)s;
    memset(r,0,sizeof(*r)); r->word_count=4; r->words[0]=REGISTER_PROBE_REPLY; r->words[1]=c->incarnation;
    if (fault==47) return false;
    if (fault==48) r->words[1]++;
    if (q->words[0]==REGISTER_PROBE_CAPTURE) {
        if (q->words[1]!=31 || q->words[2]!=29 || q->words[3]!=3) return false;
        reader->successful_reads=30; reader->revoked=true;
        r->words[2]=REGISTER_PROBE_CAPTURE_DECODED; r->words[3]=fault==68 ? 0x100200000ULL : 0xF400000000ULL;
    } else {
        if (fault==49 || q->words[0]!=REGISTER_PROBE_OBSERVATION || q->words[1]!=29 || q->words[2]>3) return false;
        ++metadata_calls; r->words[2]=q->words[2]+1;
        const u64 values[4]={0x300000ULL,(1024ULL<<32)|768ULL,1024ULL,0x10002ULL};
        r->words[3]=values[q->words[2]] ^ (fault==69 && !q->words[2]);
    }
    return true;
}
int main(int argc,char **argv) {
    fault=argc>1 ? atoi(argv[1]) : 0;
    boot.framebuffer_base=0x50000000; boot.framebuffer_size=0x300000;
    boot.framebuffer_width=1024; boot.framebuffer_height=768;
    boot.framebuffer_pixels_per_scanline=1024; boot.framebuffer_pixel_format=1;
    polaris_live_capture_run(&boot);
    CHECK(loads<=36 && location_loads<=4 && loads+location_loads<=40);
    if (!fault) CHECK(complete_message && loads==36 && location_loads==4 && grants==1 && metadata_calls==4 && release_calls==1);
    else CHECK(!complete_message);
    if (fault==1 || fault==53 || fault==54) CHECK(!assignment_live && !loads && !grants);
    if (fault==2 || fault==3 || fault==4 || fault==5 || fault==66 || fault==70) CHECK(!assignment_live && !loads && !grants);
    if (fault>=6 && fault!=53 && fault!=54 && fault!=66 && fault!=70) CHECK(assignment_live && parent_state==PCI_ASSIGNMENT_QUARANTINED);
    if (fault==45 || fault==50 || fault==51 || fault==52) CHECK(cleanup_failure);
    else CHECK(!binding_live && !reader_live && !held && service_state==MANAGED_SERVICE_STOPPED);
    if (!fault) CHECK(retained_message);
    if (fault!=1 && fault!=53 && fault!=54) {
        u32 before=loads; polaris_live_capture_run(&boot);
        CHECK(attempted_message && loads==before);
    }
    return 0;
}
