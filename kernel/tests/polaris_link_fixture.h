#ifndef JAOS_POLARIS_LINK_FIXTURE_H
#define JAOS_POLARIS_LINK_FIXTURE_H
/* Copied synthetic data only:CRTC2 -> FE1 -> BE4;no register transaction. */
static void polaris_link_fixture(unsigned *v){
 for(unsigned i=0;i<52;++i)v[i]=0;
 v[2]=v[48]=0x10001U;
 v[14]=v[34]=1;v[15]=v[35]=0x50000200U;
 v[21]=v[41]=0x01000002U;
}
#endif