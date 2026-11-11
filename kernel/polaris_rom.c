#include "polaris_rom.h"
#include "managed_service.h"
#include "display_probe.h"
#include "lib.h"
#include "terminal.h"
#include "timer.h"
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
static bool begin(u64 physical,u32 bytes,u32 identity){
 if(!timer_initialized() || !timer_frequency() || !physical || (physical&4095) || !bytes || bytes>POLARIS_ROM_LIMIT || physical>~0ULL-((bytes+4095ULL)&~4095ULL) ||
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
 if(!good)(void)stop();
 return good;
}
static bool query(u32 op,u64 index,u32 count,u64 *value){
 IpcMessage q,r;k_memset(&q,0,sizeof(q));k_memset(&r,0,sizeof(r));
 q.word_count=4;q.words[0]=POLARIS_ROM_HEADER(op);q.words[1]=index;q.words[2]=count;
 bool good=managed_service_call(&service,&connection,&q,&r) && r.word_count==4 && r.words[0]==POLARIS_ROM_REPLY &&
  r.words[1]==connection.incarnation && r.words[2]==index;
 if(good)*value=r.words[3];return good;
}
bool polaris_rom_inspect(u64 physical,u32 bytes,u32 identity,u64 values[POLARIS_ROM_QUERIES]){
 if(!values)return false;k_memset(values,0,POLARIS_ROM_QUERIES*sizeof(*values));
 if(!begin(physical,bytes,identity))return false;
 bool good=true;
 u64 started=timer_ticks(),admission=(u64)timer_frequency()*4ULL;
 u32 query_limit=POLARIS_ROM_DIRECTORY_QUERIES;
 for(u32 i=0;good && i<query_limit;++i){
  if(timer_ticks()-started>=admission){good=false;break;}
  u64 value=0;good=query(POLARIS_ROM_QUERY,i,0,&value);
  if(timer_ticks()-started>=admission)good=false;
  if(good){
   values[i]=value;
   if(!i){
    u32 status=(u32)(values[0]&0xFFFFU),count=(u32)(values[0]>>32);
    good=status<=POLARIS_ROM_CONNECTOR && count<=POLARIS_ROM_CONNECTORS && (!status || !count);
    if(good)query_limit=POLARIS_ROM_DIRECTORY_QUERIES+count*POLARIS_ROM_ROUTE_WORDS;
   }
  }
 }
 /* Topology occupies a separate bounded tail;V2 connector/route slots stay fixed. */
 u32 topology_count=good && !(values[0]&0xFFFFU) ? (u32)(values[0]>>32):0;
 for(u32 i=0;good && i<topology_count*POLARIS_ROM_TOPOLOGY_WORDS;++i){
  u32 index=POLARIS_ROM_TOPOLOGY_BASE+i;
  if(timer_ticks()-started>=admission){good=false;break;}
  good=query(POLARIS_ROM_QUERY,index,0,&values[index]);
  if(timer_ticks()-started>=admission)good=false;
 }
 /* No source RAM or service storage is reused if any teardown ledger remains. */
 bool stopped=stop();
 if(!good || !stopped){k_memset(values,0,POLARIS_ROM_QUERIES*sizeof(*values));return false;}
 return true;
}
/* Copy from already retained boot RAM only;this is not a device ROM reader. */
bool polaris_rom_copy_bytes(const BootInfo *b,u64 offset,u32 count,u8 output[POLARIS_ROM_BYTE_WINDOW]){
 if(!output)return false;k_memset(output,0,POLARIS_ROM_BYTE_WINDOW);
 if(!polaris_rom_handoff_valid(b) || !count || count>POLARIS_ROM_BYTE_WINDOW ||
    offset>=b->framebuffer_rom_size || count>b->framebuffer_rom_size-offset)return false;
 /* Only the isolated RO/NX mapping dereferences firmware ROM bytes. */
 if(!begin(b->framebuffer_rom_base,b->framebuffer_rom_size,b->framebuffer_pci_vendor_device))return false;
 bool good=true;u64 started=timer_ticks(),admission=(u64)timer_frequency()*4ULL;
 for(u32 copied=0;good && copied<count;){
  u32 take=count-copied;if(take>8)take=8;u64 value=0;
  if(timer_ticks()-started>=admission){good=false;break;}
  good=query(POLARIS_ROM_READ_BYTES,offset+copied,take,&value);
  if(timer_ticks()-started>=admission || (take<8 && (value>>(take*8))))good=false;
  if(good){for(u32 i=0;i<take;++i)output[copied+i]=(u8)(value>>(i*8));copied+=take;}
 }
 bool stopped=stop();
 if(!good || !stopped){k_memset(output,0,POLARIS_ROM_BYTE_WINDOW);return false;}
 return true;
}
static bool selected_copy(const BootInfo *b){

 for(u32 i=0;b && i<pci_device_count();++i){
  const PciDevice *d=pci_device(i);DisplayProbeInfo p;
  if(display_probe_matches_gop(b,d) && display_probe_read(d,&p) &&
     b->framebuffer_pci_subsystem==((u32)p.subsystem_vendor_id|((u32)p.subsystem_device_id<<16)))return true;
 }
 return false;
}
void polaris_rom_print_bytes(const BootInfo *b,u64 offset,u32 count){
 u8 bytes[POLARIS_ROM_BYTE_WINDOW];
 terminal_writeln("POLARIS ROM BYTES: SELECTED FIRMWARE RAM COPY; NO GPU ACCESS.");
 if(!selected_copy(b) || !polaris_rom_copy_bytes(b,offset,count,bytes)){
  terminal_writeln("ROM BYTE WINDOW REFUSED; EXACT SELECTED COPY AND BOUNDED RANGE REQUIRED.");return;
 }
 terminal_write("ROM COPY BYTES=");terminal_write_u64(b->framebuffer_rom_size);
 terminal_write(" OFFSET=");terminal_write_hex(offset);terminal_write(" COUNT=");terminal_write_u64(count);terminal_putchar('\n');
 for(u32 row=0;row<count;row+=16){
  terminal_write("ROM BYTE ");terminal_write_hex(offset+row);terminal_write(":");
  for(u32 i=row;i<count && i<row+16;++i){
   terminal_putchar(' ');terminal_putchar("0123456789ABCDEF"[bytes[i]>>4]);terminal_putchar("0123456789ABCDEF"[bytes[i]&15]);
  }
  terminal_putchar('\n');
 }
 terminal_writeln("ROM BYTE WINDOW COMPLETE; RESERVED SOURCE RETAINED; NO DEVICE ACCESS.");
}
void polaris_rom_print(const BootInfo *b){
 terminal_writeln("POLARIS ROM: SELECTED GOP FIRMWARE RAM COPY; NO GPU ACCESS.");
 terminal_write("CAPTURE STATUS=");terminal_write_u64(b ? b->framebuffer_rom_status:0);
 terminal_write(" REPORTED BYTES=");terminal_write_u64(b ? b->framebuffer_rom_reported_size:0);
 terminal_write(" COPIED BYTES=");terminal_write_u64(b ? b->framebuffer_rom_size:0);terminal_putchar('\n');
 if(!selected_copy(b) || !polaris_rom_handoff_valid(b)){
  terminal_writeln("ROM INVENTORY UNAVAILABLE: EXACT SELECTED COPY / RESERVED RAM REQUIRED.");return;
 }
 u64 values[POLARIS_ROM_QUERIES];
 if(!polaris_rom_inspect(b->framebuffer_rom_base,b->framebuffer_rom_size,b->framebuffer_pci_vendor_device,values)){
  terminal_writeln("ROM SERVICE FAILED; CPU CLEANUP MUST COMPLETE BEFORE REUSE; NO RETRY IN THIS REQUEST.");return;
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
 terminal_writeln("ROM ROUTING INVENTORY V2; NO GPIO/AUX/I2C/HPD ACCESS.");
 for(u32 i=0;i<count;++i){
  u32 base=POLARIS_ROM_DIRECTORY_QUERIES+i*POLARIS_ROM_ROUTE_WORDS;
  u64 meta=values[base];u32 status=(u32)(meta&0xFFFFU),sources=(u32)((meta>>16)&255),destinations=(u32)((meta>>24)&255);
  u32 records=(u32)((meta>>32)&255),other=(u32)((meta>>40)&255),flags=(u32)(meta>>48);
  bool valid=status<=POLARIS_ROM_ROUTE_UNSUPPORTED && sources<=POLARIS_ROM_LINK_IDS && destinations<=POLARIS_ROM_LINK_IDS &&
   records<=POLARIS_ROM_RECORD_LIMIT && other<=records && !(flags&~POLARIS_ROM_ROUTE_FLAGS);
  if(status && (meta>>16))valid=false;
  if(status)for(u32 f=1;f<POLARIS_ROM_ROUTE_WORDS;++f)if(values[base+f])valid=false;
  if(!valid){terminal_writeln("ROM ROUTE REPORT REFUSED: MALFORMED INVENTORY.");return;}
  terminal_write("ROUTE ");terminal_write_u64(i);terminal_write(" STATUS=");terminal_write_u64(status);
  terminal_write(" FLAGS=");terminal_write_hex(flags);terminal_write(" SOURCES=");terminal_write_u64(sources);
  terminal_write(" DESTINATIONS=");terminal_write_u64(destinations);terminal_write(" RECORDS=");terminal_write_u64(records);
  terminal_write(" OTHER=");terminal_write_u64(other);terminal_putchar('\n');
  for(u32 f=1;f<POLARIS_ROM_ROUTE_WORDS;++f){
   terminal_write(" ROUTE FIELD ");terminal_write_u64(f);terminal_write(" VALUE=");terminal_write_hex(values[base+f]);terminal_putchar('\n');
  }
 }
 terminal_writeln("ROM TOPOLOGY INVENTORY V1; FIRMWARE DIRECT PATHS ONLY.");
 for(u32 i=0;i<count;++i){
  u32 base=POLARIS_ROM_TOPOLOGY_BASE+i*POLARIS_ROM_TOPOLOGY_WORDS;u64 meta=values[base];
  u32 status=(u32)(meta&0xFFFFU),flags=(u32)((meta>>16)&255),objects=(u32)((meta>>24)&255);
  u32 records=(u32)((meta>>32)&255),other=(u32)((meta>>40)&255);
  bool valid=status<=POLARIS_ROM_TOPOLOGY_BAD_RECORDS && !(meta>>48);
  if(status){if(meta>>16)valid=false;for(u32 f=1;f<POLARIS_ROM_TOPOLOGY_WORDS;++f)if(values[base+f])valid=false;}
  else if(flags!=POLARIS_ROM_TOPOLOGY_FLAGS || objects!=1 || !records || records>POLARIS_ROM_RECORD_LIMIT || other>=records)valid=false;
  if(!valid){terminal_writeln("ROM TOPOLOGY REPORT REFUSED: MALFORMED INVENTORY.");return;}
  terminal_write("TOPOLOGY ");terminal_write_u64(i);terminal_write(" STATUS=");terminal_write_u64(status);
  terminal_write(" FLAGS=");terminal_write_hex(flags);terminal_write(" OBJECTS=");terminal_write_u64(objects);
  terminal_write(" RECORDS=");terminal_write_u64(records);terminal_write(" OTHER=");terminal_write_u64(other);terminal_putchar('\n');
  for(u32 f=1;f<POLARIS_ROM_TOPOLOGY_WORDS;++f){terminal_write(" TOPOLOGY FIELD ");terminal_write_u64(f);terminal_write(" VALUE=");terminal_write_hex(values[base+f]);terminal_putchar('\n');}
 }
 terminal_writeln("ROUTING DATA ONLY; ACTIVE PORT/ENCODER/AUX CHANNEL AND REGISTER ACCESS NOT QUALIFIED.");
 terminal_writeln("KIND 1=DISPLAYPORT 2=HDMI-A 0=OTHER; FIRMWARE INVENTORY, NOT ACTIVE LINK PROOF.");
 terminal_writeln("READONLY/NX RING3 PARSER STOPPED; NO ROM BAR, TABLE EXECUTION OR SCANOUT CHANGE.");
}
