# AMD display profiles and runtime state

The current implementation supports one qualified physical configuration:
RX 570 `1002:67DF`, revision `EF`, subsystem `1458:2310`, DCE11.2,
on the tested Z390 with the Philips display connected through DisplayPort.
It uses inherited operational scanout and the existing GOP allocation.
Extracting configuration into objects does not add support for other boards,
monitors, resolutions, HDMI, GPU allocations or independent output initialization.

## Current boundaries

| Object or layer | Responsibility | Authority boundary |
|---|---|---|
| `PolarisHardwareProfile` | Compiled board identity, accepted surface layout/address constraints, MMIO span/pages, selected pipeline and vertical-plan expectations | Only the original immutable descriptor is accepted; a copied descriptor is refused before dereference |
| `PolarisDisplayRuntime` | Kernel-retained framebuffer metadata, validated aperture, expected surface and separately captured MC/HDP/GOP correlation | An expected GPU address is not a discovered allocation; successful live correlation is required before takeover |
| `PolarisSurfaceRuntime` | Checked backing range, GPU address, pipeline and CRTC state for one driver reservation | RAM fixtures are allowed for CPU tests; this object alone grants no device access |
| `PolarisRetainedRescueState` | Independent kernel copy of pre-mutation vertical samples and checked report | Neither driver maps or supplies this retained baseline |
| MMIO binding and finite operation gates | Revalidate PCI/BAR/PM/CPU permissions, assignment identity, leases, operation values/order and attempt budgets | Profile detection never bypasses these checks or creates a register grant |
| Ring3 primary and rescue | Existing bounded vendor sequencing through the private broker | No arbitrary offsets, MMIO mapping or physical-address grants |
| Ring3 supervisor | Recovery and GUI reconnection decisions | Kernel enforcement remains independent of policy and failed driver state |

The implementation lives in [polaris_profile.h](../kernel/polaris_profile.h)
and [polaris_profile.c](../kernel/polaris_profile.c). Native orchestration,
driver reservation, read readiness, surface correlation and MMIO admission share
the same descriptor. Detection matches the full qualified identity, including
revision and subsystem. A PCI device ID or a family label alone cannot authorize
a board. Selected GOP provenance and live resource checks remain mandatory.

The original fixed values remain: CPU framebuffer `0x50000000`, GPU surface
`0xF400000000`, `0x300000` bytes, 1024x768 BGRX, stride1024; BAR5 extent
`0x40000`; pipe0. Vertical writes retain encoded total1132 and control`0x11`,
with zero restoration in the existing order. ASIC register definitions, ABI
virtual addresses, IPC shapes, mechanism capacities and finite attempt counts
remain in their own layers. They are not user-editable profile parameters.
Userspace binaries still implement the existing fixed layout/protocol; changing
a profile cannot make those binaries support a new mode.

Corrupt or unknown descriptors fail closed. Ambiguous MMIO cleanup retains the
registered mappings and pins. Once cleanup closes admission, restoring a pointer
does not reopen it. Existing one-attempt-per-boot restrictions, quarantine and
CPU writer exclusivity remain. CPU teardown does not prove GPU memory is reusable
or that inherited bus-master activity is isolated.

## Extension contracts

Adding another profile requires independently qualified identity, resource,
mode and recovery evidence. The descriptor table is not a permissive family
match, a script interpreter or a substitute for per-operation enforcement.
A future ASIC backend needs its own register semantics, power/reset assumptions,
finite initialization and recovery sequences, and unsupported-operation refusals.
Shared interfaces can carry validated descriptions without assuming that another
AMD generation uses DCE11.2 registers or the same reset domain.

Variable layouts require checked dimensions, pixel format and bytes per pixel,
stride/alignment and overflow-safe byte extents across the producer, GUI,
broker and scanout consumer. CPU physical addresses, GPU virtual addresses,
aperture offsets and CPU mappings must remain distinct types or fields.
A GPU allocation contract must track backing, bounds, ownership, generations,
scanout references and outstanding access. Its release must establish device
quiescence or retain quarantine; unmapping CPU pages alone cannot free it.
There is currently no native VRAM allocation/free path.

Pipeline and mode support require discovery and validation of the active
connector/encoder/CRTC route, supported monitor modes and all timing/clock/link
limits before programming. A mode request needs an independently checked plan,
exclusive ownership and a restoration plan retained before the first mutation.
Automatic DP M samples may vary; observed update mode2 must not be replaced
with an assumed initialization value. EDID or firmware tables provide input,
not permission to program unchecked timings.

Repeated initialization or device reuse requires a new lifecycle contract:
retire old sessions and generation tokens, drain mappings/IRQs/DMA, prove or
reset hardware state, then admit a fresh plan. The existing one-time CPU driver
replacement on unchanged output does not implement device reinitialization.

Independent display initialization is still unimplemented. Its acceptance must
show that programmed scanout no longer depends on firmware-established state.
Using a UEFI bootloader or validated board tables is distinct from relying on
inherited scanout. Cold GPU initialization, changed modes, DP training, HDMI,
GPU hangs and reset/isolation each require their own hardware evidence. Other
AMD generations remain unimplemented.

## Recovery and regression

Firmware framebuffer recovery is valid only while firmware scanout remains
undisrupted and the hardware remains operational. After native retirement,
recovery uses the separately reserved Ring3 rescue and independently retained
state. If restoration fails, serial control and controlled reboot remain the
last-resort path; stale pixels are not a successful rescue.

`test-polaris-native` includes golden profile values, full-identity refusals,
forged descriptor and runtime corruption checks. The same pure profile checks
run inside the actual Ring3 native/rescue kernel suite. MMIO host tests retain
independent literal addresses, values and operation-order assertions, and test
that an invalid profile cannot trigger IO or discard ambiguous cleanup pins.
The vertical audit compiles the profile values and compares them against the
previously reviewed fixed contract. Host/QEMU results cannot substitute for
matched-image physical display, rescue, serial and reboot acceptance.


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
