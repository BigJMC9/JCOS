# AMD Polaris display backend

The display service links a userspace register reader and decoder in
[user/drivers/amd_polaris_display.c](../user/drivers/amd_polaris_display.c).
Normal display-service diagnostics use synthetic fixtures. A separate, default-off
kernel-monitor experiment can sample physical registers on the exact selected
RX 570 candidate. It closes and unmaps live access before a dedicated Ring3
process decodes immutable copied values. No native modesetting, reset, connector
programming or independent native rescue is implemented by this path.

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
accept named pipe fields or the two location selectors, never a raw offset.
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
refused. It samples fifteen fields twice on that pipe. Profile 2 also reads
MC_VM_FB_LOCATION (byte0x2024) and HDP_NONSURFACE_BASE (byte0x2C04) before and
after those samples: at most forty DWORD loads, no retry, once per boot after
preflight. Each transaction is journaled before the load. A two-second admission
limit cannot interrupt or cancel a stalled device/interconnect transaction.

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
masks. It compares all ninety byte offsets, fifteen semantic selector values,
thirty-six field/enum/location facts and the four-page pipe footprint against the pinned AMD
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
make -f Makefile.msys2 test-polaris-mmio test-polaris-read-profile test-amd-polaris-display
~~~

The audit downloads nothing and fails if a cached source hash differs.
It writes its compiled facts and detailed JSON report under the chosen BUILD
directory. Completing this audit does not remove physical qualification
blockers or activate the GPU. Keep image identity, raw serial bytes, counters,
post-capture commands and visual output confirmation with each physical run.


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

These mechanisms do not program the RX570, establish hardware readiness or
start a rescue backend. The future constrained physical provider must prove
operational output before publication and remove failed Ring3 mappings before
replacement. Host fixtures supply simulated descriptors only; no automatic
diagnostic labels the live GPU output native or rescue.
