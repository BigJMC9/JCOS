#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../user/drivers/amd_polaris_rom.h"
#include "../../kernel/tests/polaris_rom_fixture.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"ROM test line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned char b[512];static PolarisRomInfo o;
static int empty(void){for(unsigned i=0;i<POLARIS_ROM_CONNECTORS;++i)if(o.connectors[i])return 0;return !o.rom_header && !o.master_data && !o.object_header && !o.image_bytes && !o.count;}
int main(void){
 rom_fixture(b);polaris_rom_parse(b,sizeof(b),0x67DF1002,&o);
 CHECK(!o.status && o.count==2 && o.revision==3 && o.image_bytes==512 && o.connectors[0]==0x0001006000003113ULL && o.connectors[1]==0x000200620000320CULL);
 CHECK(polaris_rom_query(&o,0)==((2ULL<<32)|(3ULL<<24)|(1ULL<<16)) && polaris_rom_query(&o,2)==0x008000F000B00080ULL);
 unsigned valid_crc=o.crc32;CHECK(valid_crc);
 for(unsigned rev=1;rev<=3;++rev){rom_fixture(b);b[0xF3]=(unsigned char)rev;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && o.revision==rev);}
 const unsigned positions[]={0,1,0x18,0x60,0x64,0x66,0x6A,0x70,0x74,0x48,0x80,0x82,0x84,0xA0,0xB0,0xB2,0xE0,0xF0,0xF2,0xF3,0xF6,0x102,0x106,0x107,0x10A,0x10C};
 for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);++i){rom_fixture(b);unsigned at=positions[i];b[at]=(at==0x6A || at==0x80 || at==0xB0 || at==0xF0 || at==0x106) ? 0 : 0xFF;rom_checksum(b);memset(&o,0xA5,sizeof(o));polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(o.status && empty());}
 rom_fixture(b);b[400]^=1;polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(o.status==POLARIS_ROM_CHECKSUM && empty());
 rom_fixture(b);rom_put16(b,0x10E,0x3113);rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(o.status==POLARIS_ROM_CONNECTOR && empty());
 rom_fixture(b);polaris_rom_parse(b,512,0x67FF1002,&o);CHECK(o.status==POLARIS_ROM_IDENTITY && empty());
 polaris_rom_parse((void *)1,POLARIS_ROM_LIMIT+1,0x67DF1002,&o);CHECK(o.status==POLARIS_ROM_SIZE && empty());
 polaris_rom_parse(0,512,0x67DF1002,&o);CHECK(o.status==POLARIS_ROM_SIZE && empty());
 /* Each available prefix ends exactly at a protected page. Any unchecked
  * offset/length read faults the host process, rather than finding spare RAM. */
 unsigned char *guard;
#ifdef _WIN32
 guard=VirtualAlloc(0,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(guard);DWORD previous;CHECK(VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous));
#else
 guard=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+4096,4096,PROT_NONE));
#endif
 rom_fixture(b);
 for(unsigned n=0;n<=512;++n){unsigned char *p=guard+4096-n;memcpy(p,b,n);polaris_rom_parse(p,n,0x67DF1002,&o);CHECK(n==512 ? o.status==0 : o.status!=0 && empty());}
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 CHECK(polaris_rom_query(&o,POLARIS_ROM_QUERIES)==0 && polaris_rom_query(0,0)==0);
 printf("POLARIS ROM PARSER: PASS;513 protected prefixes,26 malformed vectors,3 revisions,duplicate/identity/checksum/size guards;CRC32=%08X\n",valid_crc);return 0;
}
