#ifndef JCOS_POLARIS_NATIVE_PROTOCOL_H
#define JCOS_POLARIS_NATIVE_PROTOCOL_H
#include "polaris_plane_protocol.h"
#define POLARIS_NATIVE_VERSION 3ULL
#define POLARIS_NATIVE_HEADER(op) ((POLARIS_NATIVE_VERSION<<32)|(op))
#define POLARIS_NATIVE_PING 1ULL
#define POLARIS_NATIVE_CONFIG 2ULL
#define POLARIS_NATIVE_STEP 3ULL
#define POLARIS_NATIVE_DRAW 4ULL
#define POLARIS_NATIVE_FAULT 5ULL
#define POLARIS_NATIVE_SPIN 6ULL
#define POLARIS_NATIVE_STOP 7ULL
#define POLARIS_NATIVE_TEST_FAULT 8ULL
#define POLARIS_NATIVE_TEST_SPIN 9ULL
#define POLARIS_NATIVE_EXECUTE 10ULL
/* Fresh primary adopts validated retained scanout with RO framebuffer only. */
#define POLARIS_NATIVE_RETAINED 11ULL
#define POLARIS_NATIVE_REPLY 0x504E4154ULL
#define POLARIS_NATIVE_ROLE_PRIMARY 1U
#define POLARIS_NATIVE_ROLE_RESCUE 2U
#define POLARIS_NATIVE_FB_VA 0x0000023000000000ULL
/* Four scalar words; startup arg0=incarnation,arg1=borrowed framebuffer,
 * arg2=pinned GPU address. No BAR, PCI, DMA, port or reset authority.
 * CONFIG: header,observationID,beforeFrame|(afterFrame<<32),flipControl.
 * STEP: header,observationID,index,reserved0 -> header,incarnation,tag,value.
 * PING: header,challenge,reserved0,reserved0 -> header,incarnation,role,challenge.
 * EXECUTE: header,observationID,0,0 -> finite reverse broker conversation.
 * DRAW: header,role,reserved0,reserved0; only after independent completion and exclusive RW lease.
 * TEST_FAULT/TEST_SPIN: header,role,0,0 -> header,incarnation,op,role,
 * then fault/spin without returning to receive. Private diagnostic ACK only.
 * FAULT/SPIN deliberate diagnostic operations; STOP returns then exits. */
#endif
