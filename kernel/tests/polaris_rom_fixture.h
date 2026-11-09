#ifndef JAOS_POLARIS_ROM_FIXTURE_H
#define JAOS_POLARIS_ROM_FIXTURE_H
/* Synthetic bytes, not a captured or executable vendor ROM. */
static void rom_put16(unsigned char *b,unsigned n,unsigned v){b[n]=(unsigned char)v;b[n+1]=(unsigned char)(v>>8);}
static void rom_checksum(unsigned char *b){b[511]=0;unsigned char s=0;for(unsigned i=0;i<511;++i)s=(unsigned char)(s+b[i]);b[511]=(unsigned char)(0U-s);}
static void rom_fixture(unsigned char *b){
 for(unsigned i=0;i<512;++i)b[i]=0;
 b[0]=0x55;b[1]=0xAA;b[2]=1;rom_put16(b,0x18,0x60);rom_put16(b,0x48,0x80);
 b[0x60]='P';b[0x61]='C';b[0x62]='I';b[0x63]='R';rom_put16(b,0x64,0x1002);rom_put16(b,0x66,0x67DF);
 rom_put16(b,0x6A,0x18);rom_put16(b,0x70,1);b[0x75]=0x80;
 rom_put16(b,0x80,36);b[0x82]=1;b[0x83]=1;b[0x84]='A';b[0x85]='T';b[0x86]='O';b[0x87]='M';rom_put16(b,0xA0,0xB0);
 rom_put16(b,0xB0,50);b[0xB2]=1;b[0xB3]=1;rom_put16(b,0xE0,0xF0);
 rom_put16(b,0xF0,128);b[0xF2]=1;b[0xF3]=3;rom_put16(b,0xF6,18);
 b[0x102]=2;rom_put16(b,0x106,0x3113);rom_put16(b,0x10A,0x60);
 rom_put16(b,0x10E,0x320C);rom_put16(b,0x112,0x62);rom_checksum(b);
}
#endif
