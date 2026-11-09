#include "polaris_rom_test.h"
#include "polaris_rom_fixture.h"
#include "polaris_rom.h"
#include "pmm.h"
#include "physmap.h"
#include "lib.h"
#include "process.h"
#include "address_space.h"
#include "endpoint.h"
#include "terminal.h"
static frame_t source=FRAME_INVALID;
bool polaris_rom_test(void){
 if(source!=FRAME_INVALID)return false;
 u64 pages=pmm_stats().free_pages,processes=process_object_count(),spaces=address_space_object_count(),endpoints=endpoint_object_count();
 u64 allocation=pmm_alloc_pages(POLARIS_ROM_LIMIT/4096U);if(!allocation)return false;source=phys_to_frame(allocation);
 u64 physical=frame_to_phys(source);
 BootInfo boot;k_memset(&boot,0,sizeof(boot));BootMemoryDescriptor descriptor;k_memset(&descriptor,0,sizeof(descriptor));
 descriptor.type=2;descriptor.physical_start=physical;descriptor.number_of_pages=64;
 boot.magic=BOOT_INFO_MAGIC;boot.version=BOOT_INFO_VERSION;boot.size=sizeof(boot);boot.framebuffer_pci_valid=1;
 boot.framebuffer_pci_vendor_device=0x67DF1002;boot.framebuffer_rom_status=BOOT_DISPLAY_ROM_COPIED;
 boot.framebuffer_rom_base=physical;boot.framebuffer_rom_size=POLARIS_ROM_LIMIT;boot.framebuffer_rom_reported_size=POLARIS_ROM_LIMIT;
 boot.memory_map=(u64)&descriptor;boot.memory_map_size=sizeof(descriptor);boot.memory_map_descriptor_size=sizeof(descriptor);
 if(!polaris_rom_handoff_valid(&boot))return false;
 descriptor.type=7;if(polaris_rom_handoff_valid(&boot))return false;descriptor.type=2;
 descriptor.number_of_pages=63;if(polaris_rom_handoff_valid(&boot))return false;descriptor.number_of_pages=64;
 boot.framebuffer_rom_base++;if(polaris_rom_handoff_valid(&boot))return false;boot.framebuffer_rom_base--;
 boot.version--;if(polaris_rom_handoff_valid(&boot))return false;boot.version++;
 boot.framebuffer_rom_reported_size++;if(polaris_rom_handoff_valid(&boot))return false;boot.framebuffer_rom_reported_size--;
 boot.framebuffer_rom_status=BOOT_DISPLAY_ROM_CHANGED;if(polaris_rom_handoff_valid(&boot))return false;
 terminal_writeln("  ROM HANDOFF / LOADER RAM EXTENT / VERSION AND SOURCE-CHANGE REFUSALS: PASS");
 u8 *bytes=phys_to_virt(physical);k_memset(bytes,0,POLARIS_ROM_LIMIT);rom_fixture(bytes);
 u64 report[POLARIS_ROM_QUERIES];
 /* Every borrowed page is independently checked RO/NX by the actual launcher. */
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report))return false;
 if(report[0]!=((2ULL<<32)|(3ULL<<24)|(1ULL<<16)) || report[2]!=0x008000F000B00080ULL || report[3]!=512 ||
    report[4]!=0x0001006000003113ULL || report[5]!=0x000200620000320CULL)return false;
 for(u32 i=6;i<POLARIS_ROM_QUERIES;++i)if(report[i])return false;
 terminal_writeln("  ACTUAL RING3 ROM PARSER / 64 RO-NX PAGES / DP AND HDMI OBJECTS: PASS");
 bytes[400]^=1;
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report) || report[0]!=POLARIS_ROM_CHECKSUM)return false;
 for(u32 i=2;i<POLARIS_ROM_QUERIES;++i)if(report[i])return false;
 if(!frame_free_range(source,POLARIS_ROM_LIMIT/4096U))return false;source=FRAME_INVALID;
 bool good=pages==pmm_stats().free_pages && processes==process_object_count() && spaces==address_space_object_count() && endpoints==endpoint_object_count();
 if(good)terminal_writeln("  ACTUAL ROM CHECKSUM REFUSAL / FULL CPU RESOURCE BASELINE: PASS");return good;
}
