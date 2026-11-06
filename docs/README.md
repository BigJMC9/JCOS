# JCOS documentation

Public documentation describes implemented behavior, supported contracts, and
how to reproduce or qualify it. The repository root [README](../README.md) is
the entry point for building and running JCOS.

| Reference | Authoritative scope |
| --- | --- |
| [Service boundary](SERVICE_BOUNDARY.md) | Privileged mechanisms, userspace policy, capability authority, graphics, device mediation and recovery. |
| [Runtime profile](RUNTIME_PROFILE.md) | Single-CPU scheduling, integer-only applications, entry/startup and fault behavior. |
| [Memory ownership](memory.md) | PMM/VMM ownership, shared mappings, permissions, stack/image teardown and DMA lifetime. |
| [Display inventory](DISPLAY_PROBE.md) | Read-only PCI/GOP provenance, hardware handoff and native bring-up limits. |
| [AMD Polaris display](AMD_POLARIS_DISPLAY.md) | Userspace DCE decoding, finite protected capture, BAR5 preparation and physical qualification boundary. |
| [Graphics recovery](GRAPHICS_RECOVERY.md) | Conditional firmware recovery, required native rescue and independent serial/reboot acceptance. |
| [Console capture](CONSOLE_CAPTURE.md) | Host serial file capture, QEMU logs, UART counters and storage limits. |
| [Testing](TESTING.md) | Diagnostic commands, aggregate/deep runs, hardware checks and evidence capture. |
| [USB host support profile](USB_HOST_V1.md) | Supported xHCI keyboard topology, diagnostics and limits. |
| [Versioning](VERSIONING.md) | Public release labels, unreleased source references and interface compatibility. |

Keep one maintained reference per topic. Update the owning reference when a
contract changes, and link to it from overview pages instead of copying its
full details. Use Git history for superseded text rather than adding numbered
or `UPDATED` copies beside current documents.

Interface constants and executable behavior live in the source. A document's
reviewed revision tells readers which source it describes; a test name or
example output does not establish that a new build or platform was tested.
