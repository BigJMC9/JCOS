#ifndef JA_OS_CAPABILITY_H
#define JA_OS_CAPABILITY_H

#include "types.h"

#define CAPABILITY_TABLE_CAPACITY 64U
#define CAPABILITY_INVALID_HANDLE 0ULL

typedef u64 CapabilityHandle;
typedef u64 CapabilityRights;

/*
 * Generic rights for the first capability-table
 * implementation.
 *
 * Object-specific rights can be added later when
 * endpoints and other kernel objects exist.
 */
#define CAPABILITY_RIGHT_READ      (1ULL << 0)
#define CAPABILITY_RIGHT_WRITE     (1ULL << 1)
#define CAPABILITY_RIGHT_EXECUTE   (1ULL << 2)
#define CAPABILITY_RIGHT_TRANSFER  (1ULL << 3)
#define CAPABILITY_RIGHT_MANAGE    (1ULL << 4)
#define CAPABILITY_RIGHT_SEND      (1ULL << 5)
#define CAPABILITY_RIGHT_RECEIVE   (1ULL << 6)

#define CAPABILITY_RIGHT_ALL       \
    (CAPABILITY_RIGHT_READ |       \
     CAPABILITY_RIGHT_WRITE |      \
     CAPABILITY_RIGHT_EXECUTE |    \
     CAPABILITY_RIGHT_TRANSFER |   \
     CAPABILITY_RIGHT_MANAGE |     \
     CAPABILITY_RIGHT_SEND |       \
     CAPABILITY_RIGHT_RECEIVE)

typedef enum {
    CAPABILITY_TYPE_NONE = 0,

    CAPABILITY_TYPE_THREAD,
    CAPABILITY_TYPE_ADDRESS_SPACE,
    CAPABILITY_TYPE_ENDPOINT
} CapabilityType;


/*
 * Handles are:
 *
 *   63                    32 31                     0
 *  +------------------------+------------------------+
 *  |       generation       |       slot index       |
 *  +------------------------+------------------------+
 *
 * Generation zero is never issued.
 *
 * Reusing a revoked slot increments its generation,
 * preventing an old userspace handle from naming the
 * new object occupying the same slot.
 */
typedef struct {
    void *object;

    CapabilityRights rights;

    u32 generation;
    CapabilityType type;

    bool occupied;
} CapabilitySlot;

typedef struct {
    CapabilitySlot slots[
        CAPABILITY_TABLE_CAPACITY
    ];

    u32 count;
    bool initialized;
} CapabilityTable;

/*
 * Capability tables do not own their objects.
 *
 * Revoking a capability removes authority to the
 * object; it does not destroy or free the object.
 *
 * This first implementation is not internally
 * synchronized. The caller must serialize access.
 */
bool capability_table_init(CapabilityTable *table);
u32 capability_table_count(const CapabilityTable *table);

bool capability_insert(
    CapabilityTable *table,
    void *object,
    CapabilityType type,
    CapabilityRights rights,
    CapabilityHandle *out_handle
);

/*
 * Resolve a capability while enforcing its type.
 */
bool capability_lookup(
    const CapabilityTable *table,
    CapabilityHandle handle,
    CapabilityType expected_type,
    void **out_object
);

/*
 * Resolve a capability while enforcing both type
 * and the requested rights.
 *
 * Every bit in required_rights must be present.
 */
bool capability_lookup_rights(
    const CapabilityTable *table,
    CapabilityHandle handle,
    CapabilityType expected_type,
    CapabilityRights required_rights,
    void **out_object
);

bool capability_revoke(
    CapabilityTable *table,
    CapabilityHandle handle
);

#endif