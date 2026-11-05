# JCOS privileged mechanisms and userspace services

This document describes the current authority and lifetime boundaries. Build/run instructions are in the [project README](../README.md); execution constraints are in the [runtime profile](RUNTIME_PROFILE.md). Shared headers and the implementation remain authoritative for exact protocol values.

## Responsibility map

| Component | Runs in | Current responsibility |
| --- | --- | --- |
| Address spaces, scheduler, capabilities, IPC and process outcomes | Kernel | Protection, bounded communication and object lifetime |
| Console policy service | Ring 3 | Normal shell/editor, formatting, archive namespace, executable selection and ordinary-service policy |
| Console and boot-archive portals | Kernel | Bounded terminal operations and raw reads from the immutable archive |
| Program and ordinary-service brokers | Kernel | Validate byte extents and perform generic launch, bounded stop and reap transactions |
| Display service | Ring 3 | Render into the validated GOP framebuffer while holding the exclusive direct lease |
| Compositor service | Ring 3 | Surface placement, z-order, focus and composition requests |
| Graphical client | Ring 3 | Write its own bounded surface and receive focused input |
| PCI resource/recovery framework | Kernel | Enforce assignment, BAR/IRQ/DMA authority, access lifetimes and quarantine |
| Constrained PCI driver probe | Ring 3 | Exercise a narrow mediated request protocol through a privileged test backend |
| AHCI, xHCI, audio, hardware input and emergency monitor | Kernel | Existing device/diagnostic paths; these have not become general userspace services |

Bootstrap executable lookup, managed-service orchestration, console/input routing and graphical-session coordination also remain privileged. A generic broker accepting an archive extent does not imply that every orchestration decision has moved out of the kernel.

## Authority and lifetime

Capability handles identify authority within one capability table. Equal numeric handles in different processes do not imply access to the same object. Launch grants are explicitly attenuated: SEND, RECEIVE, READ, WRITE and MANAGE are separate rights where the object type supports them.

Capability deletion, object close, object destruction and service replacement are different operations. Endpoint/object references held by waiters or suspended kernel continuations must remain valid until those users are detached. A woken thread can still retain references before its continuation resumes. Cleanup retains ownership records when release cannot complete, allowing a defined retry or quarantine rather than losing the resource ledger.

Service incarnation identifies a particular service lifetime; it is distinct from capability slot generation and process identity. Replacing a service requires fresh authority and an explicit reconnect. A saved connection is not silently retargeted to a new process.

The common program startup ABI is defined in [program_startup.h](../include/program_startup.h). Its current envelope contains at most eight capability handles and four scalar arguments. It does not grant ambient access to hardware, a filesystem namespace or another service's management channel.

## IPC and protocol versions

The syscall IPC transport carries at most four 64-bit words in registers. The current endpoint profile has one mailbox message, one blocked sender and one blocked receiver. A C wrapper structure does not make the transport a general userspace-pointer ABI. See [user_abi.h](../include/user_abi.h), [endpoint.h](../kernel/endpoint.h) and [ipc.c](../kernel/ipc.c).

[service_protocol.h](../include/service_protocol.h) defines the common service convention:

| Word | Request | Reply |
| --- | --- | --- |
| 0 | Operation | Service result or common error |
| 1 | Protocol version | Service incarnation |
| 2 | Payload | Supported protocol version |
| 3 | Payload | Optional result/error detail |

Protocols using this convention require an exact version match. Incompatible versions, malformed requests and unknown operations have explicit error results. They do not require a service crash.

Some interfaces pack operation/version/count into a header word to fit their bounded payload. Console streams, raw archive reads, brokers and graphics use their own shared header definitions. Do not assume that the common table describes every message layout or that a public OS release number replaces an interface version. See [Versioning](VERSIONING.md).

### Completion and replay

Successful send/acceptance does not prove that the receiver consumed a request, that an operation completed or that data became durable. Cancellation, close, timeout and process death must preserve the actual commit point.

No broker or service-replacement path implicitly replays an ambiguous application request. In particular:

- A failed console write is not replayed through the emergency terminal sink.
- An input event that triggered failure is not delivered again to a replacement shell.
- A queued launch/service request is not inherited by a new policy-service incarnation.
- A stale reply is discarded according to its transport/session/incarnation contract.

A caller can make an explicit new request after reconnecting, subject to that operation's own semantics.

## Console, input and normal shell

The persistent Ring 3 console service is the normal boot command surface. It owns history, the command buffer, selection/clipboard state, editing, formatting and prompt policy. Hardware polling remains privileged, and key codes/modifiers cross the boundary as bounded public events rather than kernel enum layout or device-register access.

The [ConsolePortal](../kernel/console_portal.c) supplies byte output, cursor-left, accent-byte output and page/line/bottom scrollback operations. It does not parse commands or manage the shell's history. Its authority is granted to the console policy service; ordinary applications use separate endpoints.

| Foreground channel | Application authority | Contract |
| --- | --- | --- |
| Output | SEND only | [console_client_protocol.h](../include/console_client_protocol.h): WRITE carries a session ID and at most 16 bytes; END settles the output session |
| Input | RECEIVE only | [console_input_protocol.h](../include/console_input_protocol.h): versioned key events scoped to the active session |

The foreground policy is single-client. Session IDs are nonzero and explicit. Stale input is drained/discarded, and output/input capabilities cannot be used to inject console-management commands. Temporary TRANSFER authorities exist only across the launch transaction and are revoked after publication.

If a foreground client dies without END, its owner must terminate/reap it and abort the session. A replacement console does not silently adopt that client. Console replacement recreates application-facing and policy-broker endpoints while retaining the lower-level byte/archive portals where their lifetime permits it.

`monitor` returns to the privileged diagnostic surface. Escape is also retained as a kernel-owned emergency route. Boot activation or shell recovery failure returns to that surface with diagnostics, without replaying the failed application payload. `usershell` begins a fresh shell session over the current console incarnation.

## Archive namespace and launch policy

The [BootArchivePortal](../kernel/boot_archive_portal.c) exposes INFO and bounded READ-by-offset over the immutable bytes supplied by the loader. Each read returns at most eight payload bytes. The portal has no pathname, directory, executable-name or filesystem-mutation operation.

[user/lib/boot_archive.c](../user/lib/boot_archive.c) parses USTAR headers, validates sizes/checksums and bounds, rejects parent-path traversal, and implements the normal shell's read-only namespace. `ls`, `stat`, `cat` and `fscheck` use this Ring 3 path.

For `run PATH`, Ring 3 resolves the executable and submits its immutable archive offset and exact size through [program_broker_protocol.h](../include/program_broker_protocol.h). The kernel validates that extent and gives a temporary pathname-free byte view to the ELF loader. Validation, protected mapping, capability grants, process publication, exit and reclaim remain privileged mechanisms.

Foreground launch opens an I/O session and routes input to the launched client until process exit and session settlement. Escape cancels through the privileged broker. The media extension supplies bounded executable/media extents and uses the separate legacy presentation/audio path described below.

The kernel VFS remains available to bootstrap paths and the emergency monitor. The Ring 3 archive namespace is not a writable filesystem or a general storage-service implementation.

## Ordinary-service policy and recovery

The Ring 3 shell maps the demonstration name `worker` to an independently linked ELF and chooses start, stop, restart or replacement. It rejects unknown names locally. Its supported command form is:

```text
service start|stop|restart|replace|status|fault worker
```

`fault` is a deliberate diagnostic operation. `replace` selects the second compatible ELF; subsequent explicit restarts retain that Ring 3 selection.

[service_broker_protocol.h](../include/service_broker_protocol.h) carries the policy-service incarnation and raw archive extent for START/RESTART. The privileged [background_service](../kernel/background_service.c) component is a bounded single-slot mechanism. It validates the extent and uses `ManagedService` for launch, bounded stop, exit observation and reap. It receives neither the `worker` name nor the chosen pathname.

Managed-service recovery preserves terminal outcome records, detaches old transport state, reclaims owned resources when safe, and publishes a fresh incarnation. A failed ordinary service is retained until an explicit stop/restart decision. Clients reconnect with new authority; ambiguous requests are not replayed. The unrelated supervisor and raw portals have independent lifetimes.

The console policy service currently uses all eight startup capability slots. The fixed startup envelope is an actual bound, not an expandable service directory. Shutdown coordinates the background service, console service and supervisor before checking the kernel-only baseline.

## Display and compositor separation

[display.c](../kernel/display.c) validates the GOP resource and performs an exclusive kernel-to-userspace lease transition. It suppresses kernel framebuffer rendering before the direct mapping becomes runnable and restores the terminal only after supervised teardown. Serial diagnostics remain independent of framebuffer ownership.

The three graphical roles have separate authority:

| Role | Direct access | Policy / requests |
| --- | --- | --- |
| Graphical application | Its own surface mapped RW/NX; dedicated input RECEIVE endpoint | Produce pixels and handle focused events |
| Compositor service | Bounded command/reply authority to the display service; no GOP mapping | Placement, z-order, focus and composition order |
| Display service | Exclusive validated GOP mapping; shared surfaces mapped RO/NX | Convert/render pixels and execute bounded presentation operations |

The shared profile in [display_service_protocol.h](../include/display_service_protocol.h) has four 4 KiB slots, each 64 × 64 RGB332. The compositor protocol is defined separately in [compositor_service_protocol.h](../include/compositor_service_protocol.h). The display service supports the supplied GOP pixel-format metadata, including channel masks for PixelBitMask conversion. This does not provide native mode setting or GPU acceleration.

A compositor failure can be handled without killing the graphical client or relinquishing the display service's GOP lease. The acceptance paths explicitly replace/reconnect the compositor and restore its bounded configuration. A display-service failure retains its lease for supervised cleanup; process death alone does not transfer framebuffer ownership back to the kernel.

The interactive `gui` path currently runs one client. Its normal loop exits and cleans up if a component unexpectedly stops; it does not automatically perform every replacement scenario exercised by the tests. Escape remains kernel-owned and restores terminal ownership after successful cleanup.

The older capability-checked `DISPLAY_INFO`/`DISPLAY_PRESENT` and `AUDIO_INFO`/`AUDIO_WRITE` syscalls remain for the bounded media path. They perform kernel-mediated access, including checked reads of user buffers. Legacy presentation is blocked while the direct display lease is held. These paths are separate from the shared-surface compositor contract and remain transitional privileged functionality.

## Device authority and recovery

The current PCI framework consists primarily of kernel resource APIs. A constrained Ring 3 probe uses a dedicated IPC mediator; there is no general public PCI syscall suite for arbitrary driver deployment.

| Resource | Enforced boundary | Important limit |
| --- | --- | --- |
| Assignment | One owner process and live non-transferable MANAGE capability; subordinate resources pin the assignment | Claiming a function does not enable decode, bus mastering, BAR access or interrupts |
| BAR | Enumerated identity, verified range, explicit rights and access pins | MMIO mappings must be page-exact; PIO remains mediated; no arbitrary physical-address ABI |
| IRQ | Per-assignment event authority, sequence acknowledgement, masking and storm containment | Hardware routing uses firmware-programmed legacy INTx through IOAPIC; no MSI/MSI-X or general ACPI `_PRT` discovery |
| DMA buffer/submission | Bounded owned storage, offset/length/direction checks, active-access pins and quarantine | Submission requires the Ring 3 buffer mapping removed and excludes user BAR/PIO access; it does not enable bus mastering |
| Recovery | Stops new authority, removes CPU access, masks IRQ sources and revokes access while retaining assignment ownership and unsafe resources | Reuse requires evidence that device access has ended; clearing a mapping or killing the process is insufficient |

A kernel mediator alone receives the physical DMA span. Bus-master exposure is tracked for the assignment lifetime; merely clearing the PCI command bit later does not erase that exposure. Recovery after exposure requires an adequate reset/quiescence proof before DMA buffers and assignment identity can be reused.

The PCIe Function Level Reset implementation validates capability-list traversal and operates only on an already quarantined, CPU-quiesced function with decoding, bus mastering and INTx disabled. Unsupported or failed reset leaves the relevant quarantine intact. The presence of this implementation is not a qualification of any particular device's reset behavior.

The automated userspace probe uses a virtual device backend: an enumerated PCI function supplies identity, while CPU-mediated buffer operations and software IRQ signalling exercise request validation, fault/hang containment and lifetime recovery. No physical bus mastering is enabled for those scenarios. Read-only discovery and optional recovery of an already-inert function have separate coverage. See [Testing](TESTING.md).

There is no implemented general IOMMU protection path. The current contract therefore supports constrained/mediated drivers; arbitrary untrusted physical bus mastering is outside the supported isolation model. Device DMA, CPU mappings, IRQ delivery and process lifetime are separate obligations.

## Verification and source map

Tests cover launch/rollback, IPC terminal ordering, capability lifetime, service replacement, console sessions, graphics faults and PCI resource recovery. An acceptance-test implementation is not an execution record; retain the actual build, platform and logs using [Testing](TESTING.md).

| Contract | Primary implementation / definitions |
| --- | --- |
| Launch and service lifetime | [program.c](../kernel/program.c), [managed_service.c](../kernel/managed_service.c), [process_exit_queue.c](../kernel/process_exit_queue.c) |
| Console and policy brokers | [system_console.c](../kernel/system_console.c), [console_service.c](../user/console_service.c), [foreground_program.c](../kernel/foreground_program.c), [background_service.c](../kernel/background_service.c) |
| Graphics | [display_service.c](../user/display_service.c), [compositor_service.c](../user/compositor_service.c), [graphical_session.c](../kernel/graphical_session.c) |
| PCI access and recovery | [pci_assignment.h](../kernel/pci_assignment.h), [pci_bar_access.h](../kernel/pci_bar_access.h), [pci_irq_router.h](../kernel/pci_irq_router.h), [pci_dma_submission.h](../kernel/pci_dma_submission.h), [pci_device_recovery.h](../kernel/pci_device_recovery.h), [pci_flr.h](../kernel/pci_flr.h) |
