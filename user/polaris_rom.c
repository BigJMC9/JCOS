#include "lib/syscall.h"
#include "../include/program_startup.h"
#include "drivers/amd_polaris_rom.h"
__attribute__((noreturn)) void jcos_main(const JcosProgramStartup *s){
 if(!s || s->magic!=JCOS_PROGRAM_STARTUP_MAGIC || s->version!=1 || s->size!=sizeof(*s) || s->flags ||
    s->environment_count || s->capability_count!=2 || s->argument_count!=4 || !s->arguments[0] ||
    s->arguments[1]!=POLARIS_ROM_VA || s->arguments[2]>POLARIS_ROM_LIMIT || !s->arguments[2] ||
    s->arguments[3]!=0x67DF1002ULL || !s->capabilities[0] || !s->capabilities[1])jcos_thread_exit();
 PolarisRomInfo info;polaris_rom_parse((const unsigned char *)s->arguments[1],(JcosU32)s->arguments[2],(JcosU32)s->arguments[3],&info);
 for(;;){
  JcosIpcMessage q={0},r={0};if(!jcos_ipc_receive_blocking(s->capabilities[0],&q))jcos_thread_exit();
  if(q.word_count!=4 || q.words[2] || q.words[3])jcos_thread_exit();
  r.word_count=4;r.words[0]=POLARIS_ROM_REPLY;r.words[1]=s->arguments[0];r.words[2]=q.words[1];
  if(q.words[0]==POLARIS_ROM_HEADER(POLARIS_ROM_STOP) && !q.words[1]){
   (void)jcos_ipc_send_blocking(s->capabilities[1],&r);jcos_thread_exit();
  }
  if(q.words[0]!=POLARIS_ROM_HEADER(POLARIS_ROM_QUERY) || q.words[1]>=POLARIS_ROM_QUERIES)jcos_thread_exit();
  r.words[3]=polaris_rom_query(&info,(JcosU32)q.words[1]);
  if(!jcos_ipc_send_blocking(s->capabilities[1],&r))jcos_thread_exit();
 }
}
