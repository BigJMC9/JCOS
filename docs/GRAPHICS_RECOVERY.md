# Graphics recovery contract

Graphics failure should leave the OS running with an independent diagnostic
path. Recovery has three levels. Each hardware profile must qualify its own
access, DMA, interrupt, reset and output-restoration guarantees.

| Level | Preconditions and behavior | Current implementation |
|---|---|---|
| Firmware framebuffer recovery | Firmware-established scanout is still operational and has not been disrupted. Stop/reap the failed display service, remove its mappings, then reclaim the exclusive framebuffer lease for emergency diagnostics. | Existing GOP service recovery and lease tests; physical environments require their own validation. |
| Native GPU rescue | After native takeover, contain the primary driver, quarantine the device and retained DMA, and revoke its access. A separate minimal Ring3 rescue driver uses fresh, constrained recovery authority to reset/reinitialize a basic diagnostic display. | A default-off same-buffer experiment has observed RX 570/DP explicit-fault and monitor-detected-spin rescue cases; full modeset/reset, repetition and hardware-hang recovery remain unqualified. |
| Serial and controlled reboot | If in-place recovery is unsafe or fails, retain COM1 input/output and independent service progress. Keep unsafe resources quarantined. Offer a controlled reboot when its cleanup/storage contract permits it; firmware may initialize GOP on that boot. | Observed rescue-fault serial/quarantine and controlled platform reset; return to JA OS required operator boot selection. Failed-GPU/bus-hang survival remains unqualified. |

Native rescue programs the device itself. It does **not** invoke GOP after
ExitBootServices, and does not require restarting the kernel when in-place
recovery succeeds. A dead or non-isolated device may make recovery impossible;
never advertise unconditional GPU-failure survival without a qualified access
and isolation model.

## Ownership and preserved metadata

Retain the original GOP byte range, geometry, stride, format/masks, page span,
firmware mode index and optional PCI provenance as boot metadata. Keep it
separate from the active scanout descriptor. Metadata is evidence of the boot
configuration, not proof that those addresses still describe a working output.

Keep these independent as the native backend is added:

- CPU framebuffer writer: kernel, handoff, userspace owner, or reclaim.
- Scanout: firmware-valid, native takeover pending, native active, recovery/
  quarantine, rescue active, or unavailable.
- Device authority and recovery incarnation: owning assignment, mappings,
  IRQ delivery, outstanding submissions and retained/quarantined buffers.

The existing DisplayLeaseState represents the first item only. The framebuffer
now tracks boot scanout as unavailable, firmware-valid, retiring or retired.
A separate active-output descriptor tracks firmware, native, rescue or
quarantined output and a monotonic generation. Its publication and revocation
mechanisms exist; a constrained default-off provider supplies same-buffer
writes and reserved rescue, with observed explicit-fault and monitor-detected-spin cases.
Broader hardware qualification and device isolation remain open. A native
transaction must retire firmware fallback **before granting
authority capable of disturbing scanout**, including a writable mapping,
reset/power/translation operation or the first destructive write, even if the
transaction later fails. Process exit, lease reclamation and timeout cannot
make retired firmware scanout valid again.

Before takeover, stop legacy presentation, stop/reap the firmware display service
and verify removal of its borrowed mapping. Block new GOP mappings and kernel
framebuffer writers before authorizing native access. Preserve reservations and
the cleanup ledger. After takeover, serial diagnostics must not touch the old
GOP aperture. A successful rescue installs its own validated output descriptor
and exclusive writer; it does not silently reactivate the saved GOP descriptor.

## Firmware scanout retirement

The kernel-only display_retire_firmware_scanout() entry refuses handoff, user
and reclaim leases. The caller must first stop/reap the firmware display service
and finish borrowed-mapping teardown through its existing cleanup path. The
lease remains the sole CPU owner record; scanout validity grants no mappings.

Once admitted, retirement permanently closes kernel writer admission, prevents
new GOP borrowed mappings and suppresses terminal rendering. It returns true
only after all admitted kernel writes have drained. A false result can mean
retirement is still draining: inspect framebuffer_scanout_status(), retry
without spinning, and grant no destructive authority until retirement completes.
A suspended writer can finish its admitted primitive; hardware scanout must
remain undisturbed until that writer exits. A multi-row legacy presentation
pins the entire operation, so a new direct lease cannot interleave with it.
The admission critical sections preserve local interrupt state; drawing itself
does not disable interrupts. This synchronization applies to the current UP
profile and needs replacement before SMP.

framebuffer_info() continues to describe the immutable boot resource after
retirement or replacement. framebuffer_output_info() returns only a valid
active descriptor; geometry helpers describe the most recent output and grant
no write access. Neither metadata path is a hardware-readiness proof.
All kernel framebuffer primitives, legacy presentation, direct prepare/commit
and terminal re-enable honor validity. Reinitializing from saved BootInfo is
refused after successful initialization. Reservations and device resources
remain retained. Retirement proves neither IRQ containment nor DMA isolation,
and does not claim that native or rescue output exists.

Normal boot and automatic diagnostics do not retire live output. The explicit
`POLARIS_NATIVE_WRITE=1` monitor experiment calls retirement before its RW
grant. There is no standalone retirement syscall or command. Default builds
keep firmware scanout active; the experimental transaction cannot return to it.

## Active native/rescue descriptor lifecycle

The privileged `framebuffer_native_install()` entry publishes a descriptor
only after firmware retirement, drained kernel writers and a KERNEL CPU lease.
Its caller must independently validate actual operational scanout first. The
constructor performs no hardware operation and accepts no userspace request.
Normal boot and automatic tests do not call it on live output. The explicit same-buffer write experiment publishes only after its register/frame validation.

The first RGB/BGR32 storage profile retains exactly the boot allocation,
mapping/cache attributes, pixel format/masks, dimensions and stride. It cannot substitute
arbitrary physical memory, widen the aperture, allocate other VRAM or advertise
a different mode. A new allocation/layout needs a separate validated storage
contract. Keeping the same buffer does not make expired GOP callable or
reactivate firmware fallback.

Each native/rescue publication has a fresh nonzero identity. Revocation
immediately closes new kernel-writer admission and retains the descriptor and
generation as quarantined. Existing admitted primitives pin that descriptor
until completion; replacement refuses outstanding writers. This API does not
remove Ring3 mappings or establish GPU/DMA quiescence. The provider must stop/
reap the old owner and remove its mapping before reclaiming the CPU lease.

`display_direct_prepare_native()` checks the expected active generation and
uses the existing HANDOFF/USER/RECLAIM lease. Commit rechecks that same identity.
Revocation during handoff prevents commit. A stale revoke cannot affect a fresh
rescue generation. The firmware display-service constructor and legacy
DISPLAY_PRESENT capabilities remain firmware-only after native publication.
Kernel terminal re-enable requires a valid active output; old GOP state remains
permanently retired.

Host fixtures exercise these production paths using simulated framebuffer RAM,
including malformed descriptors, early publication, generation mismatch,
revocation during handoff and active writes, stale replacement requests and
firmware-capability refusal. They establish lifecycle behavior only, not a
native modeset or physical rescue. Native/rescue hardware qualification still
requires the sequence below.

## Native failure and rescue sequence

1. The supervisor detects process failure or a bounded liveness/completion
   timeout independently of the failed driver.
2. The kernel denies new submissions/access, revokes the failed incarnation's
   authority, masks/disarms IRQ delivery and removes CPU mappings. Preserve
   resources whose cleanup has not completed.
3. Quarantine in-flight DMA and its buffers. Failed cleanup or unsupported reset
   leaves them retained. Process death, disabled bus mastering and timeout alone
   do not establish that previously issued physical DMA has stopped.
4. Launch a fresh minimal Ring3 rescue instance with narrow recovery authority.
   It may reuse reviewed display-only backend code through a distinct recovery
   entry and immutable configuration; shared code still has common-mode risks.
   Device-specific reset/reinitialization policy stays outside generic kernel
   policy; a qualified trusted mediator may enforce the necessary reset
   operation. No unrestricted DMA/BAR authority is implied.
5. Use independently validated boot/profile data. Do not trust the primary
   driver's private heap, register cache, command queues, buffers or completion
   reports. Reset scope includes relevant sibling/audio functions and shared
   graphics/compute engines.
6. After a backend-specific proof that old operations cannot touch memory, retire
   old authority and issue fresh normal authority for the rescue scanout. Publish
   its actual mode/buffer geometry and route a usable diagnostic console to it.
7. If rescue faults, hangs, cannot prove safety, or fails output verification,
   keep the device/buffers quarantined, report through serial, and offer reboot.
   Bound rescue attempts and never restart indefinitely against unsafe hardware.

A reset-only rescue capability may need to operate while old DMA is quarantined;
ordinary access and buffer reuse remain prohibited until safety is established.
Fresh authority must not retarget stale capabilities to a replacement driver.

## Acceptance evidence

Firmware recovery and native recovery are separate capabilities. A firmware
service fault/restart test does not establish a native rescue pass.

For a firmware profile, verify metadata retention, exclusive writer handoff,
wrong-owner/concurrent-writer refusal, retained ownership after a service fault,
completed mapping teardown and terminal restoration. Establish that the selected
hardware remains in its firmware scanout configuration.

For a native profile, deliberately fault and hang the primary driver after
modesetting and during presentation. Verify unrelated service progress, serial
input/output throughout, old-access refusal, IRQ containment, retained DMA
lifetimes and bounded rescue success or safe quarantine. Fault/hang the rescue
driver too, exercise failed/unsupported reset, and verify stale-authority refusal.
Do not reuse buffers while an old device operation can still reference them.

A successful native recovery must display usable diagnostic text, accept
diagnostic interaction, and continue running without a kernel restart. Test
DisplayPort and HDMI separately. A failed in-place attempt must preserve serial
access and the controlled-reboot route without claiming a restored display.

See [Display inventory](DISPLAY_PROBE.md) and [Testing](TESTING.md) for the current
read-only hardware handoff and supported checks.

The hardware-free regression target
`make -f Makefile.msys2 test-framebuffer-scanout` links the production
framebuffer, lease and terminal code against host memory and process/IRQ/serial
doubles. It checks idle retirement, preemption during a primitive, retirement
between presentation rows, mapping/handoff refusals, retained metadata, blocked
stale rendering and continued serial/terminal processing. Separate QEMU lease
and service diagnostics check retirement refusal with a real handoff/live Ring3
owner, then exercise the existing firmware-service fault/recovery path.
These checks do not establish physical native recovery.

The [same-buffer experiment](AMD_POLARIS_DISPLAY.md#explicit-same-buffer-write-and-reserved-rescue-experiment)
reserves a distinct rescue process and read-only framebuffer pages before
takeover. It grants no direct GPU authority to either Ring3 entry. A kernel
supervised assignment retains ownership through quarantine; one narrow recovery
round under its existing pins never clears exposure history or reassigns the
device. Fresh text, monitor confirmation and serial continuity must be recorded
separately. Its supported fault set preserves operational inherited timings;
a GPU reset, general hardware-hang survival and IRQ/DMA isolation remain outside
that profile.


Controlled power commands have an explicit CPU-only native cleanup entry.
They stop/reap owners, remove aliases/leaves and revoke the mediator capability
before the ordinary clean-state check. They retain the quarantined device and
exposure history and never initiate another GPU read/write/reset for shutdown.
An incomplete teardown still refuses the controlled transition; no unrelated
object-count or storage invariant is waived.

The [observed RX 570 DisplayPort run](AMD_POLARIS_DISPLAY.md#observed-retained-scanout-rescue-run)
completed the fixed primary and fresh rescue writes, separately confirmed both
diagnostic displays, and reclaimed a readable kernel console without restarting
the kernel. It preserves operational inherited timing and demonstrates one
controlled software-owner fault. It does not exercise GPU reset, a failed GPU/
interconnect, rescue failure, changed modes or repeated recovery. Earlier
host-only lifecycle checks remain separate evidence.

The explicit experiment also has bounded private heartbeat challenges and
acknowledged fault/spin diagnostics for either current driver. A primary
liveness failure selects the reserved rescue; standby or rescue liveness
failure selects headless quarantine without another GPU round. Independent
Ring3 console progress is measured at a diagnostic checkpoint. This monitor
polling mechanism is not yet normal Ring3 supervisor policy, does not run
through arbitrary long monitor commands, and cannot interrupt a stuck GPU
bus transaction. Successful failed-path CPU cleanup revokes its mediator cap
before ordinary power readiness; it never revalidates expired GOP.

The [observed watchdog/rescue-failure run](AMD_POLARIS_DISPLAY.md#observed-watchdog-and-rescue-failure-run)
measured independent Ring3 console work during a primary spin,completed
automatic rescue,and retained serial control/quarantine after a deliberate
rescue #UD without another GPU operation. The controlled platform reset
booted Windows;the operator corrected boot priority before the captured JA OS
USB boot and readable firmware console. This demonstrates the reset request
and operator-assisted return,not unattended boot selection or arbitrary failed
GPU recovery. Earlier explicit-fault results remain distinct evidence.

The second fresh-boot v19 case separately exercised primary terminal #UD ->
automatic rescue,then active rescue SPIN -> heartbeat failure/headless
quarantine. Both independent Ring3 console checkpoints completed. Five
exhausted-state requests were denied with no additional GPU access; serial
and power readiness remained usable. Controlled reboot returned to a readable
JA OS firmware console without operator intervention. This complements the
earlier primary-SPIN/rescue-#UD case; it does not replace its assisted-return
record. See the complementary run in [Polaris display](AMD_POLARIS_DISPLAY.md).

The bounded policy runs continue/rescue/headless decisions in the
existing Ring3 supervisor. Versioned scalar snapshots,monotonic session and
sequence,and stable process/thread identity prevent stale/replacement reuse.
Failed standby wins over primary failure; failed rescue closes output.
The kernel independently enforces stage/writer validity,finite recovery,
revocation and quarantine; policy failure selects safe CPU teardown.
Scheduling is still monitor-driven. This does not yet establish autonomous
normal supervision or new hardware recovery scope.

A physical v20 case separately confirmed Ring3 action2 after primary spin ->
fresh visible blue rescue,and action3 after active-rescue #UD -> headless
quarantine with no further GPU MMIO operation. Independent console checkpoints,
serial control,power readiness and five exhausted-state refusals passed.
Controlled reboot returned unassisted to a readable firmware-backed JA OS
console. This qualifies one retained-mode DP process-failure case,with monitor
scheduling still in the kernel;see [Polaris display](AMD_POLARIS_DISPLAY.md).

The worker candidate moves health scheduling and native command ownership out
of the kernel monitor. Ring3 still chooses the recovery action;one privileged
worker applies finite mechanisms through private ordered IPC. A poisoned
control channel never replays a request or force-kills a callback. Worker stack/
endpoints remain until callback return and proven thread death;status/finish/
power performs the drain. Failed-policy cleanup retains exact process/thread
identity through partial release and cannot stop a replacement. Power checks
retain their existing baseline. Physical off-monitor/policy-failure evidence
is pending;see [Polaris display](AMD_POLARIS_DISPLAY.md).


Physical v21 checks cover delayed primary spin to fresh rescue and rescue
exception to headless serial/shell control while the normal Ring3 shell is
active. Separate fresh native boots cover policy exception and acknowledged
policy spin,each draining CPU resources without further GPU operations and
retaining clean power readiness. Reboot output was captured after every case;
the first return also has operator-confirmed readable output and no intervention.
See [the physical record](AMD_POLARIS_DISPLAY.md#observed-recovery-outside-the-monitor)
for counters,capture limits and preserved host-tool corrections. These cases
retain DP allocation/timing;GPU reset,arbitrary hardware failure,changed
modes and hardware DMA isolation remain unqualified.


The v22 candidate also moves the actual rebind sequence into the protected
primary and fresh rescue process. An independent kernel gate mediates the
existing finite semantic operation set through private IPC, with no raw BAR
or register-offset interface. Output publication requires trusted completion
and exact driver acknowledgment; partial/ambiguous execution fails closed
without replay. The v21 physical observations above remain evidence for that
older image; v22 physical execution/recovery is recorded below.


Physical v22 qualification now covers Ring3-sequenced primary and fresh
rescue execution, delayed primary spin/rescue exception outside the monitor,
and separate policy exception/spin cases. All terminal paths retained normal
shell and serial control and clean power readiness. Three controlled reset
returns have serial captures; the first also has operator-confirmed readable
output with no intervention. The first primary and rescue were separate
PID/incarnation pairs28/23 and29/24. See
[the execution record](AMD_POLARIS_DISPLAY.md#observed-ring3-execution-and-rescue).
The scope remains the retained DP allocation/timing and software-owner faults.


The v23 terminal presentation path allows a normal GUI after explicit
`polaris-finish` has removed the old framebuffer/GPU owners. An exact active
native/rescue generation binds the new display service; GOP remains retired.
Presenter/compositor faults retain the lease until aliases are removed and can
then be followed by a new session on the same valid output. This terminal
handoff does not rearm the consumed hardware experiment or retain a reserved
GPU rescue driver. Descriptor revocation closes kernel admission; stopping and
reaping live mapping owners remains required. See
[normal presentation](AMD_POLARIS_DISPLAY.md#normal-presentation-after-native-owner-cleanup)
for the test and hardware qualification boundary.


Physical v23 testing now covers real normal GUI sessions on primary and fresh
rescue output after terminal native-owner finish. Presenter/compositor exceptions
retained exclusive ownership until cleanup and restored the verified native
console and complete resource baseline. Fresh normal GUI sessions then displayed
serial-entered text, confirmed visually; Escape and two unassisted controlled
reboots passed. This reuses operational retained scanout and does not qualify
new modes, GPU reset or automatic GPU-backend restart. See
[the physical presentation record](AMD_POLARIS_DISPLAY.md#observed-normal-gui-on-native-and-rescue-output).


The supervised GUI candidate separates backend liveness from the CPU writer.
`polaris-present` first downgrades the backend alias to RO, then releases the
lease while preserving its independent heartbeat channel and reserved rescue.
Revocation makes the original GUI control thread run cleanup. The recovery
worker waits for actual GUI ledgers, lease and kernel writers to drain, with
an eight-second CPU admission limit; it performs no competing GUI cleanup or
IPC. Failure to prove that boundary prevents rescue register operations and
retains uncertain resources. A fresh rescue uses the existing one-shot budget;
normal GUI restart on that generation is explicit. See
[the supervised profile](AMD_POLARIS_DISPLAY.md#supervised-gui-candidate) for
scope, tests and physical qualification status.


The supervised profile is now physically qualified for primary spin while a
normal GUI is active, old-GUI ownership drain before reserved rescue, explicit
normal GUI relaunch on generation2, and rescue exception with another GUI active.
Serial shell control survives terminal output loss. The pre-prepare power query
refuses retained kernel objects; controlled reboot runs the graphics cleanup
preparer,checks quiescence and returns to readable firmware output without
intervention. Automatic terminal-ledger drain and general driver/client restart
remain open. This recovers operational retained scanout;hardware reset and
arbitrary GPU hangs remain unqualified. See
[the supervised hardware record](AMD_POLARIS_DISPLAY.md#observed-supervised-gui-and-fresh-rescue).


The v25 terminal CPU-cleanup candidate defers its final capability release until
original GUI/lease/writer-pin drain. Partial teardown and failed final revoke
remain retained. The read-only power query can collect a completed worker without
polling or stopping active/pending graphics. Host104 controller/worker/build
checks and four clean QEMU boots pass;normal88/88,deep88/88/611. The subsequent physical automatic
release/readiness result is recorded below. Capture the new
`TERMINAL CPU DRAIN WAIT` status and terminal capability-release journal;raw
concurrent lines may still interleave. See
[the cleanup candidate](AMD_POLARIS_DISPLAY.md#deferred-terminal-cpu-cleanup-candidate).


Physical v25 acceptance now confirms automatic final capability release after
a GUI-active rescue exception and original-GUI drain. Before any explicit
power_prepare or status collection,the read-only power query reported shutdown/
reboot ready and storage safe. The completed worker was collected;retired GOP
access stayed refused and serial control survived. Ten GUI sessions,six full
baselines and one readable/unassisted controlled reboot passed. Eleven active
sessions preserved71664 RX/890 TX plus1694 initial boot bytes;raw concurrent
journals remain saved. All captures passed with no GPU replay. See
[the automatic cleanup record](AMD_POLARIS_DISPLAY.md#observed-automatic-terminal-cleanup)
for the image,fixed114-load/four-store scope and remaining limits.


The v26 implementation adds explicit `gui reconnect` after loss and complete cleanup
of a native GUI. A consumed ticket pins the lost output; the Ring3 supervisor
admits one fresh GUI only after the kernel proves the exact rescue epoch and
exclusive ownership. It creates fresh client/surface/input identities and does
not restore or replay old state. Run it after rescue is ready; a request is
single-use even if it fails. No new GPU operation or driver restart is implied.
Host122 controller/640 policy cases and GUI ticket gates pass; four clean QEMU
boots pass,normal88/88 and deep88/88/611. QEMU covers real Ring3 command parsing,
policy CONNECT/duplicate refusal and firmware/no-ticket refusal. The subsequent
physical reconnection result is recorded below. See
[the reconnection profile](AMD_POLARIS_DISPLAY.md#explicit-gui-reconnection).


Physical v26 acceptance now confirms normal Ring3 `gui reconnect` after native
GUI loss, complete old-GUI drain and fresh rescue. One CONNECT authorization
created output-generation-2 clients with new app/input/presenter/compositor
identities; visible RECON26 input was confirmed. No manual rescue presenter
handoff, old-state replay or additional GPU operation occurred. Duplicate and
terminal requests were refused. Ten GUI sessions, six complete baselines,
automatic cleanup after rescue failure, pre-prepare power readiness and readable
unassisted reboot passed. Twelve active captures preserve 69153 RX/872 TX bytes;
initial boot was not captured. The unchanged image used 114 loads/four stores.
See [the physical reconnect record](AMD_POLARIS_DISPLAY.md#observed-explicit-gui-reconnection)
for exact identities, byte scopes and remaining hardware limits.


The normal Ring3 shell now offers `gui native` for the first qualified takeover
and GUI launch. One serialized worker request retains existing policy admission,
exact hardware checks, a fresh primary heartbeat and RO handoff before normal
GUI construction. Refusal starts no implicit firmware GUI and preserves the
shell. Existing or exhausted backends cannot be reinitialized through this entry;
normal `gui` and one-use rescue `gui reconnect` remain distinct requests. No GPU
budget, register set or authority changes. Positive hardware acceptance of this
new launch path is now recorded below. See [normal-shell native launch](AMD_POLARIS_DISPLAY.md#native-gui-launch-from-the-normal-shell).


Physical v27 acceptance confirms `gui native` from the normal Ring3 shell without
manual monitor takeover or handoff, visible NATIVE27 input, repeat-launch refusal,
and GUI-active primary failure followed by fresh rescue/`gui reconnect` with
visible RECON27 input. Ten GUI sessions, six complete baselines, automatic final
CPU cleanup after rescue failure, pre-prepare power readiness and readable
unassisted reboot passed. All eleven active serial captures passed; the unchanged
image used 114 loads/four stores with no replay. Captures preserve 69324 RX/849 TX
plus 1694 attributed post-page-table boot bytes; two leading NUL bytes are saved
and excluded. See [the normal-shell launch hardware record](AMD_POLARIS_DISPLAY.md#observed-normal-shell-native-launch)
for identities, byte scopes and remaining limits.
