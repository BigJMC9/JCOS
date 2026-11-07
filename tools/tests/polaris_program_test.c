#include <stdio.h>
#include <string.h>
#include "../../kernel/polaris_program.h"
#include "../../user/drivers/amd_polaris_plane.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"program line%d %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned operation,writes,waits,fail_at,fault;
static bool admission(void *c){(void)c;return !(fault==1 && operation>=1) && !(fault==16 && writes==1) && !(fault==17 && writes==2);}
static bool store(void *c,PolarisDisplayRegister f,u32 v) {
    (void)c;++operation;
    if (f!=(writes ? POLARIS_REG_PRIMARY_LOW : POLARIS_REG_PRIMARY_HIGH) || v!=(writes ? 0U : 0xF4U)) return false;
    ++writes;return operation!=fail_at;
}
static bool load(void *c,PolarisRebindRead f,u32 *v) {
    (void)c;++operation;
    if (operation==fail_at) return false;
    *v=0;
    if(f==POLARIS_REBIND_HIGH)*v=0xF4;
    if(f==POLARIS_REBIND_CRTC)*v=0x80410311;
    if(f==POLARIS_REBIND_FLIP)*v=0x20;
    if(f==POLARIS_REBIND_FRAME)*v=fault==2 ? 100 : 100+waits;
    if(fault==3 && f==POLARIS_REBIND_UPDATE)*v=4;
    if(fault==4 && f==POLARIS_REBIND_UPDATE)*v=0x10000;
    if(fault==5 && f==POLARIS_REBIND_LOW)*v=0x100;
    if(fault==6 && f==POLARIS_REBIND_HIGH)*v=0xF5;
    if(fault==7 && f==POLARIS_REBIND_CRTC)*v=0;
    if(fault==8 && f==POLARIS_REBIND_FLIP)*v=0;
    if(fault==9 && f==POLARIS_REBIND_FRAME)*v=~0U;
    if(fault==10 && f==POLARIS_REBIND_FRAME && waits)*v=0x1000001;
    if(fault==11 && f==POLARIS_REBIND_FRAME && waits)*v=99;
    if(fault==13 && f==POLARIS_REBIND_FRAME)*v=(0xFFFFFEU+waits)&0xFFFFFFU;
    if(fault==14 && f==POLARIS_REBIND_FRAME && waits)*v=613;
    if(fault==15 && f==POLARIS_REBIND_UPDATE && writes==2 && waits<2)*v=4;
    return true;
}
static bool pause_io(void *c){(void)c;++waits;return fault!=12;}
static void setup(PolarisProgramPlan *p) {
    memset(p,0,sizeof(*p));operation=writes=waits=fail_at=fault=0;
    p->incarnation=17;p->observation_id=29;p->gpu_address=0xF400000000ULL;p->crtc=0x80410311;
    p->witness.values[0][1]=p->witness.values[1][1]=0x20;
    p->witness.values[0][2]=100;p->witness.values[1][2]=110;
    PolarisDisplayObservation o={0};
    o.surface_gpu_address=p->gpu_address;o.linear_span_bytes=0x300000;
    o.surface_width=o.pitch_pixels=1024;o.surface_height=768;o.depth=2;o.array_mode=1;
    for(unsigned i=0;i<5;++i){JcosU32 value;polaris_plane_recipe_step(&o,&p->witness,0,i,&p->tags[i],&value);p->values[i]=value;}
}
int main(void) {
    PolarisProgramIo io={0};io.admitted=admission;io.write=store;io.read=load;io.wait=pause_io;
    PolarisProgramPlan p;PolarisProgramResult r;
    setup(&p);CHECK(polaris_program_execute(&p,&io,&r)&&r.complete&&writes==2&&r.frame_delta==2&&r.polls==3&&r.reads==26);
    CHECK(!polaris_program_execute(&p,&io,&r)&&writes==2); /* no replay */
    for(unsigned step=0;step<5;++step)for(unsigned bit=0;bit<64;++bit) {
        setup(&p);p.tags[step]^=1ULL<<bit;CHECK(!polaris_program_execute(&p,&io,&r)&&!operation);
        setup(&p);p.values[step]^=1ULL<<bit;CHECK(!polaris_program_execute(&p,&io,&r)&&!operation);
    }
    for(unsigned f=1;f<=28;++f){setup(&p);fail_at=f;CHECK(!polaris_program_execute(&p,&io,&r)&&writes<=2&&r.write_attempts<=2&&r.reads<=26);}
    for(unsigned f=1;f<=12;++f){setup(&p);fault=f;CHECK(!polaris_program_execute(&p,&io,&r)&&writes<=2&&r.polls<=64&&r.reads<=392);}
    for(unsigned f=13;f<=15;f+=2){setup(&p);fault=f;CHECK(polaris_program_execute(&p,&io,&r)&&r.complete&&writes==2&&r.frame_delta==2);}
    for(unsigned f=14;f<=17;++f){if(f==15)continue;setup(&p);fault=f;CHECK(!polaris_program_execute(&p,&io,&r)&&writes<=2);}
    setup(&p);p.gpu_address+=0x100;CHECK(!polaris_program_execute(&p,&io,&r)&&!operation);
    setup(&p);p.pipe=1;CHECK(!polaris_program_execute(&p,&io,&r)&&!operation);
    setup(&p);p.incarnation=0;CHECK(!polaris_program_execute(&p,&io,&r)&&!operation);
    puts("PASS: retained reference executor HIGH/LOW once, posted read, post-write frame baseline, idle/progress,640 pre-IO corruptions,28 transaction failures,15 state/budget failures,24-bit completion wrap,pending drain and no replay; no hardware.");
    return 0;
}
