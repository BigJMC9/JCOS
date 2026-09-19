#ifndef JA_OS_USER_STACK_H
#define JA_OS_USER_STACK_H

#include "address_space.h"
#include "pmm.h"

#define USER_STACK_INITIAL_BASE (ADDRESS_SPACE_USER_BASE + 0x100000ULL)

/*
 * Initial user-stack profile: one writable/NX page with a deliberately absent
 * page immediately below and above it. Adjacent stacks may share one guard
 * page, but no mapped object may occupy either guard.
 */
bool user_stack_initial_reservation_conflicts(u64 page_first, u64 page_end);
bool user_stack_slot_available(const AddressSpace *space, u64 stack_base);
bool user_stack_map_page(AddressSpace *space, u64 stack_base, frame_t frame);
bool user_stack_mapping_valid(const AddressSpace *space, u64 stack_base, frame_t frame);

#endif
