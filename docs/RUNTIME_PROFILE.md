# JCOS runtime profile

This reference describes the restricted runtime present at the
[2026-11-05 source baseline](https://github.com/BigJMC9/JCOS/commit/fcfe857140891d7c25a44814e1738970d72a852a).
The relevant implementation is in `kernel/scheduler.c`, `kernel/thread.c`,
`kernel/interrupts.c`, `kernel/execution_profile.c`, `kernel/user_elf.c`, and the
shared headers under `include/`.

## Supported execution model

JCOS currently supports **one logical CPU**, x86-64 four-level paging, protected
Ring3 processes, and freestanding static ELF64 `ET_EXEC` programs. Normal timer
preemption is enabled after the PIT is initialized at 100 Hz. A non-yielding
Ring3 program need not cooperate for another READY thread to receive execution.
This is not an SMP synchronization contract or a hard-real-time latency guarantee.

Threads use full interrupt-frame switching and guarded kernel stacks. Each
ordinary owned kernel stack has 16 KiB of usable storage with unmapped guard
pages. A scheduler-private guarded idle context handles the absence of ordinary
runnable threads and is not counted as an ordinary Thread object.

Blocking IPC retains the kernel call stack. A nested scheduler interruption may
suspend C code inside a syscall; waking resumes that call chain before the outer
syscall handler completes the userspace result. Code must not interpret an
arbitrary saved inner frame as a direct user-return frame.

## Waits, interruption and scheduling

- The PIT provides the current tick/frequency basis; a tick is not a wall-clock
  timestamp or a precision performance measurement.
- IPC, supervisory operations, and service shutdown have bounded wait paths.
  A service request must tolerate scheduling between publication and reply.
- The last-runnable-thread wait uses `STI; HLT; CLI`, rechecks its predicate, and
  restores the caller's interrupt-state contract.
- IRQ-save sections restore the incoming IF state. Cleanup must not enable
  interrupts unconditionally.
- Current object/memory metadata serialization uses single-CPU interrupt masking.
  It is neither a multicore lock design nor an NMI-safe synchronization model.
- A READY thread may still retain a suspended kernel continuation and object
  references. Removing it from a run queue is not sufficient permission to free
  its stack.

## Integer-only application profile

Ordinary programs must not use x87, MMX, SSE, AVX, or later FP/vector state. The
kernel does not save and restore extended processor state per Thread.

The current build and CPU enforcement work together:

1. Kernel and userspace C builds use `-mgeneral-regs-only`, `-mno-sse`, and
   `-mno-sse2`, with no red zone.
2. The kernel keeps `CR0.MP=1`, `CR0.EM=0`, and `CR0.TS=1`. Instructions covered by
   that gate trap with `#NM`; other unavailable instructions may fault under the
   active architecture configuration. No ordinary application FP/vector profile
   is advertised.
3. Supported Ring3 fault paths enter the process fault-containment machinery.
   A privileged FP/vector fault remains fatal instead of silently sharing
   unsaved state between threads.

The ELF loader also rejects `PT_TLS`. General per-thread TLS, dynamic linking,
and hosted libc/C++ compatibility are not provided by the existing C runtime.
Supporting extended state requires explicit feature discovery, initialized
per-thread ownership, restoration on ownership transfer, ABI/build changes, and
isolation tests; changing compiler flags alone is insufficient.

## Entry and syscall contracts

The register syscall transport uses `INT 0x80`. `include/user_abi.h` is the
numeric source of truth: the present calls occupy 0 through 12 and cover thread
identity/exit, nonblocking and blocking IPC, clock ticks/frequency, legacy
display/audio helpers, and a versioned capability-checked semantic register read.
The register-read mechanism currently supports kernel-owned diagnostic banks;
it has no physical-device constructor. Do not replace this transport with `SYSCALL/SYSRET`
without a separate entry/return ABI and validation design.

IPC transports at most four words in registers. `JcosIpcMessage` is a C wrapper
layout, not a general unchecked user-pointer syscall contract. Separate media
helpers have their own checked-copy behavior; their existence does not turn IPC
into a pointer-based ABI.

Ordinary programs use `JcosProgramStartup` in `include/program_startup.h`:
version 1, 128 bytes, up to eight initial capability handles and four argument
words. The current ordinary startup block has no populated general environment
array. The supervisor retains a small legacy entry contract. Do not use the
supervisor's two-handle bootstrap convention to describe every program.

## Fault and resource ownership

Supported unhandled userspace faults terminate the affected process domain and
its sibling threads. The current scheduler-managed Ring3 set is `#DE`, `#UD`,
`#NM`, `#GP`, and `#PF`; it is not a blanket NMI, double-fault or machine-check
containment claim. A normal thread exit remains distinct and may leave siblings
alive. The kernel fault path is a separate fatal boundary.

Termination first prevents further execution and authority use; reaping then
follows the relevant object's resource ledger. Stack/image/table cleanup can
fail and remains retryable. Device buffers can remain quarantined after a
driver has stopped: CPU termination or unmapping does not prove that a device
has ceased accessing memory.

See [memory ownership](memory.md) for page/stack ownership and
[service boundaries](SERVICE_BOUNDARY.md) for service incarnations, authority,
recovery and device restrictions. [Testing](TESTING.md) describes how to record
observed results; the presence of a diagnostic in source is not a new test run.
