#include "lib/syscall.h"
#include "drivers/amd_polaris_plane.h"
#include "drivers/amd_polaris_program.h"
#include "../include/program_startup.h"
#include "../include/polaris_native_protocol.h"
#include "../kernel/font8x8.h"
#ifndef POLARIS_NATIVE_ROLE
#define POLARIS_NATIVE_ROLE POLARIS_NATIVE_ROLE_PRIMARY
#endif
static void message_clear(JcosIpcMessage *m) {
    m->word_count=4; for (JcosU32 i=0;i<4;++i) m->words[i]=0;
}
static void text(volatile JcosU32 *fb,JcosU32 y,const char *s,JcosU32 color) {
    for (JcosU32 n=0;s[n] && n<100;++n) {
        JcosU32 c=(unsigned char)s[n]; if (c>=128) c='?';
        for (JcosU32 row=0;row<8;++row) for (JcosU32 bit=0;bit<8;++bit)
            if (font8x8[c][row]&(1U<<(7U-bit))) fb[(y+row)*1024U+32U+n*8U+bit]=color;
    }
}
static void draw(volatile JcosU32 *fb) {
    JcosU32 bg=POLARIS_NATIVE_ROLE==POLARIS_NATIVE_ROLE_RESCUE ? 0x00182030U : 0x00103018U;
    for (JcosU32 i=0;i<1024U*768U;++i) fb[i]=bg;
    text(fb,40,POLARIS_NATIVE_ROLE==POLARIS_NATIVE_ROLE_RESCUE ?
        "JA OS - FRESH RING3 RESCUE DISPLAY" : "JA OS - RING3 POLARIS SAME-BUFFER REBIND",0x00FFFFFFU);
    text(fb,64,"DIAGNOSTIC OUTPUT - SERIAL CONTROL REMAINS ACTIVE",0x0000FFFFU);
    text(fb,88,"INHERITED DISPLAYPORT TIMING - NO GPU RESET",0x00FFFFFFU);
}
typedef struct {JcosCapabilityHandle command,reply;JcosU64 incarnation;JcosU32 sequence;} BrokerClient;
static int exchange(void *context,JcosU32 operation,JcosU32 field,JcosU32 payload,JcosU32 *value) {
    BrokerClient *c=context;
    *value=0;
    if(c->sequence>=POLARIS_REBIND_MESSAGES)return 0;
    JcosU64 tag=POLARIS_REBIND_TAG(++c->sequence,operation,field);
    JcosIpcMessage q,r;message_clear(&q);message_clear(&r);
    q.words[0]=POLARIS_REBIND_REQUEST;q.words[1]=c->incarnation;q.words[2]=tag;q.words[3]=payload;
    if(!jcos_ipc_send_blocking(c->reply,&q) || !jcos_ipc_receive_blocking(c->command,&r) ||
       r.word_count!=4 || r.words[0]!=POLARIS_REBIND_RESPONSE || r.words[1]!=c->incarnation ||
       r.words[2]!=tag || r.words[3]>0xFFFFFFFFULL ||
       (operation!=POLARIS_REBIND_READ && r.words[3]))return 0;
    *value=(JcosU32)r.words[3];return 1;
}
__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *s) {
    if (!s || s->magic!=JCOS_PROGRAM_STARTUP_MAGIC || s->version!=1 || s->size!=sizeof(*s) ||
        s->flags || s->environment_count || s->capability_count!=2 || s->argument_count!=3 ||
        !s->arguments[0] || s->arguments[1]!=POLARIS_NATIVE_FB_VA ||
        s->arguments[2]!=0xF400000000ULL || !s->capabilities[0] || !s->capabilities[1]) jcos_thread_exit();
    PolarisDisplayObservation observation;
    observation.surface_gpu_address=s->arguments[2]; observation.linear_span_bytes=0x300000;
    observation.surface_width=1024; observation.surface_height=768; observation.pitch_pixels=1024;
    observation.depth=2; observation.format=0; observation.array_mode=1; observation.restrictions=0;
    PolarisPlaneWitness witness; JcosU64 identity=0; JcosU32 next_step=0; int drawn=0,attempted=0,programmed=0;
    for (;;) {
        JcosIpcMessage q,r; message_clear(&q); message_clear(&r);
        if (!jcos_ipc_receive_blocking(s->capabilities[0],&q)) jcos_thread_exit();
        r.words[0]=POLARIS_NATIVE_REPLY; r.words[1]=s->arguments[0];
        if (q.word_count!=4 || (q.words[0]>>32)!=POLARIS_NATIVE_VERSION) jcos_thread_exit();
        JcosU32 op=(JcosU32)q.words[0];
        if (op==POLARIS_NATIVE_PING && !q.words[2] && !q.words[3]) {
            r.words[2]=POLARIS_NATIVE_ROLE;r.words[3]=q.words[1];
        } else if (op==POLARIS_NATIVE_CONFIG && !identity && q.words[1] && !drawn) {
            identity=q.words[1];
            witness.values[0][0]=witness.values[1][0]=0;
            witness.values[0][1]=witness.values[1][1]=(JcosU32)q.words[3];
            witness.values[0][2]=(JcosU32)q.words[2]; witness.values[1][2]=(JcosU32)(q.words[2]>>32);
            if (q.words[3]>0xFFFFFFFFULL) identity=0;
            JcosU64 tag; JcosU32 value;
            if (!polaris_plane_recipe_step(&observation,&witness,0,0,&tag,&value)) identity=0;
            r.words[2]=identity;
        } else if (op==POLARIS_NATIVE_STEP && identity && q.words[1]==identity && q.words[2]==next_step && next_step<5 && !attempted && !q.words[3]) {
            JcosU64 tag; JcosU32 value;
            if (polaris_plane_recipe_step(&observation,&witness,0,(JcosU32)q.words[2],&tag,&value)) {
                r.words[2]=tag; r.words[3]=value;++next_step;
            }
        } else if (op==POLARIS_NATIVE_EXECUTE && identity && q.words[1]==identity &&
                   next_step==5 && !attempted && !drawn && !q.words[2] && !q.words[3]) {
            attempted=1;
            BrokerClient client={s->capabilities[0],s->capabilities[1],s->arguments[0],0};
            PolarisUserProgramIo io={&client,exchange};JcosU32 delta=0;
            programmed=polaris_user_program(&io,witness.values[0][POLARIS_WITNESS_FLIP_CONTROL],&delta);
            if(!programmed)jcos_thread_exit();
            r.words[2]=POLARIS_NATIVE_EXECUTE;r.words[3]=delta;
        } else if (op==POLARIS_NATIVE_DRAW && identity && programmed && !drawn && q.words[1]==POLARIS_NATIVE_ROLE && !q.words[2] && !q.words[3]) {
            draw((volatile JcosU32 *)s->arguments[1]); drawn=1; r.words[2]=POLARIS_NATIVE_ROLE;
        } else if ((op==POLARIS_NATIVE_TEST_FAULT || op==POLARIS_NATIVE_TEST_SPIN) &&
                   q.words[1]==POLARIS_NATIVE_ROLE && !q.words[2] && !q.words[3]) {
            /* Acknowledge this exact branch before leaving the command loop.
             * No second reply or new ordinary request can follow the ACK. */
            r.words[2]=op;r.words[3]=POLARIS_NATIVE_ROLE;
            if (!jcos_ipc_send_blocking(s->capabilities[1],&r)) jcos_thread_exit();
            if (op==POLARIS_NATIVE_TEST_FAULT) {__asm__ volatile ("ud2");__builtin_unreachable();}
            for (;;) __asm__ volatile ("pause" : : : "memory");
        } else if (op==POLARIS_NATIVE_FAULT && POLARIS_NATIVE_ROLE==POLARIS_NATIVE_ROLE_PRIMARY) {
            __asm__ volatile ("ud2"); __builtin_unreachable();
        } else if (op==POLARIS_NATIVE_SPIN && POLARIS_NATIVE_ROLE==POLARIS_NATIVE_ROLE_PRIMARY) {
            for (;;) __asm__ volatile ("pause");
        } else if (op==POLARIS_NATIVE_STOP && !q.words[1] && !q.words[2] && !q.words[3]) {
            if (!jcos_ipc_send_blocking(s->capabilities[1],&r)) jcos_thread_exit();
            jcos_thread_exit();
        }
        if (!jcos_ipc_send_blocking(s->capabilities[1],&r)) jcos_thread_exit();
    }
}
