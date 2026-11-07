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
