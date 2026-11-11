# AMD Polaris display backend

Physical DisplayPort qualification includes fixed vertical-register writes,
fresh rescue restoration, and serial containment when the rescue driver itself
fails on the exact tested RX 570 profile. See [the recovery record](#physical-rescue-driver-failure-and-serial-containment-qualification)
and [the stream capture](#physical-dp-stream-and-update-control-qualification)
for scope and remaining limits.

The display service links a userspace register reader and decoder in
[user/drivers/amd_polaris_display.c](../user/drivers/amd_polaris_display.c).
Normal display-service diagnostics use synthetic fixtures. A separate, default-off
kernel-monitor experiment can sample physical registers on the exact selected
RX 570 candidate. It closes and unmaps live access before a dedicated Ring3
process decodes immutable copied values. A second default-off experiment now
runs a constrained Ring3 same-buffer rebind and a separately reserved rescue
entry. It preserves the inherited timing/link; full modesetting, reset and
connector programming remain unimplemented. Physical writes/recovery require
their own acceptance evidence.

## Register contract

The target register generation is DCE11.2: AMD's
[ASIC mapping](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/display/dc/core/dc_resource.c)
maps Polaris10 to that generation. Register facts come from AMD's
[DCE11.2 offsets](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h)
and [field masks](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_sh_mask.h).
These pinned references describe register locations and fields, not proof that
reads are harmless on a particular powered/reset device.

The six pipe register banks are nonuniform. The backend converts the reference's
DWORD indices into byte offsets and accepts fifteen semantic fields per pipe:
CRTC control/blank/interlace; graphics enable/control/swap; primary surface
low/high address; pitch; surface offsets; graphics start/end coordinates.
It exposes neither arbitrary register offsets nor writes through its API.
The offset helper checks plan arithmetic and the supplied span only; that span
is not proof of a BAR's identity, assignment or accessibility.

A capture preflights every field before its first callback, then takes two
samples with at most thirty reads. Failed/all-ones/omitted reads and changed
samples leave the entire output clear. There is no polling or retry loop.
Callback duration needs a separate execution contract. Equal samples do not
prove atomic capture, exclude intervening changes, or certify the active output.

## Decoded observations

The decoder retains the primary surface address in the GPU address space.
It never converts it to a CPU physical address or adds it to a framebuffer BAR.
GRPH extents describe the surface and are distinct from viewport/timing and
monitor mode. AMD's [display programming implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/dce_v11_0.c)
programs those separately.

Observations report depth, format, array mode, pitch and restrictions. Only the
narrow zero-offset, progressive, untranslated, linear-general or linear-aligned
32-bit format subset can
produce a linear byte-span calculation. Unsupported controls, tiling, swapping,
geometry, reserved bits and address overflow suppress that span. The range
remains a GPU-space arithmetic result: a zero restriction mask is not a
validated allocation, CPU mapping, connector identity or permission to reuse
scanout. Color/LUT/viewport/link validation remains outside this decoder.
AMD's [DCE array enum](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_enum.h#L5107)
defines linear aligned as array mode 1. That layout permits bank/pipe metadata
without interpreting the surface as tiled. The accepted aligned subset requires
256-byte row alignment; privilege, Z, microtile and untranslated-format limits
remain enforced. A replay of the measured GRPH_CONTROL=0x0A10800A,
primary=0xF400000000, pitch1024, extent1024x768 yields a 0x300000 byte span.

A physical read broker must independently bind the selected device and
incarnation, validate the real BAR and power/read preconditions, enforce its
semantic operation allowlist outside the requesting driver, and qualify the
diagnostic/recovery profile. Userspace validation is not that protection boundary.
Before any disruptive authority, it must follow the existing
[scanout retirement contract](GRAPHICS_RECOVERY.md).

## Diagnostics

`make -f Makefile.msys2 test-amd-polaris-display` links the actual userspace
module against host register fixtures. It checks all six pipe maps, aperture
edges, each read failure/absence boundary, every changed sample position,
unsupported layouts/formats, geometry and address limits, malformed snapshots,
and the fixed Ring3 diagnostic vectors.

The display-service protocol adds a diagnostic-only operation to version 1.
It accepts exactly a reserved zero word and model version 1, uses only compiled
synthetic input, and returns six check bits under the existing service
incarnation. Unknown model versions are refused. It accepts no address, raw
register, snapshot or hardware-authority request.

`test display-service` runs that operation in the real Ring3 process,
checks version refusal followed by a successful ping, injects the existing
service fault and runs the model again in a fresh service instance. This is
userspace integration evidence, not an independent native rescue path.
No new bare-metal register access is exercised by these diagnostics.

## Protected semantic read interface

`include/device_register_abi.h` defines additive syscall 12 and request
version 1. The request carries a reader capability, expected resource identity,
pipe and named field. It accepts no pointer, physical address, byte offset,
width or write. Oversized selectors cannot alias another field through packing.
The shared `include/amd_polaris_registers.h` compiles the same register
facts into the kernel validator and userspace driver; decoding policy stays in
userspace.

The kernel reader checks registered resource membership, the current live owner,
READ rights, exact resource identity, request version, pipe/field allowlist and
revocation before returning a value. Capabilities are issued without WRITE,
MANAGE or TRANSFER. Revocation is permanent for that resource; replacement needs
a fresh resource ID and capability. Capability references retain the object
through owner failure and prevent early destruction. A terminal owner loses
read access before process reaping.

The kernel diagnostic constructors create either
`JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST` (unlimited test-bank reads) or
`JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST` (the finite capture below).
Both copy a fixed-size bank into protected RAM and install no hardware callback
or mapping. The distinct physical profile identifier is refused; no flag, userspace request or serial connection can turn
a test bank into physical authority. This is a working capability/syscall
mechanism. The separate kernel BAR5 preparation path below does not activate
this physical syscall profile or attach a live read backend.
The kernel-only observation constructor uses a separate profile identifier 2,
copies a completed physical snapshot into RAM and grants exactly thirty ordered
RAM reads. Test constructors cannot create or relabel that profile. No syscall
executes MMIO; profile 1 (live per-register access) still has no constructor.

`test device-register-reader` launches `registerprobe.elf` with IPC
endpoints, then issues the READ-only test resource to that process. The Polaris
adapter captures through thirty real syscalls. The diagnostic checks malformed
requests, wrong object/owner/rights, copied-bank immutability, revocation, retained
references, old handles/identities after replacement, owner fault and complete
resource cleanup. It does not give the probe a framebuffer or GPU mapping.

Before a physical reader can be constructed, the broker still needs a reviewed
exact-device safe-read profile and verified independent diagnostic control.
The staging path below supplies nonprobing bounds, mapping and lifetime
mechanisms; its power/config observations do not establish read safety.
CPU capability revocation alone proves neither IRQ containment nor DMA quiescence. Native modesetting and rescue remain
separate implementation and hardware qualification work.

## Kernel BAR5 preparation

`kernel/polaris_mmio.c` implements a separate privileged preparation lifecycle.
It accepts the currently observed board candidate 1002:67DF revision EF,
subsystem 1458:2310, the exact selected GOP PCI provenance, a live exclusive
assignment and its owner capability. This candidate identity is not a reviewed
hardware-read or recovery profile. Only the explicit opt-in monitor experiment
described below calls this constructor; normal services and syscalls cannot.

Preparation requires a nonprefetchable 32-bit BAR5 of 256 KiB. It reparses the
complete 48-byte, single-descriptor firmware resource list and checks both
saved audits against the decoded result. Inventory-only truncated prefixes and
multi-descriptor responses cannot authorize this first mapping. The firmware
query index remains distinct from the physical BAR index. A plan must cover
all ninety semantic selectors using exactly four 4 KiB pages at BAR offsets
0x6000, 0x7000, 0x10000 and 0x11000. Address-correlation profile 2 can separately
prepare a fifth RO/NX/UC page at BAR5 offset 0x2000, under the same pins and
ledger, for exactly MC_VM_FB_LOCATION and HDP_NONSURFACE_BASE. It is not part
of the four-page pipe plan. Preparation refuses active holds, stale identity,
aliases, RAM conflicts and changed configuration. Cleanup removes the optional
leaf before the pipe leaves and retains/poisons inconsistent cleanup state.

The live preparation checks bounded standard PCI configuration, PM capability
version 1-3, D0 and memory decode before mapping, then checks identity, power,
command and BAR data again afterwards. These are observations, not a hotplug,
power-transition exclusion or harmless-MMIO-read guarantee. Conflicting UEFI
memory descriptors and existing kernel identity/physmap aliases are refused;
the code does not implement a system-wide physical alias registry.

Leaves occupy a fixed reserved window in shared supervisor PML4 slot zero.
They are read-only, non-executable and inaccessible to Ring3. Creation/removal
requires the active kernel page map, UP/no-PCID, NX and CR0.WP. The new
`VM_DEVICE` flag selects PCD=PWT=1, PAT=0 only after checking PAT support,
enabled normal caching and IA32_PAT entry 3 set to UC. The existing
`VM_UNCACHED` encoding remains unchanged. PAT selection follows the
[Intel SDM, volume 3A, memory cache control/PAT sections](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf).
No PAT, MTRR or cache-mode programming is introduced.

A fixed four-record ledger pins both assignment child lifetime and active
CPU access before installing leaves. Each registered binding has a monotonic
identity. Up to sixteen kernel hold records retain bindings across prospective
CPU operations; these records grant no register-read permission. Failed preparation rolls back; failed
unmapping or pin release retains the exact remaining record for retry, including
after owner death or quarantine. Teardown verifies the original assignment ID,
page-map root, each frame and its permissions before removing a leaf, then
releases access and child pins in that order. It never frees an MMIO frame.
An impossible inconsistent unmap result poisons the record and retains its pins
for investigation. Exported kernel page-table scaffolding is boot-pinned by the
existing VMM; at most two additional table pages serve the entire reserved
window. Leaf cleanup is distinct from reclaiming that shared scaffolding.

If configuration shows inherited bus mastering, the assignment records a
sticky exposure taint before full validation. Failed decoding, mapping and
cleanup cannot erase it. This observation neither enables DMA nor isolates the
device. Ordinary release remains refused; quarantine needs an independent
quiescence/recovery proof before reuse.

Preparing page tables alone performs no CPU MMIO transaction. Separate
kernel-only semantic load entry points require an exact retained hold, live
owner, active kernel CR3, unchanged RO/NX/UC leaf and enabled interrupts. They
accept named pipe fields, the two location selectors or the three plane
witness selectors, never a raw offset. Witness reads use the existing four
pipe pages and the same owner/hold/leaf/interrupt checks.
A fenced aligned DWORD load executes with interrupts enabled. The experiment
controller limits ordering and total operations; holds alone grant no access.
This does not establish general safe-read qualification, DMA containment or
native display recovery.

`make -f Makefile.msys2 test-polaris-mmio` tests the production planner,
descriptor decoders, mapping/cleanup lifecycle and VMM page-table code. Hosted
CPU/PCI/assignment/VM doubles inject each map failure, partial rollback, release
failure, owner fault, identity/permission drift, aliases and RAM conflicts.
The separate production-VMM fixture inspects actual PTEs without accessing
their fabricated MMIO leaf. `test display-probe` includes the strict planner
vectors; `test pci-assignment` checks real sticky assignment bookkeeping using
a software-only history observation. QEMU and host checks do not exercise
physical BAR5 preparation or qualify GPU reads.

## Finite capture admission and readiness

The distinct finite copied-RAM resource exercises the admission contract intended
for the first physical observation. A kernel constructor selects one pipe and
a nonzero duration of at most two seconds in current timer ticks. It grants
READ only. The existing version-1 syscall request and return registers are
unchanged; neither diagnostic constructor can create the physical profile.

The kernel admits fields 0 through 14 in the shared semantic enum, then the same
fields once more: at most thirty successful admissions for that one pipe.
It checks ownership/identity/rights before the capture budget. An in-range
selector in the wrong order or for another pipe, changed clock frequency,
elapsed deadline, explicit revocation or an all-ones value closes the capture.
Malformed selectors rejected before resource resolution perform no read and
do not alter another resource. The thirtieth admission returns its value and
permanently seals the resource. Completion cannot rearm the same capability;
a new resource ID/capability requires destruction and a fresh constructor call.

Expiry is checked at admission; it does not run a timer-driven reaper. Faulted
owners cannot read, and cap references retain the resource through exit/reaping.
A closed or expired capture does not free buffers, certify DMA quiescence,
cancel an issued device transaction or restore a display. Current values still
come entirely from protected RAM, and the existing unlimited test-bank profile
remains available for its diagnostic role.

`display-readiness` in the kernel monitor reports the exact selected candidate,
complete BAR5 plan, PM capability/control snapshot, memory decode, CPU cache
preconditions, firmware-scanout eligibility and local COM1 availability. It
observes standard PCI configuration only. It neither claims a PCI assignment
nor creates a mapping or reader. Missing/duplicate provenance and invalid
config/power/extent observations stay blocked.

Register-read safety, selected-pipe/display-domain accessibility, independent
serial control and physical capture evidence stay unresolved in this report's
policy model. Separately retained physical evidence does not automatically
change its blockers or turn the report into an access grant. Even a complete
set of successful local observations cannot remove them. A UART responding
locally does not demonstrate communication with another computer, physical
serial input, independent recovery control or survival during a GPU failure.
The report is data, never an authorization token or activation command.

## Explicit physical observation experiment

Build with `POLARIS_LIVE_CAPTURE=1` to enable `polaris-capture` in the
kernel monitor. The default is 0. A build-directory content stamp tracks mode
changes; invalid values fail. There is no boot-time activation. Unsupported
targets refuse before assignment or MMIO. The exact selected GOP/board, complete
BAR5, D0/memory decode, CPU/cache conditions, working UART, enabled preemption
and exclusive kernel framebuffer lease must match the fixed 1024x768 profile.
Independent serial control is a separate operator-established prerequisite.

The controller queries only the six CRTC_CONTROL fields first, following the
inspected DCE enabled-state discovery pattern. Exactly one pipe must be both
requested and currently running, with read requests enabled. Other state is
refused. It samples fifteen fields twice on that pipe. Profile 2 added
MC_VM_FB_LOCATION (byte0x2024) and HDP_NONSURFACE_BASE (byte0x2C04) before and
after those samples, for forty loads. Current experimental profile 3 additionally
samples GRPH_UPDATE, GRPH_FLIP_CONTROL and CRTC_STATUS_FRAME_COUNT before and
after the pipe samples. Its maximum is forty-six DWORD loads: six pipe queries,
two location reads, three witnesses, thirty pipe reads, three witnesses and
two final location reads. It permits one attempt per boot, with no retry.
Each transaction is journaled before the load. A two-second admission limit
cannot interrupt or cancel a stalled device/interconnect transaction.

The experiment assumes operational accessible hardware, unchanged inherited
scanout and no firmware, hotplug, power or reset transition during the capture.
Requested/current CRTC state is a limited witness, not general display-domain
power proof. Equal samples do not establish atomicity or connector identity.
No PCI/GPU writes, BAR sizing, ROM execution, new interrupts or DMA grants occur.

All live mappings close and drain before an immutable snapshot is copied to
the READ-only observation resource for Ring3 decoding. The scalar protocol
uses the existing four-word IPC transport and incarnation checks. Oversized
pipe selectors refuse before narrowing; metadata queries bind the resource ID.
The reader and decoder process drain afterward. Inherited bus-master exposure
keeps the parent assignment quarantined; CPU cleanup never asserts GPU reset,
DMA quiescence or safe reassignment.

`kernel/polaris_vram.c` independently checks the fixed measured layout and
the unchanged MC/HDP pair against verified current BAR0/GOP provenance. Its
conservative correlation requires HDP base equal to MC VRAM start and the
observed GPU primary equal to that base plus the GOP's offset within BAR0.
CPU aperture, GPU VRAM and surface spans must all contain the existing GOP
allocation without overflow. Different translations/layouts refuse. This
observes the existing firmware allocation; it neither reserves other VRAM nor
grants new access. The mapping hypothesis follows AMD's
[GMC implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c#L405).
HDP has definition/initialization-write evidence in that source, rather than
a general read-side-effect guarantee. Physical profile 2 has now completed on the target: forty journaled loads,
MC_VM_FB_LOCATION=0xF5FFF400 and HDP_NONSURFACE_BASE=0xF4000000 unchanged
before/after, and the independently checked CPU0x50000000/GPU0xF400000000
correlation for the existing0x300000-byte allocation. Ring3 reports no decoder
restrictions. The programmed MC extent is not a measurement of physical VRAM
capacity. Post-capture serial commands and firmware-service recovery completed;
the operator confirmed readable monitor output. This remains a finite
inherited-scanout observation, without native register programming or rescue.

The first physical experiment (profile 1, thirty-six loads) completed over
independent serial control on the selected RX 570 with unchanged firmware
scanout. Pipe0 alone was running; both samples matched and reported primary
GPU address0xF400000000, pitch1024 and extent1024x768. A post-capture firmware
service fault/restart passed and the operator confirmed readable monitor output.
This establishes that finite read observation on that boot. It does not
qualify native modesetting, reset, native-driver failure recovery or HDMI.

Host checks cover the production controller's success and 72 failure/refusal
boundaries, optional-page rollback/poison retention, exact location selectors,
address arithmetic/changed samples and decoder replay. Ordinary JA OS tests
continue to use RAM; QEMU cannot emulate a qualifying Polaris target and must
refuse the physical command before any load.

## Source review and operation retention

The source-review result is **layout/field agreement established; physical
read qualification remains open**. The machine-readable
[review contract](POLARIS_REGISTER_REVIEW.json) pins nineteen complete reference
files to Linux commit `adc218676eef25575469234709c2d87185ca223a` (v6.12)
and SHA-256 values. Vendor files remain a separate ignored build cache with
their original notices; no vendor implementation or firmware is linked into
JA OS.

The offline audit compiles the production register helper and named decoder
masks. It compares ninety pipe byte offsets, eighteen witness byte offsets,
fifteen pipe selectors, three witness selectors, forty-four field/enum/location
facts and the unchanged four-page pipe footprint against the pinned AMD
definitions. It refuses changed source bytes, wrong offsets/selectors/masks
and duplicate probe rows. This checks source facts, not access types or
hardware behavior.

| Selector | Pipe 0 byte offset | Access evidence in the reviewed source set |
| --- | --- | --- |
| CRTC control | 0x6E70 | Enabled-state query and read/modify/write |
| CRTC blank | 0x6E74 | Blanked-state query |
| CRTC interlace | 0x6E78 | Definition; no read established by this review |
| Graphics enable | 0x6800 | Read/modify/write |
| Graphics control | 0x6804 | Read/modify/write |
| Graphics swap | 0x680C | Write with a supplied initial value |
| Primary surface low | 0x6810 | Readback following a page-flip write |
| Primary surface high | 0x681C | Write with a supplied initial value |
| Pitch | 0x6818 | Write with a supplied initial value |
| Surface offset X | 0x6824 | Write example |
| Surface offset Y | 0x6828 | Write example |
| Graphics X start | 0x682C | Write with a supplied initial value |
| Graphics Y start | 0x6830 | Write with a supplied initial value |
| Graphics X end | 0x6834 | Write with a supplied initial value |
| Graphics Y end | 0x6838 | Write with a supplied initial value |
| Plane update witness | 0x6844 | Flip-pending read |
| Flip control witness | 0x6848 | Page-flip read/modify/write |
| Frame counter witness | 0x6E98 | Vblank-counter read (24-bit field) |

AMD's [timing-generator implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce110/dce110_timing_generator.c#L1968)
contains the blank/enabled queries. Its
[memory-input code](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_mem_input.c#L428)
uses different update/set operations. The
[helper implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dc_helper.c#L221)
reads the register for an update, while a set uses its supplied initial value.
A set call is therefore not evidence of a read. The
[legacy page-flip path](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/dce_v11_0.c#L257)
reads the low surface address after writing it. That programming context is
distinct from sampling firmware-inherited state. The table is an inventory
of inspected examples; it does not say other read paths cannot exist.

The DCE11.2
[hardware sequencer](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/hwss/dce112/dce112_hwseq.c#L113)
has controller-specific display power gating, and the
[resource constructor](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/resource/dce112/dce112_resource.c#L1398)
selects that sequencer. From this, PCI D0 alone is insufficient evidence of a
chosen display domain's accessibility. GOP device provenance and geometry
identify no pipe. The first physical operation needs a justified pipe/power/
clock/reset-state contract; blindly probing all six pipes is not qualified by
the current evidence. No power-gating register or BIOS call is added to the
allowed read set.

Readiness report version 2 retains a separate unresolved pipe-power condition
and explicitly prints that limitation. The syscall request remains version 1.
The new field names in `include/amd_polaris_registers.h` replace decoder magic
numbers with the audited facts and the linear-aligned observation policy is checked separately.

Kernel operation holds close a separate lifetime gap. Acquisition requires a
prepared binding, its exact identity, current live owner, ASSIGNED parent and
no DMA exclusion gate. The registry rejects copied/duplicate token storage and
bounded capacity exhaustion. Cleanup permanently closes admission before
draining; outstanding holds retain all leaves and both assignment pins.
A close can be requested without the active kernel CR3; leaf changes retain
the existing kernel-map restriction.

Release requires registered token storage and the exact hold identity. It
works after owner death or quarantine and checks the retained binding identity.
Stale releases cannot affect a new hold after storage reuse. Hold storage must
remain valid until release, and the trusted caller must release only after its
CPU operation has stopped. A hold itself is not proof of a completed GPU
transaction, stopped DMA, safe IRQ state or physical register access.
No normal process or syscall can create these holds.

The host binding fixture checks retention through attempted cleanup, permanent
closure, owner death/quarantine, stale binding/token identities, copied tokens,
duplicate acquisition, capacity exhaustion and complete draining. No mapped
MMIO leaf is accessed by these host tests. The physical experiment is separate
from a general userspace provider or normal supervisor policy.

With the exact files from the review contract cached locally:

~~~sh
make -f Makefile.msys2 audit-polaris-registers POLARIS_SOURCE_DIR=build/rx570-source-review
make -f Makefile.msys2 test-polaris-plane test-polaris-mmio test-polaris-read-profile test-amd-polaris-display
~~~

The audit downloads nothing and fails if a cached source hash differs.
It writes its compiled facts and detailed JSON report under the chosen BUILD
directory. Completing this audit does not remove physical qualification
blockers or activate the GPU. Keep image identity, raw serial bytes, counters,
post-capture commands and visual output confirmation with each physical run.


## Ring3 plane rebind preview

[user/drivers/amd_polaris_plane.c](../user/drivers/amd_polaris_plane.c)
constructs a version-1 scalar recipe for rebinding the same inherited
1024x768, pitch1024, 32-bit linear surface. This preserves the inherited
timing/link; a successful preview is not DP/HDMI link training or a modeset.

The proposed sequence writes PRIMARY_SURFACE_ADDRESS_HIGH first, writes LOW
to trigger the update, reads LOW back, checks update idle and checks a later
frame against the retained counter. The order follows AMD's
[page-flip implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/dce_v11_0.c#L257).
Pending/update and frame-counter reads have separate evidence in the pinned
[memory-input implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_mem_input.c#L827)
and [vblank-counter path](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/dce_v11_0.c#L221).
These examples establish source semantics, not an execution qualification on
firmware-inherited hardware.

Profile 3 takes two copied sets of update/flip/frame witnesses. Both update
values may contain only the two read-only TAKEN flags; pending, lock and
unmodelled controls refuse. Flip control must be identical in both samples
and contain only the audited pending-mode bit (stable0 or0x20). Immediate,
horizontal-retrace, stereo and unknown controls refuse. This does not change
the hardware bit or establish what clears pending status. Frame counters must
fit the audited 24-bit mask and advance by 1 through 512 modulo 2^24.
This finite progress check is not a measured refresh rate, atomic snapshot or
proof that a later programming operation will complete.

The existing four-word probe protocol adds witness and step messages.
Witnesses bind the retained observation ID and accept samples0 then1 once.
Step queries return a semantic tag and a DWORD value; they accept no raw
offset, CPU address, pointer, capability or executable grant. A new capture
invalidates the preceding retained recipe, even when that capture fails.

[kernel/polaris_plane.c](../kernel/polaris_plane.c) independently validates
the witnesses and all five ordered replies against the kernel's retained
GPU surface address, pipe, observation ID and service incarnation. A mismatch
permanently closes that preview. The final step seals it; replay cannot reopen it.
Ring3 cannot choose a different framebuffer by returning a plausible address.

The controller unmaps all live access before sending witnesses or requesting
recipe steps. There is no write executor, writable BAR5 mapping, polling loop,
GOP retirement or native output publication in this path. Implementing those
requires separate authority, failure/drain and physical completion contracts.

`make -f Makefile.msys2 test-polaris-plane` links the real Ring3 planner and
independent kernel validator. It checks all six pipes, every tag/payload bit
tamper at each step, stale identities, replay, invalid snapshots and 24-bit
wrap/refusal boundaries. The MMIO host suite routes all eighteen witness
selectors through the production guards and exercises the production
forty-six-read controller with zero/pending-mode success and103 refusal/failure cases. The actual
`test device-register-reader` runs the witness/recipe exchange in Ring3
using protected RAM, including malformed, reordered and stale requests.
None of these software tests executes a GPU write.

## Measured pending-mode replay

The physical profile3 run completed all46 read-only loads with equal pipe and
MC/HDP samples. Update status was0 before/after; flip control was0x20 before/
after; the24-bit frame counter advanced0x2102 to0x211A (24frames). The original
exact-zero flip rule correctly refused recipe qualification with blocker4.
No five-step recipe was exchanged in that physical run. CPU mappings and the
reader/service drained, the tainted parent remained quarantined, subsequent
serial commands and firmware-service tests passed, and the operator confirmed
readable monitor output. This is a capture plus controlled refusal, not a
physical recipe or native-programming pass.

The audited0x20 field is GRPH_SURFACE_UPDATE_PENDING_MODE, separate from the
GRPH_UPDATE pending-status bit. The reviewed page-flip path modifies immediate
enable using read/modify/write and leaves pending-mode unchanged. The later
data-only planner/validator therefore accepts stable0x20 without writing or
clearing it; a mode change between samples still refuses. This preservation
policy grants no physical completion or recovery qualification.

`tools/replay_polaris_plane.py` reads one complete profile3 raw journal,
checks ordering and sample equality, then compiles its scalar values into
a host fixture. It runs actual unique-pipe discovery, Ring3 decoding/planning,
kernel MC/HDP/GOP correlation and independent recipe validation. It opens no
device or serial port. The preserved original policy reproduces blocker4;
the corrected policy validates five proposed steps using the same bytes.
Changed/partial journals, immediate flip and changing pending-mode refuse.

~~~sh
python tools/replay_polaris_plane.py --raw-log path/to/serial.raw.log --build-dir build/polaris-replay
~~~

Keep recorded-data replay separate from new hardware execution. The next
physical write still requires an independently constrained executor, permanent
GOP retirement before writable authority, bounded completion/failure handling
and a separately qualified fresh rescue entry. Clearing pending-mode merely
to pass a preflight is not part of this implementation.

## Native output handoff mechanism

The [graphics recovery contract](GRAPHICS_RECOVERY.md#active-nativerescue-descriptor-lifecycle)
now separates immutable boot metadata from a privileged native/rescue active
descriptor. Publication requires retired firmware fallback, drained kernel
writers and the existing KERNEL lease; the first storage profile preserves
the exact retained aperture, format and geometry. Every publication has a new
identity; revocation closes drawing admission, retains quarantine metadata and
prevents a stale identity from affecting a replacement. Native direct handoff
checks that same generation at prepare and commit. Firmware services and
legacy presentation stay blocked.

These lifecycle mechanisms alone do not program the RX570 or prove hardware
readiness. The explicit provider below uses them only after its constrained
register/frame checks. Its physical qualification is separate. It must prove
operational output before publication and remove failed Ring3 mappings before
replacement. Host fixtures supply simulated descriptors only; no automatic
diagnostic labels the live GPU output native or rescue.

## Explicit same-buffer write and reserved rescue experiment

`POLARIS_NATIVE_WRITE=1` enables `polaris-native`, `polaris-rescue`
and `polaris-finish` in the kernel monitor. Default builds keep writes
disabled. The native and read-capture build stamps are independent; invalid
values fail before changing either stamp. There is no boot activation or
userspace constructor. Unsupported targets refuse before any mapping or GPU IO.

The initial profile requires the exact selected board, pipe0, existing
CPU0x50000000/GPU0xF400000000 allocation,1024x768,pitch1024 and BGRX32.
Preparation takes46 reads; the recovery observation takes40 on the already
identified pipe. It checks unchanged pipe fields, MC/HDP/GOP correlation,
idle update state, stable flip0/0x20 and advancing24-bit frame count.
The inherited DP timing/link stays in place. No other VRAM is allocated.

Two distinct Ring3 executables reserve their own process, stack, font/code,
IPC endpoints and read-only framebuffer aliases before retirement. The rescue
entry has no dependency on the primary's heap, locks, mutable configuration,
compositor or allocations. Shared reviewed policy code leaves common-mode
software risk; separate processes do not imply implementation independence.

The kernel owns a dedicated supervised assignment. Neither process receives
an assignment/BAR/IRQ/DMA capability or MMIO mapping. Ring3 proposes the five
semantic recipe steps. Independent validation binds the service incarnation,
observation and pinned surface before any IO. After irreversible GOP retirement,
one pipe0 supervisor UC/NX leaf becomes writable. Its only store API admits
HIGH=0xF4 followed by LOW=0 exactly once per round. It accepts no raw offset
or alternative address. The normal assignment claim still rejects kernel
processes; the separate supervised claim cannot be delegated to Ring3.

The executor refreshes address/CRTC/update/flip/frame state before the first
store, posts LOW with a read, then takes a new frame baseline. At most64
six-field polls check unchanged address/control, no pending update and a
24-bit frame delta2..512. Maximum392 loads and two stores per round, with a
two-second admission deadline. Every live operation runs with interrupts
enabled and a retained exact mapping/hold. Journals precede transactions.
The deadline does not cancel a stalled MMIO/interconnect operation.

After register/frame validation, a fresh active descriptor and the existing
exclusive CPU lease permit the primary's previously RO framebuffer pages to
become RW. The page update allocates nothing and changes only WRITE on exact
owned4KiB leaves; USER/NX/cache/frame stay fixed. Any partial failure requires
stopping the process and retaining/retrying teardown, never resuming it.

`polaris-rescue` revokes the descriptor, quarantines the parent and
injects a controlled primary fault. The old process must stop/reap and lose
its mappings before the lease returns to KERNEL. Reclamation uses the retained
launch process ID: the ordinary managed-service PID accessor returns zero once
a service fails, and reaping removes its process storage. The saved identity
remains bound to that driver instance until lease reclamation completes.
The pre-reserved independent
rescue entry receives fresh kernel-observed configuration and one bounded
same-buffer rebind round. It uses the same retained kernel assignment/pins
while quarantine remains set; ordinary assignment authorization/reuse remains
refused and inherited DMA history is never cleared. It then draws new rescue
text under a fresh output generation and the exclusive lease.

A transitional monitor poll detects an unexpected primary terminal outcome;
bounded RPC detects request failure. In the policy candidate below,the existing
Ring3 supervisor chooses recovery or headless output from these observations.
A primary render failure after successful register validation requests the same
one-shot recovery decision. Unqualified register
state, failed cleanup, exhausted rescue or standby/rescue failure leaves
headless quarantine and serial control. This is a restricted process-failure
profile, not GPU reset or recovery from arbitrary device/bus hangs.
The worker candidate below removes the kernel-monitor scheduling dependency;
remaining privileged orchestration and the general Ring3 broker migration are
separate work.

Verify the green primary pattern and blue rescue pattern separately on the
physical monitor. After `polaris-finish`, the validated current descriptor
can back the kernel console while original GOP remains retired. Firmware-only
service tests are performed before takeover; they cannot reauthorize GOP
afterward. Run a fresh image boot for another experiment.

`test polaris-native` uses ordinary RAM, not the live framebuffer.
It exercises actual separate Ring3 entries, RO-write page fault, primary spin
timeout, stale IDs/connections, retained launch identity after fault/reap,
allocation-free rescue promotion, exact primary/rescue title pixels using the
canonical MSB-first font, and full software cleanup. Host targets `test-polaris-program` and
`test-polaris-native` exercise the production executor/orchestration with
stateful IO/process doubles. These software passes do not establish physical
output restoration. No new IRQ, DMA, clocks, translation, reset, ROM execution,
link training or changed-mode support is supplied.


The monitor shutdown/reboot commands first stop native CPU owners and remove
their borrowed mappings/MMIO leaves, then revoke the supervisor capability.
The device object and inherited exposure remain quarantined. This preparation
performs no GPU transaction and does not relax the existing power object/storage
checks. Failed CPU cleanup keeps the controlled transition blocked and the
retained records intact. Finishing the experiment also revokes that capability,
so an otherwise clean system can pass ordinary reboot readiness.

## Observed retained-scanout rescue run

One physical run on2026-11-07 used a Gigabyte Z390 Gaming X/BIOS F10,
i7-9700K,1002:67DFrevEF/sub1458:2310 and a Philips Momentum272M8 over
DisplayPort. Image SHA-256:
b7beab605c55c39d7269f0239e90ccdb5bab59f17f13f8ba7a8e2f40b5c74812.
Image identity is operator-confirmed flashing plus matching runtime/profile
markers; flashed bytes were not independently read back.

The primary and reserved rescue each completed HIGH0x681C=0xF4 then
LOW0x6810=0,14 executor loads/one poll and three frames of post-write
progress, with update0 and flip0x20. Preparation used46 loads for primary
and40 for rescue. The operator separately confirmed correctly oriented
green primary text, blue fresh rescue text and a readable reclaimed kernel
console. The primary was deliberately faulted with #UD and stopped/reaped
before rescue; the kernel continued running. Host serial input/replies
worked in every phase, with no reported local UART drops/timeouts.

Finish restored the KERNEL lease/owner0, removed the driver aliases and
register leaves, and revoked the mediator capability. Shutdown/reboot
readiness and storage safety reported YES; no reboot was performed.
Original GOP remained retired and the device stayed quarantined with its
inherited busmaster exposure. This records one controlled process-fault
run preserving the existing allocation and inherited DP timing. Repetition,
active-output spin/rescue-failure cases, independent progress measurements,
Ring3 supervisor orchestration, changed modes, HDMI/link training, GPU reset
and arbitrary GPU/bus-hang recovery require separate evidence.

## Bounded process-liveness monitoring

The v20 explicit write build probes the active driver once per second while the
kernel monitor loop is running. The worker candidate below schedules these
observations independently. Each heartbeat has a fresh scalar challenge
and checks reply shape, service incarnation, role and challenge echo. The
common IPC path permits up to two seconds for a reply plus a separate
two-second wait for return to the command receive loop. An already busy
nonreceiving peer refuses immediately. Primary failure enters the reserved rescue path;
failed standby or active rescue closes output and retains headless quarantine.
Heartbeat checks issue no GPU read/write. The monitor must remain active;
a long monitor command can delay polling, and this is still transitional
kernel scheduling. Recovery decisions in the candidate below run in the existing
Ring3 supervisor; autonomous scheduling and the remaining orchestration migration
are still open.

`polaris-test primary fault|spin` and `polaris-test rescue fault|spin`
select a deliberate diagnostic on the current writer only, once per role.
The exact private four-word ACK identifies the role and selected branch.
The target then leaves its receive loop and faults/spins. The command measures
a completed byte of work through the existing independent Ring3 console:
process/incarnation remain the same and its portal-write count increases.
For spin, the driver must still be live and outside its receive wait before
and after that work. This is a measured checkpoint, not a sustained throughput
or whole-recovery progress guarantee. The monitor poll in v20,or the independent worker in the candidate below,
subsequently detects the outcome;the diagnostic does not invoke rescue directly.

`polaris-status` reports CPU-only stage/heartbeat/test counters. Normal
heartbeat success is quiet; failures and explicit diagnostic checkpoints are
journaled. ACK/delivery/progress failure stops the experiment without replay.
After successful CPU teardown the failed path revokes the mediator capability,
so ordinary power-readiness checks can succeed while output stays unqualified
and the GPU parent remains quarantined. Cleanup failure retains the ledger
and continues to block power readiness.

Existing `polaris-rescue` remains the explicit controlled-fault operation.
The earlier explicit-rescue run and the watchdog/failure run below are
separate hardware evidence; neither establishes broader device recovery.

## Observed watchdog and rescue-failure run

One physical run on2026-11-07 used the same Z390/F10/RX570/Philips DisplayPort
profile with image SHA-256:
3e68fbabf09de2a311896013159d0383c01d4b9c90740d10a83e59acb4015a73.
The primary and automatically selected rescue each completed their fixed
HIGH/LOW pair,14 executor reads/one poll and three frames of progress.
The operator confirmed correct green primary and blue fresh rescue text.

The primary was acknowledged into a spin. Independent Ring3 console PID3/
incarnation1 completed work,advancing portal writes1385->1386 while the
driver remained live/nonreceiving. The monitor detected a failed heartbeat,
stopped/reaped the primary and entered rescue without an explicit rescue
command or kernel restart. Its status later reported28 completed heartbeat
intervals. A deliberate rescue #UD then advanced console work1386->1387;
the monitor selected headless quarantine without additional GPU reads/writes.
The failure status showed63 completed heartbeat intervals,test mask0x6,
no qualified scanout,retired GOP,KERNEL lease/owner0,drained CPU resources,
and shutdown/reboot readinessYES. Serial input/replies remained usable.

The controlled reboot quiesced services and reset the platform. The operator
reported that Windows booted,then changed boot priority. The subsequent JA OS
USB boot,serial monitor and readable firmware-backed console were captured
and confirmed. Fresh status was stage0/heartbeat0/test mask0. Return to JA OS
required operator boot-selection intervention; this is not an unattended
reboot-to-JA-OS result. It also does not prove in-place GPU reset or survival
of arbitrary GPU/interconnect failure.

Totals:114 GPU loads,four store attempts,no retries;41089 raw received bytes/
326 host-transmitted bytes include both JA OS boots,all phase replies and the
CPU-only post-boot status. The image identity uses operator confirmation,
matching profile/unique runtime markers and captured JA OS boot output,
without an independent flashed-byte readback. This is one primary-spin and
rescue-fault case. Active-rescue spin,automatic primary terminal failure,
repeated/exhaustion cases,Ring3 policy migration,HDMI/changed modes and hardware
isolation still need separate qualification. Counter increments are measured
checkpoints,not continuous throughput or a whole-system detection deadline.


## Complementary exception and rescue-spin run

A second fresh v19 run on2026-11-07 used the same exact image and DP profile.
An acknowledged primary #UD/vector6/thread20 caused automatic terminal-outcome
recovery; the operator confirmed correct blue fresh-rescue text. An active
rescue spin then failed its heartbeat and was forcibly stopped/reaped, leaving
headless quarantine, independent serial control and clean power readiness.
Independent Ring3 console PID3/incarnation1 advanced1385->1386 and1386->1387
at the fault/spin checkpoints. No further GPU transaction followed rescue failure.

Each primary/rescue round used the fixed HIGH/LOW pair,14 executor loads,
one poll and three frames of progress; preparation used46/40 loads. Baselines/
completion were0xB4FE->0xB501 and0xC20C->0xC20F. Totals were114 GPU loads,
four store attempts,no retries,43080 serial RX bytes and462 host TX bytes.
Status showed54 completed heartbeat intervals after rescue and77 at cleanup;
these are probe intervals,not RPC counts or a detection-latency guarantee.

Five exhausted-state native/rescue/diagnostic/finish requests were refused,
without GPU access or counter changes. A controlled reboot returned directly
to JA OS,with the readable firmware console confirmed by the operator without
boot-menu/reset/power intervention. Fresh native status was0/0/0. This is one
unassisted return; the previous run's assisted boot-selection result remains
separate. Neither result establishes GPU reset or arbitrary device/bus-hang
recovery. Both software-fault branch combinations now have separate physical
cases; statistical reliability,normal Ring3 orchestration,changed modes,HDMI,
and hardware-memory isolation remain open.

## Ring3 recovery decisions

The v20 image moved continue/rescue/headless decisions into the
existing protected supervisor process. Its immutable policy module tracks an
ordered session through armed,primary,recovering,rescue and closed states.
A failed standby takes precedence over primary failure; active rescue failure
closes the session. Closed sessions cannot rearm. A later distinct session
requires a strictly greater token. The native experiment still permits only
one attempt per boot and one reserved rescue round.

The private four-word exchange carries a versioned scalar health snapshot,
session token and sequence; replies echo session/sequence and one bounded
action. Kernel clients retain the exact supervisor process/thread identity.
Timeout,malformed reply,rejection,sequence exhaustion or replacement poisons
the client session without replay or automatic reconnection. No startup
capability,syscall,device mapping or hardware operation is added to the
supervisor. It continues to use its original two IPC capabilities.

The kernel still samples process/heartbeat state,validates legal action/stage
and known-writer liveness,enforces exclusive leases and the fixed GPU recipe,
and performs revocation/quarantine/teardown. Policy failure fails closed.
Successful CPU cleanup can still permit controlled power transitions while
device quarantine remains. In v20,heartbeat scheduling is tied to the kernel
monitor loop. The worker candidate below removes that scheduling dependency;
remaining privileged lifecycle/broker orchestration is still transitional.

`test graphics-policy` exercises the actual existing Ring3 supervisor with
four fresh ordered sessions,stale session/sequence/reserved-bit refusals,
primary exit/missed-heartbeat recovery,failed-standby precedence,rescue
failure,closed-session refusal and unrelated supervisor ping. It allocates
no pages/objects/caps and performs no GPU IO. Host checks cover62 malformed
envelope bits,64 health/stage snapshots,eight client identity/transport/no-replay
cases and75 production-controller cases,including18 policy/enforcement cases.

Image SHA-256: `e87f9f9c17f91b236f0b3d07db5fa21c31b3ffca68c2904c4662e37e9436e4eb`. Focused,two-GPU,normal86/86
and deep86/86 across603 iterations passed on four clean QEMU boots. Integer-only
build rules,zero runtime ELF relocations,default-off independent write/capture
gates and the108-offset/44-fact/19-source/9-refusal register audit are preserved.
Primary and rescue ELF bytes are unchanged from the physically tested image;
the supervisor and kernel implement the new decision path. One physical policy
case is recorded below;broader policy-failure and branch-combination coverage
remain open. Existing DP retained mode,once-per-boot limits
and inherited-DMA quarantine are unchanged;HDMI/new modes/reset/isolation
remain separate work.

## Observed Ring3 policy recovery run

One physical v20 run on2026-11-07 used the same Z390/F10/RX570/Philips DP
profile and image SHA-256
e87f9f9c17f91b236f0b3d07db5fa21c31b3ffca68c2904c4662e37e9436e4eb.
Preflight passed three targeted1/1 standard suites,including four actual
Ring3 policy sessions,stale/closed-state refusals,unrelated supervisor ping
and unchanged page/object/cap counts. The operator confirmed correct green
primary text. An acknowledged live primary spin completed independent console
work1385->1386 (PID3/incarnation1),then Ring3 action2 selected rescue. The old
owner was stopped/reaped before the fresh reserved backend drew the blue
rescue text,confirmed by the operator without a kernel restart.

A deliberate active-rescue #UD/vector6/thread21 completed independent work
1386->1387. Ring3 action3 selected headless output,CPU resources/capability
drained under retained GPU-parent quarantine,original GOP stayed retired,and
serial/power readiness remained usable. No additional GPU MMIO read/store
followed the rescue fault. Five exhausted-state commands were refused without
changing stage3/heartbeat51/test mask0x6. The after-rescue status had28 completed
heartbeat intervals;these are not exact RPC counts or a detection deadline.

Each round completed the fixed HIGH/LOW pair,14 executor loads/one poll and
three frames of progress. Primary baseline/completion0x1EFF->0x1F02;rescue
0x262C->0x262F. Preparation used46/40 loads,total114 GPU loads/four store
attempts/no retries. Controlled reboot returned directly to a readable JA OS
firmware console without intervention;fresh native status0/0/0. COM7 is closed.

The v20 capture contains46866 received bytes and491 host-transmitted bytes,
including1694 initial boot bytes and the new boot after reset. A separate182-byte
pre-flash shutdown transcript is retained and excluded from the v20 count;
all saved raw bytes total47048. Identity is operator-confirmed flashing plus
new policy/profile/runtime markers,without independent flashed-byte readback.
The monitor still schedules observations;normal independent supervision,
physical policy-process failure/replacement,additional v20 branch combinations,
HDMI/changed modes,GPU reset/arbitrary hardware hangs and hardware-memory
isolation remain unqualified. Work increments are two checkpoints,not sustained
throughput. Headless means no qualified output;stale pixels may remain visible.

## Independent periodic worker candidate

The explicit v21 image serializes native setup,health observations,diagnostics,
rescue and CPU teardown on one privileged worker. Private ordered requests
bind the control thread,request sequence and worker identity. Two kernel-only
endpoints,four nontransferable kernel capabilities and one guarded16KiB worker
stack are reserved before native preparation;none are granted to Ring3.
The worker uses existing PIT-backed timed IPC waits and polls between requests,
so neither returning to the Ring3 shell nor command traffic suppresses health
checks. No GPU authority,register sequence,buffer,IRQ/DMA/reset or mode is added.
The primary/rescue ELF bytes are unchanged.

The existing Ring3 supervisor still chooses continue,rescue or headless.
Its shared transport and start/stop/recovery entry points now use bounded
single-flight admission across kernel callers. Admission waiting sends no
request. Replacement/ambiguous policy replies still close the native session.
A failed original policy process is stopped through its retained exact
process/thread identity,including retryable partial teardown;stale cleanup
cannot stop a replacement. Independent serial/console policy remains separate.

No caller force-terminates a trusted worker callback or frees its live stack.
Ambiguous command completion poisons the channel without replay;the worker
performs CPU-only abort once its callback returns. Partial teardown retains
the exact remaining resources. After native failure,the worker can remain
quiescent until status/finish/power drains its private control resources.
The ordinary power baseline is unchanged and succeeds only after those
resources and the failed policy are actually removed.

Immediate `polaris-test policy fault|spin` tests the current policy once.
The fixed delayed forms are `primary spin-later`,`rescue fault-later`,
`policy fault-later` and`policy spin-later`. They require an active
role,wait at least three seconds and require the Ring3 shell session still
active at injection. Enter `usershell` after arming. The worker journals
its separate thread/control identities. An absent shell or changed role
cancels injection. Delayed injection never starts a competing console RPC;
normal Ring3 shell commands provide separate progress evidence. Immediate
monitor diagnostics retain their original independent console-work probe.

`test graphics-worker` uses RAM and the actual worker,drivers and Ring3
supervisor. It covers primary spin/rescue fault without monitor polling,
policy fault/spin,unrelated console work,serialized supervisor clients,
exact failed-policy stop,power readiness before replacement,stale-session/
cleanup refusal against the replacement and full resource baselines.
Host checks cover nine constructor failures,seven malformed/ambiguous replies,
no replay,owner/sequence/partial-teardown guards and the production adapter.
The production core controller has80 cases,including five policy-failure
injection cases. These checks perform no physical GPU programming.

Four clean QEMU boots PASS:focused,multi,normal87/87/87iterations and deep87/87/607iterations;exact failed-policy stop,power readiness before replacement and replacement immunity PASS.
Image SHA-256:`48acc7ccb83748351352034e22cbaaa8cc3101c1f6cccdf555e6c221d2183526`. Physical off-monitor and policy-failure results are recorded below. One per-boot primary/rescue operation budget,
retained inherited-DMA quarantine and fixed DP mode remain unchanged.
Monitor independence does not complete the general userspace lifecycle
broker,native modesetting,HDMI/link training,GPU reset or hardware isolation.

## Observed recovery outside the monitor

Three physical native runs on2026-11-07 used v21 image SHA-256
48acc7ccb83748351352034e22cbaaa8cc3101c1f6cccdf555e6c221d2183526
on the same Z390/F10/RX570/Philips DisplayPort profile. Four targeted
preflight suites passed,including actual RAM-only worker/policy fault
and spin,exact failed-policy stop,power readiness before replacement
and full resource baselines. Physical replacement during active native
output remains untested;the replacement checks used RAM.

The operator confirmed the green primary title. A delayed primary spin
fired while the normal Ring3 shell was active,with worker31 distinct from
control1. Help commands completed before and after. Ring3 action2 selected
rescue,the old owner was stopped/reaped,and the operator confirmed the
correct blue fresh-rescue title without kernel restart or monitor polling.
A delayed rescue exception then fired from the shell;action3 selected
headless output and drained CPU owners under retained GPU quarantine.
Help completed after that event too. These are command checkpoints,not
continuous throughput or a whole-system detection-latency measurement.

Primary frame count advanced0x2582->0x2585;rescue0x41B1->0x41B7.
Each used14 executor loads/one poll;preparation used46/40 loads. Status
reported137 heartbeat intervals after rescue and169 at cleanup,mask0x6.
Five exhausted requests were refused without GPU operations. Serial/power
readiness remained available. Controlled reboot returned to JA OS;the
operator confirmed a readable prompt without boot-menu/reset/power action.

Two separate fresh native boots exercised policy exception and acknowledged
policy spin while the shell remained active on worker6/control1. Policy
identity loss or unavailable transport caused headless CPU drain with no
rescue or further GPU operation. Both reached stage3/mask0x8,six heartbeat
intervals,and clean shutdown/reboot/storage checks. Help and serial commands
completed after each. Both returned with captured reboot output and firmware
scanout. Later return visibility and absence of intervention were not separately
confirmed;the first run's explicit observation remains distinct.

Totals:234 GPU loads,eight fixed store attempts,no retry. Primary deltas
were3,3,2;rescue delta6. These meet the unchanged2..512-frame completion
rule with no pending update. Saved active captures contain98279 received
bytes and972 host-transmitted bytes. The initial boot was missed while the
serial adapter was unplugged;gaps between captures are not claimed as a
continuous log. Local UART reports showed no drops,timeouts or stalls.

The original host script stopped after successful rescue because it expected
a monitor banner printed only once. The failed transcript is preserved;
CPU-only status continued qualification and a separate helper corrected the
expectation without repeating hardware operations. The offline verifier's
initial three-frame assumption was corrected to the existing two-frame
minimum;no production code,image or admission gate changed.

The machine is fresh firmware-backed v21,stage0/heartbeat0/mask0,COM7
released. Remaining privileged lifecycle/broker migration,physical active
policy replacement,changed modes,HDMI/link training,GPU reset,arbitrary
GPU/interconnect hangs and hardware memory isolation remain separate work.

## Ring3 rebind execution candidate

The v22 candidate moves the vendor rebind sequence into the protected primary
and fresh rescue programs. The old synchronous executor is excluded from the
kernel build and retained only as a host-test reference. Both driver ELFs
change; the existing supervisor ELF, GPU operation set and per-boot limits
remain unchanged.

Private native protocol v2 uses the existing two IPC capabilities and three
startup arguments. After the five-step plan, the driver requests semantic
reads, the fixed HIGH/LOW pair and bounded waits through the same endpoint
pair. No syscall, capability grant, user BAR mapping, physical address or raw
register offset is added. Each request binds incarnation and sequence. The
kernel's independent finite gate validates the plan before IO, constrains
operation order/field/payload, checks actual readback and frame progress,
and permits at most64 polls/457 operation messages plus final completion.
The conversation shares a two-second software admission budget; it cannot
interrupt a stalled device transaction.

A driver cannot authorize output by claiming success. The kernel requires
its own completed gate, the exact final frame delta and return to the
receive loop before installing output and promoting the framebuffer lease.
The driver also requires completion before drawing. Delivery, IO, identity,
deadline or completion failure consumes the attempt without replay; existing
revocation and retained quarantine apply. Fresh rescue runs the same reviewed
sequence in its separately reserved process without trusting primary state.

Host checks exercise the real user sequence and kernel gate,2048 corrupted
request fields/payloads,640 corrupt plans, pending updates, frame wrap and
exhaustion. Production broker checks cover success plus22 transport/identity/
partial-write/deadline/completion failures. Actual RAM-only Ring3 tests cover
successful execution, incomplete post-write failure, no replay, retained RO
alias and full cleanup. Four clean QEMU boots PASS: focused, multi, normal87/87 and deep87/87 across607 iterations; actual Ring3 broker completion, partial-failure RO retention/no replay and full baseline checks PASS.

Candidate image SHA-256:
130a55f2bcfa4e47cea708915296fb66fc4fea83a51317779ae142101de133c7.
The physical v22 result is recorded below. This does not add changed-mode
modesetting, HDMI/link training, GPU reset or hardware isolation, and the
remaining lifecycle/broker migration is still transitional.

## Observed Ring3 execution and rescue

Three native runs on 2026-11-07 used v22 image
130a55f2bcfa4e47cea708915296fb66fc4fea83a51317779ae142101de133c7
on the same Z390/F10/RX570/Philips DisplayPort profile. All four targeted
preflight suites passed, including actual Ring3 broker completion,
incomplete post-write refusal, retained RO alias and full resource cleanup.

The primary process PID28/incarnation23 completed its user sequence through
the independent kernel gate; the operator confirmed the correct green title.
A delayed primary spin fired with the normal Ring3 shell active on worker32/
control1. Ring3 action2 selected fresh rescue PID29/incarnation24 after
stopping/reaping the old owner. Rescue completed its own user sequence and
the operator confirmed correct blue text without a kernel restart or monitor
polling. Help commands completed before/after the event.

A delayed rescue exception on thread34 selected action3/headless CPU cleanup.
Help and serial control remained usable, as did shutdown/reboot/storage
readiness. Five exhausted requests were refused without further GPU IO.
Controlled reboot returned to readable JA OS with no intervention, confirmed
separately by the operator. Two additional native boots tested policy
exception and acknowledged spin. Both failed closed with no additional
GPU operation, preserved shell/serial/power control and returned with captured
JA OS reboot output. Later visibility/no-intervention was not separately
confirmed. Each later primary used PID4/incarnation2 in its separate boot.

Every transaction completed the fixed HIGH/LOW pair,14 executor loads and
one poll. First primary frames0x1A14->0x1A17; rescue0x22A6->0x22AC;
policy-exception primary0xA93->0xA96; policy-spin primary0x792->0x795.
Deltas3/6/3/3 satisfy the unchanged2..512-frame idle-update completion rule.
Preparation used46 loads for each primary and40 for rescue. Totals:234 GPU
loads/eight store attempts,no retry or further GPU operation after terminal
failure. First-run heartbeat intervals35/78 and policy intervals6 are
observations, not RPC totals or whole-system latency measurements.

Active captures contain98944 RX/972 host TX bytes. A separate1736-byte
initial reset transcript contains a131-byte unattributed pre-reset prefix
and1605 post-reset boot bytes. Image-scoped RX is100549;all saved raw RX is
100680. The prefix is retained and excluded from image-scoped totals.
Initial boot coverage is partial, and gaps between sessions are not a
continuous log. Runtime UART checks reported no drops,timeouts or stalls.

The image and production implementation are unchanged since the software
freeze. At that checkpoint the machine was fresh firmware-backed v22,stage0/heartbeat0/
testmask0,COM7 released. This qualifies the named Ring3-sequenced execution
and software-failure paths under retained DP timing. Remaining userspace
lifecycle/presentation integration,physical active-output policy replacement,
changed modes/HDMI,GPU reset/arbitrary hardware hangs and memory isolation
remain separate work.


## Normal presentation after native-owner cleanup

The v23 build connects the normal Ring3 display service, separate
compositor and bounded application surface to the current validated native or
rescue output. After `polaris-finish` stops and reaps the old native/rescue
owners, `gui` uses that generation under the existing exclusive framebuffer
lease. It adds no GPU operation, mode, allocation or capability authority.
The ordinary `display_service_start()` remains firmware-only; the explicit
native entry requires the exact current nonzero generation and retired GOP.
Stale, revoked, active and incompletely released ownership is refused.

This is presentation on an already validated scanout. The finite hardware
experiment is exhausted after `polaris-finish`: there is no remaining reserved
GPU rescue process, reset attempt or automatic native-backend restart. A
normal presenter/compositor process fault can be contained and cleaned up,
then a fresh session can reuse the same output if it remains valid. If the
hardware or that validation fails, independent serial/reboot diagnostics remain
necessary. Revoking a descriptor alone does not revoke an existing user mapping;
its owner must still be stopped and reaped before replacement admission.

`python tools/test_native_presentation.py` runs nine host cases against the
production framebuffer, lease, terminal and display-service code. RAM stands in
for MMIO and launch/IPC are doubles. Cases cover successful native/rescue
presentation admission, launch/commit/configuration failures, retained cleanup,
revocation, stale generation, and unchanged firmware bitmask configuration.
These are process/ownership model checks, not hardware qualification.

`test native-presentation` on a normal firmware boot only checks refused native
admission and explicitly reports that no native presentation was exercised.
After a real native/rescue operation and successful `polaris-finish`, it runs
three real GUI sessions, app input and separate compositor presentation. It
injects presenter and compositor exceptions in separate sessions, checks fresh
process identities, retained lease, native console reclamation and full object,
page and capability baselines. It never constructs native output or programs
the GPU. Manual `gui` remains available after the test.

Candidate image SHA-256: c116f5a4d7fe3f7750e7db93e03b85bef448b3056b93518417bd7ff4501a179d.
Software validation: Four clean QEMU boots PASS: focused, multi, normal88/88 and deep88/88 across611 iterations; actual firmware GUI/serial input/console restoration and native-admission refusals PASS. The physical result follows below; v22 observations remain separate.


## Observed normal GUI on native and rescue output

Two native boots on 2026-11-07 used the unchanged v23 image
c116f5a4d7fe3f7750e7db93e03b85bef448b3056b93518417bd7ff4501a179d
on the Z390/F10/RX570/Philips DisplayPort profile. Four preflight suites passed;
a second firmware boot repeated the admission-only check. These limited
firmware checks are separate from the two positive native presentation tests.

The first primary, PID25/incarnation21, completed its rebind and the operator
confirmed green correctly oriented output. After explicit owner cleanup,
`test native-presentation` ran three real sessions on native generation1.
App input and separate compositor presentation passed in each session.
Presenter and compositor exceptions were contained in separate sessions;
fresh identities, retained lease, native console reclamation and full resource
baselines passed. A subsequent normal `gui` session visibly displayed host-entered
`NATIVE23`, confirmed by the operator. Escape returned to the normal shell,
help completed, and exhausted primary/rescue requests were refused.

A second native boot used primary PID4/incarnation2. An explicit primary
exception stopped and reaped that owner before fresh rescue PID5/incarnation3
completed its own rebind. The operator confirmed correct blue rescue text.
After cleanup, three positive sessions ran on rescue generation2 with the same
input, composition, exception and complete-baseline checks. A subsequent
normal GUI visibly displayed host-entered `RESCUE23`. Escape and shell help
worked again. Both controlled reboots returned to readable JA OS prompts
without boot-menu, reset or power intervention, independently confirmed.

That is eight GUI sessions: six automated and two operator-observed sessions,
with two presenter and two compositor exceptions plus the separate primary
exception that entered fresh rescue. These are supported CPU-process failures
on retained operational scanout. After terminal owner cleanup there is no
reserved GPU rescue process or rearmed native attempt; full backend restart
and client reconnection remain separate work.

Three rebinds used174 register loads/six fixed store attempts, no replay, and
one poll each. Each advanced three frames. Normal GUI/input/fault/cleanup and
exhausted-request checks added no GPU register operation after owner finish.
Presentation still writes the exclusively mapped retained framebuffer.

Active captures contain72680 RX/783 host TX bytes. The initial1824-byte
reset transcript includes131 unattributed pre-reset bytes, retained and
excluded, plus1693 post-reset boot bytes. Image-scoped RX is74373;all saved
raw RX is74504. Boot coverage is partial and inter-session gaps are excluded.
UART diagnostics reported no drops, timeouts or stalls. A wrong host scanout
text expectation stopped capture after successful primary rebind; its failed
record is preserved and CPU-only continuation used the actual retired/GOP-refused
journal. Offline checker corrections are also retained. No hardware operation
was replayed and no image or production-code change was needed.

The software freeze has253 entries, with a separate physical inspection and
checksum manifest. The target is fresh firmware-backed v23,stage0/heartbeats0/
mask0,COM7 released. Full GPU lifecycle/restart, active-output policy replacement,
changed modes/HDMI, GPU reset/arbitrary hardware hangs and memory isolation
remain unqualified by this pass.


## Supervised GUI candidate

The v24 candidate adds `polaris-present` after a successful native or rescue
rebind. It downgrades every backend framebuffer PTE to read-only before returning
the existing CPU lease to the kernel. The backend and reserved rescue stay alive
on their private control endpoints; `usershell` then `gui` uses the normal
separate display service and compositor. Heartbeats and Ring3 recovery decisions
continue while the GUI owns its own exclusive writable mapping. The terminal
`polaris-finish` path remains available after GUI cleanup.

A primary failure first revokes that output generation and stops/reaps the old
backend. The GUI control thread observes revoked output and tears down its own
app/compositor/presenter state. The graphics worker neither races that cleanup
nor uses the GUI's reply endpoints. It waits for all GUI resource ledgers,
the KERNEL lease and kernel writer pins to drain before arming the one reserved
rescue. An eight-second CPU-drain admission budget and exact live policy/rescue
identity checks bound this wait. Timeout or lost context closes the attempt
without rescue register operations, retaining uncertain ownership for cleanup.
This is a software admission bound, not an interruptible GPU transaction or
whole-system recovery-latency guarantee.

Once fresh rescue has restored diagnostic output, `polaris-present` can admit
a new normal GUI on that new generation. This is explicit application relaunch,
not transparent state restoration. The fixed two primary/two rescue register
stores, one attempt each, remain unchanged. No new register, user ABI, raw BAR,
allocation, mode, reset or DMA/IRQ authority is added. A refused power transition
while GUI ownership remains live also keeps backend supervision running.

The host controller now covers96 cases, including16 supervised GUI/deferred
rescue cases: actual core code with service/device doubles. They cover live
GUI heartbeat progress, RO handoff, unknown ledgers/writer pins, drain deadline,
policy/rescue identity failure, power refusal and terminal no-replay paths.
Worker tests cover command dispatch and refused power retaining the worker.
The real Ring3 RAM test upgrades then downgrades a framebuffer and verifies
that an attempted write faults with no allocation. Normal GUI tests check the
drained-resource query against live sessions and full teardown.

Candidate image SHA-256: 6d95ea13a14922708c6b832b8069e0177ae526a8e75b5b87e5aa13c2bc6fe617.
Four clean final-image QEMU boots PASS:focused,multi,normal88/88 and deep88/88 across611 iterations. Actual Ring3 RW-to-RO write-protection fault and normal GUI/drained-ledger checks PASS. Subsequent physical supervised GUI evidence is recorded below.
The subsequent v24 physical supervised GUI/backend failure and fresh rescue
record follows; the v23 terminal-handoff evidence above remains separate.


## Observed supervised GUI and fresh rescue

On 2026-11-07 the Z390 Gaming X/F10, i7-9700K and RX570
1002:67DF revEF/subsystem1458:2310 passed the supervised GUI profile on the
Philips272M8 over DisplayPort. The unchanged v24 image SHA-256 is
6d95ea13a14922708c6b832b8069e0177ae526a8e75b5b87e5aa13c2bc6fe617.
The 273-entry software freeze remains separate from the physical record in
`build/rx570-session-v24/hardware`.

After the primary diagnostic rebind, `polaris-present` parked the backend RO
while its heartbeat and reserved rescue remained alive. Three actual normal
GUI/input/composition sessions passed, including separate presenter/compositor
exceptions and three complete resource baselines; heartbeats progressed250 to254.
The operator confirmed serial-entered NATIVE24 in a fourth normal GUI.
Escape and normal shell help returned successfully.

A delayed primary spin then ran with an explicit GUI-active witness. The Ring3
policy selected recovery, the old output was revoked, and the original GUI
thread completed its own teardown. The worker recorded GUI owners released
before the reserved rescue's first preparation read or store. The independent
rescue process26/incarnation22 restored the blue diagnostic display, confirmed
readable by the operator. Primary process25/incarnation21 and rescue completed
with frame deltas3 and6 respectively. The fixed total was114 register loads,
four store attempts, two polls and no replay.

Rescue `polaris-present` admitted generation2. Three more actual presentation
sessions and complete baselines passed, including separate presenter/compositor
exceptions; heartbeats progressed392 to395. A normal GUI visibly displayed
serial-entered RESCUE24, then Escape/help returned. A tenth GUI session was
active when the delayed rescue invalid-opcode fault occurred. The Ring3 policy
selected headless: no qualified output remained, the old GOP stayed retired,
the GUI drained itself, and independent serial shell help remained operational.
Five exhausted native/rescue/present/diagnostic requests were refused with no
further GPU operation.

Asynchronous terminal cleanup retained a kernel ledger while GUI teardown was
still pending. The pre-prepare `power` query correctly reported non-quiescent
objects. The controlled `reboot` command subsequently completed graphics
`power_prepare`, passed the existing object invariants and reset. JA OS returned
with fresh firmware scanout,stage0/heartbeats0/mask0 and power readiness YES.
The operator confirmed readable output with no boot-menu,reset or power
intervention. Automatic terminal-ledger draining remains follow-up work.

Thirteen active serial captures contain71273 RX and884 host TX bytes, including
the controlled reset; the passive watchers received no initial boot bytes.
Inter-session gaps are excluded, and flashed image identity is operator-confirmed
without USB byte readback. UART counters reported zero drops/timeouts/no stall.
Concurrent worker and GUI serial lines interleaved. An offline exact two-stream
byte partition verifies both complete journals in the357-byte primary and286-byte
rescue regions; the original bytes are retained unchanged. The primary host
cleanup-string assertion and the later pre-prepare readiness assertion remain
saved as FAILED records. CPU-only continuations verified state and performed
the normal prepared reboot; neither GPU operation was repeated.

This pass qualifies ten actual GUI sessions on this retained DP allocation and
software-owner fault profile. Initial GUI clients exit on generation revocation;
rescue GUI relaunch is explicit. General GPU-owner restart, client state recovery,
automatic terminal-ledger drain,new modes,HDMI,GPU hardware reset/arbitrary
bus hangs and physical DMA/IRQ/IOMMU isolation remain open. Firmware-inherited
bus mastering is still unisolated.


## Deferred terminal CPU cleanup candidate

V25 addresses the retained kernel capability observed during v24 rescue-fault
GUI teardown. An otherwise complete terminal CPU teardown keeps its worker until
original GUI ledgers,KERNEL lease and kernel writer pins drain. It then consumes
one final capability-release attempt. No GPU operation,service RPC,assignment
mutation or competing GUI cleanup occurs. Partial quarantine,MMIO or exact-policy
teardown failures remain retained for explicit cleanup. Failed final revoke is
not automatically retried. Unproven ownership can retain the CPU-only wait
indefinitely;this adds no GPU retry or display-recovery guarantee.

A read-only power query may collect a completed worker through its existing
serialized channel. It neither polls nor stops an active backend or pending
terminal drain. Controlled reboot preparation remains available. User binaries,
ABI,register sets and once-per-boot hardware budgets are unchanged. Serial line
interleaving remains a separate limitation.

Host104 controller cases pass,including eight terminal-drain cases and the
previous96 scenarios. Worker/admission tests cover completed-worker collection,
active/pending preservation and no core polling. Four clean QEMU boots pass:
focused,multi,normal88/88 and deep88/88 across611 iterations. All26 packaged user
payloads equal v24;ELFs have no runtime relocations. Default-off native/capture
build gates pass. The physical terminal cleanup result is recorded below.

Tested image SHA-256: 912aa3d0a4452b5dda7ab358f207c1b7e7406feee3876679d05fb682a30daa6f in `build/rx570-drain-v25`.
The subsequent hardware run repeats active-GUI primary/rescue failures and
captures automatic capability release and readiness before explicit reboot
preparation. V24 software273/hardware147 records and the earlier pre-prepare
readiness refusal remain preserved separately.


## Observed automatic terminal cleanup

On 2026-11-07, the unchanged v25 image
912aa3d0a4452b5dda7ab358f207c1b7e7406feee3876679d05fb682a30daa6f passed
physical terminal CPU cleanup on Z390 Gaming X/F10,i7-9700K,RX570
1002:67DF revEF/subsystem1458:2310 and Philips272M8 over DisplayPort.
The 257-entry software freeze remains unchanged; the separate physical record
is in `build/rx570-drain-v25/hardware`.

Ten actual GUI sessions passed: six automated input/composition cycles with
complete resource baselines, two visual sessions showing NATIVE25/RESCUE25,
and two sessions explicitly active during primary spin/rescue exception.
The automated suites contained two presenter and two compositor exceptions;
primary heartbeats progressed53 to56, rescue176 to180. Primary process25/inc21
and fresh rescue26/inc22 rebinds advanced3 and6 frames respectively. Old-GUI
ledgers,lease and writer pins drained before the reserved rescue operations.
Exactly114 register loads/four fixed stores/two polls occurred,without replay.

After the GUI-active rescue invalid-opcode fault, the Ring3 policy selected
headless. The old GUI returned to its serial shell and the periodic worker
recorded exactly one automatic final CPU-capability release. No explicit
finish,power_prepare,status collection or reboot preceded that release. Normal
shell help and monitor entry then succeeded. The first `power` query collected
the completed worker and reported shutdown/reboot ready and storage safe.
Following status showed terminal CPU drain wait NO and no live worker identity.
No GPU operation followed rescue failure,including five exhausted requests.
Saved GOP geometry remains available as metadata;access stayed refused while
no qualified output existed.

The controlled reboot subsequently returned to fresh firmware scanout,status0/
heartbeats0/mask0 and power readiness YES. The operator confirmed readable output
without boot-menu,reset or power intervention. Initial primary,rescue and both
normal GUI displays were also confirmed readable and correctly oriented.

Eleven active captures contain71664 RX/890 host TX bytes. Passive capture adds
1694 initial post-page-table boot bytes,for73358 image-scoped and total saved
RX bytes. Loader/earlier boot bytes and inter-session gaps are outside coverage;
image flashing is operator-confirmed without USB byte readback. UART counters
reported zero drops/timeouts/no stall. Exact two-stream byte partitions verify
the complete interleaved worker/GUI journals in357-byte primary and286-byte
rescue regions,without altering the raw bytes. All eleven active capture
sessions passed;no host assertion correction or GPU replay was needed.

This qualifies automatic cleanup after the tested software-owner failure and
otherwise successful CPU teardown. Ambiguous teardown,failed final revoke and
unproven ownership still retain resources;their refusal paths have host coverage.
The profile remains operational retained DisplayPort scanout. General driver
restart/client reconnection,state restoration,new modes,HDMI,hardware GPU reset,
arbitrary bus hangs and physical DMA/IRQ/IOMMU isolation remain separate work.
Firmware-inherited bus mastering remains unisolated.


## Explicit GUI reconnection

The v26 implementation adds `gui reconnect` to the normal userspace shell. After a
native-backed GUI loses its output, the original control thread must completely
remove its app, input portal, surface, presenter and compositor before retaining
a one-use lost-output ticket. Firmware sessions, normal Escape, service failure
on an otherwise valid output, and incomplete cleanup create no ticket. Starting
another GUI clears any outstanding ticket.

After the fresh rescue display is ready, `gui reconnect` consumes that ticket
and asks the existing Ring3 graphics supervisor to admit one new session. The
kernel checks the exact old/replacement output generations, the rescue process
and policy identities, all GUI ledgers and writer pins, and the rescue's current
exclusive CPU lease. It requires a fresh rescue heartbeat. The Ring3 policy
allows reconnection once in its rescue stage; the kernel consumes the attempt
before transport and enforces the same bound independently. Unknown or ambiguous
results are not replayed. A request issued too early or a failed launch consumes
the ticket; this entry point does not retry or restore application state.

Once authorized, the backend is downgraded RO before reclaiming the CPU lease.
The normal GUI creates new app, surface, input and service identities on the
new rescue generation. No old input, surface contents, pending operation or app
state is copied. The shell announces the fresh-session behavior and startup logs
record connection identities. Backend/rescue supervision remains active. No
GPU operation, new mode, raw BAR grant, VRAM allocation or reset is added, and
the existing once-per-boot primary/rescue programming budget is unchanged.
Initial native takeover still uses the existing explicit preparation path;
this is client reconnection, not general GPU-owner restart.

The private graphics-policy envelope is version2; unknown/legacy versions and
replayed or wrongly staged requests are refused. Kernel and supervisor are
packaged together. The normal console adds a shell action, with no new syscall
or client capability. Only supervisor.elf and consoleservice.elf change among
26 user payloads; primary, rescue, presenter, compositor and app ELFs are unchanged.

Host checks pass 122 controller cases, including 18 reconnect/unsafe-CONNECT
cases, plus 640 policy stage/fact cases, existing envelope/health checks and 11
GUI-ledger ticket gates. Actual Ring3 policy tests exercise CONNECT and duplicate
refusal without GPU I/O. Four clean QEMU boots pass: focused, multi, normal
88/88 and deep88/88 across611 iterations. Actual Ring3 `gui reconnect` refuses
an ineligible firmware session while preserving the normal shell. This emulator
result does not establish positive native client reconnection.

Tested image SHA-256: 23537263f3411634e9dfcaf66fc56fbdc2c68a68074e3be328bcf33bfe951f48 in `build/rx570-reconnect-v26`.
The physical pass below qualifies that sequence for the retained DisplayPort
profile. V25 software257/hardware137 evidence remains separate. General GPU
lifecycle/restart, state restoration, changed modes/HDMI, hardware GPU reset and
DMA/IRQ/IOMMU isolation remain open.


## Observed explicit GUI reconnection

On 2026-11-09, image
23537263f3411634e9dfcaf66fc56fbdc2c68a68074e3be328bcf33bfe951f48 passed
physical `gui reconnect` acceptance on Z390 Gaming X/F10, i7-9700K, RX 570
1002:67DF rev EF/subsystem 1458:2310 and Philips Momentum 272M8 over DisplayPort.
The 276-entry software freeze is unchanged. Separate raw logs, plans and offline
inspection are in `build/rx570-reconnect-v26/hardware`. Flashing is operator
confirmed; no USB byte readback is claimed.

The normal GUI was active during the delayed primary spin. The original GUI
control thread stopped its clients and released the display resources before
any reserved-rescue preparation reads or stores. The fresh blue rescue display
was visually confirmed. From the returned normal Ring3 shell, `gui reconnect`
then consumed the lost-output ticket and received exactly one Ring3 CONNECT
authorization. No manual rescue `polaris-present` or intervening diagnostic GUI
was used. The rescue backend became RO before the fresh GUI started on output
generation 2, replacing lost generation 1.

The old app PID 41/input 5/presenter PID 39 incarnation 31/compositor PID 40
incarnation 32 became app PID 44/input 6/presenter PID 42 incarnation 33/
compositor PID 43 incarnation 34. All recorded connection identities changed.
The operator confirmed correctly oriented NATIVE26 and RECON26 input on the
normal GUI, as well as green primary and blue rescue banners. Escape and shell
help worked. Another `gui reconnect` after normal Escape was refused without
starting a session. No old application, input or surface state was restored.

Ten GUI sessions ran: six automated input/composition cycles with six complete
resource baselines, two visually checked sessions, and two sessions active during
backend failures. The suites included two presenter and two compositor exceptions.
Primary heartbeats progressed 29 to 34; rescue 134 to 138. Primary PID 25/inc 21
and fresh rescue PID 26/inc 22 advanced 3 and 6 frames respectively. The entire
native boot used exactly 114 register loads, four fixed store attempts and two
polls: one primary transaction and one rescue transaction. Reconnection added
no GPU operation or programming-budget rearm.

A subsequent rescue exception while another GUI was active selected headless
operation. The original GUI drained and exactly one automatic final CPU
capability release occurred. The first power query, before status or explicit
reboot preparation, reported shutdown/reboot ready and storage safe. The worker
was collected, no qualified scanout remained, and saved GOP access stayed refused.
Normal shell control survived; terminal `gui reconnect` and five exhausted GPU
requests were refused without hardware operations. A controlled reboot returned
to firmware scanout, stage 0/heartbeats 0/mask 0 and power readiness. The operator
confirmed readable output without boot-menu, reset or power intervention.

All twelve active serial sessions passed, preserving 69153 RX and 872 host TX
bytes. The closed passive logger captured zero initial-boot bytes; the initial
boot, earlier loader output and inter-session gaps are outside coverage. The
controlled reboot is captured. Observed UART counters reported zero drops, zero
timeouts and no stall. Exact two-stream byte partitions verify the complete
interleaved worker/GUI journals, including the lost-ticket message, in 441-byte
primary and 370-byte rescue regions. Raw bytes are unchanged. No host capture
assertion correction, production change or hardware replay was required.

This qualifies explicit fresh client reconnection after the tested software
owner failure on the retained DisplayPort profile. Initial takeover remains
explicit. Full GPU-owner lifecycle/restart, application state restoration, changed
modes, HDMI, GPU reset/arbitrary hardware hangs and DMA/IRQ/IOMMU isolation remain
open. Firmware-inherited bus mastering remains unisolated.


## Native GUI launch from the normal shell

`gui native` requests the first qualified native takeover and opens the normal
GUI directly from the Ring3 shell. It uses the existing exact-board/firmware/
power/serial profile, reserved primary and rescue processes, Ring3 BEGIN/PRIMARY
policy admission and once-per-boot rebind. No emergency-monitor command is needed
for this launch. The trusted boot descriptor is passed through the kernel shell
context; userspace supplies neither a boot pointer nor register addresses.

One serialized graphics-worker request performs preparation, checks the current
policy/rescue identities and a fresh primary heartbeat, then downgrades the
backend framebuffer mapping RO and reclaims its CPU lease before GUI admission.
The GUI constructor independently requires a current native output generation.
A refused or uncertain launch starts no GUI implicitly, including no firmware
GUI fallback, and the shell remains available. Existing `gui` still uses the
currently valid output. The native launch entry refuses an existing or exhausted
backend; normal Escape does not rearm GPU programming. Use `gui` for another
session on healthy output, or the qualified `gui reconnect` after fresh rescue.

No new syscall, boot ABI, graphics-policy version, capability grant, GPU operation,
register budget, mode, VRAM allocation, reset, IRQ or DMA access is introduced.
Native primary/rescue executables and the supervisor are unchanged from the
physically tested reconnect build; only the console user payload changes.
General GPU-owner restart and automatic boot takeover remain separate work.

The software evidence and image identity are frozen in
`build/rx570-launch-v27/validation.json`. The subsequent physical launch pass is
recorded below. The earlier reconnect hardware record remains separate and
qualifies its own unchanged image.

Software qualification passes 136 production controller cases, including 14
launch scenarios, plus worker, presentation, ticket, policy and build checks.
Four clean QEMU boots pass: focused, multiple GPUs, normal 88/88 and deep
88/88 across 611 iterations. The actual Ring3 shell refuses unsupported native
launch and malformed commands without register access or an implicit GUI, then
still runs the normal firmware GUI. No native output is fabricated.
Tested image SHA-256: 1e880898115770c27bd3b2f068a03427a87c7f025c62f69b56c2ad6b0fe7d77c.


## Observed normal-shell native launch

On 2026-11-09, image
1e880898115770c27bd3b2f068a03427a87c7f025c62f69b56c2ad6b0fe7d77c passed
physical `gui native` acceptance on Z390 Gaming X/F10, i7-9700K, RX 570
1002:67DF rev EF/subsystem 1458:2310 and Philips Momentum 272M8 over DisplayPort.
The 273-entry software freeze remains unchanged. Separate plans, raw logs and
inspection are in `build/rx570-launch-v27/hardware`. Flashing is operator confirmed
without USB byte readback.

The normal Ring3 shell issued `gui native` from fresh firmware output. One worker
request completed the existing primary/rescue reservation, policy admission and
fixed rebind, then handed the backend framebuffer RO to the normal presenter.
No manual monitor takeover or presenter handoff command was used. The operator
confirmed NATIVE27 input on the correctly oriented GUI. Initial app PID 29/input
1/presenter PID 27 incarnation 23/compositor PID 28 incarnation 24 used native
output generation 1. Escape and shell help worked. Another `gui native` was
refused without another GUI or register operation.

Three primary and three rescue presentation cycles passed with six complete
resource baselines, including two presenter and two compositor exceptions.
Heartbeats progressed 40 to 44 and 194 to 198. A further GUI was active during
the delayed primary spin. Its original control thread drained the clients and
lease before reserved-rescue preparation reads or stores. The fresh blue rescue
banner was visually confirmed. Normal `gui reconnect` received one Ring3 CONNECT
and created generation-2 clients without manual rescue presenter handoff.
Lost app PID 41/input 5/presenter PID 39 incarnation 31/compositor PID 40
incarnation 32 became app PID 44/input 6/presenter PID 42 incarnation 33/
compositor PID 43 incarnation 34. The operator confirmed readable RECON27 input.
Duplicate reconnect after Escape and terminal reconnect were refused.

Ten GUI sessions ran in total: six automated baseline cycles, two visual sessions
and two active during backend injections. Primary PID 25/incarnation 21 and fresh
rescue PID 26/incarnation 22 advanced 3 and 6 frames. Exactly 114 register loads,
four fixed store attempts and two polls occurred: one primary and one rescue
transaction. Reconnect, repeat launch and exhausted requests added no hardware
operation, retry or programming-budget rearm. No old application state was restored.

A rescue exception during the last GUI selected headless operation. After the
original GUI drained, exactly one automatic final CPU capability release occurred.
The first power query, before status or explicit preparation, collected the
completed worker and reported shutdown/reboot ready and storage safe. Serial
shell control survived, no qualified output remained, and retired GOP access
stayed refused. Five exhausted device requests performed no GPU operation.
Controlled reboot returned to fresh firmware scanout, stage 0/heartbeats 0/mask 0
and power readiness. The operator confirmed readable output without boot-menu,
reset or power intervention.

All eleven active captures passed, preserving 69324 RX/849 host TX bytes.
Passive capture saved 1696 bytes: two leading NUL bytes remain saved and excluded
as unattributed; 1694 following bytes capture the post-page-table boot through
the normal shell. Image-scoped RX totals 71018; all saved raw RX totals 71020.
Earlier boot/loader output and inter-session gaps are outside coverage. Observed
UART counters reported zero drops, zero timeouts and no stall. Exact two-stream
partitions verify complete 441-byte primary and 370-byte rescue worker/GUI
interleavings without rewriting raw bytes. No host capture assertion correction,
production change or GPU replay was required.

This qualifies explicit first native GUI launch and fresh client reconnection
for the retained DisplayPort profile and tested software-owner failures. General
GPU-owner restart, automatic boot takeover, state restoration, changed modes,
HDMI, hardware GPU reset/arbitrary hangs and DMA/IRQ/IOMMU isolation remain open.
Firmware-inherited bus mastering remains unisolated.


## Retained primary driver restart

`gui restart-driver` requests one explicit replacement of the primary Ring3
process and starts a fresh GUI on the same validated native output. It applies
after `gui native` has parked the primary framebuffer mapping RO and the
original GUI has fully stopped. The kernel requires intact output, the KERNEL
lease, drained GUI resources and writer pins, the exact current policy/rescue
identities, and fresh primary/rescue heartbeats. An armed diagnostic, an active
GUI, firmware output, rescue output or an exhausted attempt refuses admission.

Ring3 policy version 3 admits one restart and enters a pending state. The kernel
consumes its attempt before RPC. It stops and fully reaps the old primary before
reusing its service storage, then loads a fresh primary with new PID, incarnation,
endpoints and private pages. The framebuffer is borrowed RO; the scanout buffer,
kernel device binding and independently reserved rescue remain retained.

The fresh primary enters the private version-3 RETAINED operation. The kernel
enforces RO permissions across the complete borrowed mapping before delivery and
consumes programming authority. The user process also refuses CONFIG, STEP,
EXECUTE, DRAW and duplicate adoption in this entry. Fresh heartbeat and exact
policy completion are required before a new GUI starts. Scanout generation stays
fixed while CPU driver and GUI identities change. No application state is replayed.
No register load/store, GPU reset, new mode, IRQ/DMA operation or programming
budget is added. The original primary/rescue transaction limits remain once per
boot. A later primary failure can still use the original reserved rescue.

Uncertain transport, failed teardown, failed replacement or failed completion
cannot be retried through this command. The kernel revokes output and retains
quarantine or incomplete CPU ledgers, with serial diagnostics. This qualifies
neither hardware GPU restart nor DMA quiescence. Automatic restart of an already
hung primary is outside this entry; its existing qualified path remains rescue.
The fresh GUI also starts without the previous app/input/surface state.

The software record is frozen in `build/rx570-restart-v28`; the subsequent
physical retained-primary restart is recorded below. Earlier image passes remain
separate evidence. Changed modes, HDMI and arbitrary hardware hangs are open.

The bootloader banner now reads `JA OS UEFI loader`. The boot ABI and image layout
are unchanged; the sample hello application keeps its existing program name.

Software checks pass 165 controller scenarios, including 29 retained restart
cases, plus 3584 restart and 2560 reconnect policy combinations. Actual Ring3 RAM
adoption, whole-range RO permissions and programming/drawing refusal pass.
Four clean QEMU boots pass: focused, multiple GPUs, normal 88/88 and deep 88/88
across 611 iterations. Actual firmware restart refusal leaves the shell usable.
The built loader contains the JA OS banner and no legacy Hello OS banner.
Tested image SHA-256: 01787cd072a2cfb6e92829fa82a9c06120b5f8048419776ce652774685ca9ce1.


## Observed retained primary CPU-driver restart

On 2026-11-09, image
01787cd072a2cfb6e92829fa82a9c06120b5f8048419776ce652774685ca9ce1 passed
physical retained-primary replacement on Z390 Gaming X/F10, i7-9700K, RX 570
1002:67DF rev EF/subsystem 1458:2310 and Philips Momentum 272M8 over DisplayPort.
The 278-entry software record and image remain unchanged. Separate plans, raw
logs and inspection are in `build/rx570-restart-v28/hardware`. Flashing is operator
confirmed without USB byte readback.

Normal `gui native` completed the original primary transaction and opened output
1. The operator confirmed readable NATIVE28 input. After Escape and full GUI
teardown, `gui restart-driver` stopped primary PID 27/incarnation
23 and loaded PID 32/incarnation 27 through
the RO retained entry. It started a fresh GUI on the same output generation 1,
with visible RSTART28 input. Initial app PID 31/input 1/presenter PID 29 incarnation 25/compositor PID 30 incarnation 26
became app PID 35/input 2/presenter PID 33 incarnation 28/compositor PID 34 incarnation 29.
The restart phase contains no GPU preparation reads, executor reads or stores.
Duplicate restart after Escape was refused without another GUI or transaction.
The driver and policy use the new private version-3 retained protocol; software
checks cover whole-range RO permissions and refused CONFIG/EXECUTE/DRAW requests.

Three primary and three rescue presentation cycles passed, restoring six complete
resource baselines, with two presenter and two compositor exception cases.
Heartbeats progressed 74 to 78 and
187 to 190. A GUI was active during
the delayed hang of the replacement primary. Original GUI clients drained before
rescue register access. The reserved rescue PID 28/incarnation
24 restored the blue banner, visually confirmed. Its initial
reservation identity is absent from the journal: retaining the original rescue
is established by unchanged implementation and controller tests, rather than a
directly observed initial/final rescue-identity comparison.

Normal `gui reconnect` obtained one Ring3 CONNECT for output 1 to 2 and opened
fresh clients without state replay or GPU operation. Lost
app PID 47/input 6/presenter PID 45 incarnation 36/compositor PID 46 incarnation 37
became app PID 50/input 7/presenter PID 48 incarnation 38/compositor PID 49 incarnation 39.
The operator confirmed readable RECON28 input. Duplicate reconnect after Escape,
terminal reconnect and terminal restart were refused.

Eleven GUI sessions ran: six automated baseline cycles, three visual sessions and
two active during backend injections. Exactly 114 register loads, four fixed
store attempts and two polls occurred in one primary and one rescue transaction;
frame deltas were 3 and 6. Restart, reconnect and exhausted
requests added no hardware operation, replay or programming-budget rearm.

An exception in the active rescue driver selected headless operation. The OS
retained serial control. After original GUI teardown, exactly one automatic final
CPU capability release occurred. The first power query, before status or explicit
preparation, reported shutdown/reboot ready and storage safe; the completed worker
was collected. Five exhausted device requests refused further GPU operation.
Controlled reboot returned fresh firmware scanout/status 0/heartbeats 0/mask 0 and
restart-attempt NO. The operator confirmed a readable prompt without boot-menu,
reset or power intervention.

All twelve active captures passed: 72171 RX/910 host TX bytes.
Passive capture saved 1827 bytes: 133 leading bytes remain saved and excluded as
unattributed; 1694 following bytes cover post-page-table boot through the shell.
Image-scoped RX totals 73865; all saved raw RX totals 73998.
Earlier boot/loader and inter-session gaps are excluded. UART observations report
zero drops, zero timeouts and no stall. Exact two-stream partitions verify the
441-byte primary and 370-byte rescue worker/GUI interleavings; raw bytes are
unchanged. No active host assertion correction, production change or GPU replay
was required. One offline read-only document query had a Windows quoting error;
its harmless correction is recorded separately.

The loader binary contains `JA OS UEFI loader` with the legacy banner absent.
This hardware pass qualifies one planned healthy CPU-owner replacement on the
retained DisplayPort profile, followed by the existing rescue and serial fallback
paths. General GPU reset/restart, arbitrary hardware/interconnect hangs, changed
modes, HDMI, automatic boot takeover, application-state restoration and physical
DMA/IRQ/IOMMU containment remain open. Inherited bus mastering stays unisolated.


## Firmware ROM connector inventory

`displayrom` starts a separate read-only Ring3 parser for the selected RX 570's
firmware-provided ROM RAM copy. This is preparation for native DP/HDMI modesetting;
it does not establish a new display mode or an active connector.

Before ExitBootServices, the loader copies at most 256 KiB from the exact selected
1002:67DF controller's PCI I/O RomImage/RomSize RAM buffer. It performs no ROM BAR
access, PCI write, Attributes call, decompression or firmware execution. Absence,
oversize, allocation failure or source change leaves optional inventory unavailable
and does not block boot. The boot handoff is version 11/1632 bytes with every old
field offset and the stack-at-offset-zero contract preserved. Matched loader and
kernel must be deployed together.

The copy is a dedicated EfiLoaderData allocation, excluded from the conventional
memory allocator and retained for the boot. Even a changed-source allocation
remains reserved. Only a complete bounded copy inside the recorded LoaderData
extent can be mapped to the parser. Overlapping non-loader descriptors refuse.
The process receives that RAM read-only and non-executable plus two private IPC
endpoints. It receives no device, framebuffer, IRQ, DMA or I/O-port authority.
All borrowed pages are checked independently; failed CPU teardown retains its
service ledger and prevents storage reuse. The original source allocation is
never freed by the diagnostic process.

The userspace decoder validates the first uncompressed legacy PCI image only:
ROM/PCIR signatures, PCIR identity and length, image checksum, ATOM header and
master-data revision 1.1, ObjectInfo revisions 1.1 through 1.3, complete table
extents, at most 16 unique connector objects, reserved fields/bits and bounded
link/record offsets. It reports raw object IDs and DP/HDMI-A inventory labels.
It does not follow connector records, select GPIO/AUX/I2C resources, choose a
link/encoder, interpret command tables, or program hardware. Unsupported images
and revisions report a parser status with no partial table results. CRC32 names
the copied bytes for diagnostics; it is not authenticity or firmware trust proof.

Status values: 0 accepted, 1 size, 2 ROM signature, 3 PCIR header, 4 identity,
5 legacy image length/type, 6 checksum, 7 ATOM header, 8 revision, 9 table extent,
10 connector directory. Capture status 6 means copied RAM, independently of
parser status 0. Missing/unsupported data is an expected refusal outcome.

The layout/ID facts are pinned to Linux v6.12 commit
adc218676eef25575469234709c2d87185ca223a in `POLARIS_ROM_REVIEW.json`.
`tools/audit_polaris_rom.py --source-dir build/rx570-route-v30/references`
now checks five source hashes and independent structure/ID/routing facts. The
original v29 audit checked four; its frozen record remains unchanged. UEFI's
[PCI I/O protocol](https://uefi.org/specs/UEFI/2.10/14_Protocols_PCI_Bus_Support.html#efi-pci-io-protocol)
defines RomImage as an in-memory copy. The reviewed ATOM definitions are in
[atombios.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/atombios.h)
and the connector labels in
[ObjectID.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/radeon/ObjectID.h).
These references provide format facts, not JA OS hardware qualification.

Software checks pass 513 protected-page prefix cases, 26 malformed vectors,
three revisions, duplicate/identity/checksum/size guards, 66 mocked loader cases,
the pinned-source audit and existing native/MMIO/framebuffer regressions. Four
clean final-image QEMU boots pass: focused, multiple GPUs, normal 88/88 and deep
88/88 across 611 iterations. Actual Ring3 fixtures exercise the full 64-page RO/NX
mapping, DP/HDMI objects, checksum refusal and complete CPU resource restoration.
No GPU is used by those ROM tests. No registry slot was added. The archive has
27 files/21 ELFs; all 26 previous files, including 20 ELFs, match v28 exactly.
The new parser remains absent from the kernel ELF.

Tested image SHA-256: d0a7e9cb4d07b05d834b4323f26a2b871b6c3e542dd2a1f629d455d3d30f97a8.
Software evidence is frozen in `build/rx570-rom-v29/validation.json`.
The subsequent physical firmware-copy and board-directory acceptance is below.
The existing v28 hardware record stays separate; native timing/link programming,
HDMI, hardware reset, DMA/IRQ/IOMMU isolation and full display reinitialization
remain open. The working retained-scanout and rescue programming budgets do not
change. The JA OS UEFI loader banner remains in the built image.


## Observed firmware ROM connector inventory

On 2026-11-09, image
d0a7e9cb4d07b05d834b4323f26a2b871b6c3e542dd2a1f629d455d3d30f97a8 passed
physical ROM inventory on Z390 Gaming X/F10, i7-9700K, RX 570
1002:67DF rev EF/subsystem 1458:2310 and Philips Momentum 272M8 over DisplayPort.
The 273-entry software record and 149 source fingerprints remain frozen. Separate
plans, byte-exact serial logs and inspection are in `build/rx570-rom-v29/hardware`.
Flashing is operator confirmed, without USB byte readback.

Firmware supplied a complete 118784-byte RAM copy. The isolated parser
accepted the first 59904-byte legacy PCI image and ObjectInfo
1.3. Four inspections returned CRC32 0xE2CB542E and the same directory:
two before native takeover around the RAM diagnostic, one after native takeover,
and one after controlled reboot. ROM header offset is 0x226,
master data 0x9800, ObjectInfo 0x9f2c,
with ObjectInfo extent 0x11e. Offsets below are relative to
ObjectInfo; link and record contents were not traversed.

| Index | Object ID | Firmware label | Links offset | Records offset |
| --- | --- | --- | --- | --- |
| 0 | 0x3113 | DisplayPort | 0x62 | 0x68 |
| 1 | 0x3213 | DisplayPort | 0x7d | 0x83 |
| 2 | 0x3313 | DisplayPort | 0x98 | 0x9e |
| 3 | 0x310c | HDMI-A | 0xb3 | 0xb9 |

These are three DisplayPort objects and one HDMI-A object in firmware data.
The connected physical DP port, active encoder, AUX/I2C/HPD routing and link setup
remain unknown. No raw ROM file was exported for independent host replay. CRC32
identifies the reported copy for diagnostics, not source authenticity. Successful
physical reports returned through full RO/NX mapping checks and bounded service
teardown. A fresh process per command follows the frozen launch/stop code; parser
PID/incarnation is absent from the journal, so a direct four-identity comparison
cannot be claimed. The source LoaderData allocation remains retained for the boot.

The actual Ring3 RAM diagnostic passed its 64-page RO/NX, synthetic DP/HDMI,
checksum-refusal and full CPU baseline cases. Native primary/rescue RAM tests also
passed. The separate physical ROM reports are the board-table evidence; synthetic
tables are not treated as captured hardware data.

Normal `gui native` opened generation 1, with readable NATIVE29 input visually
confirmed. Primary PID 23/incarnation 21 advanced
3 frames. Initial GUI app PID 27/input 1, presenter
PID 25/incarnation 23 and compositor PID
26/incarnation 24 were recorded.
Repeat native launch after Escape refused another GUI or device operation.
Three further normal presentation cycles restored three complete baselines,
including one presenter and one compositor exception case. Heartbeats progressed
71 to 75. Four GUI sessions ran in total.

Exactly 60 register loads, two fixed store attempts and one poll occurred in the
original primary rebind. ROM inventory performed no GPU operation. The reserved
rescue was stopped without programming; this pass does not repeat v28's rescue or
primary-replacement acceptance. Finish reclaimed the validated native console,
kept the original GOP retired, stopped CPU owners, and allowed power readiness.
Controlled reboot restored fresh firmware output/status 0/heartbeats 0/mask 0/
restart-attempt NO. The operator confirmed readable output with no intervention.
The ROM report remained identical after reboot.

All four active capture plans passed, saving 40256 RX/395 host TX bytes.
Passive capture saved 1701 bytes: seven leading bytes remain saved/excluded as
unattributed; 1694 following bytes cover post-page-table boot through the shell.
Image-scoped RX totals 41950; all saved raw RX totals 41957.
Earlier boot/loader output and inter-session gaps are excluded. UART observations
reported zero drops, zero timeouts and no stall. No host assertion correction,
production change or hardware replay was required during this physical pass.

Next is bounded parsing of connector link/record data, including AUX/I2C/HPD
routing, before a new native timing/link programming contract. This acceptance
establishes the optional firmware-copy/connector-directory path and retained
native GUI regression. New modes, HDMI operation, GPU reset/reinitialization,
arbitrary hangs and DMA/IRQ/IOMMU isolation remain unqualified. Inherited bus
mastering remains unisolated; full native modesetting is still unfinished.


## Firmware connector routing inventory

The next `displayrom` report extends the isolated ROM service with bounded
connector source/destination lists, I2C and HPD records, and their GPIO table
assignments. It reads the existing firmware RAM copy. These are firmware data;
an active physical port, encoder or AUX channel has not yet been established.
The earlier directory-only v29 physical record above remains separate.

Each list has at most eight unique object IDs. The decoder bounds up to 64
records per connector, validates exact type 1 I2C/type 2 HPD layouts and requires
a terminator. Type 0xff ends the list independently of its size byte; a zero
record with zero size is also accepted. Critical duplicate records refuse the
route. Other known types 3..22 are skipped by their checked extent: their
payload semantics are not validated. Unknown types refuse. Declared object
directories and the next declared section constrain traversal.

An I2C ID must match exactly one 27-byte GPIO_I2C_Info 1.1 assignment; an HPD ID
must match exactly one four-byte GPIO_Pin_LUT 1.1 entry. Table extents, entry
counts, GPIO shifts and supported revisions are checked. Reports retain the
raw register indices, shifts, IDs and table offsets. A register index is not a
CPU address or permission to access hardware. No GPIO, AUX, I2C or HPD operation,
ROM BAR access, ATOM command execution or new GPU authority is introduced.

Base parser status values retain their meanings. A malformed routing subsection
clears that route to status only; the accepted directory and other valid routes
survive. Status 0 means structurally decoded inventory, and presence flags must
still be checked. It does not mean an operational link.

| Route status | Meaning |
| --- | --- |
| 0 | Accepted inventory; inspect presence flags |
| 1 | Both link and record pointers absent |
| 2 | Source/destination list refused |
| 3 | Record framing, termination or critical payload refused |
| 4 | Duplicate ID/critical record or ambiguous GPIO assignment |
| 5 | Required GPIO table/assignment absent or invalid |
| 6 | Unsupported GPIO table revision |
| 7 | Table extent or section layout refused |
| 8 | Unsupported record type |

Flags are 1 links, 2 terminated records, 4 I2C record, 8 HPD record,
16 I2C GPIO assignment, 32 HPD GPIO assignment. The route line prints status,
flags and counts, followed by eleven packed hexadecimal fields:

| Field | Low-to-high packed values |
| --- | --- |
| 1, 2 | Source object IDs 0..3, then 4..7; 16 bits each |
| 3, 4 | Destination IDs 0..3, then 4..7; 16 bits each |
| 5 | I2C ID, slave, HPD GPIO ID, plugged state; 8 bits each |
| 6, 7 | Eight raw I2C GPIO register indices; 16 bits each |
| 8 | Eight GPIO shifts; 8 bits each |
| 9 | HPD raw register index (16), shift (8), GPIO ID (8) |
| 10 | Encountered record-type bitmap; bit equals type |
| 11 | I2C table offset/bytes, PinLUT offset/bytes; 16 bits each |

The private ROM IPC protocol is now version 2; deploy the packaged service and
kernel together. Its maximum is 212 query words, but the kernel queries only the
20 directory words plus twelve words per reported connector. A four-second CPU
admission check surrounds the RPC loop. An in-flight RPC and CPU teardown keep
their existing bounds; this is neither a total four-second deadline nor a bus
transaction timeout. Failed teardown keeps its ownership ledger and prevents
service storage reuse. The retained report uses process-private writable memory
to fit the existing guarded one-page stack. No stack protection was relaxed.

The pinned-source audit now checks five hashed sources, including the upstream
record consumer, using `POLARIS_ROM_REVIEW.json` schema 2. Run:

```
python tools/test_polaris_rom.py --build-dir build/host-tests
python tools/audit_polaris_rom.py --source-dir build/rx570-route-v30/references
```

Host tests pass 27 routing malformed/boundary cases, 513 protected routing
prefixes, and the prior 513 base prefixes/26 malformed vectors/three revisions.
Four clean final-image QEMU boots pass: focused, multiple GPUs, normal 88/88 and
deep 88/88 across 611 iterations. Actual Ring3 RAM fixtures verify all twelve
route words, unique GPIO lookups, zero partial fields after a duplicate record,
a surviving peer/directory, the full 64-page RO/NX mapping, checksum refusal and
full CPU resource restoration. Those fixtures do not qualify physical routing.

Only `polarisrom.elf` changes among the archive's 27 files/21 ELFs; the other
26 files/20 ELFs match v29. The kernel and service are relocation-free, and the
vendor parser is absent from the kernel. Loader sources and boot ABI 11/1632
are unchanged; rebuilt PE/COFF timestamps differ. The first QEMU image exposed
a parser stack overflow; its logs/image are saved separately, and every passing
run uses the corrected image below.

Candidate SHA-256: 5f883b111a8142ad4606f5f89037d3fb4f0ea466ee2d7716f020c78028487a0b.
Software evidence: `build/rx570-route-v30/validation.json`. The subsequent
physical record refusals and native GUI regression are recorded below.
The next hardware pass will preserve raw serial reports, including any route
refusal, before defining a new native timing/link programming operation. Native
mode changes, HDMI operation, full GPU reset/reinitialization and DMA/IRQ/IOMMU
containment remain open. Existing native/rescue sequences and budgets are unchanged.


## Observed connector record refusals

Physical v30 on Z390/F10, i7-9700K, RX 570 1002:67DF rev EF/subsystem
1458:2310 and Philips Momentum 272M8 DisplayPort preserved the accepted ROM
copy and connector directory. It did **not** qualify successful routing decode.
All four routes returned status 3 (record decoding refused), with zero flags,
counts and all eleven data fields cleared. Four identical reports were captured
before/after the RAM diagnostic, after native takeover and after controlled
reboot. The exact unsupported/malformed record condition is not known yet;
no raw board-ROM bytes were exported in this pass.

The source report remains capture 6, 118784 copied bytes, first legacy image
59904 bytes, CRC32 E2CB542E, ObjectInfo 1.3, three DP objects and one HDMI-A
object. No active physical port, encoder or AUX channel was identified. This
is physical evidence that the refusal is contained and preserves directory
reporting, not evidence that the board's routing format is malformed.

The actual Ring3 synthetic route/golden-field/refusal-isolation tests passed,
along with the 64-page RO/NX and full CPU resource baseline checks. Synthetic
success remains separate from the physical record refusals. Native GUI input
NATIVE30 was visibly readable. Four GUI sessions/three complete resource
baselines, one presenter and one compositor exception, serial control, terminal
reclamation and power readiness passed. Primary PID 25/
incarnation 23 advanced 3 frames;
heartbeats progressed 25 to 29.
Exactly 60 register loads/two fixed stores/one poll occurred in the original
primary transaction. ROM diagnostics added no GPU operation. Reserved rescue
stopped without programming; no new rescue/restart case was qualified.

Controlled reboot returned to fresh firmware output/status 0/heartbeats 0/mask 0/
restart NO; readable output without intervention was operator confirmed.
The four active plans passed: 46729 RX/395 host TX bytes.
Passive capture preserved 1825 bytes: 131 leading bytes are saved/excluded as
unattributed and 1694 post-page-table boot bytes are attributed. Image-scoped RX
is 48423; all saved raw RX is 48554.
UART observations showed zero drops/timeouts and no stall. No production change,
host assertion correction or GPU replay occurred during the hardware pass.

Image 5f883b111a8142ad4606f5f89037d3fb4f0ea466ee2d7716f020c78028487a0b and the 282-entry software/154-source-fingerprint record remain
unchanged in `build/rx570-route-v30`. Separate serial plans, raw captures and
inspection are in its `hardware` directory. Flashing is operator confirmed,
without USB byte readback. Next is a bounded firmware-RAM byte diagnostic to
identify the refused record before changing the decoder; it must not read a
device ROM BAR or relax checks speculatively. Native timing/link programming,
HDMI, full GPU reset/reinitialization and hardware isolation remain open.


## Bounded firmware RAM byte diagnostic

Initial v31 design; its physical failure and the isolated reader repair follow below.

`displayrom-bytes OFFSET COUNT` prints a window from the exact selected GPU's
retained firmware ROM RAM copy. Both arguments are decimal; COUNT is 1..256.
For example, `displayrom-bytes 40748 256` addresses the first 256 bytes of the
previously observed ObjectInfo at ROM offset 0x9f2c. This example depends on the
reported copy/layout and does not discover a live GPU address.

The command rechecks selected PCI/subsystem provenance and the complete reserved
LoaderData source. Offset/count must fit the actual copied size; overflowing,
empty, oversized, missing/changed or unqualified sources refuse. The private
256-byte destination is cleared on failure and zero-padded after a short success.
A fixed-size kernel RAM copy and ordinary console formatting implement the
window; vendor table interpretation remains in the unchanged Ring3 parser.
There is no arbitrary-address read API, new syscall, device ROM BAR read, GPU
register/GPIO/AUX/I2C/HPD operation or table execution.

Successful output names copy size, offset and exact byte count, followed by
`ROM BYTE 0x...:` rows and a completion marker. Preserve byte-exact serial logs;
check contiguous offsets/counts and reject partial or inconsistent windows.
Sparse windows are not a complete ROM export. Existing `displayrom` CRC and
route reports remain separate; CRC32 is not source authenticity.

This diagnostic addresses v30's four status-3 record refusals. The decoder is
unchanged until the actual record bytes justify a supported-format change.
All 27 archive files/21 ELFs, including primary/rescue/supervisor/ROM service,
match v30 exactly. Boot ABI 11/1632 and private ROM protocol 2 are unchanged.
Loader object code matches after normalizing rebuilt COFF/PE timestamps.

Four clean final-image QEMU boots pass: focused/multiple GPUs, normal 88/88,
deep 88/88/611. Actual RAM tests check full and short windows, the final source
byte, crossing/overflow/zero/oversize/source-change refusals and cleared output.
Focused shell tests check exact decimal arguments and unqualified-source refusal.
Existing Ring3 routing/resource tests also pass. No registry slot was added.

Candidate SHA-256: 38244f918aea8f5dcd54f9d6598f949ccdfd25265a40e010529eb3f236dfea23.
Evidence: `build/rx570-bytes-v31/validation.json`; the subsequent physical
window fault is recorded below.
Next: capture ROM/header/master-data/ObjectInfo windows over COM7, then any
bounded GPIO table windows justified by those pointers. Successful board routing
decode, changed native modes/HDMI, full GPU reset and hardware isolation remain
open. Native/rescue code, authority and finite GPU budgets are unchanged.


## Physical byte-window kernel fault

The v31 image 38244f918aea8f5dcd54f9d6598f949ccdfd25265a40e010529eb3f236dfea23 failed its first physical `displayrom-bytes 0 256`
request on 2026-11-10 (Australia/Sydney). It printed the diagnostic banner,
then a kernel page fault: vector 14/error 0, RIP 0x2ec1f0b0,
CR2 0xffff80002ef11000, followed by CPU HALTED. No ROM byte row or completion
marker was exported. The host timed out and stopped the plan without retry.
The target needs a reset/power cycle to boot a replacement image.

The kernel implementation incorrectly treated `phys_to_virt` address arithmetic
as proof that reserved LoaderData had a mapped direct alias. The faulting load
is in the compiled byte-copy path. Allocator RAM used by the successful QEMU
fixture has such an alias; that fixture did not cover the physical reserved-ROM
mapping condition. The software passes remain frozen as observed and do not
qualify this physical operation.

Nine preceding commands passed: independent serial input/output, firmware
inventory, two unchanged CRC E2CB542E/all-route-status-3 reports, actual Ring3
RAM/routing and CPU window fixtures, the native RAM model, local UART statistics
and power readiness. No native GPU transaction, GUI launch or reboot followed.
The active capture preserves 16262 RX/137 TX bytes; passive capture preserves
1825 bytes, of which 131 leading bytes are saved/excluded and 1694 kernel boot
bytes are attributed. The image-scoped RX is 17956; all saved raw RX is 18087.
Prior v30 native GUI/reboot acceptance remains separate.

The frozen 249-entry software image and 154 source fingerprints are unchanged.
Failure evidence is in `build/rx570-bytes-v31/hardware`. The repair must remove
privileged ROM-source dereferences and use the isolated service's existing
checked RO/NX borrowed mapping, with bounded private IPC reads, cleared failed
results and retained cleanup ledgers. Physical record bytes and the cause of
the record refusals remain unknown; the decoder must not be relaxed on guesses.


## Isolated firmware byte-reader repair

`displayrom-bytes` now obtains bytes through the existing isolated Ring3 ROM
service's read-only, non-executable borrowed mapping. The kernel does not
convert or dereference a ROM-source physical address. This replaces v31's
failed direct-map implementation; the failure record above stays unchanged.

Private ROM protocol 3 adds reads of one through eight bytes. The process checks
the offset/count against its actual mapped copy and packs the bytes into one
reply word. Other operations keep their existing request constraints. The
kernel checks the reply shape, incarnation, requested offset and zero high bits
for short replies. At most 32 calls assemble a 256-byte window. The same startup
path independently checks every borrowed page's exact frame and RO/NX flags.
The process stops before the window is published; failed calls or cleanup clear
all output, while retained cleanup state prevents service reuse. The original
LoaderData allocation remains reserved for the boot.

A four-second CPU admission check surrounds the query loop. Startup, an
in-flight call and teardown retain their independent existing bounds; this is
not a total four-second deadline or a hardware transaction timeout. Reader
faults use the managed-service failure path. No privileged ROM byte load,
new syscall/capability, device ROM BAR, GPIO/AUX/I2C/HPD operation or GPU budget
is added. Routing format interpretation has not been relaxed. Deploy the full
matched image because the kernel and service now use private protocol 3;
boot handoff ABI 11/1632 and route report layout V2 are unchanged.

Host tests pass nine byte-reader golden/bounds vectors and 513 protected-page
prefix/end checks, alongside the existing routing/base parser corpus and
five-source fact audit. Four final-image QEMU boots pass focused/multi-GPU,
normal 88/88 and deep 88/88/611. Actual Ring3 window reads, exact RO/NX mapping,
short/max/final-byte and refusal cases, report/routing checks and complete CPU
resource restoration pass. The synthetic source remains allocator-backed; a
successful physical reserved-source window is still pending and is not inferred
from these passes.

Only `polarisrom.elf` changes among 27 archive files/21 ELFs; the other
26 files/20 ELFs match v31. Kernel/service ELFs remain relocation-free and the
vendor decoder remains outside the kernel. Native/rescue/supervisor/GUI binaries
and finite GPU budgets are unchanged. Evidence is frozen in
`build/rx570-isolated-v32/validation.json`.

Candidate SHA-256: 3fd76803e7b1925768c2438f50faed7cb66449c487baf90479741afe23f8449c.
The target is still halted on v31; reset/power-cycle into the replacement image
before physical testing. First capture one byte, then bounded overlapping ROM
windows and current source CRC/report. Only complete validated windows count as
exported data. Board record decoding and the cause of status 3 remain unresolved
until the bytes are captured; native changed modes/HDMI/reset/isolation remain open.

An initial deep run failed the display-surface input-client wait before the ROM
test ran (87/88,608 iterations);its image and logs are retained separately.
The wait observed READY/BLOCKED fields across a possible timer switch. It now
uses the existing UP interrupt save/restore pattern to observe and yield
atomically. The two-second timeout and all authority/resource checks remain.
All four final-image boots were rerun after this test-observer correction.


## Complete isolated firmware byte capture

The physical v32 pass exported the complete 118,784-byte firmware RAM copy
through 471 bounded Ring3 windows. Overlapping bytes agree; independently
computed CRC32 `E2CB542E` matches six OS reports. Actual Ring3 byte/refusal,
RO/NX mapping and complete CPU resource checks passed twice. The kernel and
serial console remained responsive, and a controlled reboot returned to a
readable prompt without operator intervention. No GPU register transaction ran.

All four connector record streams end with a single `FF` byte immediately before
the next section. The unchanged decoder requires two bytes before examining the
type and refuses these valid terminators. Host replay of the complete captured
copy reproduces all four status-3 refusals. The next change will check the type
byte first, while retaining full-header and extent checks for ordinary records.
Successful routing, active connector discovery and native mode/link programming
require their own evidence. The private ROM copy stays in ignored build evidence.

Physical evidence: `build/rx570-isolated-v32/hardware/validation.json`.


## Single-byte ATOM record terminators

The next candidate fixes the four routing refusals reproduced from the complete
physical firmware RAM capture. A checked `FF` type byte now completes the record
stream without loading a size byte. The connector directory also permits a
record pointer to that last byte of ObjectInfo. Every ordinary record still
needs a complete two-byte header and its full checked extent. The 64-record
limit, section boundaries, duplicate checks and cleared failed-route output stay
in force. The pinned upstream consumer checks `FF` before consulting the size;
the captured board places each terminator immediately before another section.

Host replay of the actual 118,784-byte copy changes all four route results from
status 3 to status 0/flags 63. All 48 packed words match independently prepared
expected values. The source CRC remains `E2CB542E`. The decoded assignments are:

| Connector object | Firmware source object | I2C ID | HPD GPIO ID |
| --- | --- | --- | --- |
| `3113` (DP) | `2121` | `90` | 6 |
| `3213` (DP) | `2221` | `92` | 4 |
| `3313` (DP) | `2120` | `91` | 1 |
| `310C` (HDMI-A) | `2220` | `93` | 5 |

Object and I2C IDs above are hexadecimal. These are firmware assignments; they
do not establish which physical port is active or authorize GPIO/AUX/I2C/HPD
access. Active routing, DP link/mode programming and HDMI qualification remain
separate work.

The new regression failed before the fix and passes afterward. Host tests cover
the next section, exact object end, missing ordinary headers, 64-record limit
and an inaccessible page immediately after `FF`. Actual Ring3 parsing preserves
the peer section and restores all CPU resources. Four final-image QEMU boots
pass: focused, two displays, normal 88/88 and deep 88/88/611. Only the ROM service
changes among the 27 archive files/21 ELFs; the other 26 files/20 ELFs match v32,
including native, rescue, supervisor and GUI. Kernel/service remain relocation
free, with the vendor decoder outside the kernel.

Candidate: `build/rx570-terminator-v33/JA-os.img`, SHA-256 `effe8a9d8c866226ecae5093b3ca05d90e102971ad30096da49743fb9f8a6977`.
Boot ABI 11/1632, private protocol 3, routing report V2 and GPU budgets are
unchanged. Fresh physical route reports from this image remain pending;
host replay is not a physical v33 pass. Evidence is frozen in that build's
`validation.json`; the captured ROM remains private ignored evidence.


## Physical terminator and routing qualification

On the RX 570, v33 now decodes all four routes successfully: status 0, flags
`3F`, one source, no destinations and three records per connector. Four reports,
including one after reboot, match all 48 packed fields of the independent golden
report prepared from the complete captured firmware copy. CRC32 `E2CB542E`,
copy length 118,784 and first-image length 59,904 remain stable.

Actual Ring3 single-byte terminator/peer preservation, RO/NX mapping, byte-window,
malformed-input and complete CPU resource checks passed twice. The existing
native RAM model also passed. COM7 captured 30,649 received bytes and sent 236
bytes across the two active sessions; initial boot and inter-session gaps are
excluded. A controlled reboot returned readable JA OS output without operator
intervention. No GPU register or GPIO/AUX/I2C/HPD transaction ran in this pass.

Evidence: `build/rx570-terminator-v33/hardware/validation.json`. The next step is
bounded firmware display-path and encoder correlation. Successful inventory does
not establish the active physical port or qualify native mode/link programming,
HDMI or hardware reset recovery.


## Bounded firmware path and encoder correlation

The next candidate adds an isolated Ring3 decoder for direct firmware display
paths. It requires a unique version-1 path with one encoder, a supported device
tag, a connector source matching that encoder, and encoder links matching the
path's GPU and connector. It then captures one type-20 encoder-capability record.
Multiple graphics objects, routers, ambiguous paths, unsupported formats and
inconsistent links produce a separate topology refusal. Existing connector and
routing reports remain available; failed topology fields clear to status only.

The captured board data resolves to four possible firmware paths:

| Connector | Device tag | GPU | Encoder | Raw encoder capability bits |
| --- | --- | --- | --- | --- |
| `3113` | `0008` | `1100` | `2121` | `000F` |
| `3213` | `0080` | `1100` | `2221` | `000F` |
| `3313` | `0200` | `1100` | `2120` | `000F` |
| `310C` | `0400` | `1100` | `2220` | `000F` |

All table values are hexadecimal. These are firmware relationships and raw
advertised bits, not proof of the active connector, trained link, usable speed or
hardware capability. No table bytecode is executed and no GPIO/AUX/I2C/HPD or GPU
register access is added. Layouts are checked against the pinned
[ATOM definitions](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/atombios.h#L4510)
and [AMDGPU consumer](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/amdgpu_atombios.c#L333).

The decoder bounds traversal to 16 paths, 32 encoder objects, eight graphics IDs
per examined path and 64 encoder records. The supported direct profile requires
exactly one graphics ID and one GPU source/connector destination per encoder.
Headers, directories, complete path extents and conservative connector partitions
protect encoder data, including padding. A single-byte `FF` can terminate an
encoder stream at its section or object end. Duplicate capability records and
incomplete ordinary records fail without exporting partial fields.

Private ROM protocol 4 adds a separate four-word tail per connector, beginning
at query index 212. The existing V2 directory and 12-word route positions stay
fixed. The maximum report contains 276 words. Boot handoff ABI 11/1632 is
unchanged; deploy the matched kernel and service in the full image. The existing
four-second CPU admission and independent startup/RPC/cleanup bounds remain;
this is not a hardware transaction timeout.

| Topology word | Packing |
| --- | --- |
| 0 | status bits0..15; flags16..23; graphics count24..31; record count32..39; other-record count40..47; upper16 zero |
| 1 | device tag, GPU ID, encoder ID in successive 16-bit fields; upper16 zero |
| 2 | raw capability bits, encoder source GPU, encoder destination connector in successive 16-bit fields; upper16 zero |
| 3 | ObjectInfo-relative path entry, encoder entry, link list and record stream offsets in successive 16-bit fields |

Successful flags `0F` represent the checked path, encoder entry, link agreement
and capability record. A failure exports status alone. Capability bits remain
raw inventory and grant no hardware authority.

Host tests pass 33 malformed/boundary topology cases, 513 protected prefixes,
568 byte mutations and final-byte terminator/header checks. Replay of the actual
captured ROM preserves all 48 V2 route words and matches all 16 new topology
words against independent expected values. Actual Ring3 success, malformed
capability refusal, peer preservation, RO/NX mapping and complete CPU baselines
pass. Four separate final-image QEMU runs pass: focused, two displays, normal
88/88 and deep 88/88/611. Only the ROM service changes among 27 archive files/21
ELFs; the other 26 files/20 ELFs, including native/rescue/supervisor/GUI, match v33.
Kernel/service are relocation-free and vendor decoders remain outside the kernel.

An initial normal QEMU run passed 87/88 tests: the existing native RAM test failed
between completed fault cleanup and its next service reservation. Cleanup and
later ROM tests passed. The transcript does not identify the precise reservation
substep. Source review exposed interruptible readiness observations; these now
use the existing UP interrupt save/restore pattern to inspect and schedule
atomically. Timeout, 64-turn fallback,cleanup and no-replay rules are unchanged.
The initial image/logs remain in `attempt-readiness`;all four checks were rerun
on the corrected final image.

Candidate: `build/rx570-topology-v34/JA-os.img`, SHA-256 `bb75a67ef0881acbe5e3c65e81e5410936cf956bef819b9365288b339ece20c9`.
Fresh physical topology reports remain pending. The target stays on the
qualified v33 firmware console until this image is flashed. Native changed-mode,
DP link programming, HDMI and full hardware reset/isolation qualification remain
open. Evidence is frozen in the candidate's `validation.json`.


## Physical firmware path and encoder qualification

On the RX 570, four v34 reports match all 64 expected fields: the 48 existing
route words and 16 new topology words. One report was captured after reboot.
Each firmware path has one encoder, matching connector and encoder links, and
raw capability bits `000F`. CRC32 `E2CB542E`, copy length 118,784 and first-image
length 59,904 remain stable. These relationships do not identify the active port
or prove trained link speed.

Actual Ring3 path/encoder/capability success, malformed-capability refusal,
cleared tails, connector and peer preservation, RO/NX mapping, byte windows and
complete CPU resource checks passed twice. The native RAM model passed once.
COM7 captured 34,095 active-session bytes plus 1,694 attributed kernel-boot bytes;
143 early bytes remain retained without attribution. The active sessions sent
236 bytes. A controlled reboot returned readable JA OS output without operator
intervention. No GPU register or GPIO/AUX/I2C/HPD transaction ran in this pass.

Evidence: `build/rx570-topology-v34/hardware/validation.json`. The next step is
reviewing a finite read-only register profile for active DIG/CRTC/transmitter
correlation. Native changed modes, DP link programming, HDMI and full hardware
reset/isolation qualification remain open.


## Bounded read-only DIG observation

The explicit kernel-monitor command `polaris-link` captures the inherited
CRTC/frontend/backend relationship on the selected RX 570. It is enabled only
by the existing default-off `POLARIS_NATIVE_WRITE=1` hardware build profile.
The exact board/GOP/BAR5/D0/decode/UP CPU/COM1 checks, fixed 1024x768 BGRX mode,
kernel framebuffer lease and idle writers must pass first. Qualification also
requires independent serial control. The operation assumes an operational DCE
domain, unchanged firmware scanout and no concurrent hotplug, power or reset.
PCI D0 alone does not establish that display register domains are powered.

One admitted attempt is permitted per boot, including a failed preparation.
The maximum is 52 ordered DWORD load attempts: six CRTC controls, two samples
of seven backend enable/control pairs and six frontend controls, then six CRTC
controls again. The first six must show exactly one coherently running CRTC
before any DIG reads. Each attempted transaction is journaled over serial.
There are no GPU stores, polling loops or retries. The two-second software
admission bound cannot time out a stalled bus transaction.

Three optional supervisor-only RO/NX/UC pages at BAR5 offsets `12000`, `13000`
and `15000` hold the 20 selected DIG words. They use the existing checked BAR5
binding with exact owner, identity, current-plan and leaf/frame validation.
Preparation failure closes admission; partial mappings remain in the cleanup
ledger. Cleanup validates and unmaps the exact leaves, releases the hold and
capability, and quarantines the inherited assignment. Ordinary release/reuse is
unqualified. A failed cleanup retains its ledger and prevents reopening.
No new userspace MMIO grant, PCI write, DMA gate or interrupt route is created;
quarantine does not prove that inherited device DMA has been isolated.

Physical access closes before a fresh `registerprobe.elf` decodes copied words
in Ring3. Private copied-data operations 10/11/12 implement one BEGIN, 52 ordered
DATA messages and four report queries. The existing operations 1-9 and device
register ABI are unchanged. Invalid shape/order closes the process; no reset or
replay exists in an instance. The CPU admission is four seconds, at most 57
RPCs, with the existing independent startup/RPC/stop bounds. Failure clears the
report and retains incomplete CPU cleanup.

The decoder requires matching paired samples and CRTC brackets, no all-ones
word, one running CRTC, one enabled backend, a one-hot frontend selection among
six frontends, and agreement with that frontend's CRTC source. It exports raw
mode, HPD-select and frontend clock fields without interpreting a physical port,
trained link, usable speed or hardware capability. A refusal exports status
alone. Four report words pack:

| Word | Fields |
| --- | --- |
| 0 | status bits0..15; version1 bits16..31; CRTC32..39; backend40..47; frontend48..55; raw mode56..63 |
| 1 | enabled-backend mask low32; raw HPD-select32..39; frontend clock flag40 |
| 2 | raw backend control low32; raw frontend control high32 |
| 3 | raw selected CRTC control low32; load count52 high32 |

The reviewed offsets/masks and getter usage are pinned in
[POLARIS_LINK_REVIEW.json](POLARIS_LINK_REVIEW.json). Linux's
[DCE link getters](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_link_encoder.c),
[stream source getter](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_stream_encoder.c),
[DCE11.2 resources](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/resource/dce112/dce112_resource.c)
and [register definitions](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h)
support this narrow operational-device experiment. They do not establish safe
access to an unpowered, resetting or arbitrarily hung GPU.

Host tests cover 20 golden offsets/aperture edges, 52 all-ones positions, 26
changed positions, ambiguous/malformed mux and CRTC cases, backend G, raw modes
and 53 protected prefixes. The seven-source audit checks hashes, 20 compiled
offsets, nine masks, three pages and getter/resource facts. Actual Ring3 golden,
changed/all-ones refusal, cleared report, duplicate-index process closure and
full CPU baseline tests pass. Four final-image QEMU runs pass: focused, two
displays, normal 88/88 and deep 88/88/611. QEMU verifies the physical command's
unqualified-device refusal; it does not exercise positive GPU reads or mapping
cleanup. The disabled controller imports only terminal output. Only the register
probe changes among 27 archive files/21 ELFs; the other 26 files/20 ELFs, including
ROM/native/rescue/supervisor/GUI, are byte-identical to v34. Kernel/probe are
relocation-free; vendor decoding remains outside the kernel. Two initial shell
registration/link-relocation build errors were corrected before any image boot;
the logs remain in the candidate directory.

Candidate: `build/rx570-link-v35/JA-os.img`, SHA-256 `038d8df56f9299278f0c0c2b56e75b032eb17b7d1c28ac3ebbbbc33ce2b23e5a`.
Fresh physical DIG capture and cleanup remain pending. Software evidence is in
the candidate's `validation.json`; v34 physical evidence remains unchanged.
This is one observation, not an atomic snapshot or new mode/link programming.
Native changed modes, DP link programming, HDMI and full GPU reset/isolation
qualification remain open.


## Physical read-only DIG qualification

The v35 command completed all 52 ordered DWORD loads on the Z390/F10 RX 570
DisplayPort profile. The six CRTC brackets and both 20-word DIG samples match.
Only CRTC 0 and backend 2 are enabled; backend 2 selects frontend 2, whose CRTC
source is 0. The raw mode and HPD-select fields are zero and the frontend clock
flag is one. This identifies the observed internal route, without establishing
a physical connector, trained link speed or an atomic snapshot.

| Observed field | Hex value |
| --- | --- |
| CRTC 0 control | `80410311` |
| Backend 2 enable | `00000101` |
| Backend 2 control | `00000400` |
| Frontend 2 control | `01000000` |
| Report word 0 | `0002020000010000` |
| Report word 1 | `0000010000000004` |
| Report word 2 | `0100000000000400` |
| Report word 3 | `0000003480410311` |

An independent host calculation matches all four Ring3 report words. Checked
MMIO close/unmap, hold and capability removal succeeded; the inherited assignment
stayed quarantined. A second invocation refused without additional reads.
Serial input/output remained responsive and the monitor stayed readable. The
controlled reboot returned readable JA OS output without operator intervention.
There were no GPU stores, polls or native programming transactions in this pass.
Quarantine does not demonstrate DMA isolation or safe ordinary assignment reuse.

Actual Ring3 copied-link golden/refusal/cleared-report, duplicate-index process
closure and full CPU baseline tests passed twice. The native RAM model passed
once. Four firmware reports, including one after reboot, match all 64 expected
route/topology fields and CRC32 `E2CB542E`. Five active COM7 sessions captured
39,994 bytes and sent 297. An earlier 218-byte passive shutdown fragment has no
build identity and is retained separately, excluded from v35 attribution.
Inter-session gaps are excluded.

Evidence: `build/rx570-link-v35/hardware/validation.json`. The frozen 283-file
software record and tested image are unchanged. The next bounded work is an
inherited timing/DP stream-parameter review before native changed-mode and
restoration programming. Native changed resolutions, DP link training, HDMI,
full GPU reset/reinitialization and broader DMA/IRQ isolation remain open.


## Bounded inherited timing and DP stream observation

The explicit kernel-monitor command `polaris-timing` adds a read-only observation
of the inherited raster and selected DP stream. It uses the existing default-off
`POLARIS_NATIVE_WRITE=1` build profile and exact board/GOP/BAR5/D0/CPU/COM1/kernel
lease checks. The GOP surface remains 1024x768 BGRX with stride 1024. Scanout,
clocking and link programming are unchanged.

The first 26 loads must confirm the physically observed CRTC 0 -> frontend 2 ->
backend 2 route, raw DIG mode zero and frontend clock enabled. The DP stream
control is then read first; its enable bit must be set before the other timing
fields. This is an operational-domain check under unchanged firmware scanout,
with no hotplug, power transition or reset. PCI D0 alone is insufficient.

The maximum is 80 ordered DWORD load attempts: six CRTC controls, 20 DIG words,
14 timing/stream words, repeated DIG and timing samples, then six CRTC controls
again. The 14 fields are DP stream control; CRTC horizontal total, blank, sync
and polarity; vertical total, blank, sync and polarity; interlace; DP pixel
format, video timing, N and M. They lie in existing RO/NX/UC pages `6000` and
`13000`; no mappings are added. Per-load identity/hold/leaf/frame checks and
checked cleanup are retained. `polaris-link` and `polaris-timing` share one
admitted attempt per boot, including preparation failure. There are no GPU
stores, polls or retries. The two-second admission cannot time out a stalled
bus transaction.

Access closes and unmaps, its hold/capability are removed and the inherited
assignment is quarantined before fresh Ring3 decoding. Cleanup failure retains
the ledger and prevents reopening. Quarantine does not prove DMA isolation or
safe ordinary assignment reuse. No new user MMIO grant or syscall is added.

Private copied-data operations 13/14/15 provide one BEGIN, 80 strictly ordered
DATA messages and eight queries: at most 89 RPCs, with four-second CPU admission
and the existing independent startup/RPC/stop bounds. The earlier link role and
operations 1-12 remain. Early queries, cross-role requests, duplicate indices and
oversized DWORDs close the process. No role reset/replay exists. Failed decode
or transport clears output while retaining incomplete CPU cleanup.

Ring3 requires equal paired words and CRTC brackets, no all-ones value, the
qualified internal route, enabled DP stream, progressive scanout and consistent
non-wrapping blank/sync geometry. Totals are encoded as N-1; blank-start minus
blank-end gives the active raster extent including borders. This observed
raster can differ from the separately qualified GOP surface size. Raw DP fields
do not establish pixel clock, refresh, lane rate, link training or active
physical port. Automatic M/N measurement may change sampled values and cause a
conservative refusal.

| Report word | Fields |
| --- | --- |
| 0 | status/version/CRTC/backend/frontend/raw mode, as in the link report |
| 1 | horizontal total, active extent, blank start, blank end in successive 16-bit fields |
| 2 | vertical total, active extent, blank start, blank end in successive 16-bit fields |
| 3 | H sync start/end then V sync start/end in successive 16-bit fields |
| 4 | raw H polarity bit0 and V polarity bit8 |
| 5 | raw DP pixel format low32; raw DP video timing high32 |
| 6 | raw DP N low32; raw DP M high32 |
| 7 | raw DP stream control low32; load count80 high32 |

A refusal exports status alone. Successful fields describe observations and do
not authorize programming. The reviewed facts and read assumptions are in
[POLARIS_TIMING_REVIEW.json](POLARIS_TIMING_REVIEW.json). Pinned Linux
[timing-generator code](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce110/dce110_timing_generator.c)
and [stream-encoder code](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_stream_encoder.c)
support the total/blank interpretation and control-register read/RMW review.
This is a bounded operational-device inference, not a guarantee for powered-off,
resetting or arbitrarily hung hardware.

Host tests pass 14 golden offsets/aperture edges, 80 all-ones positions, 40
changed positions, 12 geometry/stream/interlace cases, six route refusals,
independent raster sizes, 26 admission all-ones cases and 81+27 guarded prefixes.
The eight-source audit matches 14 compiled offsets and 18 field facts. Actual
Ring3 tests pass the 1024x768 golden report, an independent 1920x1080 raster
fixture, refusal/cleared output, four malformed-protocol cases and full CPU
resource restoration. The 1920x1080 fixture is CPU data, not a physical mode
change. Four final-image QEMU boots pass: focused, two displays, normal 88/88 and
deep 88/88/611. Unqualified QEMU hardware refuses both physical commands before
assignment or MMIO. Positive timing access remains untested.

An earlier four-pass QEMU set retained a needless GOP/raster equality restriction.
Source review removed it before hardware use; all four checks were rerun on the
corrected image. Those earlier images/logs remain in `attempt-gop-equality`.
Only `registerprobe.elf` changes among 27 archive files/21 ELFs; 26 files/20 ELFs,
including native/rescue/ROM/supervisor/GUI, match v35. Kernel/probe are relocation
free and vendor interpretation stays outside the kernel. Boot ABI 11/1632 is
unchanged; the disabled controller imports only terminal output.

Candidate: `build/rx570-timing-v36/JA-os.img`, SHA-256 `3ed3b0d8c0ed8d014cd51a94b3e2def1558f6ab0820b4693051ba021341f20c5`.
Fresh physical timing/stream capture is pending. The v35 52-load capture remains
separate qualified evidence. Native changed modes, DP link training, HDMI and
full GPU reset/reinitialization/isolation remain open.


## Physical timing capture and automatic M refusal

V36 completed all 80 ordered read attempts on the qualified RX 570 DisplayPort
profile. CRTC brackets, DIG samples and 13 timing/stream fields matched. Only
DP video M changed: `0000758D` to `0000758B`, while video timing was `00000100`
(automatic M/N enabled). The strict Ring3 decoder returned CHANGED/status3 and
cleared all seven report-tail words. The capture session remains FAILED against
its required successful semantic report. Host replay of the actual values
reproduces that refusal; no GPU access was retried.

The stable raw CRTC fields independently calculate a 1920x1080 active raster
with totals 2200x1125, while the GOP surface is 1024x768. This is an observation
of inherited timing, not a physical mode change or pixel-clock/refresh proof.
Automatic measurement is consistent with the M difference; it does not establish
that every future M change is harmless. The pinned stream encoder explicitly
enables automatic measurement after programming initial M/N values.

Checked MMIO close/unmap and hold/capability removal succeeded, with the inherited
assignment quarantined. Serial control, readable output and power readiness
survived; controlled reboot returned readable JA OS output without intervention.
There were no GPU stores, polls, retries or native programming transactions.
The new actual Ring3/CPU tests and native RAM model passed before capture. Two
firmware reports match all 64 expected fields and CRC32 `E2CB542E`.

COM7 captured 28,433 active bytes and sent 232; passive boot adds 1,698 attributed
bytes plus one retained, unattributed prefix byte. Inter-session gaps are excluded.
Evidence: `build/rx570-timing-v36/hardware/validation.json`. The tested image and
326-file software record remain unchanged.

The next correction separates the documented automatic 24-bit M measurement
from static controls and preserves both raw samples in a versioned report.
Control/N/route changes, manual-M changes and changes outside documented M bits
must still refuse. Native changed modes, DP training, HDMI, full GPU reset and
broader isolation remain open.


## Automatic M sample reporting

V37 corrects the copied timing decoder for the changing automatic M measurement
observed on v36. The GPU read code, pages, field order, shared one-attempt budget,
80-load maximum and two-second software admission are unchanged. No GPU command
was issued during this correction.

Every CRTC, DIG, N, video-timing and other control sample must still match.
Differing M bits are accepted as an observation only when the stable
`DP_VID_M_N_GEN_EN` bit is enabled and the difference is confined to the
documented 24-bit `DP_VID_M` field. Manual-M differences, changed reserved M bits,
N changes and automatic-mode/control changes still refuse with cleared output.
Both full raw M samples are retained. Neither sample is chosen or averaged into
a claimed pixel clock, rate or programming value.

The timing report is now version 2 with nine words. Words 1-7 retain their field
positions; word 0 carries version 2, and new word 8 contains first M in low32
and second M in high32. Word 6 still contains N and first M. A stable pair is
exported twice as equal values. Private operations 13-15 require timing version
2 at BEGIN/QUERY and eight-word version 1 BEGIN is rejected in a fresh process.
The maximum copied transport is 90 RPCs; four-second CPU admission and separate
startup/RPC/stop bounds remain. Deploy the matched kernel/probe through the full
image. Boot ABI 11/1632 and the earlier link role/version remain unchanged.

Host replay of the actual v36 register values matches nine independently packed
expected words, including final pair `0000758B0000758D`. Actual Ring3 replay
matches them too. Host and Ring3 negative tests refuse manual M, reserved M, N
and GEN_EN changes; all-ones, static-change, geometry, route and protocol checks
remain. Eight pinned source hashes, 14 offsets and 20 compiled field facts pass
the extended audit. The automatic-measurement and 24-bit-field facts come from
the existing pinned
[stream encoder](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce/dce_stream_encoder.c)
and [DCE11.2 masks](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_sh_mask.h).
The review remains in [POLARIS_TIMING_REVIEW.json](POLARIS_TIMING_REVIEW.json).

Four final-image QEMU boots pass: focused, two displays, normal 88/88 and deep
88/88/611. They verify actual Ring3 interpretation/process cleanup and physical
command refusal on unqualified QEMU hardware. Positive v37 hardware capture is
pending. Only the diagnostic probe changes among 27 archive files/21 ELFs;
26 files/20 ELFs, including native/rescue/ROM/supervisor/GUI, match v36. Kernel
and probe are relocation-free, vendor interpretation stays in Ring3 and the
disabled controller imports only terminal output.

An initial host-harness generation mistake expanded an 80-case loop to 90;
the existing check caught it. The loop was corrected before the first image
build and the failed host log is retained. No failed v37 QEMU run was observed.
The original v36 failed semantic acceptance, its 80 completed reads and its
cleanup/serial/reboot evidence remain sealed separately.

Candidate: `build/rx570-auto-m-v37/JA-os.img`, SHA-256 `dcbb0a9898a2340bdaa8c2354df43aa1b50c80791eb56d9a8ab467c18e188b18`.
The next physical check must verify all 80 raw values, stable controls, the
conditional M exception and all nine report words, followed by cleanup and
continued diagnostics. This remains inherited-state observation. Native changed
modes, DP link training, HDMI, full GPU reset/reinitialization and broader
isolation remain open.


## Physical paired-M timing observation

The v37 timing capture passed on the qualified RX 570 DisplayPort setup. All
80 ordered loads completed. CRTC brackets, DIG samples and the 13 static timing
and stream fields matched. With automatic M/N enabled (`DP_VID_TIMING=00000100`),
M changed from `0000758D` to `0000758F`, entirely within its documented 24-bit
field. The version 2 Ring3 report retained both samples, and all nine words
matched an independent calculation from the captured values. Word 8 was
`0000758F0000758D`.

The inherited raster was 1920x1080 with totals 2200x1125; the GOP surface remained
1024x768. These are separate observations. The report does not establish a new
mode, pixel clock, refresh rate, trained link rate or physical connector port.
The original v36 strict-decoder failure remains recorded separately.

Checked MMIO close/unmap and hold/capability removal completed before Ring3
interpretation. The inherited assignment remained quarantined. Both repeated
timing and link commands refused before any additional GPU read. Serial control,
power readiness and readable output survived. Controlled reboot returned readable
JA OS output without boot-menu, reset or power intervention. This pass performed
80 GPU loads, zero stores, zero polls and zero retries; it did not exercise native
GUI output or rescue/reset programming.

Before capture, actual Ring3 timing/link/ROM tests, rejection cases and full CPU
resource baselines passed, along with the native RAM model. Two firmware reports
matched all 64 expected fields and CRC32 `E2CB542E`. The earlier four final-image
QEMU passes remain unchanged: focused, two displays, normal 88/88 and deep
88/88/611.

COM7 received 29,092 bytes and sent 245 across five active sessions. Passive logs
received no bytes; the initial boot and inter-session gaps are excluded. The
controlled-reboot boot output is included in its active session. Evidence is in
`build/rx570-auto-m-v37/hardware/validation.json`; the 302-file software record and
tested image are unchanged.

The next implementation work is a bounded native changed-mode transaction with
an independently retained restoration recipe. Its timing, clock and link writes
need review before new authority is granted. DP training, HDMI, full GPU reset,
arbitrary hardware hang recovery and broader device isolation remain open.


## Vertical-total preparation and restoration plan

V38 adds the missing preflight for the first bounded native timing adjustment.
The proposed adjustment retains the current resolution, pixel clock and trained
DisplayPort link, and changes the vertical total from 1125 to 1133 through the
CRTC vertical-total override. This candidate prepares data only. It does not
execute the adjustment or restore hardware, and it does not add resolution
switching, clock programming or link training.

The explicit `polaris-vertical` command shares the existing link/timing
once-per-boot attempt. It captures the existing 80 timing values, checks stable
CRTC/DIG/stream admission, then reads two ordered samples of CRTC0
`V_TOTAL_MIN`, `V_TOTAL_MAX` and `V_TOTAL_CONTROL`: six additional DWORD loads,
86 maximum. They use the existing supervisor RO/NX/UC page at BAR5+`6000`;
no additional page or writable promotion is needed. The same two-second
software admission, operational-DCE/no-hotplug-power-reset assumptions and
uncancellable bus-stall limitation apply. The older 52/80-load commands retain
their own budgets but consume the same shared attempt. Checked close/unmap and
hold/capability removal plus inherited assignment quarantine precede Ring3
interpretation. Partial failure retains the exact cleanup ledger, with no retry.

A fresh Ring3 probe reuses the accepted timing V2 decoder and requires the exact
previously observed raster/route/DP controls. Both original min/max values must
be either zero or 1124, and the entire original override control must be zero.
Changed samples, all-ones values, MC-training bits, reserved/control bits and
other baselines refuse with cleared report data. The allowed control baseline
is deliberately narrow; its final six fixture values are synthetic until the
new physical capture establishes them.

The copied report has version 1 and 18 words. It retains the complete timing V2
report, original min/max/control, a target total-minus-one, three proposed
primary steps and three proposed restoration steps. Primary data proposes
MIN=1132, MAX=1132, CONTROL=`11`. Restoration data disables the override first,
then restores the admitted original min/max. Steps contain semantic field IDs
and values, never raw register addresses. They have no execution authority.
Future code must retain the restoration configuration independently before any
write, enforce every step in a trusted gate and use a fresh rescue process.
Restoring these fields would not revive expired GOP services or automatically
validate firmware fallback.

Private copied operations 16-18 require a fresh role, 86 ordered inputs and
exactly ordered report queries 0-17. Malformed order, wrong versions, wide
values, cross-role requests and duplicate BEGIN/DATA/QUERY close the process.
There are at most 105 ordinary RPCs with the existing four-second CPU admission
and separate startup/RPC/stop bounds. Failure clears output and retains any
incomplete CPU cleanup ledger. Existing link V1, timing V2, boot ABI 11/1632,
syscall and native/rebind/rescue write authority remain unchanged.

The [pinned review](POLARIS_VERTICAL_REVIEW.json) audits eight source hashes,
three byte offsets and four field-mask facts. The pinned
[timing generator](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/dce110/dce110_timing_generator.c)
reads and writes these three fields in its dynamic-refresh path and uses totals
minus one. The target, strict original-state admission and disable-first
restoration are JA OS design choices; the source does not establish successful
JA OS hardware restoration or safe access to an arbitrary hung/powered-off GPU.

Host tests check 18 independently packed golden words, all 86 all-ones positions,
unstable control samples, 96 unsafe paired control values, all four allowed
original min/max pairs and 16 interrupted-primary prefixes, profile refusals,
page boundaries and protected short inputs. Actual Ring3 tests validate the same
report with a synthetic control baseline, semantic refusals, malformed IPC,
forced process closure and full pages/processes/spaces/endpoints/threads/exit
queues/capabilities baselines. The older timing and link host tests pass.

Four final-image QEMU boots pass: focused, two displays, normal 88/88 and deep
88/88/611. QEMU refuses physical vertical preparation before assignment or MMIO.
Kernel and probe remain relocation-free; vendor planning stays in Ring3.
Only the register probe changes among the 27 archive files/21 ELFs; the other
26 files/20 ELFs, including primary/rescue/supervisor/ROM/GUI, match v37. The
loader object code and JA OS banner remain unchanged. An initial host runner
omitted the include search path; the compile failed, the runner was corrected
before the first image build and its original log is retained.

Candidate: `build/rx570-vertical-v38/JA-os.img`, SHA-256 `e39df8d4e57534f62c48b51e1825a0fd6622622848d13061b189dc9ae2674f35`.
The physical gate is the six new values, stable first-80 context, exact copied
plan or explicit refusal, cleanup, repeat refusal and continued serial/display/
reboot diagnostics. The tested v37 observation remains sealed separately.
Native timing writes and independently executed restoration are subsequent work;
HDMI, full reset/reinitialization and broader isolation remain open.


## Physical vertical-total preparation

The v38 read-only preparation passed on the qualified RX 570 DisplayPort setup.
All 86 ordered MMIO loads completed. The existing timing context remained
stable apart from the permitted automatic M samples (`0000758F`, `0000758C`).
Both new MIN/MAX/CONTROL samples were `0/0/0`: the override was inactive, with
an independently observed zero restoration baseline. All 18 Ring3 report words
matched an independent calculation, including the original controls and all
six proposed primary/restoration steps.

The proposed primary data is MIN=1132, MAX=1132, CONTROL=`11`; restoration data
is CONTROL=0, MIN=0, MAX=0. This pass did not execute those steps. It establishes
the input baseline and copied plan for the next write implementation; it does
not establish a changed effective vertical total, display rate or successful
hardware restoration. The original CPU fixture controls remain marked synthetic.

Checked close/unmap and hold/capability removal completed before Ring3 planning,
and the inherited assignment stayed quarantined. Repeated vertical, timing and
link commands all refused before new reads. Independent serial control, power
readiness and readable monitor output survived. Controlled reboot returned
readable JA OS output without boot-menu, reset or power intervention. GPU totals
were 86 MMIO loads, zero MMIO stores, zero polls and zero retries. No native GUI
or rescue/reset fault injection ran in this pass.

Actual Ring3 vertical/timing/link/ROM tests, malformed-request refusals and full
CPU baselines passed before capture, as did the native RAM model. Two firmware
reports matched all 64 expected fields and CRC32 `E2CB542E`. The four final-image
QEMU passes remain unchanged: focused, two displays, normal 88/88 and deep
88/88/611.

COM7 received 30,665 active bytes and sent 264 across five sessions. Passive boot
capture contains 1,694 attributed kernel bytes plus 769 pre-kernel bytes retained
as uninterpreted, unattributed data. Scoped received bytes total 32,359; all raw
received bytes total 33,128. Inter-session gaps are excluded. Evidence is in
`build/rx570-vertical-v38/hardware/validation.json`; the tested image and 311-file
software record are unchanged.

Next is the finite timing write gate and fresh Ring3 primary/rescue execution,
with trusted restoration retention before mutation, bounded completion and
interrupted-prefix tests. Native resolution switching, DP training, HDMI,
full reset/reinitialization and broader device isolation remain open.


## Fixed vertical-total writes and fresh rescue restoration

V39 implements the explicit kernel-monitor command `polaris-vtotal`. It uses
fresh Ring3 primary and rescue processes under the existing serialized graphics
worker and Ring3 recovery supervisor. Normal `gui native` retains the previously
qualified address-only rebind. The new operation requires the exact selected
1002:67DF rev EF / 1458:2310 board and inherited CRTC0/FE2/BE2 DP profile.
Physical write/restoration qualification is pending for this image.

Before any store, a fresh 86-value RO snapshot must match the accepted timing
context and original MIN/MAX/CONTROL=0/0/0. Both the snapshot and checked copied
18-word Ring3 proposal remain in kernel-owned storage. The binding retains its
four pipe pages, location page and three DIG pages: eight supervisor-only
NX/UC leaves. GOP access is permanently retired before the existing BAR5+6000
leaf becomes writable. Neither driver receives a BAR or CPU MMIO mapping.
The two existing fixed plane-address stores complete before vertical writes.

The primary sequencer requests MIN=1132, MAX=1132, CONTROL=0x11, targeting total
1133 instead of the retained base 1125. A separate kernel gate validates each
request, and the MMIO binding independently checks field, value, exclusive
lease, identity, assignment state and exact RW leaf. Both consume the attempt
before a store, including an ambiguous failure. No generic register operation,
arbitrary payload, retry or second vertical round is available.

After primary failure, its output is revoked and its process is stopped/reaped.
The assignment remains quarantined. The reserved fresh Ring3 rescue sequencer
first requests CONTROL=0, MIN=0, MAX=0 using the independently retained zero
baseline. Plane rebind writes are blocked until those three restoration stores
finish. Rescue then performs the existing plane observation/rebind and draws a
new diagnostic display. Failure retains ledgers and serial control; it does not
reuse expired GOP services, trust the failed driver's state or reset the GPU.

Each vertical round reads six before-values, performs three stores, reads back
CONTROL, takes a frame baseline and polls at most 64 six-value samples with at
most 63 waits. Exact control readback and 2..512 modulo-24-bit frame progress
are required. Per-round limits are 392 read attempts, three store attempts and
458 broker requests under two-second software admission. This verifies register
readback and scanout progress; it does not measure the effective vertical total,
refresh rate or monitor output. An MMIO bus stall remains uncancellable.

Including preflight and the existing plane operations, primary limits are
916 read attempts/five stores/128 polls; rescue limits are 824/five/128. Minimum
successful traces use 160/five and 68/five respectively. Each GPU phase has its
own two-second admission, copied planning has four seconds, and service startup,
RPC and stop have separate bounds. There is no combined wall-clock guarantee.
Existing GUI drain, heartbeat, headless and CPU-only power cleanup apply without
extra GPU operations. The native private protocol advances to version 4 with
operation 12; old version-3 requests close the process. Flash the matched full
image. Boot ABI 11/1632 and the general register syscall remain unchanged.

`polaris-vtotal fault1`, `fault2` and `fault3` deliberately execute UD2 in the
primary after acknowledgement of each respective vertical store, then select
the fresh rescue path. Each is a separate fresh-boot experiment. The same
once-per-boot native ownership and reserved-rescue limits apply; other native
commands cannot create a competing binding or rearm the device.

The [pinned write review](POLARIS_VTOTAL_REVIEW.json) checks eight source hashes,
three register offsets and five mask/frame facts. The upstream dynamic-refresh
routine supports the named registers and minus-one encoding. The target,
zero-baseline admission, disable-first restoration and finite execution rules
are JA OS design decisions requiring physical qualification.

Host tests cover 4,352 malformed requests before IO, 2,704 snapshot mutations,
all four interrupted primary prefixes, exhausted polling, failed stores,
readback/frame failures and 22 broker-transport refusals. All 165 prior native
controller cases plus 24 new cases pass. Production MMIO tests check all four
prefixes, nine authority/context/leaf refusals, six read routes, exact payloads,
restoration-before-plane ordering and publication of the ledger before stores.
Actual Ring3 RAM tests run primary success, UD2 after each store, fresh rescue,
three partial rescue failures, RO framebuffer retention, old-version refusal,
correct title pixels and full CPU-resource baselines. RAM tests are not GPU
write or visible-recovery evidence.

Four final-image QEMU boots pass: focused, two displays, normal 88/88 and deep
88/88/611. Every run includes the new actual Ring3 RAM tests. QEMU refuses all
four physical write commands before assignment/MMIO. Only primary and rescue
change among the archive's 27 files/21 ELFs; the other 25 files/19 ELFs match
v38. Loader object code and the JA OS banner remain unchanged; kernel and user
payloads are relocation-free. Default-off native/worker objects import only
terminal output. Initial runtime-test indentation and constant-callback
relocation checks failed; both were fixed before the first image was produced,
and the failure logs are retained.

Candidate: `build/rx570-vtotal-v39/JA-os.img`, SHA-256 `9ef8015f68416efb1b9c50cf9105e8d32eec4684c9479e75f9bcb5a81c8f2a28`.
The physical sequence is fresh preflight, one primary transaction, visible
primary confirmation, automatic primary-failure rescue, independent restoration
trace/visible rescue confirmation, serial diagnostics, cleanup and controlled
reboot. Each fault-prefix case needs another fresh boot. Do not run the older
read-only capture commands first: their quarantined assignment cannot be reused.
HDMI, DP training, native resolution switching, full reset/reinitialization,
arbitrary hardware hangs, DMA/IRQ/IOMMU isolation and acceleration remain open.


## Physical fixed vertical writes and fresh rescue qualification

Physical v39 passed the fixed write/restoration profile on the Z390 Gaming X,
BIOS F10, i7-9700K, RX 570 1002:67DF rev EF / 1458:2310 and Philips Momentum
272M8 over DisplayPort. The tested image remains `9ef8015f68416efb1b9c50cf9105e8d32eec4684c9479e75f9bcb5a81c8f2a28`. This is acceptance
of the observed register transaction and diagnostic recovery in four named
runs; effective vertical total/refresh rate and full modesetting are unmeasured.

The first run took the fresh 86-value preflight and retained the zero baseline
before mutation, performed the two plane stores and three vertical stores,
then read back MIN1132/MAX1132/CONTROL17. The primary vertical phase completed
14 reads, three stores and one poll with four frames of progress. The operator
confirmed the green primary title and `VERTICAL OVERRIDE READBACK VERIFIED -
TARGET 1133` line were correctly oriented and readable.

An acknowledged primary spin made the independent Ring3 console advance its
write counter from 1338 to 1339. The autonomous worker observed the heartbeat
failure and the Ring3 policy selected rescue (action 2). After the primary was
stopped/reaped and the assignment quarantined, the pre-reserved fresh process
restored CONTROL0/MIN0/MAX0 before its two plane stores. Its vertical round
completed 14 reads, three stores and one poll with six frames of progress.
Primary/rescue PIDs79/80 and incarnations77/78 were distinct within that boot.
The blue rescue title and zero-restoration line were visibly confirmed without
restarting the kernel.

Three more fresh firmware boots tested UD2 immediately after each successive
primary vertical-store acknowledgement. The actual user exception was vector 6
and the process was terminated with resources retained for reaping. The broker
reported exactly one, two or three stores, six reads, no polls and no completion;
no primary operation was replayed. Fresh rescue observed each expected partial
baseline, restored zero controls before plane rebind, verified readback/progress
and displayed both readable rescue lines without a kernel restart.

| Run | Primary controls at rescue entry (MIN/MAX/CONTROL) | Rescue vertical reads/stores/polls | Rescue frame progress | Visible rescue |
| --- | --- | --- | --- | --- |
| Primary hang after complete override | 1132 / 1132 / 17 | 14 / 3 / 1 | 6 | Confirmed |
| Crash after MIN | 1132 / 0 / 0 | 14 / 3 / 1 | 4 | Confirmed |
| Crash after MAX | 1132 / 1132 / 0 | 14 / 3 / 1 | 3 | Confirmed |
| Crash after CONTROL | 1132 / 1132 / 17 | 14 / 3 / 1 | 3 | Confirmed |

The normal primary/rescue transactions used 160/68 reads and five stores each.
Each interrupted-primary run used 152 primary reads, 68 rescue reads and eight,
nine or ten combined stores. Across all four boots: 888 completed MMIO reads,
37 stores, 13 polls and no replay or explicit poll wait. All five completed
vertical phases used one poll. Per-phase finite budgets remain unchanged.
These are one sample per named case, not a reliability or performance study.

Every run refused competing native ownership, a second rescue and post-cleanup
native reuse before new GPU IO. `polaris-finish` reclaimed the validated native
console, kept original GOP retired and retained GPU quarantine. Status, serial,
cleanup and power readiness added no GPU IO. All four controlled reboots
returned JA OS without boot-menu/reset/power intervention; the final prompt was
visibly readable, with stage0/heartbeat0/mask0/verticalNO/baselineNO/restartNO and
power ready. COM7 is closed and available.

Initial actual Ring3 RAM and display-probe tests passed once, including the
new prefix-fault, partial-rescue-failure, protocol and complete CPU baselines.
The three later fresh-boot preflights did not repeat those runtime tests. Eight
firmware reports matched all 64 golden fields per report and CRC32 E2CB542E.
There were 18 active serial sessions and 96 host commands: RX139545/TX1222.
Passive capture retained 1825 bytes, of which 1694 are attributed to kernel boot
and 131 remain an uninterpreted/unattributed initial prefix. Scoped RX141239;
all raw RX141370. Inter-session gaps are excluded.

The 536-file software record and tested image remain unchanged. All 200 source
fingerprints matched before documentation updates. Only five public Markdown
records and the write-review JSON change for physical acceptance. The original
requirements and previous sealed evidence remain intact. A read-only host
checker adaptation accidentally changed a ROM split count; its assertion stopped
before the first prefix test. The checker was corrected, with no GPU retry or
product change. Other capture/manifest tooling corrections remain journaled.

No physical rescue partial-store failure, hardware hang, GPU reset or GUI
presentation ran in this pass. Partial rescue failures remain RAM evidence.
The profile still assumes operational DCE, no hotplug/power/reset and the
inherited surface/clock/DP link; a stalled MMIO bus transaction is uncancellable.
Quarantine establishes neither DMA/IRQ/IOMMU isolation nor ordinary assignment
reuse. Resolution switching, DP training, HDMI, full GPU reinitialization,
acceleration and the Intel backend require separate implementation/qualification.


## Interrupted rescue restoration and serial-only containment

V40 adds explicit `polaris-vtotal rescue-fault1`, `rescue-fault2` and
`rescue-fault3` diagnostics for a rescue process that fails during restoration.
Each starts the existing fixed primary transaction and arms one rescue fault
prefix before mutation. A later primary fault/hang selects the same pre-reserved
fresh rescue. It executes UD2 immediately after the selected restoration store
acknowledgement. The primary sequence and ordinary rescue entry retain their
existing behavior. No new register, payload, BAR mapping, DMA, IRQ or reset
authority is added.

The private native protocol advances to version 5. Operation 12 still requires
prefix zero for ordinary rescue. Diagnostic operation 13 requires rescue role,
prefix 1..3, reserved zero, no prior configuration or program, and the same
unused vertical attempt. Both entries consume the shared attempt before IPC.
The kernel broker independently stops further reads, stores, waits or completion
messages after the selected prefix, even if the process ignores the deliberate
fault or forges a reply. At most six initial reads, N stores, N+6 ACKs and N+7
incoming messages are allowed, with no polls/waits and two-second software
admission. A stalled bus transaction remains uncancellable.

A failed diagnostic rescue cannot publish success, rebind its plane or draw.
The existing failure path stops/reaps both owners, revokes output, closes and
unmaps MMIO, drops CPU capability references when cleanup is complete and retains
the quarantined GPU parent. Original GOP eligibility stays retired. Partial
cleanup retains its precise ledger for bounded CPU cleanup; it never retries
the GPU transaction. No kernel restart is needed to retain independent serial
control. Controlled reboot is available only when the existing CPU/storage
readiness checks succeed.

The screen may retain the old primary image after failure. That is stale
scanout, not a new rescue display or proof of current display validity. Even
failure after all three stores is incomplete restoration: no readback/frame
verification or publication followed. The supported outcome here is headless
containment, Ring3/serial progress and controlled reboot, rather than a claim
of restored graphics.

Host coverage includes all 189 prior controller cases plus 13 new manual,
watchdog, GUI-drain, retained-cleanup and invalid-entry cases. Production
transport tests cover all three prefixes against missing replies, continued
valid IO, forged completion and wait requests; 18 malformed first requests and
nine admission guards also refuse. The worker tests cover all prefix dispatches,
null/zero/invalid/active-owner refusal, reservation and ambiguous transport.
The extended pinned audit verifies protocol 5/op13 and the broker's prefix cap
alongside the unchanged eight reference hashes/three offsets/five field facts.

Actual Ring3 RAM tests capture vector-6 process exceptions after each rescue
store ACK, require six reads/N writes/no completion, inspect all 768 RO
framebuffer pages and preserve the primary pixels. Wrong role, zero/oversized
prefix and reserved-word requests leave the process responsive. Normal/recovery
attempts cannot replay the failed diagnostic. The old protocol-4 request closes
the process and the full CPU-resource baseline is restored. Initial controller
coverage exposed a test-double mismatch between a raw render flag and effective
output validity; the double was corrected to match the existing terminal guard.
No product change was needed for that mismatch. An edit-helper signature check
also stopped before its dependent edits; the continuation used the actual header.

All four final-image QEMU boots pass, including actual Ring3 diagnostics:
focused, two displays, normal 88/88 and deep 88/88/611. Unqualified QEMU commands
refuse before assignment or MMIO. Only the two primary/rescue ELFs change among
27 archive files/21 ELFs; the other 25 files/19 ELFs match v39. Kernel/service
payloads remain relocation-free, loader object code is unchanged, and disabled
native/worker objects import terminal output only. No hardware operation ran
during preparation. The accepted v39 physical evidence stays sealed separately.

Candidate: `build/rx570-rescue-fault-v40/JA-os.img`, SHA-256 `01273146025237172210a3f0b9b75bb932b2ac27eed5d091272a2e411b83fa1d`.
The physical gate requires three separate fresh boots: valid primary output,
primary failure, exact partial rescue trace and user exception, no further GPU
IO or false publication, independent Ring3 shell/serial control, accurate
quarantine/CPU cleanup, power readiness and controlled firmware reboot. This
new diagnostic/headless qualification is pending. Broader native modesetting,
effective rate measurement, HDMI and GPU reset/isolation remain separate work.


## Physical rescue-driver failure and serial containment qualification

Physical v40 passed all three deliberately interrupted rescue-restoration
cases on the same Z390 Gaming X/F10/i7-9700K, RX 570 1002:67DF rev EF /
1458:2310, Philips Momentum 272M8 DisplayPort profile. Tested image:
`build/rx570-rescue-fault-v40/JA-os.img`, SHA-256 `01273146025237172210a3f0b9b75bb932b2ac27eed5d091272a2e411b83fa1d`.
The software record remains sealed separately from this physical acceptance:
`build/rx570-rescue-fault-v40/hardware/validation.json`.

Each fresh boot armed one rescue prefix, then completed the normal primary
plane rebind and MIN1132/MAX1132/CONTROL17 transaction. All 86 preflight values
matched the saved baseline except the two permitted automatic M measurements.
Primary readback and frame progress passed, and the operator confirmed both
correctly oriented green primary lines, including the target-1133 message.
Primary vertical frame deltas were 3, 4 and 3, with one vertical poll each.
This does not measure effective vertical total or refresh rate.

An acknowledged primary spin let the independent Ring3 console advance its
write counter in every case (1338 to 1339; 1334 to 1335; 1334 to 1335). The
autonomous worker and Ring3 policy action 2 selected the reserved fresh rescue
after stopping/reaping the old primary and quarantining the assignment.
Primary/rescue PID/incarnation pairs were 85/83 versus 86/84 in the first boot,
and 6/4 versus 7/5 in each later boot. Identities are compared within each boot.

| Rescue fault after store ACK | Restoration store prefix | Rescue reads / stores / polls | Result |
| --- | --- | --- | --- |
| 1 | CONTROL0 | 6 / 1 / 0 | User exception vector 6; headless containment |
| 2 | CONTROL0, MIN0 | 6 / 2 / 0 | User exception vector 6; headless containment |
| 3 | CONTROL0, MIN0, MAX0 | 6 / 3 / 0 | User exception vector 6; headless containment |

Every rescue read the same initial MIN1132/MAX1132/CONTROL17 and inherited
CRTC/base-total controls, then executed UD2 after its selected store ACK.
There was no subsequent GPU read, plane rebind, draw, completion or restored
output publication. Even all three stores are incomplete restoration without
readback/frame validation. Any old primary pixels are stale output. No visible
rescue success is claimed for these failure runs; the separate v39 successful
restoration evidence remains unchanged.

Each failure stopped/reaped the CPU owners, closed/unmapped MMIO and drained
CPU resources while retaining GPU quarantine. Status reported stage 3 with no
pending terminal CPU drain. `displayprobe` reported no qualified scanout,
retired boot output and zero kernel writers; saved GOP metadata remained
available but framebuffer access was refused. Extra rescue, finish without
validated output and a new native attempt all refused before GPU IO.

COM7 remained usable: each case entered the normal Ring3 shell, executed
`help` and returned to the kernel monitor. Serial reported zero dropped bytes,
zero timeouts and no stall. CPU/storage readiness permitted controlled reboot;
all three returned JA OS boot output and operator-confirmed readable firmware
prompts without boot-menu, reset or power intervention. The final target is
fresh v40 firmware output, stage 0, power ready, with COM7 closed and available.

Each boot used 160 primary reads/five stores/two polls plus six rescue reads
and its selected one, two or three stores. Total: 498 completed MMIO reads,
21 store attempts and six polls; no replay or explicit poll wait. Post-failure
status, shell, cleanup and reboot added no GPU IO. These are one sample per
named software failure, not a hardware-hang or reliability qualification.

The initial actual Ring3 RAM and display-probe suites passed, including all
six vertical runtime markers and complete CPU baselines. Later shorter
preflights did not repeat those suites. Six firmware reports matched all 64
golden fields each and CRC32 E2CB542E. Sixteen active COM7 sessions/78 commands
retained RX116611/TX1007 bytes. Passive capture retained 1694 attributable kernel
boot bytes, giving scoped/all-raw RX118305; there were no unattributed bytes
inside these sessions. Inter-session gaps remain excluded. The first passive
connection disconnected before receiving data; its zero-byte journal is kept.
No active serial session failed and no GPU command was retried.

All 498 sealed software files and 200 source hashes matched before these six
qualification-document updates. Original requirements, the tested artifacts,
42 older images and 48 prior manifest roots remain preserved. No new GPU
register, value, mapping, PCI, DMA, IRQ or reset authority was introduced.
Operational inherited DCE/DP and no hotplug/power/reset remain assumptions;
software deadlines cannot cancel a stalled bus transaction. Quarantine is not
DMA/IRQ/IOMMU isolation or safe assignment reuse. Native resolution changes,
VRAM/clock/link programming, DP training, HDMI, full GPU reinitialization,
acceleration and Intel HD620 still require separate work.


## DP stream and update-control observation

The next prerequisite for native mode changes is a bounded observation of the
DP stream steering FIFO and CRTC update controls. `polaris-stream` captures the
existing 86-value timing/zero-override context, then two ordered samples of five
fixed fields: DP2 stream control, DP2 steering FIFO, DIG2 frontend control, and
CRTC0 master update lock/mode. Three of these are new read targets; all use the
existing RO/NX/UC pages 0x6000 and 0x13000. It adds no writable mapping or GPU
store. The command shares the one-attempt link/timing/vertical capture budget.

The exact selected RX570/GOP/BAR5/D0/CPU/COM1/kernel-lease admission remains in
force. Before the ten extended reads, the original 86 values must match the
qualified zero-override profile. There are at most 96 dword loads, no poll,
wait, retry or assignment reuse, and a two-second software admission check
immediately before each load. A stalled bus load is still uncancellable.
After access closes/unmaps and CPU holds/capabilities are removed, a fresh
Ring3 probe decodes only copied data. The existing inherited assignment stays
quarantined; firmware scanout is retained under the declared operational-DCE,
unchanged-link and no hotplug/power/reset assumptions.

The fresh BEGIN/DATA/QUERY role accepts version1, exactly 96 ordered uint32
values and 14 ordered report queries. Changed tail values, changed route or
baseline, all-ones data and malformed protocol close/refuse with no partial
report. The report preserves raw stream/FIFO/frontend/update words and flags
reset, held/unknown update controls or other FIFO bits. Unknown bits are never
silently cleared. A successful observation or zero flags grants no authority
to blank, unblank, commit timings or publish restored output.

The pinned consumer waits for inactive stream status before FIFO reset;
unblank sets DIG_START, clears FIFO reset, primes for100us, then enables the
stream. These are dependencies for a future fixed transaction and fresh rescue,
not implemented writes in this pass. The source audit checks eight hashes,
five compiled offsets and seven field masks; the review names the exact
read subranges, assumptions and remaining write prerequisites in
[POLARIS_STREAM_REVIEW.json](POLARIS_STREAM_REVIEW.json).

Host tests cover five offset boundaries,96 all-ones inputs,10 changed samples,
84 baseline refusals/two allowed automatic-M values,two route mismatches,
96 individual raw FIFO/update bits and97+87 protected buffer prefixes. The
production MMIO helper passes identity/context/frame/PTE/IRQ/quarantine and
field refusals without loads, plus both existing page domains. The first
PTE fault test targeted a different mock leaf; it was corrected to find the
intended virtual address. The first build caught a missing shell-registration
argument list. Both failed logs are retained; no hardware command ran.

Actual Ring3 tests pass the independent 14-word golden report, refusal clearing,
raw state flags,nine malformed-order/version/role cases and complete CPU
resource baselines. Four final-image QEMU runs pass: focused,two displays,
normal88/88 and deep88/88/611. Unsupported QEMU hardware refuses before MMIO.
Only registerprobe.elf changed among27 archive files/21 ELFs;26 files/20 ELFs,
including the primary and rescue drivers, match v40. Kernel/probe are free of
runtime relocations, vendor decoders stay in userspace, loader code is unchanged,
and the default-off capture object imports terminal output only.

Candidate: `build/rx570-stream-v41/JA-os.img`, SHA-256 `24df103e3abb491476a78c4ab65e1aebf0770d7f91672af7db2342d468c39c22`.
Physical qualification of these new read targets is pending. The next hardware
pass captures raw values and the Ring3 report over COM7, confirms firmware
output survives, checks cleanup/refusal and returns through controlled reboot.
Existing v40 rescue-failure and v39 successful-restoration evidence remains
sealed separately. Full mode changes, VRAM/clock/link authority, effective-rate
measurement, DP training, HDMI and hardware reset/isolation remain open.


## Physical DP stream and update-control qualification

Physical v41 passed its single read-only capture on the Z390 Gaming X/F10,
i7-9700K, RX 570 1002:67DF rev EF / 1458:2310 and Philips Momentum 272M8 over
DisplayPort. The image remains `24df103e3abb491476a78c4ab65e1aebf0770d7f91672af7db2342d468c39c22`. Evidence is recorded in
`build/rx570-stream-v41/hardware/validation.json`; the 493-file software record
and tested artifacts remain unchanged.

The capture completed exactly 96 ordered dword loads. The first 86 matched
the saved physical baseline apart from the two permitted automatic-M samples,
which were 0x758B and 0x758C and remain separately recorded. Both extended
five-field samples were identical:

| Register | Raw value |
| --- | --- |
| DP2_DP_VID_STREAM_CNTL | 0x00010201 |
| DP2_DP_STEER_FIFO | 0x00000000 |
| DIG2_DIG_FE_CNTL | 0x01000000 |
| CRTC0_CRTC_MASTER_UPDATE_LOCK | 0x00000000 |
| CRTC0_CRTC_MASTER_UPDATE_MODE | 0x00000002 |

The stream was enabled with status 1 and disable-defer 2. FIFO reset and
master update lock were clear. Update mode was 2; a future timing transaction
must preserve or explicitly qualify changing that observed value. The copied
Ring3 report matched all 14 independently calculated words, with diagnostic
flags zero. Neither report success nor zero flags grants write permission.

There were no stores, polls, waits or retries. Access closed/unmapped and CPU
holds/capabilities were removed before copied decoding; the inherited GPU
parent remained quarantined. A second capture refused before GPU IO.
`displayprobe` retained firmware scanout, and the operator confirmed readable
JA OS output and the prompt. Serial reported no drops, timeouts or stall;
power readiness passed before reboot preparation. Controlled reboot returned
JA OS boot output and an operator-confirmed readable prompt without boot-menu,
reset or power intervention. The final target is a fresh firmware monitor,
stage 0 and power ready, with COM7 closed and available.

Initial actual Ring3 stream/refusal/resource tests, display-probe and native
RAM recovery tests passed. Two firmware reports matched all 64 golden fields
each and CRC32 E2CB542E. Five active COM7 sessions/21 commands retained
RX35742/TX245 bytes, with no unassigned bytes inside sessions. Initial boot was
not captured: the adapter was unplugged for flashing and reconnected after
boot confirmation. The passive zero-byte journal is retained; the controlled
reboot is captured in its active session. Inter-session gaps remain excluded,
and no USB byte readback was performed. No active session failed or physical
GPU attempt was retried.

All 210 source hashes matched before these six qualification-document changes;
204 sources remain identical to software freeze. The original requirements,
43 older images and 50 prior manifest roots remain preserved. This qualifies
only the named read-only observation under operational inherited DCE/DP and
no hotplug/power/reset assumptions. Software admission cannot cancel a stalled
bus load; inherited bus mastering is unisolated and quarantine is not safe
assignment reuse. No blank/unblank or changed-mode write was executed.

Next is a finite DP stream blank/unblank transaction with independently retained
state and a fresh rescue path. Exact write values, status polling, FIFO ordering,
priming delay and failure after each mutation need their own execution gate.
Native resolution changes, VRAM/clock/link programming, DP training, HDMI,
effective-rate measurement and hardware reset/isolation remain open.


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
