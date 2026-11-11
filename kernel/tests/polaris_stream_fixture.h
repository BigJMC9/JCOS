#ifndef JAOS_POLARIS_STREAM_FIXTURE_H
#define JAOS_POLARIS_STREAM_FIXTURE_H
#include "polaris_vertical_fixture.h"
#include "polaris_stream_protocol.h"
/* Inherited timing fixture plus SYNTHETIC zero override and FIFO/update tail.
 * The ten extended values are not physical evidence. */
static inline void polaris_stream_fixture(JcosU32 v[POLARIS_STREAM_VALUES]){
 polaris_vertical_fixture(v);for(JcosU32 i=80;i<86;++i)v[i]=0;
 const JcosU32 extra[5]={0x10201U,0,0x01000000U,0,0};
 for(JcosU32 i=0;i<10;++i)v[86+i]=extra[i%5];
}
static const JcosU64 polaris_stream_golden[POLARIS_STREAM_WORDS]={
 0x10000ULL,0x0002020000020000ULL,0x00C0084007800898ULL,
 0x0029046104380465ULL,0x00050000002C0000ULL,0,0x0000010002000000ULL,
 0x0000758D00008000ULL,0x0000005000010201ULL,0x0000758F0000758DULL,
 0x10201ULL,0x01000000ULL,0,0};
#endif
