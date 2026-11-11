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
 u8 window[POLARIS_ROM_BYTE_WINDOW];boot.framebuffer_rom_status=BOOT_DISPLAY_ROM_COPIED;
 if(!polaris_rom_copy_bytes(&boot,0,256,window))return false;
 for(u32 i=0;i<256;++i)if(window[i]!=bytes[i])return false;
 if(!polaris_rom_copy_bytes(&boot,0x101,17,window))return false;
 for(u32 i=0;i<256;++i)if(window[i]!=(i<17 ? bytes[0x101+i]:0))return false;
 bytes[POLARIS_ROM_LIMIT-1]=0xA7;
 if(!polaris_rom_copy_bytes(&boot,POLARIS_ROM_LIMIT-1,1,window) || window[0]!=0xA7)return false;
 for(u32 i=1;i<256;++i)if(window[i])return false;bytes[POLARIS_ROM_LIMIT-1]=0;
 const u64 offsets[5]={0,0,POLARIS_ROM_LIMIT,POLARIS_ROM_LIMIT-1,~0ULL};
 const u32 counts[5]={0,257,1,2,1};
 for(u32 n=0;n<5;++n){
  k_memset(window,0xA5,sizeof(window));
  if(polaris_rom_copy_bytes(&boot,offsets[n],counts[n],window))return false;
  for(u32 i=0;i<sizeof(window);++i)if(window[i])return false;
 }
 boot.framebuffer_rom_status=BOOT_DISPLAY_ROM_CHANGED;k_memset(window,0xA5,sizeof(window));
 if(polaris_rom_copy_bytes(&boot,0,1,window) || polaris_rom_copy_bytes(&boot,0,1,0))return false;
 for(u32 i=0;i<sizeof(window);++i)if(window[i])return false;boot.framebuffer_rom_status=BOOT_DISPLAY_ROM_COPIED;
 terminal_writeln("  RESERVED ROM RAM BYTE WINDOWS / LAST BYTE / RANGE REFUSALS AND CLEARED OUTPUT: PASS");
 u64 report[POLARIS_ROM_QUERIES];
 /* Every borrowed page is independently checked RO/NX by the actual launcher. */
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report))return false;
 if(report[0]!=((2ULL<<32)|(3ULL<<24)|(1ULL<<16)) || report[2]!=0x008000F000B00080ULL || report[3]!=512 ||
    report[4]!=0x0001006000003113ULL || report[5]!=0x000200620000320CULL)return false;
 for(u32 i=6;i<POLARIS_ROM_DIRECTORY_QUERIES;++i)if(report[i])return false;
 terminal_writeln("  ACTUAL RING3 ROM PARSER / 64 RO-NX PAGES / DP AND HDMI OBJECTS: PASS");
 rom_routing_fixture(bytes);
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report))return false;
 const u64 expected[12]={0x003F010301010000ULL,0x211E,0,0x1100,0,0x01050090,
  0x0103010201010100ULL,0x0107010601050104ULL,0x0706050403020100ULL,0x05030200,0x16,0x000C01C0001F0180ULL};
 for(u32 i=0;i<12;++i)if(report[POLARIS_ROM_DIRECTORY_QUERIES+i]!=expected[i])return false;
 terminal_writeln("  ACTUAL RING3 ROM ROUTES / BOUNDED LINKS / I2C AND HPD GPIO LOOKUPS: PASS");
 rom_single_byte_terminator_fixture(bytes);
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report))return false;
 for(u32 i=0;i<12;++i)if(report[POLARIS_ROM_DIRECTORY_QUERIES+i]!=expected[i])return false;
 if(report[POLARIS_ROM_DIRECTORY_QUERIES+12]!=expected[0])return false;
 terminal_writeln("  ACTUAL SINGLE-BYTE ROM TERMINATOR / NEXT SECTION PRESERVED: PASS");
 rom_topology_fixture(bytes);
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report))return false;
 const u64 topology[2][4]={{0x00000001010F0000ULL,0x0000211E11000008ULL,0x000031131100000FULL,0x007E007800680016ULL},
  {0x00000001010F0000ULL,0x0000221E11000080ULL,0x0000320C1100000FULL,0x0089008300700020ULL}};
 for(u32 i=0;i<2;++i)for(u32 f=0;f<4;++f)if(report[POLARIS_ROM_TOPOLOGY_BASE+i*4+f]!=topology[i][f])return false;
 terminal_writeln("  ACTUAL RING3 DIRECT PATH / ENCODER / LINK AND CAPABILITY CORRELATION: PASS");
 bytes[0x16F]=3;rom_checksum(bytes);
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report) || (report[POLARIS_ROM_DIRECTORY_QUERIES]&65535) || report[POLARIS_ROM_TOPOLOGY_BASE]!=POLARIS_ROM_TOPOLOGY_BAD_RECORDS)return false;
 for(u32 f=1;f<4;++f)if(report[POLARIS_ROM_TOPOLOGY_BASE+f])return false;
 for(u32 f=0;f<4;++f)if(report[POLARIS_ROM_TOPOLOGY_BASE+4+f]!=topology[1][f])return false;
 terminal_writeln("  ACTUAL TOPOLOGY REFUSAL / CLEARED TAIL / CONNECTOR AND PEER PRESERVED: PASS");
 rom_single_byte_terminator_fixture(bytes);
 /* Duplicate critical record refuses only that route and exports no partial fields. */
 bytes[0xF0+0x50+8]=1;rom_checksum(bytes);
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report) || report[0]!=((2ULL<<32)|(3ULL<<24)|(1ULL<<16)) ||
    report[POLARIS_ROM_DIRECTORY_QUERIES]!=POLARIS_ROM_ROUTE_DUPLICATE)return false;
 for(u32 i=1;i<12;++i)if(report[POLARIS_ROM_DIRECTORY_QUERIES+i])return false;
 if(report[POLARIS_ROM_DIRECTORY_QUERIES+12]!=expected[0])return false;
 terminal_writeln("  ACTUAL MALFORMED ROUTE / ZERO PARTIAL FIELDS / DIRECTORY AND PEER PRESERVED: PASS");
 bytes[400]^=1;
 if(!polaris_rom_inspect(physical,POLARIS_ROM_LIMIT,0x67DF1002,report) || report[0]!=POLARIS_ROM_CHECKSUM)return false;
 for(u32 i=2;i<POLARIS_ROM_QUERIES;++i)if(report[i])return false;
 if(!frame_free_range(source,POLARIS_ROM_LIMIT/4096U))return false;source=FRAME_INVALID;
 bool good=pages==pmm_stats().free_pages && processes==process_object_count() && spaces==address_space_object_count() && endpoints==endpoint_object_count();
 if(good){
  terminal_writeln("  ACTUAL ROM CHECKSUM REFUSAL / FULL CPU RESOURCE BASELINE: PASS");
  terminal_writeln("  ACTUAL RING3 ROM BYTE READER / CHECKED RO-NX MAPPING / FULL CPU BASELINE: PASS");
 }
 return good;
}
