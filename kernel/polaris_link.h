#ifndef JAOS_POLARIS_LINK_H
#define JAOS_POLARIS_LINK_H
#include "types.h"
#include "polaris_link_protocol.h"
#include "polaris_timing_protocol.h"
#include "polaris_vertical_protocol.h"
bool polaris_link_decode_copy(const u32 values[POLARIS_LINK_VALUES],u64 report[POLARIS_LINK_WORDS]);
bool polaris_timing_decode_copy(const u32 values[POLARIS_TIMING_VALUES],u64 report[POLARIS_TIMING_WORDS]);
bool polaris_vertical_decode_copy(const u32 values[POLARIS_VERTICAL_VALUES],u64 report[POLARIS_VERTICAL_WORDS]);
void polaris_vertical_capture_run(const BootInfo *boot);
void polaris_timing_capture_run(const BootInfo *boot);
void polaris_link_capture_run(const BootInfo *boot);
#endif