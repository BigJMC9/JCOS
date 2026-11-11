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

The shared profile in [display_service_protocol.h](../include/display_service_protocol.h) has four 4 KiB slots, each 64 Ã— 64 RGB332. The compositor protocol is defined separately in [compositor_service_protocol.h](../include/compositor_service_protocol.h). The display service supports the supplied GOP pixel-format metadata, including channel masks for PixelBitMask conversion. This does not provide native mode setting or GPU acceleration.

A compositor failure can be handled without killing the graphical client or relinquishing the display service's GOP lease. The acceptance paths explicitly replace/reconnect the compositor and restore its bounded configuration. A display-service failure retains its lease for supervised cleanup; process death alone does not transfer framebuffer ownership back to the kernel.

The interactive `gui` path currently runs one client. Its normal loop exits and cleans up if a component unexpectedly stops; it does not automatically perform every replacement scenario exercised by the tests. Escape remains kernel-owned and restores terminal ownership after successful cleanup.

The older capability-checked `DISPLAY_INFO`/`DISPLAY_PRESENT` and `AUDIO_INFO`/`AUDIO_WRITE` syscalls remain for the bounded media path. They perform kernel-mediated access, including checked reads of user buffers. Legacy presentation is blocked while the direct display lease is held. These paths are separate from the shared-surface compositor contract and remain transitional privileged functionality.

## Device authority and recovery

A versioned semantic register-read syscall now checks a dedicated READ-only,
owner-bound resource and exact resource identity. It reads protected RAM,
including immutable snapshots copied after a separate physical experiment;
no syscall performs live MMIO and no live per-register resource can be constructed.
The [Polaris reference](AMD_POLARIS_DISPLAY.md) defines its enforced interface
and the remaining physical binding requirements.

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

The privileged Polaris BAR5 preparation lifecycle can stage four supervisor-only
RO/NX/UC pages from the exact complete firmware extent while retaining assignment
lifetime/access pins, with a separately guarded fifth page for two location
fields. It has no userspace constructor or live physical reader grant. The
default-off monitor experiment uses kernel-only semantic loads with exact holds
and enabled interrupts, then closes/unmaps before dispatching a copied snapshot
to Ring3. Normal vendor modesetting policy is not implemented by this controller. Failed cleanup retains its ledger; inherited bus
mastering sets sticky exposure history and prevents ordinary release.
See [the Polaris backend](AMD_POLARIS_DISPLAY.md) for preconditions and the
distinction between page-table preparation and qualified physical access.

The optional finite register-capture diagnostic grants one pipe and at most
thirty ordered reads before permanent closure, with admission expiry of at
most two seconds. Values remain copied protected RAM. Expiry does not reap
resources or cancel hardware transactions; cap references survive owner failure.
The readiness report is observational and cannot issue authority. Physical
register safety, independent serial control and physical capture remain blocked
in that report's policy model. Actual experiment evidence is recorded separately;
the report does not auto-authorize access. See [finite capture admission](AMD_POLARIS_DISPLAY.md#finite-capture-admission-and-readiness).

The prepared Polaris BAR5 binding supports bounded kernel operation holds with
independent binding/token identities. Cleanup closes new admission before
waiting for holds and retains leaf mappings plus assignment pins until they
drain. Holds alone authorize no transaction or userspace grant; the privileged
experiment checks its fixed semantic allowlist, owner and mapped leaf separately.
Release requires trusted evidence that the retained CPU operation stopped;
it does not certify hardware completion or DMA quiescence. Readiness reports
selected-pipe power as unresolved independently of PCI D0.


The native/rescue active-output lifecycle is a separate kernel mechanism from
boot metadata and CPU ownership. A privileged provider can publish only after
firmware retirement, writer draining and KERNEL lease reclamation, within the
exact retained-buffer layout. It must supply the hardware validation separately.
Native direct handoff binds a fresh descriptor generation to the existing lease;
revocation during handoff prevents commit, and stale generations cannot revoke
a replacement. Legacy firmware services/presentation cannot use that descriptor.
There is no public constructor or automatic takeover. A separate default-off monitor transaction now uses a restricted same-buffer write broker.


The Ring3 Polaris plane module now proposes a five-step versioned same-surface
rebind recipe. Its witness/step messages carry copied scalar data only. An
independent kernel preview checks exact ordering, incarnation, observation,
pipe and the kernel-pinned surface value; any mismatch closes it permanently.
The controller closes/unmaps live read access before this exchange.
There is no write executor or GPU programming grant. A valid preview does not
retire GOP, publish a native descriptor, prove completion or restore a display.
See [the plane preview contract](AMD_POLARIS_DISPLAY.md#ring3-plane-rebind-preview).


Stable inherited flip pending-mode0x20 is now accepted as copied preview data,
alongside0. Unknown bits and changes between witness samples refuse. This is
a preservation policy: it neither writes that control nor supplies a GPU
completion certificate. Actual physical capture refusal and later offline
recipe replay remain distinct evidence.

The native experiment pre-reserves distinct primary/rescue Ring3 executables,
private state, endpoints and RO framebuffer mappings. Ring3 supplies fixed
semantic rebind proposals; a kernel validator permits two pinned address
stores only after GOP retirement. The kernel owns the supervised assignment;
no device/IRQ/DMA grant crosses into either process. The primary's stopped/
reaped state and removed mappings precede rescue writer promotion under the
existing lease. Recovery uses the retained quarantined kernel assignment and
one separate generation/round, never ordinary release/reassignment. This
transitional monitor orchestration does not implement a general driver ABI
or complete migration of graphics-supervisor policy into Ring3.

The explicit graphics monitor uses private scalar heartbeat challenges; a
reply must match the incarnation, role and challenge and the peer must reblock.
Fault/spin diagnostics use a distinct bounded ACK receive because the peer
intentionally never reblocks afterward. They add no startup capabilities or
GPU authority. A separate Ring3 console performs measured work while the
diagnosed driver is unresponsive. This is a transitional monitor test/control
path; normal graphics supervision policy has not migrated into Ring3 yet.

The private graphics policy exchange now asks the existing Ring3 supervisor
to select continue,rescue or headless from versioned scalar health observations.
Its ordered state ledger has no GPU addresses or device authority. The kernel
client binds session/sequence to the exact supervisor process and thread;
ambiguous results or replacement poison that session without replay/reconnect.
Existing two-capability supervisor startup is unchanged. Kernel stage/liveness,
lease,write-budget,revocation and quarantine enforcement remain authoritative.
The monitor still schedules observations and applies bounded mechanisms;normal
independent supervision outside that loop remains a separate migration.

The independent graphics worker owns native command/observation execution.
It has one privileged thread,two private endpoints and four kernel-only
nontransferable capabilities;it adds no userspace startup grants or syscalls.
Existing supervisor RPC/lifecycle admission is serialized across kernel clients.
The worker samples health and applies bounded mechanisms;Ring3 chooses recovery.
Private command sequence and worker/control identities prevent stale reuse.
Unknown completion retains a poisoned channel with no replay or trusted-stack
termination;failed-policy cleanup binds the original process/thread identity.
This removes the monitor-loop dependency while broader userspace lifecycle/
broker orchestration remains transitional.


The rebind execution candidate places the vendor sequence in the protected
primary/rescue ELF. Existing private endpoints carry bounded semantic
operations to a finite kernel validator; no user MMIO mapping, raw offset,
syscall or startup grant is added. The validator independently constrains
the fixed operation set and checks hardware completion before output/lease
publication. An ambiguous exchange consumes the attempt without replay.
The old synchronous executor is now a host-test reference excluded from the
kernel build. This moves execution sequencing into Ring3 while existing
privileged device ownership and lifecycle mechanisms remain transitional.


A planned primary graphics-driver replacement uses Ring3 policy admission and
completion with one kernel-enforced attempt. The kernel proves old CPU mappings,
endpoints and private pages drained before service storage reuse; scanout and the
device binding remain retained. The new primary receives a RO framebuffer and a
private adoption entry that cannot reprogram or draw. Its PID/service incarnation
changes independently of the unchanged output generation. This CPU lifetime
boundary is not evidence of GPU reset, DMA quiescence or device reassignment.


`displayrom` inspects the selected RX 570's bounded firmware ROM RAM copy through
an isolated Ring3 service. The v11 boot handoff preserves a dedicated LoaderData
allocation for the boot; only complete copies in reserved RAM map RO/NX. Two
private IPC endpoints are the only capabilities. The diagnostic neither executes
ATOM tables nor accesses a device ROM BAR or GPU registers. Parser refusals leave
the current display path untouched; incomplete CPU teardown retains ownership.
Software host/audit and four QEMU boots pass, including actual 64-page RO/NX,
checksum refusal and resource baselines. Physical board-ROM acceptance is recorded below.
See [firmware ROM connector inventory](AMD_POLARIS_DISPLAY.md#firmware-rom-connector-inventory).


Physical v29 acceptance records a complete 118784-byte firmware RAM copy, accepted
59904-byte first legacy image/ObjectInfo1.3 and four stable CRC32 E2CB542E reports
around native takeover and controlled reboot. The directory contains DP objects
3113/3213/3313 and HDMI-A object310C; active port/encoder/link routing remains
unknown. Native GUI input was visibly correct; four GUI sessions/three complete
baselines, presenter/compositor exception cleanup, serial/power readiness and
readable unassisted reboot passed. Only the original primary transaction used
GPU access:60loads/two stores/one poll. ROM inventory added none. Four active
captures saved40256RX/395TX plus1694 attributed passive boot bytes;seven leading
bytes stay saved/excluded. No actual ROM blob export or new rescue/restart case.
See [physical ROM inventory](AMD_POLARIS_DISPLAY.md#observed-firmware-rom-connector-inventory)
for offsets, provenance, byte scopes and limitations. Native modes/link programming
and hardware reset/isolation remain open.


The private ROM protocol advances to version 2 for at most 212 query words;
only reported connectors are queried. The kernel checks a four-second CPU
admission budget before/after calls. An in-flight call and teardown retain
existing independent bounds, so this is not a four-second total deadline.
The ROM parser keeps its report in private process memory under the unchanged
stack guard. Its bounded routing decoder is userspace-only; GPIO indices are
inventory values with no MMIO/port/IRQ/DMA or framebuffer authority.
See [firmware routing inventory](AMD_POLARIS_DISPLAY.md#firmware-connector-routing-inventory)
for field encoding, refusal statuses and evidence limits.


Physical v30 kept all four refused ROM routes at status 3 with zero partial
fields across four stable reports; directory reporting and the normal native GUI
remained usable. Actual Ring3 RAM tests, four GUI sessions/three full baselines,
presenter/compositor exception cleanup, serial/power readiness and readable
unassisted reboot passed. Record decoding on the board remains unresolved.
See [physical record refusals](AMD_POLARIS_DISPLAY.md#observed-connector-record-refusals)
for capture scopes and the next bounded RAM-byte diagnostic. No new GPU operation
or successful physical routing/HDMI/reset/isolation claim is made.


The development monitor adds a fixed-size selected-ROM RAM-copy diagnostic,
with no arbitrary physical-address argument or new syscall/capability. Its kernel
code checks retained source/range and formats bytes; all vendor parsing remains
in the unchanged Ring3 service. Existing owners, mappings and GPU budgets remain
unchanged. Physical byte windows and successful board routing decode are pending.
See [the bounded RAM diagnostic](AMD_POLARIS_DISPLAY.md#bounded-firmware-ram-byte-diagnostic).


Physical v31's first ROM byte-window read caused a kernel page fault on an
unmapped reserved-RAM direct alias; no bytes were exported. The PMM-backed
synthetic window tests did not cover that mapping condition. Serial captured
the halt; no retry, GPU transaction or reboot followed. A replacement image
must move source reads into the isolated service's checked RO/NX mapping.
See [the physical window failure](AMD_POLARIS_DISPLAY.md#physical-byte-window-kernel-fault).


The replacement byte diagnostic uses private ROM protocol 3 and the isolated
service's checked RO/NX mapping. The kernel receives up to eight bytes per reply
and performs no ROM-source dereference. A 256-byte window needs at most 32 calls;
failed calls/cleanup clear output and retain cleanup ownership when necessary.
Host protected-page checks and four QEMU runs pass, including actual Ring3 reads
and full CPU baselines. Physical reserved-source capture remains pending after
resetting the halted v31 target. Native/rescue code and GPU budgets are unchanged.
See [the isolated byte-reader repair](AMD_POLARIS_DISPLAY.md#isolated-firmware-byte-reader-repair).


Physical v32 exported the complete firmware RAM copy through the isolated service;
CRC/overlap checks and unassisted controlled reboot pass. Actual-copy host replay
identifies a single-byte terminator refusal;successful routing remains pending.
See [the byte-capture record](AMD_POLARIS_DISPLAY.md#complete-isolated-firmware-byte-capture).


ROM directory admission requires one available record type byte. An FF token
completes the stream;every ordinary record still requires its two-byte header
and full bounded extent. This remains isolated Ring3 data parsing with existing
protocol3 and RO/NX mapping;no GPIO/AUX/I2C/HPD or GPU authority is added.


Physical v33 routing qualification passes:four reports/all48 golden fields,
actualRing3 single-byteFF/peer preservation/fullCPUbaselines and unassisted
controlledreboot. No new GPU or GPIO/AUX/I2C/HPD transaction ran.
See [the physical routing record](AMD_POLARIS_DISPLAY.md#physical-terminator-and-routing-qualification).


Private ROM protocol 4 adds a separate bounded topology tail after the unchanged
V2 route slots. Vendor path, encoder and capability decoding stays in the
isolated RO/NX service. Kernel code transports bounded words and checks report
shape. Failed topology clears independently while connector and route data
remain available. Source RAM stays reserved for the boot.

Generic managed-service readiness checks now use the existing UP interrupt
save/restore pattern to observe state and schedule atomically. Timeouts, the
64-turn fallback and cleanup rules remain unchanged.


Physical v34 topology qualification passes: four reports match all 64 expected
fields. Actual Ring3 parsing, refusal, peer preservation and complete CPU
resource checks pass, followed by an unassisted controlled reboot. No GPU or
GPIO/AUX/I2C/HPD transaction ran in this pass.
See [the physical topology record](AMD_POLARIS_DISPLAY.md#physical-firmware-path-and-encoder-qualification).


`polaris-link` uses privileged fixed semantic reads under the exact inherited
hardware profile; it grants no userspace MMIO access. The existing binding adds
three RO/NX/UC pages with checked leaf/frame lifetime. Access closes before a
fresh Ring3 process decodes copied words through private BEGIN/DATA/QUERY
operations. Ordered upload is one-use; malformed input closes that process.
Vendor interpretation remains userspace. Inherited assignment quarantine is
retained and is not evidence of DMA isolation or safe ordinary reuse. This
physical positive path remains pending; CPU-only Ring3 and refusal tests pass.
See [the bounded profile](AMD_POLARIS_DISPLAY.md#bounded-read-only-dig-observation).


Physical v35 completed52 ordered read-only DIG/CRTC loads with matching paired
samples and CRTCbrackets;freshRing3 and independent host decoding agree on
CRTC0-FE2-BE2. Checked MMIOcleanup/quarantine,duplicateattemptrefusal,continued
serial/readabledisplay and unassisted controlledreboot pass. No GPUstores/polls
or newnativeprogramming ran;native changedmodes/HDMI/reset/isolation stay open.
See [the physical DIG record](AMD_POLARIS_DISPLAY.md#physical-read-only-dig-qualification).


`polaris-timing` reuses the seven existing RO/NX/UC leaves and fixed semantic
reads. Kernel admission requires the observed CRTC0-FE2-BE2 route and DP stream
enabled;userspace supplies no address or target. After MMIO closure/unmap and
quarantine, a fresh Ring3 process interprets copied words. New private operations
13-15 are a one-use copied-data role; malformed/cross-role requests close it.
Observational raster size grants no mode authority. Positive physical timing
access remains pending; no new syscall, mapping or device write is introduced.
See [the timing profile](AMD_POLARIS_DISPLAY.md#bounded-inherited-timing-and-dp-stream-observation).


Physical v36 completed80reads and checkedcleanup/quarantine, but its strict
Ring3 report refused the changing automatic M measurement (758D->758B);status3
cleared the tail. The original failed session is retained. Serial/readableoutput
and unassistedreboot pass;no GPUretry/stores/polls/nativeprogramming ran.
See [the timing refusal record](AMD_POLARIS_DISPLAY.md#physical-timing-capture-and-automatic-m-refusal).


Timing private protocol2 exports nine copied words and rejects the old eight-word
BEGIN version. Only low24 automaticM differences are observed when stable GEN_EN
is set;all static control/N/route/manualM/reserved changes retain strict refusal.
Both fullraw M samples are returned,with no new hardware/programming authority.
The hardware read controller,RO pages,budgets and quarantine lifetime are unchanged.
See [automatic M reporting](AMD_POLARIS_DISPLAY.md#automatic-m-sample-reporting).


The physical v37 paired-M timing observation passed: 80 ordered loads, stable
controls, both automatic M samples retained and all nine report words checked
independently. Checked cleanup/quarantine, repeat refusals, serial control,
readable output and unassisted controlled reboot passed. There were no GPU
stores, polls or retries. Native changed-mode and reset programming remain open.
See [the physical timing record](AMD_POLARIS_DISPLAY.md#physical-paired-m-timing-observation).


## Vertical-total preparation and restoration plan

V38 adds explicit `polaris-vertical`: the existing 80 timing reads plus two
MIN/MAX/CONTROL samples, 86 maximum, in existing supervisor RO pages with a
shared once-per-boot attempt. A fresh Ring3 probe emits an 18-word copied primary
and restoration proposal only for a strict inactive-override baseline. There
is no new write executor or hardware recovery qualification. Host, pinned-source
audit, actual Ring3/CPU-baseline and four final-image QEMU checks pass; physical
values and plan acceptance remain pending. See [the detailed preparation
contract](AMD_POLARIS_DISPLAY.md#vertical-total-preparation-and-restoration-plan).


The physical v38 read-only vertical preparation passed: 86 ordered loads,
stable zero MIN/MAX/CONTROL samples and all 18 copied plan words independently
checked. Cleanup/quarantine, all shared repeat refusals, serial/readable output
and unassisted controlled reboot passed. No timing writes or restoration were
executed. See [the physical preparation record](AMD_POLARIS_DISPLAY.md#physical-vertical-total-preparation).


## Fixed vertical-total writes and fresh rescue restoration

V39 adds explicit `polaris-vtotal [fault1|fault2|fault3]`: a fresh checked
86-value preflight, three fixed primary vertical stores and a fresh Ring3
rescue that disables the override and restores the retained zero baseline
before plane rebind. It reuses the existing worker, supervisor, exclusive lease,
quarantine and serial cleanup paths. Normal `gui native` stays address-only.
Host boundary/transport/controller checks, actual Ring3 RAM faults/restoration
and four QEMU boots pass (88/88 normal; 88/88/611 deep). Physical writes and
visible restoration are pending. Use separate fresh boots for prefix faults;
leave COM7 available. Readback/frame progress does not prove effective refresh
rate, full modesetting or GPU reset. See [the bounded write and rescue
contract](AMD_POLARIS_DISPLAY.md#fixed-vertical-total-writes-and-fresh-rescue-restoration).


## Physical fixed vertical writes and fresh rescue qualification

Physical v39 passed on the exact RX 570/Philips DP profile: fixed primary
MIN1132/MAX1132/CONTROL17 writes, automatic fresh rescue after a primary hang,
and separate fresh-boot crashes after each of the three writes. All four fresh
rescues restored CONTROL0/MIN0/MAX0 before plane rebind and produced visibly
readable diagnostic output without a kernel restart. Total888reads/37stores/
13polls;no replay. Independent serial/Ring3 progress, cleanup and ownership
refusals on all four boots, and four unassisted controlled reboots passed. Final firmware prompt
is readable and power ready; COM7 is free. The matched 536-file software record
is unchanged. Effective total/rate, resolution switching, HDMI and GPU reset/
DMA isolation remain unqualified. See [the physical write and restoration
record](AMD_POLARIS_DISPLAY.md#physical-fixed-vertical-writes-and-fresh-rescue-qualification).


## Interrupted rescue restoration and serial-only containment

V40 adds `polaris-vtotal rescue-fault1|rescue-fault2|rescue-fault3`: arm one
rescue exception before takeover, then use the existing primary-failure path.
A distinct protocol5/op13 entry shares the one vertical attempt and independently
caps rescue IO at six reads/N stores, refusing every subsequent operation and
successful completion. The existing headless failure/cleanup path keeps original
GOP retired, retains quarantine and serial control; partial cleanup cannot rearm
GPU access. Stale pixels do not mean rescue succeeded.

Host/controller/transport/worker/audit, actual Ring3 exceptions/RO mappings and
four QEMU boots pass (normal88/88,deep88/88/611). Physical partial-rescue failure,
post-failure Ring3 shell/serial progress and controlled reboot need separate
qualification on the new matched image. See [the interrupted-rescue
contract](AMD_POLARIS_DISPLAY.md#interrupted-rescue-restoration-and-serial-only-containment).


## Physical rescue-driver failure and serial containment qualification

Physical v40 passed three fresh-boot rescue UD2 cases after restoration store
ACK 1, 2 and 3. Each rescue performed six reads and exactly its selected store
prefix, then faulted without readback, plane rebind or output publication.
CPU cleanup retained GPU quarantine and original GOP access stayed retired.
Independent Ring3 shell/COM7 control and three unassisted controlled reboots
passed. Total: 498 reads/21 stores/six polls, no replay; 16 sessions/78 commands,
RX116611/TX1007 plus 1694 attributed passive boot bytes. Initial actual Ring3
RAM/display-probe suites passed once. The 498-file software record is unchanged.
The final firmware prompt is readable and power ready; COM7 is available.
This qualifies containment when the rescue process itself fails under the
inherited DP profile. Full modesetting, effective refresh, HDMI and GPU reset/
isolation remain open. See [the physical rescue-failure record](AMD_POLARIS_DISPLAY.md#physical-rescue-driver-failure-and-serial-containment-qualification).


## DP stream and update-control observation

The matched v41 candidate adds `polaris-stream`:96 read-only dword loads at most,
including two samples of five fixed DP2/FIFO/DIG2/CRTC0 update controls within
existing RO pages. Fresh Ring3 copied decoding,raw unknown-bit diagnostics,
malformed-protocol containment and full CPU baselines pass;all four QEMU runs
pass. No new write,blank/unblank or restoration operation is granted. Physical
new-register capture is pending;v40 failure containment remains sealed separately.
See [the stream observation record](AMD_POLARIS_DISPLAY.md#dp-stream-and-update-control-observation).


## Physical DP stream and update-control qualification

Physical v41 passed one 96-load read-only capture and the copied Ring3 14-word
report. Both stream/FIFO/frontend/update samples matched: stream0x10201,
FIFO0, frontend0x01000000, update lock0 and update mode2; diagnostic flags0.
No GPU stores, polls or retries occurred. CPU cleanup, quarantined-parent
retention, repeat refusal, readable firmware output, COM7 control and one
unassisted controlled reboot passed. Five sessions/21 commands retained
RX35742/TX245; initial boot was not captured while the adapter was unplugged.
The 493-file software record is unchanged. No blank/unblank or changed-mode
write is qualified by this pass. See [the physical stream record](AMD_POLARIS_DISPLAY.md#physical-dp-stream-and-update-control-qualification).


## Validated profile and runtime extraction

The native controller, driver reservations and MMIO admission now share the
sole compiled RX 570 profile, with separate checked runtime observations and
kernel-retained rescue state. Physical support and operation budgets remain
unchanged. Host checks, pinned stream/vertical audits and all four QEMU boots
pass (normal 88/88, deep 88/88 with 611 iterations); all 27 rootfs files,
including 21 ELFs, match the accepted
build. Matched-image hardware regression passed on the original physical target; see below.
See [the profile architecture](AMD_DISPLAY_ARCHITECTURE.md) for trust boundaries,
extension contracts and the distinction between configuration extraction and
implemented mode, memory or lifecycle support.


## Physical profile/runtime regression qualification

The matched profile refactor passed on the original Z390 / RX 570
1002:67DF EF / 1458:2310 / Philips Momentum272M8 DisplayPort target.
All 47 active serial sessions passed. The 96-load read-only capture,
normal GUI launch and healthy CPU driver replacement, active-GUI hang recovery,
visible fresh rescue/reconnection and terminal serial-only cleanup matched the
existing contracts. Fixed vertical override/restoration and all three primary
and all three rescue interruption prefixes also passed. The operator confirmed
readable output and nine unassisted controlled reboots.

Independent trace checks counted 1596 GPU loads,
62 stores and 21 bounded polls, with no replay or
retry. All 27 rootfs files / 21 ELFs remain identical to the accepted parent;
software and physical evidence are sealed separately. Raw serial journals,
command offsets and operator observations are retained; inter-session gaps
and USB byte readback are excluded.

This confirms behavior preservation on the sole original inherited DP profile.
Dynamic memory/layout/mode/pipeline support, independent output initialization,
DP blank/unblank/training, HDMI, GPU reset/isolation and safe device reuse remain
unimplemented. Other AMD generations remain outside the supported scope.
