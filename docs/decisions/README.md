# Decisions (ADRs)

Architecture Decision Records for LGP-Recomp. Local threads (PC/Mac) have no project memory, so this folder is the binding source for decisions that have been made.

## Status values

- **Proposed**: options and recommendation are in place, decision open.
- **Accepted**: approved by Paul. Only then does the decision apply to code.
- **Superseded by NNNN**: replaced by a later ADR.

According to the project instructions, core decisions (0001–0005) need Paul's approval.

## Overview

| No. | Title | Status |
|---|---|---|
| [0001](0001-gast-adressraum-wasm.md) | Guest address space in WASM: memory64 vs. 32-bit remapping | Accepted |
| [0002](0002-threads.md) | Threads: real WASM threads vs. cooperative scheduling | Accepted |
| [0003](0003-grafik-hle-lle.md) | Graphics: NVN HLE vs. Maxwell LLE; shaders offline vs. at runtime | Accepted |
| [0004](0004-indirekte-spruenge.md) | Indirect jumps, self-modifying and JIT code | Accepted |
| [0005](0005-sdk-nsos.md) | `sdk`/`subsdk*` NSOs: recompile them too vs. HLE | Accepted |
| [0006](0006-differential-test-harness.md) | Differential test harness: Hypervisor.framework (Mac) + Unicorn (CI) | Accepted |
| [0007](0007-ios-nativ-statt-wasm.md) | iOS/iPadOS port: native arm64 build instead of WASM | Accepted |
| [0008](0008-native-apps-auf-allen-geraeten.md) | P7: native apps on all devices (SDL3 + wgpu-native, Android native, tvOS conditional) | Accepted |

Decision paper for 0001–0005 with the P1 measurement data: [`P1.6-entscheidungsvorlage.md`](P1.6-entscheidungsvorlage.md).

## Template for new ADRs

```markdown
# NNNN – Title

Status: Proposed · Date: YYYY-MM-DD · Decides: Paul

## Context
## Options
### A …
Pros / cons
## Recommendation
## Open questions / data needed
## Consequences
## Sources
```
