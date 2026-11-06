#ifndef JCOS_DEVICE_REGISTER_ABI_H
#define JCOS_DEVICE_REGISTER_ABI_H
#include "user_abi.h"

#define JCOS_REGISTER_READ_VERSION 1ULL
/* The physical profile has no constructor in the current implementation.
 * Test resources cannot be promoted or relabelled as physical resources. */
#define JCOS_REGISTER_PROFILE_POLARIS_DCE112       1U
/* Immutable copied observation delivered by a trusted kernel producer.
 * Reads return retained RAM, never live MMIO or hardware authority. */
#define JCOS_REGISTER_PROFILE_POLARIS_DCE112_OBSERVATION 2U
#define JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST  0x80000001U
/* Finite copied-RAM capture: distinct from both physical and unlimited test. */
#define JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST 0x80000002U
static inline JcosU64 jcos_register_read_request(JcosU64 pipe, JcosU64 field) {
    if (pipe > 0xFFFFULL || field > 0xFFFFULL) return 0ULL;
    return (JCOS_REGISTER_READ_VERSION << 32) | (pipe << 16) | field;
}
#define JCOS_REGISTER_READ_REQUEST(pipe, field) jcos_register_read_request((pipe), (field))
/* INT 0x80 call 12: RBX=reader capability, RDX=expected resource identity,
 * RCX=version[63:32]/pipe[31:16]/field[15:0].
 * Returns RAX=OK/FAILED and RCX=value (zero on failure). No user pointers,
 * physical addresses, offsets, widths or writes enter the kernel. */
#endif
