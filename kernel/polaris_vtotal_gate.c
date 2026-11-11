#include "polaris_vtotal_gate.h"
#include "lib.h"
enum {BEFORE,WRITE,POSTED,BASELINE,POLL,WAIT,DONE};
void polaris_vtotal_gate_init(PolarisVtotalGate *g,bool restore){
 if(!g)return;k_memset(g,0,sizeof(*g));g->restore=restore;g->initialized=true;
}
static bool prefix(const u32 v[3]){
 return (v[0]==0 || v[0]==1132) && (v[1]==0 || v[1]==1132) &&
  (v[2]==0 || v[2]==0x11) && (!v[1] || v[0]==1132) &&
  (!v[2] || (v[0]==1132 && v[1]==1132));
}
bool polaris_vtotal_gate_step(PolarisVtotalGate *g,const PolarisVtotalIo *io,u64 tag,u64 payload,u32 *out){
 if(!out)return false;*out=0;
 if(!g || !g->initialized || g->poisoned || g->result.complete)return false;
 u32 op=(u32)((tag>>16)&65535U),field=(u32)(tag&65535U);
 if(!io || !io->admitted || !io->read || !io->write || !io->wait ||
  g->sequence>=POLARIS_VTOTAL_MESSAGES || (tag>>32)!=(u64)g->sequence+1 || payload>0xFFFFFFFFULL)goto fail;
 u32 expected=POLARIS_VTOTAL_READ,f=g->field;
 if(g->state==WRITE){expected=POLARIS_VTOTAL_WRITE;f=g->restore?(g->field+2)%3:g->field;
  u32 value=g->restore?0:(g->field<2?1132:0x11);if(payload!=value)goto fail;
 }else if(g->state==WAIT){expected=POLARIS_VTOTAL_WAIT;f=0;}
 else if(g->state==POSTED)f=2;
 else if(g->state==BASELINE)f=5;
 if(g->state==DONE || op!=expected || field!=f || (op!=POLARIS_VTOTAL_WRITE && payload) || !io->admitted(io->context))goto fail;
 ++g->sequence; /* Consume before any effect, including failed stores. */
 if(op==POLARIS_VTOTAL_WRITE){
  ++g->result.write_attempts;if(!io->write(io->context,field,(u32)payload))goto fail;
  if(++g->field==3){g->state=POSTED;g->field=0;}return true;
 }
 if(op==POLARIS_VTOTAL_WAIT){if(!io->wait(io->context))goto fail;g->state=POLL;g->field=0;return true;}
 if(g->state==POLL && !field)++g->result.polls;
 u32 value=0;++g->result.read_attempts;
 if(!io->read(io->context,field,&value) || value==~0U)goto fail;
 ++g->result.reads;
 if(field==3 && value!=0x80410311U)goto fail;
 if(field==4 && value!=1124)goto fail;
 if(field==5 && (value&~0xFFFFFFU))goto fail;
 if(field<3){
  if(g->state==BEFORE){
   g->initial[field]=value;
   if(!g->restore && value)goto fail;
   if(field==2 && !prefix(g->initial))goto fail;
  }else if(value!=(g->restore?0:(field<2?1132U:0x11U)))goto fail;
 }
 *out=value;
 if(g->state==BEFORE){if(++g->field==6){g->state=WRITE;g->field=0;}}
 else if(g->state==POSTED)g->state=BASELINE;
 else if(g->state==BASELINE){g->baseline=value;g->state=POLL;g->field=0;}
 else if(g->state==POLL && ++g->field==6){
  u32 delta=(value-g->baseline)&0xFFFFFFU;if(delta>512U)goto fail;
  if(delta>=2){g->result.frame_delta=delta;g->result.complete=true;g->state=DONE;}
  else {if(g->result.polls>=64)goto fail;g->state=WAIT;}
 }
 return true;
fail:g->poisoned=true;*out=0;return false;
}
