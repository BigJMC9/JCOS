#include "polaris_program_gate.h"
#include "lib.h"
enum {BEFORE=0,HIGH,LOW,POSTED,BASELINE,POLL,WAIT,DONE};
bool polaris_program_gate_init(PolarisProgramGate *g,PolarisProgramPlan *p) {
    if(!g)return false;
    k_memset(g,0,sizeof(*g));
    if(!p || p->consumed)return false;
    p->consumed=true;
    if(p->pipe || p->gpu_address!=0xF400000000ULL || p->crtc!=0x80410311U)return false;
    PolarisPlanePreview preview;
    if(!polaris_plane_preview_init(&preview,p->incarnation,p->observation_id,p->gpu_address,p->pipe,&p->witness))return false;
    for(u32 i=0;i<5;++i)
        if(!polaris_plane_preview_accept(&preview,p->incarnation,p->observation_id,p->tags[i],p->values[i]))return false;
    g->flip=p->witness.values[0][POLARIS_WITNESS_FLIP_CONTROL];
    g->initialized=true;return true;
}
static bool read_valid(u32 f,u32 v,u32 flip,bool polling) {
    const u32 taken=POLARIS_GRPH_MODE_UPDATE_TAKEN_MASK|POLARIS_GRPH_SURFACE_UPDATE_TAKEN_MASK;
    const u32 pending=POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK;
    if(v==~0U)return false;
    switch(f) {
    case 0:return !v;
    case 1:return v==0xF4U;
    case 2:return v==0x80410311U;
    case 3:return !(v&~(taken|(polling?pending:0U)));
    case 4:return v==flip;
    case 5:return !(v&~POLARIS_CRTC_FRAME_COUNT_MASK);
    default:return false;
    }
}
bool polaris_program_gate_step(PolarisProgramGate *g,const PolarisProgramIo *io,u64 tag,u64 payload,u32 *out) {
    if(!out)return false;
    *out=0;
    if(!g || !g->initialized || g->poisoned || g->result.complete)return false;
    u32 sequence=(u32)(tag>>32),operation=(u32)((tag>>16)&0xFFFFU),field=(u32)(tag&0xFFFFU);
    if(!io || !io->admitted || !io->read || !io->write || !io->wait ||
       g->sequence>=POLARIS_REBIND_MESSAGES || sequence!=g->sequence+1 ||
       payload>0xFFFFFFFFULL)goto fail;
    u32 expected_op=POLARIS_REBIND_READ,expected_field=g->field;
    if(g->state==HIGH || g->state==LOW) {
        expected_op=POLARIS_REBIND_WRITE;
        expected_field=g->state==HIGH?POLARIS_REG_PRIMARY_HIGH:POLARIS_REG_PRIMARY_LOW;
        if(payload!=(g->state==HIGH?0xF4U:0U))goto fail;
    } else if(g->state==WAIT) {expected_op=POLARIS_REBIND_WAIT;expected_field=0;}
    else if(g->state==POSTED)expected_field=0;
    else if(g->state==BASELINE)expected_field=5;
    if(g->state==DONE || operation!=expected_op || field!=expected_field ||
       (operation!=POLARIS_REBIND_WRITE && payload) || !io->admitted(io->context))goto fail;
    ++g->sequence; /* Consume before a potentially ambiguous hardware operation. */
    if(operation==POLARIS_REBIND_WRITE) {
        ++g->result.write_attempts;
        if(!io->write(io->context,(PolarisDisplayRegister)field,(u32)payload))goto fail;
        g->state=g->state==HIGH?LOW:POSTED;return true;
    }
    if(operation==POLARIS_REBIND_WAIT) {
        if(!io->wait(io->context))goto fail;
        g->state=POLL;g->field=0;return true;
    }
    if(g->state==POLL && !field)++g->result.polls;
    u32 value=0;
    if(!io->read(io->context,(PolarisRebindRead)field,&value))goto fail;
    ++g->result.reads;
    if(!read_valid(field,value,g->flip,g->state==POLL))goto fail;
    *out=value;
    if(g->state==BEFORE) {
        if(++g->field==6){g->state=HIGH;g->field=0;}
    } else if(g->state==POSTED)g->state=BASELINE;
    else if(g->state==BASELINE){g->baseline=value;g->state=POLL;g->field=0;}
    else if(g->state==POLL) {
        if(field==3)g->update=value;
        if(++g->field==6) {
            u32 delta=(value-g->baseline)&POLARIS_CRTC_FRAME_COUNT_MASK;
            if(delta>512U)goto fail;
            if(delta>=2U && !(g->update&(POLARIS_GRPH_MODE_UPDATE_PENDING_MASK|POLARIS_GRPH_SURFACE_UPDATE_PENDING_MASK))) {
                g->result.complete=true;g->result.frame_delta=delta;g->state=DONE;
            } else {
                if(g->result.polls>=POLARIS_REBIND_POLLS)goto fail;
                g->state=WAIT;
            }
        }
    }
    return true;
fail:
    g->poisoned=true;*out=0;return false;
}
