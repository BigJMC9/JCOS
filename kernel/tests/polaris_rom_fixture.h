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
/* Complete routing fixture, independent of the parser implementation. */
static void rom_routing_fixture(unsigned char *b){
 rom_fixture(b);rom_put16(b,0xC8,0x180);rom_put16(b,0xCC,0x1C0);
 rom_put16(b,0x108,0x30);rom_put16(b,0x10A,0x50);
 rom_put16(b,0x110,0x38);rom_put16(b,0x112,0x60);
 for(unsigned c=0;c<2;++c){
  unsigned at=0xF0+(c ? 0x38:0x30);
  b[at]=1;rom_put16(b,at+1,0x211E);b[at+3]=1;rom_put16(b,at+4,0x1100);
  at=0xF0+(c ? 0x60:0x50);
  b[at]=1;b[at+1]=4;b[at+2]=0x90;b[at+3]=0;
  b[at+4]=2;b[at+5]=4;b[at+6]=(unsigned char)(5+c);b[at+7]=1;
  b[at+8]=4;b[at+9]=4;b[at+10]=0;b[at+11]=0;
  b[at+12]=255;b[at+13]=0;
 }
 rom_put16(b,0x180,31);b[0x182]=1;b[0x183]=1;
 for(unsigned i=0;i<8;++i){rom_put16(b,0x184+i*2,0x100+i);b[0x195+i]=(unsigned char)i;}
 b[0x194]=0x90;b[0x19D]=b[0x19E]=0;
 rom_put16(b,0x1C0,12);b[0x1C2]=1;b[0x1C3]=1;
 rom_put16(b,0x1C4,0x200);b[0x1C6]=3;b[0x1C7]=5;
 rom_put16(b,0x1C8,0x200);b[0x1CA]=4;b[0x1CB]=6;rom_checksum(b);
}
/* The first FF is the last byte before the peer record stream begins. */
static void rom_single_byte_terminator_fixture(unsigned char *b){
 rom_routing_fixture(b);
 for(unsigned i=0;i<14;++i)b[0x14D+i]=b[0x150+i];
 rom_put16(b,0x112,0x5D);rom_checksum(b);
}
/* Two complete direct paths,with single-byte encoder end tokens. */
static void rom_topology_fixture(unsigned char *b){
 rom_routing_fixture(b);for(unsigned i=0xF0;i<0x180;++i)b[i]=0;
 rom_put16(b,0xF0,0x8E);b[0xF2]=1;b[0xF3]=3;rom_put16(b,0xF4,0x88);
 rom_put16(b,0xF6,0x2A);rom_put16(b,0xFA,0x64);rom_put16(b,0xFE,0x12);
 b[0x102]=2;b[0x103]=1;b[0x11A]=2;b[0x154]=2;
 for(unsigned i=0;i<2;++i){
  unsigned connector=i ? 0x320C:0x3113,encoder=i ? 0x221E:0x211E;
  unsigned path=0xF0+0x16+i*10,row=0xF0+0x2E + i*8;
  unsigned links=i ? 0x51:0x3E,records=i ? 0x57:0x44;
  rom_put16(b,path,i ? 0x80:8);rom_put16(b,path+2,10);rom_put16(b,path+4,connector);
  rom_put16(b,path+6,0x1100);rom_put16(b,path+8,encoder);
  rom_put16(b,row,connector);rom_put16(b,row+2,links);rom_put16(b,row+4,records);
  b[0xF0+links]=1;rom_put16(b,0xF0+links+1,encoder);
  unsigned at=0xF0+records;
  b[at]=1;b[at+1]=4;b[at+2]=0x90;b[at+3]=0;
  b[at+4]=2;b[at+5]=4;b[at+6]=(unsigned char)(5+i);b[at+7]=1;
  b[at+8]=4;b[at+9]=4;b[at+12]=255;
  row=0xF0+0x68+i*8;links=i ? 0x83:0x78;records=i ? 0x89:0x7E;
  rom_put16(b,row,encoder);rom_put16(b,row+2,links);rom_put16(b,row+4,records);
  at=0xF0+links;b[at]=1;rom_put16(b,at+1,0x1100);b[at+3]=1;rom_put16(b,at+4,connector);
  at=0xF0+records;b[at]=20;b[at+1]=4;rom_put16(b,at+2,15);b[at+4]=255;
 }
 rom_checksum(b);
}
#endif
