#include "polaris_rom.h"
#include "managed_service.h"
#include "display_probe.h"
#include "lib.h"
#include "terminal.h"
static ManagedService service;
static ManagedServiceConnection connection;
_Static_assert(BOOT_DISPLAY_ROM_LIMIT==POLARIS_ROM_LIMIT,"ROM limits agree");
bool polaris_rom_handoff_valid(const BootInfo *b){
 if(!b || b->magic!=BOOT_INFO_MAGIC || b->version!=BOOT_INFO_VERSION || b->size<sizeof(*b) ||
    b->framebuffer_pci_valid!=1 || b->framebuffer_pci_segment || b->reserved1 || b->framebuffer_metadata_reserved ||
    b->framebuffer_pci_vendor_device!=0x67DF1002U || b->framebuffer_rom_status!=BOOT_DISPLAY_ROM_COPIED ||
    !b->framebuffer_rom_base || (b->framebuffer_rom_base&4095) || !b->framebuffer_rom_size ||
    b->framebuffer_rom_size>POLARIS_ROM_LIMIT || b->framebuffer_rom_reported_size!=b->framebuffer_rom_size ||
    !b->memory_map || !b->memory_map_size || b->memory_map_descriptor_size<sizeof(BootMemoryDescriptor) ||
    b->memory_map_size%b->memory_map_descriptor_size || b->memory_map>~0ULL-b->memory_map_size)return false;
 u64 extent=((u64)b->framebuffer_rom_size+4095)&~4095ULL;
 if(b->framebuffer_rom_base>~0ULL-extent)return false;
 bool covered=false;
 for(u64 off=0;off<b->memory_map_size;off+=b->memory_map_descriptor_size){
  const BootMemoryDescriptor *d=(const BootMemoryDescriptor *)(b->memory_map+off);
  if(!d->number_of_pages || (d->physical_start&4095) || d->number_of_pages>~0ULL/4096)continue;
  u64 bytes=d->number_of_pages*4096;
  if(d->physical_start>~0ULL-bytes)continue;
  if(b->framebuffer_rom_base<d->physical_start+bytes && d->physical_start<b->framebuffer_rom_base+extent && d->type!=2)return false;
  if(d->type==2 && b->framebuffer_rom_base>=d->physical_start && b->framebuffer_rom_base+extent<=d->physical_start+bytes)covered=true;
 }
 return covered;
}
static bool stop(void){
 if(managed_service_state(&service)==MANAGED_SERVICE_STOPPED)return !program_instance_needs_cleanup(&service.program);
 ManagedServiceStopResult result=managed_service_stop_bounded(&service);
 return (result==MANAGED_SERVICE_STOP_GRACEFUL || result==MANAGED_SERVICE_STOP_FORCED) && !program_instance_needs_cleanup(&service.program);
}
bool polaris_rom_inspect(u64 physical,u32 bytes,u32 identity,u64 values[POLARIS_ROM_QUERIES]){
 if(!values)return false;k_memset(values,0,POLARIS_ROM_QUERIES*sizeof(*values));
 if(!physical || (physical&4095) || !bytes || bytes>POLARIS_ROM_LIMIT || physical>~0ULL-((bytes+4095ULL)&~4095ULL) ||
    identity!=0x67DF1002U || managed_service_state(&service)!=MANAGED_SERVICE_STOPPED || program_instance_needs_cleanup(&service.program))return false;
 ManagedServiceSpec spec;spec.path="/bin/polarisrom.elf";
 spec.shutdown_message=POLARIS_ROM_HEADER(POLARIS_ROM_STOP);spec.shutdown_reply=POLARIS_ROM_REPLY;
 ManagedServiceLaunchExtras extras;k_memset(&extras,0,sizeof(extras));
 extras.borrowed_mapping=(ProgramBorrowedMappingSpec){physical,bytes,POLARIS_ROM_VA,0};
 extras.startup_argument_count=3;extras.startup_arguments[0]=POLARIS_ROM_VA;extras.startup_arguments[1]=bytes;extras.startup_arguments[2]=identity;
 extras.shutdown_request_word_count=4;extras.shutdown_request_words[0]=spec.shutdown_message;
 if(!managed_service_start_ex(&service,&spec,&extras))return false;
 bool good=managed_service_connect_kernel(&service,&connection);
 for(u32 i=0;good && i<(bytes+4095U)/4096U;++i){
  frame_t frame=FRAME_INVALID;vm_flags_t flags=0;
  good=address_space_query_page(process_address_space(&service.program.process),POLARIS_ROM_VA+(u64)i*4096,&frame,&flags) && frame==phys_to_frame(physical)+i && flags==VM_USER;
 }
 for(u32 i=0;good && i<POLARIS_ROM_QUERIES;++i){
  IpcMessage q,r;k_memset(&q,0,sizeof(q));k_memset(&r,0,sizeof(r));q.word_count=4;q.words[0]=POLARIS_ROM_HEADER(POLARIS_ROM_QUERY);q.words[1]=i;
  good=managed_service_call(&service,&connection,&q,&r) && r.word_count==4 && r.words[0]==POLARIS_ROM_REPLY && r.words[1]==connection.incarnation && r.words[2]==i;
  if(good)values[i]=r.words[3];
 }
 /* No source RAM or service storage is reused if any teardown ledger remains. */
 bool stopped=stop();
 if(!good || !stopped){k_memset(values,0,POLARIS_ROM_QUERIES*sizeof(*values));return false;}
 return true;
}
void polaris_rom_print(const BootInfo *b){
 terminal_writeln("POLARIS ROM: SELECTED GOP FIRMWARE RAM COPY; NO GPU ACCESS.");
 terminal_write("CAPTURE STATUS=");terminal_write_u64(b ? b->framebuffer_rom_status:0);
 terminal_write(" REPORTED BYTES=");terminal_write_u64(b ? b->framebuffer_rom_reported_size:0);
 terminal_write(" COPIED BYTES=");terminal_write_u64(b ? b->framebuffer_rom_size:0);terminal_putchar('\n');
 bool selected=false;
 for(u32 i=0;b && i<pci_device_count();++i){
  const PciDevice *d=pci_device(i);DisplayProbeInfo p;
  if(display_probe_matches_gop(b,d) && display_probe_read(d,&p) &&
     b->framebuffer_pci_subsystem==((u32)p.subsystem_vendor_id|((u32)p.subsystem_device_id<<16)))selected=true;
 }
 if(!selected || !polaris_rom_handoff_valid(b)){
  terminal_writeln("ROM INVENTORY UNAVAILABLE: EXACT SELECTED COPY / RESERVED RAM REQUIRED.");return;
 }
 u64 values[POLARIS_ROM_QUERIES];
 if(!polaris_rom_inspect(b->framebuffer_rom_base,b->framebuffer_rom_size,b->framebuffer_pci_vendor_device,values)){
  terminal_writeln("ROM SERVICE FAILED; INCOMPLETE CPU RESOURCES RETAINED; NO RETRY IN THIS REQUEST.");return;
 }
 u32 status=(u32)(values[0]&0xFFFFU),format=(u32)((values[0]>>16)&255),revision=(u32)((values[0]>>24)&255),count=(u32)(values[0]>>32);
 if(status>POLARIS_ROM_CONNECTOR || count>POLARIS_ROM_CONNECTORS || (status && (count || format || revision)) ||
    (!status && (format!=1 || revision<1 || revision>3 || !count || values[3]>b->framebuffer_rom_size))){
  terminal_writeln("ROM SERVICE REPORT REFUSED: MALFORMED INVENTORY.");return;
 }
 terminal_write("RING3 PARSER STATUS=");terminal_write_u64(status);terminal_write(" CRC32=");terminal_write_hex(values[1]);
 terminal_write(" IMAGE BYTES=");terminal_write_u64(values[3]);terminal_putchar('\n');
 terminal_write("ATOM HEADER/DATA/OBJECT/SIZE=");terminal_write_hex(values[2]);
 terminal_write(" OBJECT REVISION=");terminal_write_u64(format);terminal_putchar('.');terminal_write_u64(revision);
 terminal_write(" CONNECTORS=");terminal_write_u64(count);terminal_putchar('\n');
 for(u32 i=0;i<count;++i){
  terminal_write("CONNECTOR ");terminal_write_u64(i);terminal_write(" OBJECT/LINKS/RECORDS/KIND=");terminal_write_hex(values[4+i]);terminal_putchar('\n');
 }
 terminal_writeln("KIND 1=DISPLAYPORT 2=HDMI-A 0=OTHER; FIRMWARE INVENTORY, NOT ACTIVE LINK PROOF.");
 terminal_writeln("READONLY/NX RING3 PARSER STOPPED; NO ROM BAR, TABLE EXECUTION OR SCANOUT CHANGE.");
}
