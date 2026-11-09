/* Execute production lost-generation ticket logic,not a second ownership model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/graphical_session.c"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"GUI reconnect ticket line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static bool app_retained,surface_retained;
static unsigned comp_state,disp_state,surfaces,inputs;
static u64 current_output=22;
bool program_instance_needs_cleanup(const ProgramInstance *p){(void)p;return app_retained;}
bool display_surface_valid(const DisplaySurface *s){(void)s;return surface_retained;}
ManagedServiceState compositor_service_state(void){return (ManagedServiceState)comp_state;}
ManagedServiceState display_service_state(void){return (ManagedServiceState)disp_state;}
u32 display_service_surface_count(void){return surfaces;}
u32 display_service_input_client_count(void){return inputs;}
bool framebuffer_output_matches(u64 id){return id==current_output;}
static void reset(void){
 g_active=g_exit_queue_created=false;g_writer_pid=g_lost_output=0;
 memset(&g_client,0,sizeof(g_client));app_retained=surface_retained=false;
 comp_state=disp_state=MANAGED_SERVICE_STOPPED;surfaces=inputs=0;current_output=22;
}
int main(void){
 reset();CHECK(!graphical_session_take_reconnect());
 remember_lost_output(0,true);CHECK(!graphical_session_take_reconnect());
 remember_lost_output(11,false);CHECK(!graphical_session_take_reconnect());
 current_output=11;remember_lost_output(11,true);CHECK(!graphical_session_take_reconnect());
 for(unsigned gate=0;gate<11;++gate){
  reset();
  switch(gate){
   case 0:g_active=true;break;case 1:g_exit_queue_created=true;break;
   case 2:g_writer_pid=1;break;case 3:app_retained=true;break;
   case 4:g_client.active=true;break;case 5:g_client.endpoint_created=true;break;
   case 6:surface_retained=true;break;case 7:comp_state=MANAGED_SERVICE_RUNNING;break;
   case 8:disp_state=MANAGED_SERVICE_RUNNING;break;case 9:surfaces=1;break;case 10:inputs=1;break;
  }
  CHECK(!graphical_session_drained());remember_lost_output(11,true);CHECK(!g_lost_output&&!graphical_session_take_reconnect());
 }
 reset();remember_lost_output(11,true);CHECK(graphical_session_take_reconnect()==11&&!graphical_session_take_reconnect());
 remember_lost_output(11,true);g_active=true;CHECK(!graphical_session_take_reconnect());g_active=false;CHECK(!graphical_session_take_reconnect());
 remember_lost_output(11,true);current_output=11;CHECK(!graphical_session_take_reconnect());
 reset();remember_lost_output(11,true);remember_lost_output(22,false);CHECK(!graphical_session_take_reconnect());
 puts("PASS: production GUI loss ticket,11 retained-ledger gates,firmware/valid-output/normal-exit refusal,one-shot consume and changed-context refusal;no native hardware.");return 0;
}
