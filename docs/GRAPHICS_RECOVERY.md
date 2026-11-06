# Graphics recovery contract

Graphics failure should leave the OS running with an independent diagnostic
path. Recovery has three levels. Each hardware profile must qualify its own
access, DMA, interrupt, reset and output-restoration guarantees.

| Level | Preconditions and behavior | Current implementation |
|---|---|---|
| Firmware framebuffer recovery | Firmware-established scanout is still operational and has not been disrupted. Stop/reap the failed display service, remove its mappings, then reclaim the exclusive framebuffer lease for emergency diagnostics. | Existing GOP service recovery and lease tests; physical environments require their own validation. |
| Native GPU rescue | After native takeover, contain the primary driver, quarantine the device and retained DMA, and revoke its access. A separate minimal Ring3 rescue driver uses fresh, constrained recovery authority to reset/reinitialize a basic diagnostic display. | Required design; no native or rescue backend is implemented/qualified yet. |
| Serial and controlled reboot | If in-place recovery is unsafe or fails, retain COM1 input/output and independent service progress. Keep unsafe resources quarantined. Offer a controlled reboot when its cleanup/storage contract permits it; firmware may initialize GOP on that boot. | Serial monitor and controlled reboot exist; behavior with a failed native GPU remains unqualified. |

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
mechanisms exist; a native hardware provider and device isolation remain open. A native transaction must retire firmware fallback **before granting
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

Normal boot and diagnostic commands do not retire the live framebuffer. The
retirement entry has no userspace syscall or monitor command; a future qualified
native-grant transaction must call it before exposing disruptive authority.
The current image keeps firmware scanout active on the RX 570.

## Active native/rescue descriptor lifecycle

The privileged `framebuffer_native_install()` entry publishes a descriptor
only after firmware retirement, drained kernel writers and a KERNEL CPU lease.
Its caller must independently validate actual operational scanout first. The
constructor performs no hardware operation and accepts no userspace request.
No normal boot, monitor command or automatic test calls it on a live output.

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
