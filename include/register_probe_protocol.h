#ifndef JCOS_REGISTER_PROBE_PROTOCOL_H
#define JCOS_REGISTER_PROBE_PROTOCOL_H
#include "device_register_abi.h"
#define REGISTER_PROBE_VERSION 1ULL
#define REGISTER_PROBE_HEADER(op) ((REGISTER_PROBE_VERSION << 32) | (op))
#define REGISTER_PROBE_CHECK    REGISTER_PROBE_HEADER(1ULL)
#define REGISTER_PROBE_READ     REGISTER_PROBE_HEADER(2ULL)
#define REGISTER_PROBE_FAULT    REGISTER_PROBE_HEADER(3ULL)
#define REGISTER_PROBE_SHUTDOWN REGISTER_PROBE_HEADER(4ULL)
#define REGISTER_PROBE_SPIN REGISTER_PROBE_HEADER(5ULL)
#define REGISTER_PROBE_CAPTURE REGISTER_PROBE_HEADER(6ULL)
#define REGISTER_PROBE_CAPTURE_DECODED (1ULL << 16)
#define REGISTER_PROBE_OBSERVATION REGISTER_PROBE_HEADER(7ULL)
/* Four-word replies preserve the existing IPC ABI.
 * CAPTURE: header,incarnation,result|decoded-bit|restrictions<<32,GPU address.
 * OBSERVATION request: op,expected resource ID,metadata selector,reserved0.
 * Reply: header,incarnation,selector+1,value (zero tag on refusal).
 * Metadata 0=span,1=width<<32|height,2=pitch,3=depth|format<<8|array<<16. */
#define REGISTER_PROBE_PLANE_WITNESS REGISTER_PROBE_HEADER(8ULL)
#define REGISTER_PROBE_PLANE_STEP REGISTER_PROBE_HEADER(9ULL)
/* WITNESS request: op,observation ID,update|flip<<32,frame|sample<<32.
 * Exactly samples0 then1, once per retained capture; reply tag=sample+1.
 * STEP request: op,observation ID,index,reserved0.
 * Reply header,incarnation,versioned semantic tag,value. Data only: no GPU grant. */
/* Copied-link diagnostic,one BEGIN/DATA stream per fresh probe;no device cap.
 * BEGIN:op,version1,0,0 ->tag1,count52. DATA:op,nextindex,uint32value,0
 * ->tagindex+1,value. QUERY:op,index0..3,version1,0 ->tagindex+1,word.
 * Invalid order/shape closes the process;no reset/replay within an instance. */
#define REGISTER_PROBE_LINK_BEGIN REGISTER_PROBE_HEADER(10ULL)
#define REGISTER_PROBE_LINK_DATA REGISTER_PROBE_HEADER(11ULL)
#define REGISTER_PROBE_LINK_QUERY REGISTER_PROBE_HEADER(12ULL)
/* Copied timing diagnostic:BEGIN version2/count80;DATA ordered0..79;QUERY0..8.
 * Word8 retains first/second rawM;version1 eightword role is rejected.
 * One role per fresh process;no device access/reset/replay. */
#define REGISTER_PROBE_TIMING_BEGIN REGISTER_PROBE_HEADER(13ULL)
#define REGISTER_PROBE_TIMING_DATA REGISTER_PROBE_HEADER(14ULL)
#define REGISTER_PROBE_TIMING_QUERY REGISTER_PROBE_HEADER(15ULL)
/* Copied vertical preparation: fresh role, BEGIN version1/count86,
 * ordered DATA0..85 and QUERY0..17. No GPU write permission. */
#define REGISTER_PROBE_VERTICAL_BEGIN REGISTER_PROBE_HEADER(16ULL)
#define REGISTER_PROBE_VERTICAL_DATA REGISTER_PROBE_HEADER(17ULL)
#define REGISTER_PROBE_VERTICAL_QUERY REGISTER_PROBE_HEADER(18ULL)
/* Fresh stream-observation role:BEGIN version1/count96,orderedDATA0..95,
 * orderedQUERY0..13. No capabilities, MMIO or execution plan. */
#define REGISTER_PROBE_STREAM_BEGIN REGISTER_PROBE_HEADER(19ULL)
#define REGISTER_PROBE_STREAM_DATA REGISTER_PROBE_HEADER(20ULL)
#define REGISTER_PROBE_STREAM_QUERY REGISTER_PROBE_HEADER(21ULL)
#define REGISTER_PROBE_REPLY    0x52454750524F4245ULL
#define REGISTER_PROBE_STOPPED  0x52454753544F5050ULL
#define REGISTER_PROBE_EXPECTED 0xFFULL
#endif
