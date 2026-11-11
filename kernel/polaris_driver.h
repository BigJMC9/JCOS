#ifndef JA_OS_POLARIS_DRIVER_H
#define JA_OS_POLARIS_DRIVER_H
#include "managed_service.h"
#include "polaris_program.h"
#include "polaris_vtotal_gate.h"
#include "polaris_native_protocol.h"
typedef struct {
    ManagedService service;
    ManagedServiceConnection connection;
    u32 role;
    /* Retained launch identity, including after failure/reap. The managed
     * service's public PID accessor intentionally exposes running state only. */
    u64 process_id;
    bool program_attempted;
    bool retained_only;
    bool vtotal_attempted;
} PolarisDriver;
/* Zero-initialized retained storage. Preload independent ELF/font/IPC/pages,
 * with a RO borrowed framebuffer; no allocation is needed to enable drawing. */
bool polaris_driver_reserve(PolarisDriver *driver,u32 role,const ProgramBorrowedMappingSpec *readonly_mapping);
/* Consume before RPC;enforce the complete borrowed range RO before adoption. */
bool polaris_driver_attach_retained(PolarisDriver *driver,u64 output_identity);
bool polaris_driver_plan(PolarisDriver *driver,u64 observation_id,const PolarisPlaneWitness *witness,PolarisProgramPlan *out);
/* One finite Ring3-driven transaction on the existing private endpoints.
 * Kernel validates every operation; completion must match its own gate.
 * Ambiguous delivery/completion consumes the attempt; never reconnect/replay. */
bool polaris_driver_program(PolarisDriver *driver,PolarisProgramPlan *plan,
    const PolarisProgramIo *io,PolarisProgramResult *out);
bool polaris_driver_vtotal(PolarisDriver *driver,const PolarisVtotalIo *io,
    u32 diagnostic_fault_prefix,PolarisVtotalResult *out);
bool polaris_driver_draw(PolarisDriver *driver);
bool polaris_driver_ping(PolarisDriver *driver,u64 challenge);
/* Private diagnostics: consume exactly one acknowledged fault/spin branch;
 * unlike normal RPC, the target intentionally never reblocks afterward. */
bool polaris_driver_diagnostic(PolarisDriver *driver,bool spin);
typedef struct {
    u64 console_process_id,console_incarnation,writes_before,writes_after;
} PolarisDriverProgress;
/* Measures completed work in the existing independent Ring3 console. For
 * spin, require the original target still live and outside its receive wait. */
bool polaris_driver_console_progress(PolarisDriver *driver,bool require_busy,PolarisDriverProgress *out);
bool polaris_driver_stop(PolarisDriver *driver);
#endif
