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
| Graphics | `display-lease`, `display-service`, `display-surface` |
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

The QEMU run targets boot an image for interactive testing; they are not an unattended test runner. COM1 is connected to the launching terminal. Preserve the full serial transcript, including boot state, per-step outcomes and the final summary.

The default QEMU setup does not add a native xHCI USB keyboard. `test usb-host` checks capability-field decoding, context offsets, speed/EP0 models, completion classification and descriptor parsing. It does not enumerate a physical keyboard or test its disconnect behavior.

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
