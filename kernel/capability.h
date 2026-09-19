#ifndef JA_OS_CAPABILITY_H
#define JA_OS_CAPABILITY_H

#include "types.h"

#define CAPABILITY_TABLE_CAPACITY 64U
#define CAPABILITY_INVALID_HANDLE 0ULL
#define CAPABILITY_TABLE_STORAGE_CAPACITY 256U
#define CAPABILITY_GENERATION_MAX 0xFFFFFFFFULL

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
 * Each slot INDEX has one boot-global nonzero generation issuer.
 * Slot/table/object reuse never repeats an issued handle in this boot.
 * Each index permits 2^32-1 grants across all tables per boot. Exhausted
 * indices are skipped; insertion fails when no eligible free index remains.
 * Issuers NEVER wrap or reset on table initialization/destruction.
 */
typedef struct {
    void *object;
    u64 object_id;

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
 * Every occupied slot pins canonical object storage. Deleting a slot
 * releases that pin; it does not close, stop, or destroy the object.
 * revoke/revoke_all are slot deletion, NOT recursive authority revocation.
 * Tables must remain at stable addresses until table_destroy succeeds.
 * Init rejects live tables (even empty); destroy requires an empty table.
 * Operations use local IRQ exclusion: UP only, not NMI/SMP synchronization.
 * Lookup returns a BORROW. Callers must keep exclusion/other ownership
 * through use, or publish a supported wait reservation before blocking.
 */
bool capability_table_init(CapabilityTable *table);
u32 capability_table_count(const CapabilityTable *table);
bool capability_table_empty(const CapabilityTable *table);
bool capability_table_destroy(CapabilityTable *table);
bool capability_table_storage_in_use(const CapabilityTable *table);
u32 capability_table_object_count(void);

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

bool capability_revoke_all(CapabilityTable *table);

/* Kernel lifecycle helper: delete only matching slots in this one table. */
bool capability_revoke_object(CapabilityTable *table, const void *object, CapabilityType type);

#endif