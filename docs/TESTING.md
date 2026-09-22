# JCOS diagnostic suites and bare-metal testing

JCOS keeps destructive kernel diagnostics in the emergency/development monitor.
The normal Ring3 shell remains userspace policy.

## Commands

`test list [GROUP]` lists individual diagnostics.

`test NAME` runs one diagnostic and appends a structured summary. The summary
collects every emitted `...: FAILED` step for that run, so a test may report
multiple failing stages instead of only a final failed result.

`test NAME cleanup` retries the registered retained-cleanup action, where one
exists.

`test all` runs every registry-backed diagnostic once and then the legacy
monitor diagnostics once. The final report includes distinct tests passed and
failed, total run iterations, skipped tests if a safety abort was required, and
all captured failure steps.

`test all deep` repeats stress-sensitive diagnostics. Ordering, lifetime,
scheduler and recovery tests receive the highest repetition counts; deterministic
model checks stay low. `test all-deep` is accepted as a convenience alias.

Deep mode suppresses per-character framebuffer rendering while an individual
test is running. Full diagnostic text still goes to COM1 and remains in the
logical terminal scrollback. The framebuffer is refreshed between tests and for
the final summary.

## Failure and cleanup rules

A diagnostic is not counted as passing merely because it emitted some successful
steps. A normal run must emit an unindented final `...: PASS` or
`...: FAILED` result. If it returns without a final result, the harness records
`NO FINAL PASS/FAIL RESULT EMITTED`.

All `...: FAILED` lines emitted while a test is active are collected. Duplicate
failure text from the same run is collapsed in the summary.

If a registry-backed test fails and provides a cleanup callback, the suite tries
that cleanup before continuing. If cleanup cannot establish a safe baseline, or
the test reports retained/reboot-required state without a cleanup path, the
suite stops and reports the remaining tests as skipped.

Legacy monitor tests do not have a formal cleanup contract. A legacy failure
therefore stops the aggregate suite rather than assuming that partially-created
kernel state is safe for subsequent diagnostics.

## Terminal behavior during long runs

Live output fills the current screen from top to bottom. When the next terminal
line would enter a new screenful, JCOS clears/redraws the framebuffer for the new
page instead of shifting almost the entire framebuffer upward on every newline.
The 512-line logical scrollback remains available.

- Page Up / Page Down: move by a page.
- Ctrl+Page Up / Ctrl+Page Down: move by one line.
- Ordinary editing/navigation returns to the live viewport as before.

These controls work in both the kernel monitor editor and the normal Ring3 shell.

## Bare-metal R7 / USB-GPT verification

For a hardware checkpoint, capture COM1 output whenever practical. A useful
sequence is:

1. Boot the exact image/revision and record the build identity/platform.
2. Verify keyboard input through the intended xHCI controller.
3. Run `test usb-host`.
4. Run `disks` and `partitions`; inspect the actual block devices and GPT
   layout discovered on the machine.
5. Run `test userspace-shell` to cover Ring3 console/input and both page and
   one-line scrollback controls.
6. Run `test all`.
7. Reboot to a clean baseline, then run `test all deep`.
8. Preserve the final summaries and COM1 logs with the tested commit and machine
   details.

A deep-suite pass is stress evidence for the exercised single-logical-CPU
profile. It is not a proof that all possible interleavings, hardware faults or
future SMP races have been exhausted.
