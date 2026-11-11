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
static int empty(void){for(unsigned i=POLARIS_ROM_DIRECTORY_QUERIES;i<POLARIS_ROM_QUERIES;++i)if(polaris_rom_query(&o,i))return 0;for(unsigned i=0;i<POLARIS_ROM_CONNECTORS;++i)if(o.connectors[i])return 0;return !o.rom_header && !o.master_data && !o.object_header && !o.image_bytes && !o.count;}
static int routing_tests(void){
 const JcosU64 expected[12]={0x003F010301010000ULL,0x211E,0,0x1100,0,0x01050090,
  0x0103010201010100ULL,0x0107010601050104ULL,0x0706050403020100ULL,0x05030200,0x16,0x000C01C0001F0180ULL};
 rom_routing_fixture(b);polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status);
 for(unsigned i=0;i<12;++i)CHECK(polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+i)==expected[i]);
 CHECK(o.routes[1].hpd_gpio==6 && o.routes[1].hpd_shift==4 && o.routes[1].flags==63);
 unsigned cases=0;
 const struct {unsigned at,value,status;} bad[]={
  {0x120,9,POLARIS_ROM_ROUTE_BAD_LINKS},{0x123,9,POLARIS_ROM_ROUTE_BAD_LINKS},
  {0x122,0x29,POLARIS_ROM_ROUTE_BAD_LINKS},{0x122,0x51,POLARIS_ROM_ROUTE_BAD_LINKS},
  {0x141,0,POLARIS_ROM_ROUTE_BAD_RECORDS},{0x141,5,POLARIS_ROM_ROUTE_BAD_RECORDS},
  {0x147,2,POLARIS_ROM_ROUTE_BAD_RECORDS},{0x148,1,POLARIS_ROM_ROUTE_DUPLICATE},
  {0x148,23,POLARIS_ROM_ROUTE_UNSUPPORTED},{0x195,32,POLARIS_ROM_ROUTE_GPIO},
  {0x19D,1,POLARIS_ROM_ROUTE_GPIO},{0x194,0x91,POLARIS_ROM_ROUTE_GPIO},
  {0x1C6,32,POLARIS_ROM_ROUTE_GPIO},{0x1CB,5,POLARIS_ROM_ROUTE_DUPLICATE},
  {0x180,30,POLARIS_ROM_ROUTE_BOUNDS},{0x182,2,POLARIS_ROM_ROUTE_REVISION},
  {0xC8,0,POLARIS_ROM_ROUTE_BOUNDS},{0x10A,0x30,POLARIS_ROM_ROUTE_BOUNDS},
  {0x108,0x16,POLARIS_ROM_ROUTE_BOUNDS},{0xF8,18,POLARIS_ROM_ROUTE_BOUNDS}
 };
 for(unsigned n=0;n<sizeof(bad)/sizeof(bad[0]);++n){
  rom_routing_fixture(b);b[bad[n].at]=(unsigned char)bad[n].value;rom_checksum(b);
  memset(&o,0xA5,sizeof(o));polaris_rom_parse(b,512,0x67DF1002,&o);
  CHECK(!o.status && o.count==2 && o.connectors[0]);if(o.routes[0].status!=bad[n].status)fprintf(stderr,"routing vector%u at%X:got%u expected%u\n",n,bad[n].at,o.routes[0].status,bad[n].status);CHECK(o.routes[0].status==bad[n].status);
  CHECK(polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES)==bad[n].status);
  for(unsigned f=1;f<12;++f)CHECK(!polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+f));++cases;
 }
 rom_routing_fixture(b);b[0x120]=2;rom_put16(b,0x123,0x211E);b[0x125]=0;rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && o.routes[0].status==POLARIS_ROM_ROUTE_DUPLICATE);++cases;
 rom_routing_fixture(b);b[0x14C]=4;b[0x14D]=2;b[0x14E]=4;b[0x14F]=2;rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && o.routes[0].status==POLARIS_ROM_ROUTE_BAD_RECORDS && !o.routes[1].status);++cases;
 rom_routing_fixture(b);rom_put16(b,0xC8,0);rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && o.routes[0].status==POLARIS_ROM_ROUTE_GPIO);++cases;
 rom_routing_fixture(b);b[0x14D]=255;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && !o.routes[0].status && o.routes[0].record_count==3);++cases;
 /* Exact eight-source/eight-destination boundary. Raw IDs are not resolved into an active graph. */
 rom_routing_fixture(b);b[0x102]=1;rom_put16(b,0x10A,0x60);b[0x150]=255;b[0x151]=0;b[0x120]=8;
 for(unsigned n=0;n<8;++n)rom_put16(b,0x121+n*2,0x2101+n);
 b[0x131]=8;for(unsigned n=0;n<8;++n)rom_put16(b,0x132+n*2,0x3101+n);rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && !o.routes[0].status && o.routes[0].source_count==8 && o.routes[0].dest_count==8);++cases;
 /* Accept exactly64 bounded records; refuse65 without looking into another section. */
 rom_fixture(b);rom_put16(b,0xF0,272);b[0x102]=1;rom_put16(b,0x10A,0x40);
 for(unsigned n=0;n<64;++n){b[0x130+n*2]=4;b[0x131+n*2]=2;}
 b[0x1B0]=255;b[0x1B1]=0;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && !o.routes[0].status && o.routes[0].record_count==64 && o.routes[0].other_count==64);++cases;
 b[0x1B0]=4;b[0x1B1]=2;b[0x1B2]=255;b[0x1B3]=0;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && o.routes[0].status==POLARIS_ROM_ROUTE_BAD_RECORDS);++cases;
 printf("ROM ROUTING: PASS;%u malformed/boundary cases;golden source/destination/I2C/HPD/GPIO report,no partial failed routes\n",cases);return 0;
}
static int terminator_tests(void){
 const JcosU64 expected[12]={0x003F010301010000ULL,0x211E,0,0x1100,0,0x01050090,
  0x0103010201010100ULL,0x0107010601050104ULL,0x0706050403020100ULL,0x05030200,0x16,0x000C01C0001F0180ULL};
 /* No size byte belongs to this FF: the next byte belongs to a peer. */
 rom_single_byte_terminator_fixture(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && !o.routes[0].status && !o.routes[1].status && o.routes[1].hpd_gpio==6);
 for(unsigned i=0;i<12;++i)CHECK(polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+i)==expected[i]);
 /* Exactly one byte remains in ObjectInfo, while GPIO tables remain in ROM. */
 b[0x102]=1;rom_put16(b,0xF0,0x5D);rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && o.count==1);
 for(unsigned i=0;i<12;++i)CHECK(polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+i)==expected[i]);
 for(unsigned n=0;n<2;++n){
  b[0x14C]=(unsigned char)(n ? 4:0);rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
  CHECK(!o.status && o.routes[0].status==POLARIS_ROM_ROUTE_BAD_RECORDS);
  for(unsigned i=1;i<12;++i)CHECK(!polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+i));
 }
 /* Exactly64 ordinary records plus a single FF still respects the budget. */
 rom_fixture(b);rom_put16(b,0xF0,0xC1);b[0x102]=1;rom_put16(b,0x10A,0x40);
 for(unsigned n=0;n<64;++n){b[0x130+n*2]=4;b[0x131+n*2]=2;}
 b[0x1B0]=255;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && !o.routes[0].status && o.routes[0].record_count==64 && o.routes[0].other_count==64);
 puts("ROM SINGLE-BYTE TERMINATOR: PASS;section peer,exact object end,missing non-FF headers and64-record budget");return 0;
}
static int topology_tests(void){
 const JcosU64 expected[2][4]={{0x00000001010F0000ULL,0x0000211E11000008ULL,0x000031131100000FULL,0x007E007800680016ULL},
  {0x00000001010F0000ULL,0x0000221E11000080ULL,0x0000320C1100000FULL,0x0089008300700020ULL}};
 rom_topology_fixture(b);polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && o.count==2);
 for(unsigned i=0;i<2;++i){CHECK(!o.routes[i].status);for(unsigned f=0;f<4;++f)CHECK(o.topology[i][f]==expected[i][f] && polaris_rom_query(&o,POLARIS_ROM_TOPOLOGY_BASE+i*4+f)==expected[i][f]);}
 JcosU64 connectors[2]={o.connectors[0],o.connectors[1]};unsigned cases=0;
 const struct {unsigned at,width,value,first,peer;} bad[]={
  {0x102,1,17,2,2},{0x103,1,2,3,3},{0x104,1,1,4,4},
  {0x108,2,0,2,2},{0x108,2,9,2,2},{0x108,2,26,2,2},{0x108,2,65535,2,2},
  {0x10A,2,0x320C,1,5},{0x10C,2,0x211E,6,6},{0x10E,2,0x221E,6,0},{0x106,2,0,6,0},
  {0x154,1,33,2,2},{0x155,1,1,4,4},{0x158,2,0x221E,5,5},{0x15E,2,1,6,6},
  {0x15A,2,65535,2,2},{0x15C,2,65535,2,2},{0x168,1,2,4,0},
  {0x169,2,0x1101,6,0},{0x16B,1,0,4,0},{0x16C,2,0x320C,6,0},
  {0x16E,1,23,4,0},{0x16F,1,3,7,0},{0x172,1,0,7,0},{0x15C,2,0x78,7,0},
  {0xFE,2,0,1,1},{0xFA,2,0,1,1},{0x12F,2,0x221E,6,0},{0xF4,2,0,6,6}
 };
 for(unsigned n=0;n<sizeof(bad)/sizeof(bad[0]);++n){
  rom_topology_fixture(b);if(bad[n].width==2)rom_put16(b,bad[n].at,bad[n].value);else b[bad[n].at]=(unsigned char)bad[n].value;rom_checksum(b);
  polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && o.count==2 && o.connectors[0]==connectors[0] && o.connectors[1]==connectors[1]);
  const unsigned status[2]={bad[n].first,bad[n].peer};
  for(unsigned i=0;i<2;++i){
   if((o.topology[i][0]&65535)!=status[i])fprintf(stderr,"topology vector%u at%X route%u:got%llu expected%u\n",n,bad[n].at,i,(unsigned long long)(o.topology[i][0]&65535),status[i]);
   CHECK((o.topology[i][0]&65535)==status[i]);
   for(unsigned f=0;f<4;++f)CHECK(o.topology[i][f]==(status[i] ? (f ? 0:status[i]):expected[i][f]));
  }
  ++cases;
 }
 /* Multiple graphics objects are explicitly outside the direct-path profile. */
 rom_topology_fixture(b);b[0x102]=1;rom_put16(b,0x108,12);rom_put16(b,0x110,0x211E);rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && o.topology[0][0]==POLARIS_ROM_TOPOLOGY_UNSUPPORTED && o.topology[1][0]==POLARIS_ROM_TOPOLOGY_ABSENT);++cases;
 /* Duplicate capability record is refused without publishing the first copy. */
 rom_topology_fixture(b);b[0x102]=b[0x11A]=b[0x154]=1;b[0x172]=20;b[0x173]=4;b[0x174]=15;b[0x175]=0;b[0x176]=255;rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && !o.routes[0].status && o.topology[0][0]==POLARIS_ROM_TOPOLOGY_DUPLICATE);
 for(unsigned f=1;f<4;++f)CHECK(!o.topology[0][f]);++cases;
 /* Exactly64 encoder records plus singleFF;the65th record is refused. */
 rom_topology_fixture(b);b[0x102]=b[0x11A]=b[0x154]=1;b[0x134]=255;rom_put16(b,0xF0,0x101);
 for(unsigned n=0;n<63;++n){b[0x172+n*2]=4;b[0x173+n*2]=2;}b[0x1F0]=255;rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && !o.routes[0].status && !(o.topology[0][0]&65535) && ((o.topology[0][0]>>32)&255)==64 && ((o.topology[0][0]>>40)&255)==63);++cases;
 rom_put16(b,0xF0,0x103);b[0x1F0]=4;b[0x1F1]=2;b[0x1F2]=255;rom_checksum(b);polaris_rom_parse(b,512,0x67DF1002,&o);
 CHECK(!o.status && o.topology[0][0]==POLARIS_ROM_TOPOLOGY_BAD_RECORDS);for(unsigned f=1;f<4;++f)CHECK(!o.topology[0][f]);++cases;
 printf("ROM TOPOLOGY: PASS;%u malformed/boundary cases;direct path/encoder/source/destination/capability golden,independent refusals\n",cases);return 0;
}
static int byte_tests(void){
 const unsigned char bytes[8]={0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};JcosU64 value=0;
 CHECK(polaris_rom_read_bytes(bytes,8,0,8,&value) && value==0x8877665544332211ULL);
 CHECK(polaris_rom_read_bytes(bytes,8,3,3,&value) && value==0x665544ULL);
 CHECK(!polaris_rom_read_bytes(bytes,8,0,0,&value) && !value);value=~0ULL;
 CHECK(!polaris_rom_read_bytes(bytes,8,0,9,&value) && !value);value=~0ULL;
 CHECK(!polaris_rom_read_bytes(bytes,8,7,2,&value) && !value);value=~0ULL;
 CHECK(!polaris_rom_read_bytes(bytes,8,~0ULL,1,&value) && !value);value=~0ULL;
 CHECK(!polaris_rom_read_bytes((void *)1,POLARIS_ROM_LIMIT+1,0,1,&value) && !value);value=~0ULL;
 CHECK(!polaris_rom_read_bytes(0,8,0,1,&value) && !value);
 CHECK(!polaris_rom_read_bytes(bytes,8,0,1,0));
 return 0;
}
int main(void){
 if(byte_tests() || routing_tests() || terminator_tests() || topology_tests())return 1;
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
 for(unsigned n=0;n<=512;++n){
  unsigned char *p=guard+4096-n;memcpy(p,b,n);polaris_rom_parse(p,n,0x67DF1002,&o);CHECK(n==512 ? o.status==0 : o.status!=0 && empty());
  JcosU64 value=~0ULL;CHECK(!polaris_rom_read_bytes(p,n,n,1,&value) && !value);
  if(n){
   CHECK(polaris_rom_read_bytes(p,n,n-1,1,&value) && value==b[n-1]);
   unsigned width=n<8 ? n:8;JcosU64 expected=0;
   for(unsigned i=0;i<width;++i)expected|=(JcosU64)b[n-width+i]<<(i*8);
   CHECK(polaris_rom_read_bytes(p,n,n-width,width,&value) && value==expected);
  }
 }
 puts("ROM BYTE READER: PASS;9 fixed/bounds vectors;513 protected prefixes with final-byte/1..8byte and overrun checks");
 /* Exercise routing bounds directly after valid directory decoding: each
  * candidate prefix ends at the protected page, including inside GPIO tables. */
 rom_routing_fixture(b);polaris_rom_parse(b,512,0x67DF1002,&o);PolarisRomInfo directory=o;
 for(unsigned n=0;n<=512;++n){
  unsigned char *p=guard+4096-n;memcpy(p,b,n);o=directory;polaris_rom_routes_parse(p,n,&o);
  CHECK(o.status==0 && o.count==2);
  if(n>=460)CHECK(!o.routes[0].status && !o.routes[1].status);
  else CHECK(o.routes[0].status && o.routes[1].status);
 }
 puts("ROM ROUTING GUARDED PREFIXES: PASS;513 protected object/link/record/GPIO table boundaries");
 /* Place the FF itself against inaccessible memory: never read a size byte. */
 rom_fixture(b);b[0x102]=1;rom_put16(b,0xF0,0x61);rom_put16(b,0x10A,0x60);b[0x150]=255;rom_checksum(b);
 polaris_rom_parse(b,512,0x67DF1002,&o);CHECK(!o.status && !o.routes[0].status);directory=o;
 for(unsigned n=0;n<3;++n){
  b[0x150]=(unsigned char)(n==0 ? 255:n==1 ? 0:4);
  unsigned char *p=guard+4096-0x151;memcpy(p,b,0x151);o=directory;polaris_rom_routes_parse(p,0x151,&o);
  CHECK(!o.status && o.count==1 && o.routes[0].status==(n ? POLARIS_ROM_ROUTE_BAD_RECORDS:POLARIS_ROM_ROUTE_OK));
  CHECK(polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES)==(n ? POLARIS_ROM_ROUTE_BAD_RECORDS:0x0002000000000000ULL));
  for(unsigned i=1;i<12;++i)CHECK(!polaris_rom_query(&o,POLARIS_ROM_DIRECTORY_QUERIES+i));
 }
 puts("ROM SINGLE-BYTE GUARDED END: PASS;FF accepted without size-byte load;incomplete ordinary headers cleared/refused");
 /* The final encoder FF is the last accessible byte of ObjectInfo. */
 rom_topology_fixture(b);polaris_rom_parse(b,512,0x67DF1002,&o);directory=o;
 for(unsigned n=0;n<=512;++n){
  unsigned char *p=guard+4096-n;memcpy(p,b,n);o=directory;polaris_rom_topology_parse(p,n,&o);
  for(unsigned i=0;i<2;++i){
   if(n>=0x17E)for(unsigned f=0;f<4;++f)CHECK(o.topology[i][f]==directory.topology[i][f]);
   else {CHECK(o.topology[i][0]==POLARIS_ROM_TOPOLOGY_BOUNDS);for(unsigned f=1;f<4;++f)CHECK(!o.topology[i][f]);}
  }
 }
 for(unsigned n=0;n<3;++n){
  b[0x17D]=(unsigned char)(n==0 ? 255:n==1 ? 0:4);unsigned char *p=guard+4096-0x17E;memcpy(p,b,0x17E);o=directory;polaris_rom_topology_parse(p,0x17E,&o);
  for(unsigned f=0;f<4;++f)CHECK(o.topology[0][f]==directory.topology[0][f]);
  CHECK(o.topology[1][0]==(n ? POLARIS_ROM_TOPOLOGY_BAD_RECORDS:directory.topology[1][0]));
  for(unsigned f=1;f<4;++f)CHECK(o.topology[1][f]==(n ? 0:directory.topology[1][f]));
 }
 /* Mutate every object byte at a protected image end;all failed topology tails clear. */
 const unsigned values[4]={0,1,128,255};
 for(unsigned at=0xF0;at<0x17E;++at)for(unsigned v=0;v<4;++v){
  rom_topology_fixture(b);b[at]=(unsigned char)values[v];rom_checksum(b);unsigned char *p=guard+4096-512;memcpy(p,b,512);polaris_rom_parse(p,512,0x67DF1002,&o);
  for(unsigned i=0;i<POLARIS_ROM_CONNECTORS;++i)if(o.topology[i][0]&65535)for(unsigned f=1;f<4;++f)CHECK(!o.topology[i][f]);
 }
 puts("ROM TOPOLOGY GUARDED: PASS;513 protected prefixes,finalFF/truncated headers,568 object-byte mutations and cleared failures");
#ifdef _WIN32
 CHECK(VirtualFree(guard,0,MEM_RELEASE));
#else
 CHECK(!munmap(guard,8192));
#endif
 CHECK(polaris_rom_query(&o,POLARIS_ROM_QUERIES)==0 && polaris_rom_query(0,0)==0);
 printf("POLARIS ROM PARSER: PASS;513 protected prefixes,26 malformed vectors,3 revisions,duplicate/identity/checksum/size guards;CRC32=%08X\n",valid_crc);return 0;
}
