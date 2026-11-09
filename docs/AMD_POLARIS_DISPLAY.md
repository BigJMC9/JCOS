# AMD Polaris display backend

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
