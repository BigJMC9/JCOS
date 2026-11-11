#include "polaris_link.h"
#include "terminal.h"
#ifndef JCOS_POLARIS_NATIVE_WRITE
#define JCOS_POLARIS_NATIVE_WRITE 0
#endif
#if JCOS_POLARIS_NATIVE_WRITE
#include "polaris_mmio.h"
#include "polaris_read_profile.h"
#include "display.h"
#include "framebuffer.h"
#include "serial.h"
#include "scheduler.h"
#include "timer.h"
#include "device_mmio.h"
#include "lib.h"
static PciAssignment assignment;
static PolarisMmioBinding binding;
static PolarisMmioHold hold;
static CapabilityHandle cap;
static u64 hold_id;
static bool attempted;
static bool release(void){
 bool ok=true;
 if(polaris_mmio_binding_valid(&binding))ok=polaris_mmio_close(&binding) && ok;
 if(hold_id){if(polaris_mmio_hold_release(&hold,hold_id))hold_id=0;else ok=false;}
 if(pci_assignment_valid(&assignment) && pci_assignment_owner_process_id(&assignment) && pci_assignment_state(&assignment)!=PCI_ASSIGNMENT_QUARANTINED)
  ok=pci_assignment_force_quarantine(&assignment,process_kernel()->id) && ok;
 if(polaris_mmio_binding_valid(&binding))ok=polaris_mmio_cleanup(&binding) && ok;
 if(cap){if(capability_revoke(process_capabilities(process_kernel()),cap))cap=0;else ok=false;}
 if(pci_assignment_valid(&assignment) && !pci_assignment_owner_process_id(&assignment))ok=pci_assignment_destroy(&assignment) && ok;
 return ok;
}
static bool admitted(u64 start,u32 frequency){
 return frequency && timer_frequency()==frequency && timer_ticks()-start<(u64)frequency*2ULL &&
  serial_available() && framebuffer_firmware_valid() && device_mmio_interrupts_enabled();
}
static void capture(const BootInfo *boot,bool timing,bool vertical){
 terminal_writeln(vertical ? "POLARIS VERTICAL PREFLIGHT V1 / READ-ONLY PLAN; NO GPU WRITES":timing ? "POLARIS TIMING CAPTURE V1 / READ-ONLY INHERITED TIMING OBSERVATION":"POLARIS LINK CAPTURE V1 / READ-ONLY INHERITED DIG OBSERVATION");
 if(attempted){terminal_writeln(vertical ? "REFUSED: ONE LINK/TIMING/VERTICAL ATTEMPT PER BOOT; NO RETRY OR ASSIGNMENT REUSE.":timing ? "REFUSED: ONE LINK/TIMING ATTEMPT PER BOOT; NO RETRY OR ASSIGNMENT REUSE.":"REFUSED: ONE LINK ATTEMPT PER BOOT; NO RETRY OR ASSIGNMENT REUSE.");return;}
 PolarisReadReport profile;const u32 local=POLARIS_READ_BLOCK_TARGET|POLARIS_READ_BLOCK_BAR_PLAN|POLARIS_READ_BLOCK_POWER|POLARIS_READ_BLOCK_CPU|POLARIS_READ_BLOCK_UART|POLARIS_READ_BLOCK_SCANOUT;
 if(!boot || !polaris_read_profile_inspect(boot,&profile) || (profile.blockers&local) || !timer_initialized() || !timer_frequency() ||
  !scheduler_preemption_enabled() || !device_mmio_interrupts_enabled() || !serial_available() || !framebuffer_writers_idle() || display_direct_state()!=DISPLAY_LEASE_KERNEL ||
  boot->framebuffer_width!=1024 || boot->framebuffer_height!=768 || boot->framebuffer_pixels_per_scanline!=1024 || boot->framebuffer_pixel_format!=1){
  terminal_writeln("REFUSED: EXACT BOARD/GOP/BAR5/D0/CPU/SERIAL/KERNEL-LEASE PROFILE REQUIRED.");return;
 }
 const PciDevice *device=0;
 for(u32 i=0;i<pci_device_count();++i){const PciDevice *d=pci_device(i);if(display_probe_matches_gop(boot,d)){if(device){terminal_writeln("REFUSED: AMBIGUOUS SELECTED DEVICE.");return;}device=d;}}
 if(!device)return;attempted=true;
 terminal_writeln("ASSUMPTIONS: OPERATIONAL DCE DOMAIN, UNCHANGED FIRMWARE SCANOUT; NO HOTPLUG/POWER/RESET.");
 terminal_writeln(vertical ? "PLAN: EXISTING 80 TIMING VALUES; TWO MIN/MAX/CONTROL SAMPLES IN EXISTING RO PAGE.":timing ? "PLAN: CRTC/DIG BRACKETS; TWO SAMPLES OF 14 FIXED CRTC0/FE2 TIMING FIELDS.":"PLAN: SIX CRTC BEFORE; TWO SAMPLES OF SEVEN BE PAIRS AND SIX FE CONTROLS; SIX CRTC AFTER.");
 terminal_writeln(vertical ? "MAXIMUM 86 ORDERED DWORD LOAD ATTEMPTS; TWO-SECOND ADMISSION; NO BUS-TRANSACTION TIMEOUT.":timing ? "MAXIMUM 80 ORDERED DWORD LOAD ATTEMPTS; TWO-SECOND ADMISSION; NO BUS-TRANSACTION TIMEOUT.":"MAXIMUM 52 ORDERED DWORD LOAD ATTEMPTS; TWO-SECOND ADMISSION; NO BUS-TRANSACTION TIMEOUT.");
 u32 values[POLARIS_VERTICAL_VALUES];const u32 count=vertical ? POLARIS_VERTICAL_VALUES:timing ? POLARIS_TIMING_VALUES:POLARIS_LINK_VALUES;k_memset(values,0,sizeof(values));u32 reads=0,frequency=timer_frequency();bool captured=false;
 if(!pci_assignment_create(&assignment,device) || !pci_assignment_claim_supervised(&assignment,&cap) ||
  !polaris_mmio_prepare_supervised(&binding,boot,&assignment,cap) ||
  !polaris_mmio_prepare_links(&binding,boot,process_kernel(),polaris_mmio_binding_identity(&binding)) ||
  !polaris_mmio_hold_acquire(&hold,&binding,polaris_mmio_binding_identity(&binding),process_kernel(),&hold_id))goto finish;
 u64 start=timer_ticks();
 for(u32 index=0;index<count;++index){
  if(!admitted(start,frequency))goto finish;
  if(index==6){
   u32 running=0;
   for(u32 i=0;i<6;++i){u32 enable=values[i]&0x10001U;if(enable && (enable!=0x10001U || (values[i]&0x1000000U)))goto finish;if(enable)++running;}
   if(running!=1){terminal_writeln("LINK CAPTURE REFUSED: UNIQUE STABLE RUNNING CRTC REQUIRED BEFORE DIG READS.");goto finish;}
  }
  if(timing && index==26 && !jcos_polaris_timing_read_admitted(values,26)){terminal_writeln("TIMING CAPTURE REFUSED: EXACT CRTC0-FE2-BE2/DP/CLOCK WITNESS REQUIRED.");goto finish;}
  if(vertical && index==80 && !jcos_polaris_vertical_read_admitted(values,80)){terminal_writeln("VERTICAL PREFLIGHT REFUSED: STABLE CRTC/DIG/STREAM WITNESS REQUIRED.");goto finish;}
  terminal_write(vertical ? "GPU VERTICAL READ #":timing ? "GPU TIMING READ #":"GPU LINK READ #");terminal_write_u64(index+1);terminal_write(" INDEX=");terminal_write_u64(index);terminal_putchar('\n');
  ++reads;bool ok;
  u32 tail=timing ? 74:46;
  if(vertical && index>=80)ok=polaris_mmio_read_vertical_held(&hold,hold_id,process_kernel(),(index-80)%3,&values[index]);
  else if(index<6 || index>=tail){u32 pipe=index<6 ? index:index-tail;ok=polaris_mmio_read_held(&hold,hold_id,process_kernel(),pipe,POLARIS_REG_CRTC_CONTROL,&values[index]);}
  else if(timing && ((index>=26 && index<40) || (index>=60 && index<74))){u32 field=index<40 ? index-26:index-60;
   ok=polaris_mmio_read_timing_held(&hold,hold_id,process_kernel(),field,&values[index]);}
  else {u32 n=index<26 ? index-6:index-(timing ? 40:26),unit=n<14 ? n/2:n-14;PolarisLinkRegister field=n<14 ? (PolarisLinkRegister)(n%2):POLARIS_LINK_FRONTEND;
   ok=polaris_mmio_read_link_held(&hold,hold_id,process_kernel(),unit,field,&values[index]);}
  if(!ok || values[index]==~0U)goto finish;
  terminal_write("  VALUE=");terminal_write_hex(values[index]);terminal_putchar('\n');
  if(timing && (index==26 || index==60) && !(values[index]&POLARIS_TIMING_STREAM_MASK)){terminal_writeln("TIMING CAPTURE REFUSED: ENABLED DP STREAM REQUIRED BEFORE TIMING FIELDS.");goto finish;}
 }
 if(!admitted(start,frequency))goto finish;captured=true;
finish:
 terminal_write(vertical ? "VERTICAL CAPTURE LOAD ATTEMPTS=":timing ? "TIMING CAPTURE LOAD ATTEMPTS=":"LINK CAPTURE LOAD ATTEMPTS=");terminal_write_u64(reads);terminal_putchar('\n');
 bool cleaned=release();
 terminal_writeln(cleaned ? "LINK MMIO CLOSED/UNMAPPED; HOLD AND CAP REMOVED.":"LINK CLEANUP RETAINED; NO ACCESS REOPEN OR REPLAY.");
 if(pci_assignment_valid(&assignment) && pci_assignment_state(&assignment)==PCI_ASSIGNMENT_QUARANTINED)terminal_writeln("INHERITED ASSIGNMENT QUARANTINED; ORDINARY RELEASE/REUSE NOT QUALIFIED.");
 if(!captured || !cleaned){k_memset(values,0,sizeof(values));terminal_writeln("LINK CAPTURE REFUSED/INCOMPLETE; NO DECODED OBSERVATION.");return;}

 u64 report[POLARIS_VERTICAL_WORDS];u32 words=vertical ? POLARIS_VERTICAL_WORDS:timing ? POLARIS_TIMING_WORDS:POLARIS_LINK_WORDS;
 bool decoded=vertical ? polaris_vertical_decode_copy(values,report):timing ? polaris_timing_decode_copy(values,report):polaris_link_decode_copy(values,report);
 if(!decoded){terminal_writeln("LINK/TIMING DECODE SERVICE FAILED; CPU CLEANUP REQUIRED BEFORE REUSE.");return;}
 terminal_write(vertical ? "RING3 VERTICAL STATUS=":timing ? "RING3 TIMING STATUS=":"RING3 LINK STATUS=");terminal_write_u64(report[0]&65535U);terminal_putchar('\n');
 for(u32 i=0;i<words;++i){terminal_write(vertical ? " VERTICAL WORD ":timing ? " TIMING WORD ":" LINK WORD ");terminal_write_u64(i);terminal_write(" VALUE=");terminal_write_hex(report[i]);terminal_putchar('\n');}
 if(vertical){
  if(!(report[0]&65535U))terminal_writeln("PROPOSED VERTICAL TOTAL=1133; RESTORATION TOTAL=1125; DATA ONLY.");
  terminal_writeln("NO WRITE MAPPING/STORE/POLL/RETRY; RESTORATION EXECUTION NOT IMPLEMENTED OR QUALIFIED.");
  return;
 }
 if(!(report[0]&65535U)){
  terminal_write("OBSERVED CRTC=");terminal_write_u64((report[0]>>32)&255);terminal_write(" BACKEND=");terminal_write_u64((report[0]>>40)&255);
  terminal_write(" FRONTEND=");terminal_write_u64((report[0]>>48)&255);terminal_write(" RAW MODE=");terminal_write_u64(report[0]>>56);terminal_putchar('\n');
  if(timing){
   terminal_write("TIMING ACTIVE=");terminal_write_u64((report[1]>>16)&65535);terminal_putchar('x');terminal_write_u64((report[2]>>16)&65535);
   terminal_write(" TOTAL=");terminal_write_u64(report[1]&65535);terminal_putchar('x');terminal_write_u64(report[2]&65535);terminal_putchar('\n');
  }
 }
 terminal_writeln("COPIED READ-ONLY OBSERVATION ONLY; NO PORT/LINK SPEED/MODESETTING OR RESET QUALIFICATION.");

}
void polaris_link_capture_run(const BootInfo *boot){capture(boot,false,false);}
void polaris_timing_capture_run(const BootInfo *boot){capture(boot,true,false);}
void polaris_vertical_capture_run(const BootInfo *boot){capture(boot,true,true);}
#else
void polaris_vertical_capture_run(const BootInfo *boot){(void)boot;terminal_writeln("POLARIS VERTICAL PREFLIGHT: REFUSED; EXPLICIT HARDWARE BUILD PROFILE REQUIRED.");}
void polaris_timing_capture_run(const BootInfo *boot){(void)boot;terminal_writeln("POLARIS TIMING CAPTURE: REFUSED; EXPLICIT HARDWARE BUILD PROFILE REQUIRED.");}
void polaris_link_capture_run(const BootInfo *boot){(void)boot;terminal_writeln("POLARIS LINK CAPTURE: REFUSED; EXPLICIT HARDWARE BUILD PROFILE REQUIRED.");}
#endif