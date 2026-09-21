# JA OS privileged mechanism and userspace service boundary

Status: R7 exit gate reported functional on the 2026-09-21 local working tree;
formal verified-milestone status awaits a committed revision.

## Privileged mechanism boundary

The microkernel owns protection and mechanism: address spaces, threads and
scheduling, capability/object lifetime, bounded IPC/waits, terminal process
outcomes, and minimal hardware-facing mechanisms. Policy such as service names,
client protocols, formatting, retry/replay decisions, filesystem namespace and
ordinary launch/restart policy belongs in userspace.

R7a.1 introduces a console-output example without a new console syscall or a
console-specific capability type. `ConsolePortal` is built from the existing
endpoint, capability and kernel-thread mechanisms. The console service receives
only SEND authority to that portal. The privileged consumer accepts one bounded
byte-write message and emits it through the existing terminal/serial path.
Formatting and client-facing behavior remain outside the portal.

The kernel emergency monitor, terminal implementation, VFS pathname lookup and
bootstrap service orchestration remain transitional privileged policy after
this patch. The normal Ring3 shell is the boot-default command surface; the
monitor remains a recovery/development surface, not a permanent application API.

## Versioned service protocol boundary

New R7 services use the bounded four-word IPC convention in
`include/service_protocol.h`.

Request:

- word 0: service operation
- word 1: protocol version
- words 2-3: operation payload

Reply:

- word 0: service-specific result or common protocol error
- word 1: service incarnation
- word 2: protocol version supported by the replying service
- word 3: optional result/error detail

Version 1 requires an exact version match. A mismatch returns
`JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION`; an unknown operation returns
`JCOS_SERVICE_REPLY_UNKNOWN_OPERATION`. Neither condition is a service crash.
Future compatibility rules require a new documented protocol version rather
than silently reinterpreting words.

## Authority and lifetime

Capability handles are local authority, not global object names. A transferred
capability is attenuated to the rights explicitly granted by the launcher. The
R7 console service receives RECEIVE on its command endpoint, SEND on its reply
endpoint and SEND-only authority to the privileged console portal.

Capability generation and service incarnation remain separate identities. A
replacement service must use a new incarnation and clients must reconnect; old
handles are never silently retargeted.

## Cancellation, retry and replay

IPC peer death and cancellation continue to use the R2/R6 terminal wait
semantics. Successful delivery does not imply durable application completion.
The kernel and generic launcher do not replay requests after service failure.
Any retry or idempotency rule is part of the userspace protocol and must be
explicitly documented per service.

## Emergency diagnostics

Serial and minimal kernel terminal diagnostics remain independent of ordinary
userspace services so a broken console/shell service cannot make kernel failure
indistinguishable from display-service failure. R7 will reduce normal kernel
console/shell policy while retaining that emergency path.

## R7a.2 persistent bootstrap console

R7a.2 makes the console service a required persistent bootstrap service. The
kernel still performs bootstrap path lookup and initial service orchestration as
a transitional policy, but ordinary post-bootstrap status output can now cross
the versioned Ring3 console protocol before reaching the privileged byte portal.

The portal is intentionally longer-lived than one console-service incarnation.
A console process fault is contained and reaped through `ManagedService`; restart
creates a new process, endpoint authority and service incarnation while retaining
the same privileged portal endpoint and consumer thread. A saved client
connection to the failed incarnation remains stale and is never retargeted.

`ManagedServiceLaunchExtras` now permits bounded protocol-specific startup
grants, arguments and shutdown request words after the fixed command/reply
envelope. The legacy `ManagedServiceSpec` remains unchanged for R6 callers.
This is generic launch plumbing for R7 services, not a console-specific kernel
API.

A failed ordinary console write is not replayed through the emergency sink. The
kernel may emit a fixed serial diagnostic stating that userspace output is
unavailable, but the ambiguous application payload is suppressed. This keeps
the R6 retry/replay rule intact.

The raw framebuffer terminal and serial path remain available for bootstrap,
kernel faults, test diagnostics and power-transition failures. The kernel monitor remains transitional privileged policy for diagnostics.
R7a.3 moves the first normal shell parser/line policy into Ring3; broader
application-facing input/output endpoints remain subsequent R7 work.

## R7a.3 userspace normal-shell handoff

R7a.3 moves the first normal interactive shell policy into the persistent Ring3
console service. The kernel monitor remains available for development and
emergency diagnostics, but it no longer needs to parse commands while a normal
userspace session is active.

The transitional handoff is explicit: the kernel monitor command `usershell`
activates the Ring3 shell and then becomes only an input broker. It polls the
existing PS/2/serial mechanism and forwards bounded, versioned key events to the
console service. Character echo, line buffering, command parsing, command
results and the normal prompt are all userspace policy. The initial userspace
command set is deliberately small (`help`, `echo`, `about`, `status`,
`monitor`) while filesystem and power policy remain migration targets.

The public key-event vocabulary in `console_service_protocol.h` is versioned
separately from the kernel-internal `KeyCode` enum. Modifiers and the printable
byte are carried as bounded scalar payload; Ring3 receives no PS/2 ports,
controller state, serial registers, framebuffer pointer or terminal internals.

Every forwarded event is a bounded request/reply transaction. A console-service
failure does not cause the triggering input event to be replayed after restart.
The broker may restart the service, reactivate a fresh shell session and accept
the next event. This preserves the R6 no-implicit-replay rule for input as well
as output.

`monitor` returns an explicit userspace action in the event reply. Escape is
also reserved as a kernel-owned emergency return path, so a broken userspace
parser cannot trap the operator outside the kernel monitor.

## R7a.4 boot-default userspace shell

R7a.4 changes only the boot policy, not the public protocol or privilege
boundary. After the persistent console service starts and the boot-status banner
is emitted, the kernel activates the Ring3 shell before entering the kernel
monitor. The monitor is entered only after the userspace `monitor` command, the
kernel-owned Escape emergency return, or a userspace-shell activation/recovery
failure.

The boot wrapper records successful default-shell handoffs and total Ring3 shell
sessions. The acceptance test can therefore prove, after returning to the
monitor, that this boot actually passed through the userspace shell first rather
than merely compiling a default-shell branch. Returning to the monitor does not
stop or replace the persistent console service; `usershell` re-enters a fresh
Ring3 shell session over the same current service incarnation.

A boot-default activation failure is fail-open only with respect to operator
diagnostics: the kernel emits a fixed serial/terminal error and enters the
emergency monitor. No ambiguous key or output payload is replayed across that
fallback.

## R7b.1 foreground application console output

R7b.1 adds the first application-facing console ABI without exposing the
console service's private management endpoint. A separate foreground-application
endpoint is created by `system_console` and granted RECEIVE-only to the Ring3
console service. An application launch receives only SEND authority to that
endpoint, attenuated through the ordinary `ProgramLaunchSpec` transaction.

The public application protocol is `include/console_client_protocol.h`. It is a
one-way, bounded stream protocol: each WRITE carries a service-session ID and up
to 16 bytes; END closes the foreground output session and returns the console
service to its private management/shell receive loop. Malformed, incompatible or
stale application messages are discarded without generating management replies.
Applications therefore cannot inject shutdown, diagnostic-fault or shell-control
operations merely by possessing console-output authority.

A foreground session is explicitly single-client policy for this milestone.
The kernel opens a nonzero session ID before launch, the application receives
that ID as startup data, and the console service accepts payload only while the
matching session is active. There is no implicit replay. If an application dies
without END, its owner must terminate/reap the client and abort the session;
normal console-service replacement is not allowed to silently adopt an active
client session.

The temporary kernel SEND|TRANSFER capability used to attenuate a child output
capability exists only across `program_launch()` and is revoked immediately
after publication. On a normal console-service restart the application endpoint
is destroyed and recreated; its endpoint identity changes while the lower-level
privileged byte portal remains stable. Old transport authority therefore never
silently becomes authority for the replacement service.

`/bin/hello.elf` is the first independently linked Ring3 utility using the new
client library (`user/lib/console.c`). The R7b.1 acceptance gate launches it,
checks that its capability table contains SEND but not RECEIVE authority,
observes its output through the userspace console policy and byte portal, reaps
it to the exact baseline, replaces the console service, and launches a second
copy through a fresh foreground session.

R7b.2 adds the complementary foreground input channel without weakening the
output capability boundary established here.

## R7b.2 foreground application input and focus

R7b.2 adds a second public application endpoint for input. The privileged
hardware-input mechanism retains SEND-only authority to this endpoint while a
foreground application may receive an attenuated RECEIVE-only capability at
launch. Output and input are therefore separate one-way authorities: an
application cannot manufacture input by writing to its output endpoint, and it
cannot inject console-management requests through either public channel.

The public event format is `include/console_input_protocol.h`. It carries a
versioned key-event operation, the foreground session ID, a public key code and
a packed printable-byte/modifier value. This vocabulary is shared by the normal
Ring3 shell boundary and standalone applications but remains independent of the
kernel-internal `KeyCode` enum.

Foreground focus is explicit and single-client in this milestone. The same
nonzero session ID that scopes application output also scopes input. The kernel
input broker refuses events for any other session ID. The application-side
client library consumes and discards stale-session input rather than returning
it to a newly focused application. The input endpoint is also drained before a
new session starts, so an ambiguous old event is never replayed as fresh input.

The kernel retains one SEND capability to the application-input endpoint for
the lifetime of the current console-service incarnation. A temporary
RECEIVE|TRANSFER authority exists only while the generic launcher attenuates a
child RECEIVE capability and is revoked immediately after publication. On
console-service restart both public application endpoints are destroyed and
recreated, while the lower privileged byte portal remains stable.

`/bin/inputecho.elf` is the first independently linked application that receives
both directions of the public console contract. It receives exactly SEND-only
output and RECEIVE-only input authority, waits for one focused key event,
validates the printable byte and modifiers, writes a result through the normal
Ring3 console service and ends the foreground session. The acceptance gate runs
that transaction on two service incarnations and requires exact object/resource
baselines after each reap.

The physical PS/2/serial poller is still privileged mechanism in R7b.2. Moving
ordinary executable lookup/launch policy and broader focus/service orchestration
into userspace remains subsequent R7 work; this slice establishes the authority
and cancellation contract those policies can use.

## R7c.1 userspace boot-archive namespace

R7c.1 moves the first filesystem namespace/read policy across the Ring3
boundary without introducing a pathname-aware kernel API. The privileged
mechanism is `BootArchivePortal`: two capability-controlled endpoints plus a
kernel worker thread over the immutable boot archive byte range supplied by the
loader. Its protocol (`include/boot_archive_portal_abi.h`) supports only INFO
and bounded READ-by-offset. It does not parse tar headers and has no operation
for path lookup, directories, files, executables or application names.

The persistent console service receives attenuated SEND authority to the raw
request endpoint and RECEIVE authority from the raw reply endpoint. Together
with its existing command/reply, byte-portal and application-output grants this
uses six startup capabilities, within the v1 eight-capability startup envelope.
Archive discovery is intentionally lazy: process startup installs the handles
but performs no archive IPC until the first filesystem command. This preserves
the ManagedService readiness contract that a newly launched service first
becomes observable blocked on its ordinary command endpoint.

`user/lib/boot_archive.c` owns the USTAR parser and namespace policy in Ring3.
It validates header magic/checksum, octal sizes and archive bounds, canonicalizes
names, rejects `..` path components, iterates entries and reads regular-file
content only through bounded raw portal requests. The Ring3 shell now implements
`ls`, `stat PATH`, `cat PATH` and `fscheck` using that library. The known
`/etc/r7c1.txt` fixture is an acceptance artifact proving that path resolution
and file-content policy are not delegated back to the kernel VFS.

The raw archive portal is mechanism lifetime, not console-policy lifetime. A
normal console-service restart recreates the Ring3 service incarnation while
preserving the archive portal request endpoint, reply endpoint and kernel thread.
Requests and replies carry the current managed-service incarnation; a stale reply
from a dead policy incarnation is discarded rather than replayed into the
replacement. Ambiguous reads are never converted into pathname-level retries by
the kernel.

This milestone is deliberately read-only and immutable. The raw portal exposes
at most eight bytes per READ because the current IPC ABI has four 64-bit words;
filesystem caching, writable storage, richer file-service concurrency and
memory-backed bulk transfer are later work. The transitional kernel VFS still
exists for the kernel monitor and current kernel-owned program launcher. The
next R7 launch-policy slice should reuse the Ring3-resolved archive entry and
move executable pathname choice out of the kernel rather than adding a kernel
`launch(path)` interface.

## R7c.2 userspace pathname selection / generic foreground launch

R7c.2 moves executable naming and foreground launch choice into the Ring3 shell.
The shell resolves `run PATH` with the same userspace USTAR namespace introduced
in R7c.1. The privileged side never receives the pathname. Instead the policy
service submits a versioned one-way request containing only:

- the current console-service incarnation,
- an immutable boot-archive data offset,
- the exact byte length of the selected file.

The kernel validates that raw extent against the configured immutable boot
archive and passes a temporary `VfsNode` byte view to the existing generic ELF
loader. This view has no namespace identity and is not inserted into kernel VFS.
ELF validity, mapping, capability attenuation, exit publication and reclaim
remain generic privileged mechanisms.

The launch request endpoint is a policy transport and reincarnates with the
console/shell service. A queued request is therefore never inherited across a
service replacement. The raw BootArchivePortal and ConsolePortal remain stable
privileged mechanisms.

A foreground launch opens the existing R7b application I/O session, attenuates
SEND-only output and RECEIVE-only input capabilities into the child, revokes the
temporary TRANSFER authorities after publication, routes focus to the child, and
waits for both normal process exit and application-session settlement before
returning to the Ring3 shell. Escape is a kernel-owned emergency cancellation
path; cancellation does not replay the launch request or input into a replacement
service.


## R7d.1 userspace ordinary-service naming and restart policy

R7d.1 moves the first ordinary background-service naming, executable selection
and restart decision into Ring3. The normal shell/policy service owns the
mapping `worker -> /bin/ordinaryservice.elf`, resolves that executable through
the R7c userspace boot-archive namespace, and submits only an immutable archive
extent to the privileged service broker. The broker never receives the service
name or pathname.

The service-broker request protocol is `include/service_broker_protocol.h`.
START and RESTART contain only the policy-service incarnation, immutable archive
offset and exact byte length. STOP, STATUS and the acceptance-only diagnostic
fault contain no executable identity. The broker endpoint is one-way SEND
from the Ring3 policy service and RECEIVE in the kernel. It reincarnates when
the policy service is replaced, so an unconsumed request cannot silently become
a command from a replacement policy incarnation.

The privileged `background_service` component is deliberately a bounded
single-slot mechanism in protocol v1. It validates the requested extent against
the immutable boot archive, creates a temporary pathname-free file view, and
reuses `ManagedService` for capability attenuation, program publication,
bounded stop, fault observation and reap. It does not maintain a service-name
registry and does not choose which executable should be restarted.

`include/ordinary_service_protocol.h` defines the common lifecycle profile used
by the first ordinary service (`/bin/ordinaryservice.elf`): versioned PING,
SHUTDOWN and an acceptance-only diagnostic fault operation. Replies carry the
managed-service incarnation. Capability generation remains the transport
identity; the protocol incarnation remains the service identity. Neither is
silently retargeted after replacement.

The Ring3 commands are `service start worker`, `service status worker`,
`service fault worker`, `service restart worker`, and `service stop worker`.
An unknown service name is rejected before any service-broker IPC occurs.
START/RESTART pathname resolution is likewise entirely Ring3 policy. The
kernel only observes the resulting extent and requested generic lifecycle
operation.

Control is not replayed after failure. A service-broker request is consumed at
most once. If the ordinary service faults, its failed incarnation is retained
until an explicit Ring3 restart/stop decision causes bounded recovery. Replacing
the console/policy service creates a new service-broker endpoint rather than
adopting an old mailbox. The unrelated supervisor, raw boot-archive portal and
console byte portal remain independent.

The persistent console service now receives exactly eight startup capabilities,
the maximum in `JcosProgramStartup` v1: its managed command/reply pair plus the
byte portal, application-output endpoint, two raw boot-archive handles,
foreground-launch broker and ordinary-service broker. R7d.1 deliberately does
not enlarge the startup ABI; future policy transports should use a more scalable
service-directory/wait-set design rather than incrementally adding fixed startup
slots.

Shutdown/reboot accounting includes an active ordinary background service as an
explicit process/address-space/thread/capability-table, two managed-service
endpoints, one exit queue and four kernel transport/grant capabilities. Power
quiesce first releases the background slot, then the console policy service and
finally the legacy supervisor, and still requires the exact kernel-only object
baseline before firmware transition.


## R7d.1b - Ring3 shell editor parity and local-error reset

The normal Ring3 shell owns its complete interactive line-editor state. The
console portal exposes only minimal terminal mechanisms: byte emission, cursor
left, page-up/page-down/to-bottom scrollback control, and one-shot accent-byte
emission for selection rendering. No history, clipboard, command buffer, or
service-name policy moves back into the kernel.

The Ring3 editor now supports Up/Down history, Left/Right, Home/End, Delete,
Backspace, Page Up/Page Down, Shift+arrow selection, and Ctrl+Shift+C/X/V.
Editor history/clipboard storage is static userspace BSS rather than the one-page
initial user stack.

Locally rejected `service` commands are complete shell transactions: they clear
the submitted line and emit a fresh prompt without sending a broker request.
This prevents a rejected name such as `service start nope` from contaminating
the next command.


## R7d.2 - userspace executable replacement

The Ring3 policy command `service replace worker` resolves
`/bin/ordinaryservice-v2.elf` and submits the existing generic
`RESTART_EXTENT` broker request. The kernel receives only an immutable archive
offset and size. It does not receive the `worker` name, either pathname, or a
replacement-specific operation.

`ordinaryservice.elf` and `ordinaryservice-v2.elf` are independently linked
executables implementing ordinary-service protocol v1. The protocol's
`IDENTIFY` operation distinguishes the primary and replacement artifacts for
acceptance testing without changing their lifecycle contract. Incompatible
versions, malformed requests, and unknown operations return explicit common
service errors and leave the service alive.

Replacement uses the existing managed-service teardown transaction. A failed
incarnation remains retained until the Ring3 decision arrives, then its process,
thread, endpoints, capabilities and exit publication are reaped before the new
extent is launched. Requests are not replayed, old authority is not retargeted,
and the replacement receives a fresh service incarnation.
The selected image remains Ring3 policy state, so a later
`service restart worker` launches the replacement again rather than silently
returning to the primary executable.

The `service-policy` acceptance test starts and identifies the primary ELF,
probes version and unknown-operation handling, faults it, verifies the unrelated
supervisor and console policy remain usable, replaces it with the distinct
second ELF, verifies the new image identity and incarnation, and restores the
exact resource baseline. This switches OS-service executables at runtime from
Ring3 policy without rebuilding or rebooting the kernel.
