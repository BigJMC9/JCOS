# JCOS diagnostics and hardware validation

JCOS keeps its kernel diagnostic harness in the emergency/development monitor. The normal shell runs in Ring 3. Enter `monitor` from that shell, or use its kernel-owned Escape route, before running the commands below. `usershell` returns to normal userspace interaction.

These instructions describe available checks and their interpretation. They do not record a fresh QEMU or bare-metal test run. Build/image commands are maintained in the [README](../README.md).

## Commands

| Command | Behavior |
| --- | --- |
| `test list` | List the diagnostics in this build |
| `test list GROUP` | Filter the list by group |
| `test NAME` | Run one diagnostic and print its structured summary |
| `test NAME cleanup` | Retry a registered retained-cleanup action, when the diagnostic provides one |
| `test all` | Run registry-backed diagnostics, then legacy monitor diagnostics, once each |
| `test all deep` | Run the same suite with configured repetitions for stress-sensitive cases |
| `test all-deep` | Alias for `test all deep` |

Groups are `memory`, `task`, `ipc`, `lifetime`, `userspace`, `scheduling` and `acceptance`. Group selection filters the list; it is not a command to run an entire group.

Test names and repetition settings come from [test_registry.c](../kernel/tests/test_registry.c) and the shell-local registry in [shell.c](../kernel/shell/shell.c). Use the build's `test list` and `TESTS DISCOVERED` result instead of carrying forward a count from an older image. `TEST ITERATIONS` counts top-level harness runs; a test can perform additional internal cycles.

Registry metadata selects whether a diagnostic needs normal live preemption or a deterministic quiesced policy. The harness restores the previous policy afterwards and reports a failure if it cannot. Deep mode uses higher repetition counts for ordering, lifetime, scheduler and recovery paths; it is not a different execution architecture.

## Useful targeted checks

| Area | Diagnostics |
| --- | --- |
| Core lifetime and IPC | `lifetime-ipc-acceptance`, `lifetime-stress`, `wait-order`, `peer-death`, `timeout` |
| Protection and loader | `permission-audit`, `user-protection`, `user-stack-guard`, `elf-malformed`, `elf-reclaim`, `stack-reclaim`, `vmm-reclaim` |
| Runtime and scheduling | `user-runtime`, `user-runtime-block`, `execution-profile`, `scheduler-idle`, `scheduler-idle-exit`, `runtime-preemption` |
| Launch and services | `program-launch`, `program-startup`, `service-incarnation`, `service-robustness`, `service-policy` |
| Console and archive policy | `userspace-shell`, `console-app`, `console-input`, `files-userspace`, `launch-userspace` |
| Graphics | `polaris-native`, `polaris-read-profile`, `device-register-reader`, `display-probe`, `display-lease`, `display-service`, `display-surface` |
| PCI access | `pci-config`, `pci-assignment`, `pci-bar-resource`, `pci-bar-access` |
| IRQ and DMA lifetime | `pci-irq-resource`, `pci-irq-router`, `pci-dma-buffer`, `pci-dma-submission` |
| Device recovery | `pci-device-recovery`, `pci-driver-hostile` |
| USB models/descriptors | `usb-host` |

Prefix any name with `test`, for example `test pci-driver-hostile`. Use `program-launch`, not `program-launcher`.

## Results and cleanup

A normal diagnostic must emit an unindented final `...: PASS` or `...: FAILED`. Returning without one produces `NO FINAL PASS/FAIL RESULT EMITTED`. Passing intermediate steps do not override a failing step.

The harness captures every emitted `...: FAILED` line while a test runs, collapses duplicate failure text from that iteration and prints failed steps with their test name and run number. Capture storage is bounded; the summary reports omitted failure lines if it fills. Expected user-fault diagnostics can be part of a successful containment test: read the final test result and resource checks, not just the fault banner.

For an aggregate run:

1. A failing registry test with a cleanup callback receives an automatic cleanup attempt.
2. If cleanup cannot confirm a safe baseline, or retained/reboot-required state has no cleanup path, the suite aborts and reports the remaining diagnostics as skipped.
3. A failing legacy monitor test aborts the aggregate suite because it has no formal automatic-cleanup contract.
4. A failed diagnostic remains failed even if cleanup succeeds; cleanup only decides whether subsequent tests may safely run.

An individual `test NAME` run does not automatically perform the aggregate suite's cleanup attempt. Follow its retained-state instructions; use `test NAME cleanup` only when a registered action exists. Reboot when the harness requires it. Do not continue from a baseline whose ownership or device state is uncertain.

Compare resource counts to the fixture's captured baseline, including persistent services. Do not hardcode a historical free-frame count. Matching frame totals alone do not establish correct capability, endpoint, process, mapping, IRQ or DMA lifetime.

## Coverage boundaries

### Emulator and automated checks

The QEMU run targets boot an image for interactive testing; they are not an unattended test runner. COM1 remains connected to the launching terminal and a timestamped host log is saved automatically. For physical-machine capture, use [Console capture](CONSOLE_CAPTURE.md). Preserve boot state, per-step outcomes and the final summary.

The default QEMU setup does not add a native xHCI USB keyboard. `test usb-host` checks capability-field decoding, context offsets, speed/EP0 models, completion classification and descriptor parsing. It does not enumerate a physical keyboard or test its disconnect behavior.

Firmware fallback and native rescue have separate acceptance contracts; see [Graphics recovery](GRAPHICS_RECOVERY.md).

Use `displayprobe` for read-only display PCI/GOP inventory and `displayfirmware` for all six copied firmware queries, source indices and raw descriptor prefixes; see [Display inventory](DISPLAY_PROBE.md). The `display-probe` test exercises decoder refusals and preserved configuration/lease state, including fallback on QEMU.

Graphics checks require a valid GOP resource and an allowed mapping. `display-surface` includes bounded surfaces, composition/focus and failure/recovery scenarios. A passing automated input case does not replace manual verification of the physical keyboard route or the interactive `gui` session.

### PCI and DMA checks

The Ring 3 virtual-driver and hostile-driver scenarios use an enumerated PCI function as assignment identity. The mediator performs CPU-side buffer operations and software IRQ signalling; those scenarios do not program the function's BARs/interrupt route or enable physical bus mastering.

`pci-device-recovery` also checks read-only PCIe/FLR discovery and may exercise an already-inert PCI function. If none is available, it can report `NO INERT PCI FUNCTION / DESTRUCTIVE RECOVERY REFUSED` and finish with PASS for the exercised safe path. Read the individual steps: that outcome is not a hardware-reset result, and it is different from a whole test skipped after a suite abort.

The FLR backend's presence, or a virtual DMA-quarantine pass, does not establish physical DMA containment, IOMMU support, IRQ routing or reset behavior for a specific device. Record separate device/platform qualification before making those claims.

### Stress interpretation

A deep-suite pass is evidence for the exercised single-CPU profile and configured interleavings. Repetition does not prove exhaustive race coverage, general SMP correctness, arbitrary device behavior or immunity to hardware failure. Record actual timing/throughput measurements separately from correctness results.

## Hardware verification sequence

For a hardware checkpoint, retain COM1 output whenever practical and record:

1. The exact commit, local changes, build command and SHA-256 hashes of the image and relevant binaries.
2. Motherboard/firmware, CPU, RAM, display adapter/GOP mode, input controller/port and storage devices. Identify emulator runs separately.
3. Keyboard typing and navigation through the intended native input path. For xHCI, follow [USB Host v1](USB_HOST_V1.md).
4. `disks` and `partitions` from the monitor, with the actual discovered devices and GPT layout.
5. `test userspace-shell` and manual shell history, editing, page scroll and line scroll.
6. `test display-lease`, `test display-service`, `test display-surface`, then a manual `gui` session from the userspace shell. Verify focused input and Escape/terminal restoration.
7. `test all`, preserving its entire summary and failed-step list.
8. A reboot to a clean baseline, then `test all deep` with its complete log.
9. Any conditional/refused hardware paths, cleanup retries, retained resources and deviations from the expected configuration.

The automatic suite includes deliberate userspace faults and hostile-driver probes. Their expected containment and exact cleanup results are part of the test; they are not instructions to run the separate kernel `fault` command.

## Terminal behavior

Live output advances in screenfuls to avoid shifting most of the framebuffer on every newline. The 512-line logical scrollback is retained independently of the live viewport.

- **Page Up / Page Down:** move by a page.
- **Ctrl+Page Up / Ctrl+Page Down:** move by one line.
- Ordinary editing/navigation returns to the live viewport.

These controls exist in the kernel monitor and the normal Ring 3 shell. Deep mode suppresses detailed framebuffer drawing during a test while preserving COM1 output and logical scrollback, then refreshes progress/final results. Long logs can exceed scrollback capacity; use serial capture for a complete record.

## Result record

Keep one record per actual run with:

| Field | Record |
| --- | --- |
| Source | Commit, branch/tag if relevant, and local modifications |
| Artifacts | Build command/toolchain and image/EFI/kernel/archive hashes |
| Platform | QEMU version/configuration or exact hardware/firmware |
| Commands | Targeted checks, aggregate mode and manual scenarios |
| Outcomes | Discovered/run/passed/failed tests, iterations, skipped/refused paths and failed stages |
| Lifetime evidence | Baselines, cleanup/retry results, quarantine and any required reboot |
| Limits | What was not exercised or measured |

Keep public release references independent of protocol versions; see [Versioning](VERSIONING.md).

## Firmware scanout access checks

Run `make -f Makefile.msys2 test-framebuffer-scanout` for hardware-free
regressions against the actual framebuffer, display lease and terminal code.
The three fresh-process scenarios cover idle retirement, simulated preemption
inside rendering and retirement during a multi-row presentation. They verify
that retired boot metadata cannot re-enable framebuffer writes, new GOP
mappings or terminal rendering, and that serial/terminal processing continues.

The `display-lease` and `display-service` diagnostics also verify
retirement refusal while a handoff or real Ring3 direct lease exists. They do
not retire the active test display. `displayprobe` reports boot-scanout
validity independently of the writer lease and saved memory bounds. See the
[graphics recovery contract](GRAPHICS_RECOVERY.md) for native-grant ordering and
the limits of these software checks.

The display-service test also runs the userspace Polaris reader/decoder against
fixed synthetic register data, rejects an unsupported model version and repeats
the check in the replacement service after its injected fault.
`make -f Makefile.msys2 test-amd-polaris-display` runs the broader host
negative-input matrix. Neither path reads physical GPU registers; see the
[AMD Polaris backend reference](AMD_POLARIS_DISPLAY.md).

`make -f Makefile.msys2 test-supervisor-wait` exercises the production
supervisor terminal wait with a simulated completion between state observations,
plus already-completed, scheduling-failure and timeout paths. The wait observes
state and schedules under the existing single-CPU interrupt discipline.

`test device-register-reader` exercises the new capability-checked
semantic read syscall from a dedicated Ring3 process. It verifies the Polaris
adapter's thirty-call capture, malformed requests, owner/rights restrictions,
revocation, stale IDs/handles, owner faults and restored resource counts.
All register values come from a kernel-owned RAM bank; the physical profile
constructor remains unavailable. No GPU mapping or register read is performed.

`make -f Makefile.msys2 test-managed-service-wait` runs the same
deterministic terminal-completion boundary regression against the generic
managed-service wait. The existing `test-supervisor-wait` target continues
to cover the supervisor wait.

`make -f Makefile.msys2 test-polaris-mmio` exercises the production
BAR5 planner/mapping lifecycle with hosted hardware/VM doubles and the real
VMM with hosted page-table RAM. It checks complete raw descriptor/audit matching,
D0/decode, all mapping failure positions, rollback/release retries, retained
assignment pins, stale identity and leaf refusals, RAM/alias conflicts and
strict UC PTE encoding/refusal. No mapped MMIO leaf is accessed.

The existing `display-probe` test also checks the strict four-page planner.
The `pci-assignment` test checks sticky inherited bus-master history in the
real assignment implementation, without enabling DMA or resetting hardware.
The physical reader profile remains unavailable. No bare-metal run is needed
to validate these software-only additions; live GPU reads require separate
hardware qualification and independent serial control.

`make -f Makefile.msys2 test-polaris-read-profile` checks finite capture
ordering/budgets/expiry, immutable physical qualification blockers, and the
production PCI observation/report path using hosted hardware facts. In JA OS,
`test polaris-read-profile` runs the deterministic models.

The expanded `device-register-reader` test also exercises the finite
copied-RAM profile through actual Ring3 syscalls. It checks thirty-read
completion, no rearm, wrong-order/all-ones closure and factory bounds. A spinning
partial-capture process leaves an independent Ring3 console service able to
perform new work, then undergoes deadline refusal and bounded forced cleanup.
A fresh process faults after a partial capture. No physical GPU is accessed.

`display-readiness` prints local RX 570 qualification observations and
unresolved blockers using read-only PCI config. It cannot activate hardware.
Keep its output with a future serial capture; COM1 availability reported by
the OS is distinct from verified input/output on another physical computer.
No fresh monitor transcription is required for these software-only tests.


The offline `audit-polaris-registers` target compares the compiled semantic
selectors, offsets and named decoder masks with the exact upstream files and
SHA-256 values in `docs/POLARIS_REGISTER_REVIEW.json`. Supply the separate
source cache through `POLARIS_SOURCE_DIR`. The target downloads nothing and
does not touch a device. Its source/offset/selector/mask/duplicate refusal
checks and decoder host matrix qualify source agreement only.

The binding host suite also covers kernel operation holds: cleanup closes
admission and retains mappings/assignment pins until all holds drain.
Copied/duplicate token storage, stale identities, capacity overflow and
post-close acquisition are refused. Release is tested after owner death and
quarantine. No MMIO leaf is read.

`display-readiness` version 2 explicitly retains a selected-pipe/display-power
blocker: observed PCI D0 does not prove the chosen pipe's power/clock state.
This report change does not alter the version-1 register-read syscall.


The default-off `POLARIS_LIVE_CAPTURE=1` build enables a separate monitor
experiment, `polaris-capture`. It is excluded from automatic tests and boot.
QEMU must refuse its unsupported target before any load. The host MMIO suite
also covers the production forty-six-read controller, 103 failure/refusal cases,
optional global-page cleanup/poisoning, GMC/HDP/GOP correlation arithmetic and
replay of the measured linear-aligned snapshot. No host test accesses MMIO.

For a physical run, establish independent serial command/output control first;
record the exact image and selected profile, raw bytes and last-read journal.
Capture serial counters and a fresh command after the operation. Confirm new
readable monitor output separately. The once-per-boot controller retains the
bus-master-tainted parent quarantine even after CPU mappings and the Ring3
decoder drain. Do not treat its two-second software admission as a hardware
transaction timeout or its observation as native output/reset/recovery.

The Ring3 reader test additionally checks immutable observation-profile
construction, test-profile relabel refusal, oversized selectors without reads,
thirty RAM reads and four-word metadata queries bound to the reader identity.


The framebuffer host suite additionally checks privileged native/rescue output
publication against simulated framebuffer storage: irrevocable GOP retirement,
retained-range/cache/format/geometry constraints, monotonic descriptor identities,
stale generation refusal, revocation during handoff and admitted drawing,
replacement only after draining and lease reclamation, and refusal of legacy
firmware presentation on native output. The simulated provider supplies no real
hardware proof; automatic tests never publish a fictitious native descriptor
over the running QEMU/physical framebuffer.


`make -f Makefile.msys2 test-display-surface-wait` reproduces completion
between the focused-client diagnostic's queue and thread observations. The
wait now observes and schedules under the same UP interrupt discipline as the
supervisor/service waits. Its deterministic cases cover that preemption
boundary, already-dead and scheduled completion, blocked IPC, timeout, scheduler
failure and malformed queue/thread state. It does not lengthen deadlines or
rerun a failed hardware operation.


`make -f Makefile.msys2 test-polaris-plane` exercises the real Ring3
same-surface rebind planner against the independent kernel preview validator:
all six pipes, five-step ordering, 640 individual tag/payload bit corruptions,
stale identities/replay, invalid layouts and 24-bit frame progress/wrap.
The copied-RAM `test device-register-reader` additionally exchanges two
ordered witness samples and the five recipe steps with the real Ring3 process;
missing/repeated witnesses, malformed selectors and invalidated observations
refuse. These tests grant no GPU writes or native output.

Experimental physical capture profile3 adds six update/flip/frame witness reads
to the existing forty-load batch. The eighteen semantic witness offsets stay
within the existing four pipe pages; host guards check every route and refusal.
The source audit now compares108 offsets and44 field/enum/location facts with
nineteen pinned files, including nine deliberate source/probe corruption checks.
Physical qualification of this expanded batch requires a new image boot,
independent serial logging and separate post-capture monitor confirmation.


The plane host suite accepts only stable flip control0 or0x20, tests62 unknown
flip-bit combinations and rejects mode changes in either direction. The real
Ring3 reader diagnostic includes the observed0x20 witness pair and a separate
preservation marker. Pending-mode is copied data; no hardware bit is cleared.

`tools/replay_polaris_plane.py --raw-log path/to/serial.raw.log` runs the
actual discovery/decoder/planner, kernel VRAM correlation and independent
preview using a complete recorded profile3 journal. It refuses partial/changed
samples and invalid flip state. The optional preserved policy sources and
`--expect-refusal` reproduce the original exact-zero preflight. Replay is
a host test and does not demonstrate a physical recipe exchange, new reads,
native output or recovery. Keep the physical refusal and subsequent replay
as separate results.

`test polaris-native` maps private test RAM into distinct actual primary/
rescue Ring3 processes. It checks a supervisor-owned assignment model without
hardware access, refusal of ordinary kernel claim, RO-store fault containment,
spin timeout, independent retained rescue state, stale PID/connection refusal,
allocation-free RW promotion, original launch identity after failure/reap, and
full resource cleanup. Primary and rescue title checks compare every foreground
and background bit against the canonical MSB-first font, including asymmetric
glyphs; a merely present white pixel cannot pass mirrored text.
It never retires or programs the live QEMU/physical display.

`test-polaris-program` exercises the production executor with semantic IO
doubles: complete validation before IO,640 tag/value corruptions, transaction
failures, one HIGH/LOW pair, posted read, post-write frame baseline, bounded
pending drain,24-bit completion wrap and no replay. `test-polaris-native`
exercises actual orchestration with stateful doubles: retirement before writable
authority, exclusive old-owner teardown, one reserved rescue under retained
quarantine, watchdog outcomes and retained failed cleanup. Failed/stopped-service
doubles hide the live PID and clear reaped process storage, matching the real
API; the original lease owner must come from the driver's retained identity. The MMIO/VMM suite
also checks real PTE WRITE-only changes and fixed-store guards.

`POLARIS_NATIVE_WRITE=1` is an independent default-off experiment. QEMU
must refuse the unsupported target before mapping or GPU IO. A physical run
requires a new exact image boot and independent serial control. Test the
RAM-only diagnostics before takeover, then separately confirm primary text,
rescue text and the reclaimed native/rescue console. Keep logs/hashes and
record any controlled refusal. The inherited GOP is permanently retired before
RW authority; old firmware-only diagnostics cannot establish a fallback after
that point.

The native RAM diagnostic now challenges both actual Ring3 roles and exercises
four acknowledged branches: primary/rescue fault and spin. Each measures an
independent Ring3 console portal-write increment and checks the original peer
identity, liveness failure, forced/graceful cleanup and full baseline. The
controller host suite has57 cases, including heartbeat timing, failed standby/
rescue, acknowledged injection, lost ACK/progress and exhausted test budgets.
A separate host harness links the production RPC/progress helpers to test
12 corrupt/failed heartbeat cases and28 malformed/failed diagnostic ACK cases,
plus stale peer/console identities and absent actual work. None accesses a GPU.

Physical watchdog acceptance requires a fresh boot of the exact write image,
correct primary output, `polaris-test primary spin` and independent-progress/
automatic-rescue markers, then a separate visual check of fresh rescue text.
A deliberate active-rescue fault/spin should leave serial control and quarantine
without another GPU transaction. Record clean-state power readiness separately
from actually performing a controlled reboot. Never use these cases to infer
GPU reset, device-hang survival or unrestricted repeated GPU attempts.

The recorded physical watchdog run exercises primary SPIN -> monitor heartbeat
failure -> fresh rescue,then active rescue #UD -> headless quarantine/serial
and clean-state reboot readiness. Ring3 console portal counters advance
1385->1386 and1386->1387 at separate checkpoints. Four GPU stores/114 reads
are recorded,with no extra GPU operation after rescue failure. A controlled
reset booted Windows;operator boot-priority correction preceded the captured
JA OS USB return. Preserve that assistance in the result instead of reporting
an unattended JA OS reboot. The new boot has firmware-valid output and zero
native/watchdog/test counters;no second GPU experiment was started there.

The second fresh-boot v19 case separately exercised primary terminal #UD ->
automatic rescue,then active rescue SPIN -> heartbeat failure/headless
quarantine. Both independent Ring3 console checkpoints completed. Five
exhausted-state requests were denied with no additional GPU access; serial
and power readiness remained usable. Controlled reboot returned to a readable
JA OS firmware console without operator intervention. This complements the
earlier primary-SPIN/rescue-#UD case; it does not replace its assisted-return
record. See the complementary run in [Polaris display](AMD_POLARIS_DISPLAY.md).

`test graphics-policy` covers actual supervisor IPC,ordered sessions,stale
and malformed requests,primary exit/heartbeat recovery,failed-standby priority,
rescue/headless closure and unrelated ping with unchanged resource counts.
`make -f Makefile.msys2 test-graphics-policy` adds production policy/client
host checks; `test-polaris-native` includes75 controller cases. The policy
candidate passed four clean QEMU boots:focused,two-GPU,normal86/86 and deep86/86
with603 iterations. These software checks perform no physical GPU programming
and do not replace the separately observed physical RX570 policy-path case.

The v20 physical case passed three targeted1/1 preflight suites,then primary
spin/Ring3 action2/fresh visible rescue and rescue #UD/Ring3 action3/headless
fallback. Independent console work advanced1385->1386->1387 in two checkpoints.
Five exhausted-state requests were refused with no additional GPU MMIO access.
Unassisted controlled reboot returned to readable firmware output and status0/0/0.
Totals114 GPU loads/four stores,no retries,46866 v20 RX/491 host TX;182 earlier
shutdown bytes are saved separately. This is one monitor-scoped policy run,
without broader modes/HDMI/reset/isolation or autonomous supervision proof.

`test graphics-worker` runs actual periodic IPC work with real Ring3 policy/
driver RAM faults. It checks monitor-independent primary spin/rescue fault,
policy fault/spin,unrelated console work,serialized supervisor clients,exact
failed-policy stop,direct power readiness before replacement,stale cleanup and
full object/page/cap baselines. `test-serial-worker` runs worker retention,
admission and adapter host checks; `test-polaris-native` now has80 cases.
Delayed diagnostic commands require an active Ring3 shell after a fixed
three-second minimum and do not issue a competing worker console RPC.
Four clean QEMU boots PASS:focused,multi,normal87/87/87iterations and deep87/87/607iterations;exact failed-policy stop,power readiness before replacement and replacement immunity PASS.
These are software checks;physical off-monitor/policy-failure acceptance
requires the concrete image and independently captured serial/visual evidence.


Physical v21 qualification passed four preflight suites and three distinct
native cases:primary spin/fresh rescue plus rescue exception;policy exception;
policy spin. Injections occurred with the normal Ring3 shell active,distinct
worker/control identities and completed help commands before/after. Terminal
cases retained serial/power readiness;each ended with captured reboot output.
The first return also has visual/no-intervention confirmation. Physical active
policy replacement remains untested;replacement immunity passed the actual
RAM-only worker suite.

The record contains234 GPU loads/eight store attempts without replay,
98279 received bytes/972 host-transmitted bytes. Original boot and capture
gaps are excluded. A failed host-banner expectation followed successful rescue;
the preserved transcript,CPU-only continuation and separate corrected helper
record this tooling issue. The offline verifier uses the unchanged two-frame
production minimum. See [the physical worker record](AMD_POLARIS_DISPLAY.md#observed-recovery-outside-the-monitor).


The rebind execution candidate adds host target test-polaris-broker. It runs
the real Ring3 sequence against the independent kernel gate, checks2048
request tag/payload corruptions before IO and640 malformed plans, and covers
pending updates, counter wrap, poll exhaustion and bounded no-replay state.
A separate production transport test covers success plus22 malformed,
partial-write, deadline, identity, forged-completion and readiness cases.

The actual test polaris-native now runs the real primary/rescue ELF and
private broker against RAM effectors before draw tests. It checks incomplete
post-write failure, consumed-attempt refusal and retained RO mapping, then
restores the complete resource baseline. The older synchronous executor
remains a host reference and is excluded from the kernel binary.
Four clean QEMU boots PASS: focused, multi, normal87/87 and deep87/87 across607 iterations; actual Ring3 broker completion, partial-failure RO retention/no replay and full baseline checks PASS. Physical v22 execution/recovery is recorded below.


The physical v22 pass completed four preflight suites and three native
cases: primary spin/fresh rescue then rescue exception; policy exception;
policy spin. Actual user sequences completed through the independent kernel
gate before output publication. Normal Ring3 help completed before/after
injected failures, with serial/power readiness and three captured reboot
returns. The first return also has visual/no-intervention confirmation.
No production or image change followed the tested software candidate.

The raw verifier records234 GPU loads/eight stores, no retry, and no GPU IO
after terminal failure. Active RX98944/host TX972, plus1605 partial initial
post-reset boot bytes, give100549 image-scoped received bytes. An additional
131-byte unattributed pre-reset prefix remains saved and excluded. All raw
received bytes total100680; capture gaps are not claimed. See
[the physical Ring3 execution record](AMD_POLARIS_DISPLAY.md#observed-ring3-execution-and-rescue).


The normal-native presentation candidate adds
`python tools/test_native_presentation.py --build-dir build/host-tests`, nine
production-code host launch/lease/cleanup cases using simulated MMIO and IPC.
`test native-presentation` refuses native admission on firmware boots, explicitly
without claiming a positive native run. After real native/rescue validation
and either terminal `polaris-finish` or supervised `polaris-present`,it exercises three GUI launches, application
input, separate composition, presenter/compositor exceptions and complete
resource reclamation on the retained generation. It adds no hardware operation.
Four clean QEMU boots PASS: focused, multi, normal88/88 and deep88/88 across611 iterations; actual firmware GUI/serial input/console restoration and native-admission refusals PASS. The subsequent physical result is recorded below; see
[the candidate profile](AMD_POLARIS_DISPLAY.md#normal-presentation-after-native-owner-cleanup).


Physical v23 acceptance passed two positive native-presentation suites, on
primary generation1 and fresh rescue generation2, after four preflight suites
and a second firmware admission check. Six actual GUI/input/composition and
full-resource-baseline cycles passed, including two presenter and two compositor
exceptions. Two additional normal GUI sessions visibly displayed serial-entered
NATIVE23 and RESCUE23. Escape, shell help, exhausted-attempt refusals, serial and
power readiness passed. Both captured controlled reboots have operator-confirmed
readable/unassisted returns. No GPU register operation followed terminal finish.
See [the physical GUI record](AMD_POLARIS_DISPLAY.md#observed-normal-gui-on-native-and-rescue-output)
for the exact image,174 register loads/six stores and raw-byte/correction scopes.
This tests retained native/rescue presentation, not new modes or hardware reset.


The supervised GUI candidate extends `test-polaris-native` from80 to96 host
controller cases. Sixteen new cases exercise RO backend parking, live GUI
heartbeats, pending rescue, retained GUI/lease/kernel-writer gates, timeout,
policy/rescue/context loss and no extra GPU operation. `test-serial-worker`
checks dispatch and power refusal preserving the active worker. The actual
`test polaris-native` RAM path now verifies an RW-to-RO downgrade before an
actual Ring3 store faults; this is CPU protection evidence, not GPU recovery.
Normal GUI tests also verify the drained query during/after a live session.
Four clean final-image QEMU boots PASS:focused,multi,normal88/88 and deep88/88 across611 iterations. Actual Ring3 RW-to-RO write-protection fault and normal GUI/drained-ledger checks PASS. Subsequent physical supervised GUI evidence is recorded below. The subsequent physical active-GUI result is recorded below; see [the candidate](AMD_POLARIS_DISPLAY.md#supervised-gui-candidate).


An initial v24 focused run exposed the standalone surface-app test wait rejecting
a legal READY task with a completed IPC wait reservation. The original helper
fails a deterministic reproduction. It now uses a coherent UP queue/thread
snapshot,matching the focused-client wait,while preserving the two-second
budget and complete exit/watch/terminal-state requirements. The original failed
image,log and test source are retained separately from final-image qualification.


Physical v24 qualification passed both positive native-presentation suites while
the backend stayed RO and supervised: six actual GUI/input/composition cycles,
six complete baselines,two presenter and two compositor exceptions. Two visual
GUI sessions displayed NATIVE24/RESCUE24. Two further GUI sessions were active
during delayed primary spin and rescue exception. Policy-selected fresh rescue
followed proven old-GUI teardown;final headless output loss retained serial
shell control and refused five exhausted GPU requests. One prepared controlled
reset returned readable and unassisted. The earlier power readiness query
correctly refused retained kernel objects;automatic terminal-ledger drain remains
open. No production/image change or GPU replay was made for host assertion
corrections. See [the exact hardware evidence](AMD_POLARIS_DISPLAY.md#observed-supervised-gui-and-fresh-rescue)
for114 loads/four stores,serial byte scopes and concurrent-journal verification.


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


The normal shell adds `gui restart-driver` for one explicit primary CPU process
replacement on an intact RO native scanout after complete GUI drain. Ring3 policy
admits and completes the transaction. The old process must be fully reaped before
a fresh RO primary adopts the retained output; programming/drawing requests are
refused in its new entry. CPU identities change while output generation stays
fixed. No GPU operation or budget rearm occurs, and the reserved rescue remains
available for a later primary fault. Failed or ambiguous replacement retains
quarantine/CPU ledgers and serial access. Physical acceptance is recorded below.
See [the retained restart contract](AMD_POLARIS_DISPLAY.md#retained-primary-driver-restart).


Physical v28 acceptance confirms one normal-shell `gui restart-driver`: primary
PID 27/incarnation 23 became PID 32/incarnation 27 on unchanged output 1,
with fresh GUI identities and visibly readable RSTART28 input. The restart added
no register operation; duplicate and terminal restart refused. The replacement
primary's later GUI-active hang used reserved rescue, followed by one fresh
`gui reconnect` with visible RECON28. Eleven GUI sessions/six complete baselines,
rescue failure/automatic CPU cleanup, pre-prepare power readiness, serial control
and readable unassisted reboot passed. Twelve active captures preserve 72171 RX/
910 TX bytes, plus 1694 attributed passive boot bytes; 133 leading passive bytes
are saved and excluded. Fixed 114 loads/four stores/two polls, without replay.
Initial rescue identity was not journaled; original reservation retention follows
implementation/controller evidence. See [the physical retained-primary restart record](AMD_POLARIS_DISPLAY.md#observed-retained-primary-cpu-driver-restart)
for identities, byte scopes and the healthy CPU-replacement limit. Hardware GPU
reset, arbitrary hangs, changed modes/HDMI and isolation remain open.


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


ROM routing coverage adds 27 malformed/boundary cases and 513 protected routing
prefixes alongside the 513 base-prefix/26-vector tests. Actual Ring3 tests in
`test display-probe` compare twelve golden route words and verify that one bad
route is cleared while its directory and peer survive. The four final-image
QEMU boots pass focused/multi-GPU, normal 88/88 and deep 88/88/611. An initial
stack-guard failure is retained with its image; final runs use the corrected
process-private report storage. Hardware route acceptance remains pending.
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


`test display-probe` now exercises bounded reserved-ROM RAM windows, final-byte
access, short-copy zero tail and zero/oversize/cross-end/overflow/source-change
refusals with cleared output. Four final-image QEMU boots pass focused/multi,
normal88/88 and deep88/88/611; focused command checks cover invalid decimal
arguments and unqualified-source refusal. Physical raw-window capture is pending.
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


Single-byte record regressions cover a peer starting immediately after FF,
exact ObjectInfo end, missing ordinary headers, the64-record budget and a
protected page immediately after the token. The actual Ring3 fixture prints
`ACTUAL SINGLE-BYTE ROM TERMINATOR / NEXT SECTION PRESERVED: PASS` and retains
full CPU baseline checks. All four final-image QEMU boots pass;actual-copy host
replay checks all48 packed route words. Fresh physicalv33 reports are pending.


Physical v33 routing qualification passes:four reports/all48 golden fields,
actualRing3 single-byteFF/peer preservation/fullCPUbaselines and unassisted
controlledreboot. No new GPU or GPIO/AUX/I2C/HPD transaction ran.
See [the physical routing record](AMD_POLARIS_DISPLAY.md#physical-terminator-and-routing-qualification).


The direct firmware topology decoder passes 33 malformed-input and boundary
cases, 513 protected prefixes, 568 byte mutations, and final-byte terminator and
truncated-header checks. The actual Ring3 fixtures report:

- `ACTUAL RING3 DIRECT PATH / ENCODER / LINK AND CAPABILITY CORRELATION: PASS`
- `ACTUAL TOPOLOGY REFUSAL / CLEARED TAIL / CONNECTOR AND PEER PRESERVED: PASS`

Host replay of the captured ROM preserves all 48 route words and matches all
16 new topology words. All four final-image QEMU runs pass. Fresh physical v34
reports remain pending.

The initial normal run passed 87/88 tests; its native RAM reservation failure
and image are retained. After correcting the readiness observation race, all
four checks were rerun on the final image.


Physical v34 topology qualification passes: four reports match all 64 expected
fields. Actual Ring3 parsing, refusal, peer preservation and complete CPU
resource checks pass, followed by an unassisted controlled reboot. No GPU or
GPIO/AUX/I2C/HPD transaction ran in this pass.
See [the physical topology record](AMD_POLARIS_DISPLAY.md#physical-firmware-path-and-encoder-qualification).


The copied DIG decoder is checked with `python tools/test_polaris_link.py` and
`python tools/audit_polaris_link.py --source-dir build/rx570-link-v35/references`
(with the pinned source files present).
Actual Ring3 fixtures in `test display-probe` emit:

- `ACTUAL RING3 COPIED LINK / UNIQUE CRTC-FE-BE / GOLDEN REPORT: PASS`
- `ACTUAL RING3 LINK CHANGED/ALL-ONES / CLEARED REPORT: PASS`
- `ACTUAL LINK DUPLICATE INDEX / PROCESS CLOSED / NO REPLAY: PASS`
- `ACTUAL LINK PROBE / FULL CPU RESOURCE BASELINE / NO GPU IO: PASS`

The focused/two-display/normal88/deep611 final-image runs pass. These CPU tests
use copied fixtures and confer no physical MMIO qualification. `polaris-link`
refuses the unqualified QEMU GPU before assignment or reads. Positive 52-load
capture, exact mapping teardown, quarantine and continued serial/display access
require the matched v35 image on the selected board. The current command has
one admitted attempt per boot and no retry. See
[the profile and limits](AMD_POLARIS_DISPLAY.md#bounded-read-only-dig-observation).


Physical v35 completed52 ordered read-only DIG/CRTC loads with matching paired
samples and CRTCbrackets;freshRing3 and independent host decoding agree on
CRTC0-FE2-BE2. Checked MMIOcleanup/quarantine,duplicateattemptrefusal,continued
serial/readabledisplay and unassisted controlledreboot pass. No GPUstores/polls
or newnativeprogramming ran;native changedmodes/HDMI/reset/isolation stay open.
See [the physical DIG record](AMD_POLARIS_DISPLAY.md#physical-read-only-dig-qualification).


The inherited timing decoder is checked with `python tools/test_polaris_timing.py`
and `python tools/audit_polaris_timing.py --source-dir build/rx570-timing-v36/references`
(with the pinned sources present). Actual Ring3 timing golden/raster independence,
changed/all-ones/geometry refusal, early query/cross-role/duplicate/wide-value
process closure and full CPU baselines extend `test display-probe`. The kernel
registry and suite totals remain unchanged: normal88 and deep611 iterations.
All four final-image QEMU runs pass. Positive physical timing reads/cleanup still
require the matched v36 image; CPU raster fixtures are not physical mode changes.
See [the timing profile](AMD_POLARIS_DISPLAY.md#bounded-inherited-timing-and-dp-stream-observation).


Physical v36 completed80reads and checkedcleanup/quarantine, but its strict
Ring3 report refused the changing automatic M measurement (758D->758B);status3
cleared the tail. The original failed session is retained. Serial/readableoutput
and unassistedreboot pass;no GPUretry/stores/polls/nativeprogramming ran.
See [the timing refusal record](AMD_POLARIS_DISPLAY.md#physical-timing-capture-and-automatic-m-refusal).


The timing version2 tests replay all80 actualv36 register words against nine
independent expected report words. The copied actualfixture contains register
values only. Host and actualRing3 tests refuse manualM/reservedM/N/GEN changes
and a fresh version1 BEGIN;CPU resources return to baseline. The extended audit
checks eight source hashes/14offsets/20facts. All four v37 final-image QEMU runs
pass (normal88/deep611). Positive physical version2 capture remains pending;
the v36 failed semantic session stays failed and sealed.
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
