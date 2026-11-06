#include "polaris_live_capture.h"
#include "terminal.h"
#ifndef JCOS_POLARIS_LIVE_CAPTURE
#define JCOS_POLARIS_LIVE_CAPTURE 0
#endif
#if JCOS_POLARIS_LIVE_CAPTURE
#include "polaris_observation.h"
#include "polaris_vram.h"
#include "polaris_plane.h"
#include "polaris_mmio.h"
#include "polaris_read_profile.h"
#include "device_mmio.h"
#include "device_register_reader.h"
#include "managed_service.h"
#include "display.h"
#include "framebuffer.h"
#include "serial.h"
#include "scheduler.h"
#include "timer.h"
#include "lib.h"
#include "register_probe_protocol.h"

static ManagedService g_capture_service;
static PciAssignment g_capture_assignment;
static PolarisMmioBinding g_capture_binding;
static PolarisMmioHold g_capture_hold;
static DeviceRegisterReader g_capture_reader;
static u64 g_capture_hold_id, g_capture_owner_id;
static bool g_capture_attempted;

static void pair(const char *label, u64 value) {
    terminal_write(label); terminal_write_hex(value);
}
static bool cleanup_capture(void) {
    bool ok = true;
    if (device_register_reader_valid(&g_capture_reader))
        ok = device_register_reader_revoke(&g_capture_reader) && ok;
    /* No CPU load remains in progress when this synchronous controller returns.
     * Close before dropping its hold; never erase a failed cleanup record. */
    if (polaris_mmio_binding_valid(&g_capture_binding))
        ok = polaris_mmio_close(&g_capture_binding) && ok;
    if (g_capture_hold_id) {
        if (polaris_mmio_hold_release(&g_capture_hold, g_capture_hold_id)) g_capture_hold_id = 0;
        else ok = false;
    }
    if (pci_assignment_valid(&g_capture_assignment) && g_capture_owner_id &&
        pci_assignment_state(&g_capture_assignment) != PCI_ASSIGNMENT_QUARANTINED)
        ok = pci_assignment_force_quarantine(&g_capture_assignment, g_capture_owner_id) && ok;
    if (polaris_mmio_binding_valid(&g_capture_binding))
        ok = polaris_mmio_cleanup(&g_capture_binding) && ok;
    ManagedServiceState state = managed_service_state(&g_capture_service);
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult stopped = managed_service_stop_bounded(&g_capture_service);
        ok = (stopped == MANAGED_SERVICE_STOP_GRACEFUL || stopped == MANAGED_SERVICE_STOP_FORCED) && ok;
    } else if (state != MANAGED_SERVICE_STOPPED) ok = managed_service_recover(&g_capture_service) && ok;
    if (device_register_reader_valid(&g_capture_reader))
        ok = device_register_reader_destroy(&g_capture_reader) && ok;
    if (pci_assignment_valid(&g_capture_assignment) && !g_capture_owner_id)
        ok = pci_assignment_destroy(&g_capture_assignment) && ok;
    return ok;
}
static bool admitted(u64 start, u32 frequency) {
    return frequency && timer_frequency()==frequency &&
        timer_ticks()-start < (u64)frequency*POLARIS_CAPTURE_MAX_SECONDS &&
        framebuffer_firmware_valid();
}
static bool capture_location(Process *owner, u64 start, u32 frequency,
    u32 pipe_reads, u32 *extra_reads, u32 values[2]) {
    for (u32 field=0; field<POLARIS_LOCATION_COUNT; ++field) {
        if (!admitted(start,frequency)) return false;
        terminal_write("GPU READ #"); terminal_write_u64(pipe_reads+*extra_reads+1U);
        terminal_write(" LOCATION="); terminal_write_u64(field); terminal_putchar('\n');
        if (!polaris_mmio_read_location_held(&g_capture_hold,g_capture_hold_id,owner,
                (PolarisLocationRegister)field,&values[field])) return false;
        ++*extra_reads;
        pair("  VALUE=",values[field]); terminal_putchar('\n');
        if (values[field]==~0U) return false;
    }
    return true;
}
static bool capture_witness(Process *owner,u64 start,u32 frequency,u32 pipe,
    u32 pipe_reads,u32 *extra_reads,u32 values[POLARIS_PLANE_WITNESS_COUNT]) {
    for (u32 field=0;field<POLARIS_PLANE_WITNESS_COUNT;++field) {
        if (!admitted(start,frequency)) return false;
        terminal_write("GPU READ #"); terminal_write_u64(pipe_reads+*extra_reads+1U);
        terminal_write(" PIPE="); terminal_write_u64(pipe);
        terminal_write(" WITNESS="); terminal_write_u64(field); terminal_putchar('\n');
        if (!polaris_mmio_read_witness_held(&g_capture_hold,g_capture_hold_id,owner,pipe,
                (PolarisPlaneWitnessRegister)field,&values[field])) return false;
        ++*extra_reads;
        pair("  VALUE=",values[field]); terminal_putchar('\n');
        if (values[field]==~0U) return false;
    }
    return true;
}
void polaris_live_capture_run(const BootInfo *boot) {
    terminal_writeln("POLARIS INHERITED-SCANOUT CAPTURE / EXPERIMENTAL BUILD PROFILE 3");
    terminal_writeln("READ-ONLY GPU REGISTER EXPERIMENT; NOT NATIVE MODESETTING OR RESET.");
    if (g_capture_attempted) {
        terminal_writeln("REFUSED: ONE ATTEMPT PER BOOT; RETAINED ASSIGNMENT MUST NOT BE REUSED.");
        return;
    }
    PolarisReadReport report;
    const u32 local = POLARIS_READ_BLOCK_TARGET | POLARIS_READ_BLOCK_BAR_PLAN |
        POLARIS_READ_BLOCK_POWER | POLARIS_READ_BLOCK_CPU | POLARIS_READ_BLOCK_UART |
        POLARIS_READ_BLOCK_SCANOUT;
    if (!boot || !polaris_read_profile_inspect(boot, &report) || (report.blockers & local) ||
        !serial_available() || !scheduler_preemption_enabled() || !timer_initialized() ||
        !device_mmio_interrupts_enabled() || !framebuffer_writers_idle() ||
        display_direct_state() != DISPLAY_LEASE_KERNEL ||
        boot->framebuffer_width != 1024U || boot->framebuffer_height != 768U ||
        boot->framebuffer_pixels_per_scanline != 1024U || boot->framebuffer_pixel_format != 1U) {
        terminal_writeln("REFUSED: EXACT GOP/BOARD/BAR5/D0/CPU/UART/KERNEL-LEASE PROFILE NOT SATISFIED.");
        return;
    }
    const PciDevice *selected = 0;
    for (u32 i = 0; i < pci_device_count(); ++i) {
        const PciDevice *candidate = pci_device(i);
        if (!display_probe_matches_gop(boot, candidate)) continue;
        if (selected) { terminal_writeln("REFUSED: AMBIGUOUS SELECTED DEVICE."); return; }
        selected = candidate;
    }
    if (!selected) return;
    g_capture_attempted = true; /* Includes failed preparation: no silent retry. */
    terminal_writeln("ASSUMPTIONS: OPERATIONAL GPU, UNCHANGED FIRMWARE SCANOUT; NO HOTPLUG/POWER/RESET.");
    terminal_writeln("PLAN: SIX CRTC CONTROL QUERIES; UNIQUE RUNNING PIPE; TWO FIFTEEN-FIELD SAMPLES.");
    terminal_writeln("MAXIMUM 46 DWORD LOADS; TWO-SECOND ADMISSION; NO BUS-TRANSACTION TIMEOUT.");
    terminal_writeln("MC/HDP LOCATION PAIR BEFORE/AFTER PIPE SAMPLES; VERIFIED BAR0/GOP CORRELATION ONLY.");
    terminal_writeln("PLANE WITNESSES: UPDATE, FLIP CONTROL, FRAME COUNT BEFORE/AFTER; RECIPE PREVIEW ONLY.");
    bool success = false;
    BootDisplayBar aperture;
    DisplayProbeInfo inventory;
    if (!display_probe_read(selected,&inventory) ||
        !display_probe_firmware_bar(boot,selected,&inventory,0U,&aperture)) {
        terminal_writeln("REFUSED: CURRENT SELECTED BAR0 FIRMWARE PROVENANCE UNAVAILABLE.");
        return;
    }
    ManagedServiceSpec spec;
    spec.path = "/bin/registerprobe.elf";
    spec.shutdown_message = REGISTER_PROBE_SHUTDOWN;
    spec.shutdown_reply = REGISTER_PROBE_STOPPED;
    ManagedServiceConnection connection;
    if (!managed_service_start(&g_capture_service, &spec) ||
        !managed_service_connect_kernel(&g_capture_service, &connection)) goto finish;
    Process *owner = &g_capture_service.program.process;
    CapabilityHandle assignment_cap = 0;
    if (!pci_assignment_create(&g_capture_assignment, selected) ||
        !pci_assignment_claim(&g_capture_assignment, owner, &assignment_cap)) goto finish;
    g_capture_owner_id = owner->id;
    if (!polaris_mmio_prepare(&g_capture_binding, boot, &g_capture_assignment, owner, assignment_cap) ||
        !polaris_mmio_prepare_location(&g_capture_binding,boot,owner,
            polaris_mmio_binding_identity(&g_capture_binding)) ||
        !polaris_mmio_hold_acquire(&g_capture_hold, &g_capture_binding,
            polaris_mmio_binding_identity(&g_capture_binding), owner, &g_capture_hold_id)) goto finish;
    PolarisObservationBatch batch;
    polaris_observation_init(&batch);
    u32 frequency = timer_frequency();
    u64 start = timer_ticks();
    u32 location_before[2], location_after[2], extra_reads=0;
    PolarisPlaneWitness witness;
    while (!batch.complete) {
        if (batch.reads==POLARIS_DISPLAY_PIPE_COUNT &&
            (!capture_location(owner,start,frequency,batch.reads,&extra_reads,location_before) ||
             !capture_witness(owner,start,frequency,batch.pipe,batch.reads,&extra_reads,witness.values[0]))) goto finish;
        u32 pipe, field, value;
        if (!admitted(start,frequency) ||
            !polaris_observation_next(&batch, &pipe, &field)) goto finish;
        /* Journal before the transaction so a platform stall has a last known
         * operation in the independently retained host serial log. */
        terminal_write("GPU READ #"); terminal_write_u64(batch.reads + extra_reads + 1U);
        terminal_write(" PIPE="); terminal_write_u64(pipe);
        terminal_write(" FIELD="); terminal_write_u64(field); terminal_putchar('\n');
        if (!polaris_mmio_read_held(&g_capture_hold, g_capture_hold_id, owner,
                pipe, (PolarisDisplayRegister)field, &value)) goto finish;
        pair("  VALUE=", value); terminal_putchar('\n');
        if (!polaris_observation_accept(&batch, value)) {
            terminal_writeln("CAPTURE STOPPED: ABSENT/TRANSITIONING/AMBIGUOUS/BLANK/CHANGED STATE.");
            goto finish;
        }
    }
    if (!capture_witness(owner,start,frequency,batch.pipe,batch.reads,&extra_reads,witness.values[1]) ||
        !capture_location(owner,start,frequency,batch.reads,&extra_reads,location_after)) goto finish;
    terminal_write("PHYSICAL GPU CAPTURE COMPLETE: READS="); terminal_write_u64(batch.reads+extra_reads);
    terminal_write(" PIPE="); terminal_write_u64(batch.pipe); terminal_putchar('\n');
    /* Stop live access before dispatching immutable data to Ring3. The INT80
     * reader remains RAM-only; no potentially stalling load runs IRQ-masked. */
    if (!polaris_mmio_close(&g_capture_binding) ||
        !polaris_mmio_hold_release(&g_capture_hold, g_capture_hold_id)) goto finish;
    g_capture_hold_id = 0;
    if (!polaris_mmio_cleanup(&g_capture_binding)) goto finish;
    PolarisVramObservation vram;
    if (!display_probe_read(selected,&inventory) ||
        !display_probe_firmware_bar(boot,selected,&inventory,0U,&aperture) ||
        !polaris_vram_observe(boot,&aperture,batch.values,location_before,location_after,&vram)) {
        terminal_writeln("MC/HDP/GOP ADDRESS CORRELATION: REFUSED / UNCHANGED FIXED PROFILE REQUIRED.");
        goto finish;
    }
    pair("MC VRAM GPU START=",vram.vram_gpu_start); pair(" END EXCLUSIVE=",vram.vram_gpu_end);
    pair(" HDP GPU START=",vram.hdp_gpu_start); terminal_putchar('\n');
    pair("EXISTING GOP SURFACE: CPU=",vram.surface_cpu_address);
    pair(" GPU=",vram.surface_gpu_address); pair(" BYTES=",vram.surface_bytes); terminal_putchar('\n');
    terminal_writeln("MC/HDP/GOP ADDRESS CORRELATION: OBSERVED; NO NEW VRAM ALLOCATION OR ACCESS GRANT.");
    CapabilityHandle reader_cap = 0;
    if (!device_register_reader_create_observation(&g_capture_reader, owner, batch.pipe,
            batch.values, (u64)timer_frequency() * POLARIS_CAPTURE_MAX_SECONDS, &reader_cap)) goto finish;
    IpcMessage request, reply;
    k_memset(&request, 0, sizeof(request)); k_memset(&reply, 0, sizeof(reply));
    request.word_count = 4U;
    request.words[0] = REGISTER_PROBE_CAPTURE;
    request.words[1] = reader_cap;
    request.words[2] = g_capture_reader.resource.id;
    request.words[3] = batch.pipe;
    if (!managed_service_call(&g_capture_service, &connection, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != REGISTER_PROBE_REPLY ||
        reply.words[1] != connection.incarnation ||
        (reply.words[2] & 0xFFFFULL) != 0ULL ||
        !(reply.words[2] & REGISTER_PROBE_CAPTURE_DECODED) ||
        g_capture_reader.successful_reads != 30ULL || !g_capture_reader.revoked) goto finish;
    u64 gpu_address = reply.words[3], restrictions = reply.words[2] >> 32, metadata[4];
    for (u32 i = 0; i < 4U; ++i) {
        request.words[0] = REGISTER_PROBE_OBSERVATION;
        request.words[1] = g_capture_reader.resource.id;
        request.words[2] = i;
        request.words[3] = 0;
        if (!managed_service_call(&g_capture_service, &connection, &request, &reply) ||
            reply.word_count != 4U || reply.words[0] != REGISTER_PROBE_REPLY ||
            reply.words[1] != connection.incarnation || reply.words[2] != i + 1ULL) goto finish;
        metadata[i] = reply.words[3];
    }
    terminal_writeln("RING3 DECODE OF IMMUTABLE PHYSICAL OBSERVATION:");
    pair("  GPU SURFACE=", gpu_address); pair(" LINEAR SPAN=", metadata[0]);
    pair(" RESTRICTIONS=", restrictions); terminal_putchar('\n');
    terminal_write("  GRPH EXTENT="); terminal_write_u64(metadata[1] >> 32);
    terminal_putchar('x'); terminal_write_u64((u32)metadata[1]);
    terminal_write(" PITCH="); terminal_write_u64(metadata[2]);
    pair(" DEPTH/FORMAT/ARRAY=", metadata[3]); terminal_putchar('\n');
    terminal_writeln("GPU ADDRESS IS NOT A CPU PHYSICAL ADDRESS; NO VRAM AUTHORITY GRANTED.");
    if (restrictions || gpu_address!=vram.surface_gpu_address || metadata[0]!=vram.surface_bytes ||
        metadata[1]!=((1024ULL<<32)|768ULL) || metadata[2]!=1024ULL || metadata[3]!=0x10002ULL) {
        terminal_writeln("REFUSED: RING3 DECODE DISAGREES WITH FIXED PHYSICAL CORRELATION PROFILE.");
        goto finish;
    }
    u32 blockers=polaris_plane_blockers(&witness);
    pair("PLANE PREFLIGHT BLOCKERS=",blockers); terminal_putchar('\n');
    if (blockers) {
        terminal_writeln("PLANE REBIND PREVIEW: REFUSED; UPDATE/FLIP/FRAME STATE REQUIRES QUALIFICATION.");
        goto finish;
    }
    PolarisPlanePreview preview;
    if (!polaris_plane_preview_init(&preview,connection.incarnation,g_capture_reader.resource.id,
            vram.surface_gpu_address,batch.pipe,&witness)) goto finish;
    for (u32 sample=0;sample<2U;++sample) {
        request.words[0]=REGISTER_PROBE_PLANE_WITNESS;
        request.words[1]=g_capture_reader.resource.id;
        request.words[2]=(u64)witness.values[sample][POLARIS_WITNESS_UPDATE] |
            ((u64)witness.values[sample][POLARIS_WITNESS_FLIP_CONTROL]<<32);
        request.words[3]=(u64)witness.values[sample][POLARIS_WITNESS_FRAME_COUNT] | ((u64)sample<<32);
        if (!managed_service_call(&g_capture_service,&connection,&request,&reply) ||
            reply.word_count!=4U || reply.words[0]!=REGISTER_PROBE_REPLY ||
            reply.words[1]!=connection.incarnation || reply.words[2]!=sample+1ULL || reply.words[3]) goto finish;
    }
    for (u32 index=0;index<POLARIS_PLANE_RECIPE_STEPS;++index) {
        request.words[0]=REGISTER_PROBE_PLANE_STEP; request.words[1]=g_capture_reader.resource.id;
        request.words[2]=index; request.words[3]=0;
        if (!managed_service_call(&g_capture_service,&connection,&request,&reply) ||
            reply.word_count!=4U || reply.words[0]!=REGISTER_PROBE_REPLY ||
            !polaris_plane_preview_accept(&preview,reply.words[1],g_capture_reader.resource.id,
                reply.words[2],reply.words[3])) goto finish;
        terminal_write("RING3 REBIND STEP "); terminal_write_u64(index);
        pair(" TAG=",reply.words[2]); pair(" VALUE=",reply.words[3]); terminal_putchar('\n');
    }
    terminal_writeln("PLANE REBIND RECIPE: FIVE STEPS VALIDATED / PREVIEW ONLY; NO EXECUTOR OR WRITE GRANT.");
    success = true;
finish:
    {
        bool clean = cleanup_capture();
        terminal_writeln(clean ? "CPU MAPPINGS/READER/SERVICE DRAINED; PARENT QUARANTINE RETAINED." :
            "CLEANUP INCOMPLETE: RETAINED LEDGERS/PINS; NO RETRY OR REASSIGNMENT.");
        terminal_writeln(success && clean ? "POLARIS PHYSICAL OBSERVATION: COMPLETE" :
            "POLARIS PHYSICAL OBSERVATION: INCOMPLETE / SEE LAST GPU READ JOURNAL.");
        terminal_writeln("FIRMWARE SCANOUT UNCHANGED; NO GPU WRITES, MODESET, RESET OR DMA GRANT.");
    }
}
#else
void polaris_live_capture_run(const BootInfo *boot) {
    (void)boot;
    terminal_writeln("POLARIS PHYSICAL CAPTURE DISABLED IN THIS BUILD.");
    terminal_writeln("EXPLICIT BRING-UP BUILD REQUIRED; NO GPU ACCESS PERFORMED.");
}
#endif
