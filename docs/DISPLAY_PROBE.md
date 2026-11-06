# Display inventory and native driver bring-up

The monitor's `displayprobe` command (alias `display-probe`) collects
read-only PCI and copied firmware display inventory. The active backend remains
firmware GOP; this command does not implement native modesetting or acceleration.

Enter `monitor` from the normal userspace shell, then run:

```text
cpu
pci
displayprobe
displayfirmware
test display-probe
test display-lease
test display-service
test display-surface
```

Use [Console capture](CONSOLE_CAPTURE.md) to record COM1 directly to a host file;
photographs remain a fallback when no serial connection is available. The current UART uses
**38400 baud, 8 data bits, no parity, 1 stop bit (8N1)**. UART discovery alone does
not verify an external serial input/output path on a physical machine.

## Boot metadata and provenance

BootInfo version 10 retains the firmware mode index and optional PCI provenance
introduced in version 7, and adds the captured PCI identity, optional BAR extents,
an optional 128-byte active EDID base block, per-BAR query evidence, and explicit
query-to-BAR associations with bounded raw descriptor prefixes.
Deploy the matching loader and kernel together. Layout/size assertions preserve the entry stack at offset zero.
No callable firmware pointers survive ExitBootServices.

The loader keeps the existing GOP selection. It requires one unambiguous handle
for that selected interface and resolves its PCI I/O controller directly or by
device path. Duplicate selected interfaces, failed handle inspection, bridge
ancestors, bad widths, missing protocols and more than 64 GOP handles leave the
optional association unknown. They do not prevent framebuffer boot.

SELECTED GOP requires matching segment-zero BDF, vendor/device and class/revision
against the enumerated display function. Nonzero segments are retained in boot
metadata and refused by the current segment-zero backend. No owner is guessed
from the first display function or a nearby BAR. Mode index zero remains valid,
including when PCI provenance is unavailable.

1002:67DF is a Polaris10 candidate ID shared by multiple board products. Confirm
the actual board/subsystem/revision before selecting a native device profile.

## Firmware BAR bounds

Before firmware exit, the loader queries the selected controller's
GetBarAttributes resource descriptors. It accepts one to eight checked QWord
address descriptors followed by End Tag, validates contiguous base/length against
the PCI header, and rejects unsupported translation and malformed/overflowing
ranges. It frees successful protocol allocations. It performs no BAR sizing writes.

The probe binds captured extents to exact current device/subsystem identity,
BAR type/flags and base. It reports unknown/unverified when no matching extent
exists. Secondary GPU resources are not borrowed from the selected controller.
The extent is **inventory, not register/mapping authority**; safe register
semantics, isolation and recovery remain separate requirements.

The aggregate must have a nonzero, aligned power-of-two length and fit the
BAR's address width. Gaps, overlaps, mixed resource types/widths, translations,
unsupported forms, excess descriptors and invalid End Tag checksums are refused.
No partial extent is published. The firmware allocation/pointer contract remains
trusted. A descriptor's AddrRangeMax is retained for diagnosis, not used as an
endpoint; EDK2 also uses it for alignment. Base and length determine the checked
extent.

The loader queries all six legal firmware indices, including indices that are
continuations or unassigned in the physical PCI header. Firmware query numbering
is kept separate from physical BAR slots. Each complete copied resource list is
checked against every assigned lower BAR slot on that same selected controller.
A result must match the exact base, type and width. An association is retained
only when one query matches one real BAR, and no other query claims that BAR.
Multiple matches, duplicate/conflicting responses and configuration changes
leave bounds unknown. No dense numbering scheme or missing length is guessed.
Nonstandard type values are refused without masking unknown bits.

The probe's FW BIND line reports the accepted source query index, or NONE.
The separate `displayfirmware` command prints all six query outcomes, raw EFI
status, exact matching-BAR bitmasks, descriptor fields and up to 48 copied prefix
bytes per response. A mask bit identifies a physical BAR slot, not a query
number. An accepted inventory association from a different index is visible;
that index must never be used for device reads/writes. This diagnostic reads
only copied boot metadata. It performs no new firmware call after firmware exit.

Empty End-Tag-only lists are reported as NO ADDRESS DESCRIPTORS. Query failure
outputs are never dereferenced or freed; successful protocol allocations are
freed. Unsupported forms and translated/noncontiguous ranges remain refused.
The successful firmware allocation/pointer contract remains trusted.

These associations supply **inventory only**. A firmware numbering discrepancy,
a copied length or a successful decoder does not qualify native register access,
a device-specific operation, or recovery. Retain the exact response and current
PCI identity when qualifying any later access profile.

## Optional EDID

Only EFI_EDID_ACTIVE_PROTOCOL from the unique selected GOP handle is considered.
The loader copies a checksum/header-validated EDID 1.0-1.4 base block. The probe
reports bounded printable monitor-name data, numeric IDs and DTD0 geometry/clock
when present. Firmware may override EDID. Extension blocks are not copied, and
the result does not prove connector routing or authorize a native mode.

Missing/invalid EDID is reported explicitly. The loader does not select another
GPU's EDID or issue a new DDC/AUX transaction through a JA OS native driver.

## Safety and tests

MSI/MSI-X and INTx fields describe firmware state; no IRQ route is installed.
FLR advertisement does not establish successful reset/output restoration.
Inherited bus mastering is reported without changing it. Sibling reset domains,
IOMMU protection, quiescence and DMA isolation remain unqualified.

The inventory does not size BARs, access vendor MMIO or ROM, change PCI command
bits, claim/reset a GPU, submit DMA, replace the display service or grant device
authority. Optional firmware queries run before the final memory-map/firmware
exit sequence.

The `display-probe` diagnostic covers decoder refusals, exact firmware/
current-config binding, EDID bounds/checksum, and unchanged command/BAR/ROM/IRQ/
display-lease state. Its synthetic BAR lengths are test fixtures, not physical
measurements. Host firmware doubles test the real loader routine separately:

```sh
make -f Makefile.msys2 test-boot-display
```

This builds a host test executable, without adding hosted runtime features to
JA OS. The tests exercise multiple GPUs, selected-child lookup, ambiguity,
narrowing, missing resources, failed-call outputs, non-slot query numbering,
duplicate/ambiguous association refusals, raw-prefix bounds, multi-descriptor acceptance
and refusal reasons, checksum/length bounds, and allocation cleanup.
QEMU inventory success does not qualify a native RX 570 or Intel driver.

## Physical-machine handoff

Record source/local changes, image/binary hashes, motherboard/CPU/BIOS/settings,
GPU model/PCI IDs, connector/cable, monitor, GOP configuration and independent
diagnostics/recovery. Keep unknowns explicit and distinguish raw serial logs
from photo transcriptions.

Return to `usershell`, run `gui`, exercise focused input, press Escape
and verify terminal restoration. Qualify DisplayPort and HDMI separately.
Changing cables between clean boots does not establish hotplug support. Preserve
complete normal/deep results and conditional refusals; see [Testing](TESTING.md).

Native takeover requires a qualified access, buffer and recovery profile. The
[Graphics recovery contract](GRAPHICS_RECOVERY.md) separates conditional firmware
recovery, native rescue, and independent serial/controlled reboot.

## References

Candidate identity follows the [Linux v6.12 AMDGPU PCI table](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c).
Firmware queries follow [UEFI PCI I/O](https://uefi.org/specs/UEFI/2.10/14_Protocols_PCI_Bus_Support.html),
the [EDK2 resource-descriptor implementation](https://github.com/tianocore/edk2/blob/edk2-stable202408/MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c),
and the [active EDID protocol](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/Protocol/EdidActive.h).
No external vendor driver or firmware binary is bundled.

Boot-scanout status is reported separately from the CPU writer lease. A safe
boot page span describes preserved metadata; direct mapping additionally needs
firmware-valid scanout and the exclusive lease. Retired/retiring scanout is
reported without claiming that the saved GOP address remains active. The
[graphics recovery contract](GRAPHICS_RECOVERY.md) describes this retirement
gate and its CPU-only proof limits.

The kernel-monitor `display-readiness` command also reports the proposed
finite Polaris read operation and its local PCI/PM/BAR/CPU/COM1 observations.
It performs no assignment, mapping, GPU access or activation. Unresolved
register-read safety, independent serial control and physical evidence remain
blocked even if every local observation succeeds. See the
[Polaris backend reference](AMD_POLARIS_DISPLAY.md#finite-capture-admission-and-readiness).
