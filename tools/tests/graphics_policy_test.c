#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/graphics_policy.h"
#include "../../kernel/graphics_supervisor.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"graphics policy line%d: %s\n",__LINE__,#x);exit(1); } } while(0)
static GraphicsPolicy model;
static u64 pid=100,tid=101,exchanges;
static bool idle=true,failed,replace,corrupt;
bool supervisor_idle(void);
bool supervisor_running(void){return supervisor_idle();}
bool supervisor_idle(void){return idle;}
u64 supervisor_process_id(void){return pid;}
u64 supervisor_thread_id(void){return tid;}
bool supervisor_graphics_exchange(u64 session,u64 sequence,u64 snapshot,u64 *out){
 ++exchanges;
 if(failed)return false;
 *out=graphics_policy_decide(&model,session,sequence,snapshot);
 if(replace)++pid;
 if(corrupt)*out=99;
 return *out!=GRAPHICS_POLICY_REJECT;
}
static JcosU64 take(GraphicsPolicy *p,JcosU64 event,JcosU64 facts){
 return graphics_policy_decide(p,p->session,p->sequence+1,GRAPHICS_POLICY_VERSION|event|facts);
}
int main(void){
 unsigned envelopes=0,observations=0;
 const u64 alive=GRAPHICS_POLICY_PRIMARY_LIVE|GRAPHICS_POLICY_RESCUE_LIVE;
 const u64 all=alive|GRAPHICS_POLICY_PROBED|GRAPHICS_POLICY_PRIMARY_PONG|GRAPHICS_POLICY_RESCUE_PONG;
 for(unsigned bits=0;bits<64;++bits){
   GraphicsPolicy p={0};
   CHECK(graphics_policy_decide(&p,1,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_ACCEPT);
   CHECK(take(&p,GRAPHICS_POLICY_PRIMARY,0)==GRAPHICS_POLICY_KEEP);
   GraphicsPolicy before=p;
   u64 packet=GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_OBSERVE|all;
   packet^=1ULL<<bits;
   u64 result=graphics_policy_decide(&p,1,3,packet);
   if(result==GRAPHICS_POLICY_REJECT){CHECK(!memcmp(&p,&before,sizeof(p)));++envelopes;}
 }
 for(unsigned phase=0;phase<2;++phase)for(unsigned flags=0;flags<32;++flags){
   GraphicsPolicy p={0};
   CHECK(graphics_policy_decide(&p,1,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_ACCEPT);
   CHECK(take(&p,GRAPHICS_POLICY_PRIMARY,0)==GRAPHICS_POLICY_KEEP);
   if(phase){
     CHECK(take(&p,GRAPHICS_POLICY_REQUEST_RESCUE,0)==GRAPHICS_POLICY_RECOVER);
     CHECK(take(&p,GRAPHICS_POLICY_RESCUED,0)==GRAPHICS_POLICY_KEEP);
   }
   GraphicsPolicy before=p;
   u64 facts=(u64)flags<<8;
   u64 result=take(&p,GRAPHICS_POLICY_OBSERVE,facts);
   bool valid=(!((flags&12)&&!(flags&16)))&&(!(flags&4)||(flags&1))&&(!(flags&8)||(flags&2))&&(!phase||!(flags&5));
   if(!valid){CHECK(result==GRAPHICS_POLICY_REJECT&&!memcmp(&p,&before,sizeof(p)));}
   else if(!(flags&2)||((flags&16)&&!(flags&8)))CHECK(result==GRAPHICS_POLICY_HEADLESS);
   else if(!phase&&(!(flags&1)||((flags&16)&&!(flags&4))))CHECK(result==GRAPHICS_POLICY_RECOVER);
   else CHECK(result==GRAPHICS_POLICY_KEEP);
   ++observations;
 }
 GraphicsPolicy p={0};
 CHECK(graphics_policy_decide(&p,8,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_ACCEPT);
 CHECK(graphics_policy_decide(&p,9,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT);
 for(unsigned event=1;event<=7;++event){
   GraphicsPolicy q=p;
   CHECK(graphics_policy_decide(&q,7,2,GRAPHICS_POLICY_VERSION|event)==GRAPHICS_POLICY_REJECT);
   CHECK(graphics_policy_decide(&q,8,1,GRAPHICS_POLICY_VERSION|event)==GRAPHICS_POLICY_REJECT);
   CHECK(graphics_policy_decide(&q,8,3,GRAPHICS_POLICY_VERSION|event)==GRAPHICS_POLICY_REJECT);
   CHECK(!memcmp(&q,&p,sizeof(q)));
 }
 CHECK(take(&p,GRAPHICS_POLICY_RESCUED,0)==GRAPHICS_POLICY_REJECT);
 CHECK(take(&p,GRAPHICS_POLICY_CLOSE,0)==GRAPHICS_POLICY_HEADLESS);
 CHECK(graphics_policy_decide(&p,8,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT);
 CHECK(graphics_policy_decide(&p,7,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT);
 CHECK(graphics_policy_decide(&p,9,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_ACCEPT);
 p.sequence=~0ULL;CHECK(take(&p,GRAPHICS_POLICY_CLOSE,0)==GRAPHICS_POLICY_REJECT);
 CHECK(graphics_policy_decide(0,1,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT);
 CHECK(graphics_policy_decide(&p,0,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT);
 for(unsigned test=0;test<8;++test){
   GraphicsSupervisorSession s={0};memset(&model,0,sizeof(model));
   failed=replace=corrupt=false;idle=true;
   CHECK(graphics_supervisor_begin(&s));
   CHECK(!graphics_supervisor_begin(&s));
   u64 count=exchanges,action;
   if(test==0)++pid;
   if(test==1)++tid;
   if(test==2)idle=false;
   if(test==3)failed=true;
   if(test==4)replace=true;
   if(test==5)corrupt=true;
   if(test==6)s.sequence=~0ULL;
   if(test==7){
     CHECK(!graphics_supervisor_request(&s,GRAPHICS_POLICY_PRIMARY,1ULL<<60,&action));
     CHECK(exchanges==count&&!s.failed&&s.sequence==1);
     CHECK(graphics_supervisor_request(&s,GRAPHICS_POLICY_PRIMARY,0,&action)&&action==GRAPHICS_POLICY_KEEP);
     CHECK(graphics_supervisor_request(&s,GRAPHICS_POLICY_OBSERVE,all,&action)&&action==GRAPHICS_POLICY_KEEP);
     CHECK(graphics_supervisor_request(&s,GRAPHICS_POLICY_CLOSE,0,&action)&&action==GRAPHICS_POLICY_HEADLESS);
     continue;
   }
   CHECK(!graphics_supervisor_request(&s,GRAPHICS_POLICY_PRIMARY,0,&action)&&s.failed);
   u64 after=exchanges;failed=replace=corrupt=false;idle=true;
   CHECK(!graphics_supervisor_request(&s,GRAPHICS_POLICY_CLOSE,0,&action)&&exchanges==after);
   if(test<3||test==6)CHECK(count==after);
 }
 unsigned reconnect_cases=0;
 for(unsigned stage=0;stage<5;++stage)for(unsigned flags=0;flags<128;++flags){
   GraphicsPolicy q={0};
   CHECK(graphics_policy_decide(&q,1,1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_ACCEPT);
   if(stage>=1)CHECK(take(&q,GRAPHICS_POLICY_PRIMARY,0)==GRAPHICS_POLICY_KEEP);
   if(stage>=2)CHECK(take(&q,GRAPHICS_POLICY_REQUEST_RESCUE,0)==GRAPHICS_POLICY_RECOVER);
   if(stage>=3)CHECK(take(&q,GRAPHICS_POLICY_RESCUED,0)==GRAPHICS_POLICY_KEEP);
   if(stage==4)CHECK(take(&q,GRAPHICS_POLICY_CLOSE,0)==GRAPHICS_POLICY_HEADLESS);
   GraphicsPolicy before=q;u64 facts=(u64)flags<<8;
   u64 decision=take(&q,GRAPHICS_POLICY_CLIENT_RECONNECT,facts);
   if(stage==3&&facts==GRAPHICS_POLICY_RECONNECT_FACTS){
     CHECK(decision==GRAPHICS_POLICY_CONNECT&&q.client_reconnected==1);
     before=q;CHECK(take(&q,GRAPHICS_POLICY_CLIENT_RECONNECT,facts)==GRAPHICS_POLICY_REJECT&&!memcmp(&q,&before,sizeof(q)));
     CHECK(take(&q,GRAPHICS_POLICY_OBSERVE,GRAPHICS_POLICY_RESCUE_LIVE|GRAPHICS_POLICY_PROBED|GRAPHICS_POLICY_RESCUE_PONG)==GRAPHICS_POLICY_KEEP);
   }else CHECK(decision==GRAPHICS_POLICY_REJECT&&!memcmp(&q,&before,sizeof(q)));
   ++reconnect_cases;
 }
 GraphicsPolicy legacy={0};CHECK(graphics_policy_decide(&legacy,1,1,(1ULL<<56)|GRAPHICS_POLICY_BEGIN)==GRAPHICS_POLICY_REJECT&&!legacy.session);
 CHECK(reconnect_cases==640);
 printf("PASS:640 reconnect stage/fact cases,one-shot reconnection,no replay/ledger change on refusal,legacy protocol refused.\n");
 printf("PASS: %u malformed envelope-bit refusals,64 stage/health observations,stale session/sequence and closed-state refusal;8 production-client identity/transport/corrupt/local-validation/no-replay cases;no hardware.\n",envelopes);
 CHECK(observations==64);
 return 0;
}
