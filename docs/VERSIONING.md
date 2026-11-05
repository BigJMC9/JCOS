# JCOS versioning and release references

## Current release status

The source baseline reviewed on **2026-11-05** is
[`fcfe857140891d7c25a44814e1738970d72a852a`](https://github.com/BigJMC9/JCOS/commit/fcfe857140891d7c25a44814e1738970d72a852a).
No release tags or published GitHub releases existed at that review.

Use **Unreleased — commit `<full SHA>`** when reporting a build from the current
tree. The proposed first public release target is **`v0.1.0-alpha.1`**. That name
is a release target, not an existing tag, a completed release, or a claim of
stable application compatibility. This documentation change does not create a
tag or change the version printed by a binary.

## Public version format

Public release tags use `vMAJOR.MINOR.PATCH`, optionally followed by a prerelease
suffix such as `-alpha.1`, `-beta.1`, or `-rc.1`.

| Component | Project policy |
| --- | --- |
| `MAJOR` | A deliberately declared compatibility generation. `0` means the project is still establishing its supported interfaces. |
| `MINOR` | A coherent capability increment. During `0.x`, an intentional incompatible public-interface change also requires a minor increment and migration notes. |
| `PATCH` | Compatible fixes to the published version's supported behavior. |
| Prerelease suffix | An explicitly incomplete release within that version line. Increment its sequence for a new candidate; do not move an existing tag. |
| Commit SHA | Exact source identity, required for development builds and retained in release provenance. |

Version numbers describe the public product. Do not derive them from a task
number, test count, document revision, or development phase. Release notes
describe available behavior, changes, requirements, and limitations.

## Interface and documentation versions

A product version does not replace a protocol version. Startup structures,
service message formats, file formats, and device-support profiles have their
own compatibility contracts. Their version constants remain defined alongside
the interface in `include/` or the relevant implementation. A filename such as
`USB_HOST_V1.md` identifies that bounded support profile; it is not the OS release
number.

Documentation is maintained with the source and identified by Git history and
its reviewed source baseline. A spelling or explanation correction does not
require a product release. A behavior change requires the associated public
contract and tests to be updated in the same change.

## Referencing a build

For an untagged build, record:

```text
Product: JCOS
Release: Unreleased
Source: <full commit SHA>
Local changes: clean | describe changes or attach patch
Build: <exact command and configuration>
Artifacts: <filenames and SHA-256 hashes>
Platform: <machine/firmware or QEMU/OVMF configuration>
Validation: <commands, outcomes, logs and unexercised paths>
```

For a published release, replace `Unreleased` with the actual immutable tag and
retain the commit and artifact identities. A suggested version in a document is
never sufficient evidence that a binary came from that version.

## Preparing the first release

The proposed `v0.1.0-alpha.1` release should describe the actual restricted
runtime, service and graphical interfaces, and the constrained device-support
profile documented in [the documentation index](README.md). Before creating the
tag, bind the release to a specific commit, validated boot image, captured test
results, and stated hardware coverage. Record any refused or virtual device
paths so a passing suite does not imply untested physical-device support.

The intended scope is the documented experimental system. Broader native device
support, unrestricted bus mastering, multicore execution, and general hosted
application compatibility are not implied by this proposed version.
