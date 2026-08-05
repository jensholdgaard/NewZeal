# Why Zeal's OTLP exporter is hand-rolled and not opentelemetry-cpp

**Status:** decided (2026-08-01) — hand-rolled, with spec validation in CI.
**Evaluated on:** branch `otlp-sdk` (a complete, working SDK implementation) and the
`spike-otel-cpp` / `build-sdk` workflows.

## The question

The exporter builds OTLP/JSON by hand with `nlohmann::json` and posts it with WinHTTP. Should it use
the official `opentelemetry-cpp` SDK instead? The SDK would give spec-correct serialization, proper
instruments, batching, retry and cardinality limits for free — and a hand-rolled serializer had
already shipped one real bug (a single attribute emitted as an object instead of a one-element
array, which invalidated *every* metrics payload whenever a zone name was attached).

So this was evaluated properly rather than argued from taste.

## What was measured

`opentelemetry-cpp[otlp-http]` **does** build for Zeal's exact target (`x86-windows-static`, i.e.
32-bit with the static CRT). It is not impossible, only expensive:

| | |
|---|---|
| vcpkg build time | **29 min** (protobuf + abseil + curl from source) |
| static libs produced | **122 files, 667 MB** unlinked (`libprotobuf.lib` alone is 194 MB) |
| transitive deps | protobuf, abseil, **libcurl**, zlib |

A full SDK-backed implementation was written (`Zeal/otlp_exporter_sdk.cpp`): same `/otlp` commands,
same metric names and `eq.*` attributes, OTLP HTTP metric + log exporters, a periodic reader, counters
for damage/heal and observable gauges for attack/haste. It compiles.

## What stopped it

Integration failures, in order — the first three were ours to fix, the last one is structural:

1. A project-wide `NOMINMAX` broke 164 existing call sites (Zeal uses the Windows `min`/`max` macros).
2. `ObserverResult` passed to a `void*` trampoline — our glue.
3. `InitSecurityInterfaceW` / `if_nametoindex` unresolved → **libcurl** needs `secur32` + `iphlpapi`.
4. **`zs.lib(inflate.c.obj)`: `_inflate` already defined in `d3dx8.lib(inflate.obj)`.**

That last one is the finding. **libcurl pulls in zlib, and zlib is already statically baked into
`d3dx8.lib`** — the 2003-era DirectX 8 library the game links against. Two copies of the same C
library in one binary. The ways through looked bad: `/FORCE:MULTIPLE` (the linker silently picks one
zlib; curl may then call 2003 code through modern headers — memory corruption risk inside a DLL
injected into the game) or permanently maintaining a custom vcpkg feature set that builds curl
without zlib, surviving every upstream Zeal sync.

> **Correction (2026-08-05).** That last sentence was wrong, and the conclusion it supported was
> reached too quickly. curl is *optional*, and so is the zlib it drags in. `spike/otel-winhttp`
> builds opentelemetry-cpp v1.28.0 for **x86** with `WITH_HTTP_CLIENT_CURL=OFF` and a WinHTTP
> transport supplied instead, then delivers a real OTLP payload end to end:
>
> ```
> WITH_HTTP_CLIENT_CURL:BOOL=OFF
> installed libs: 24 · no curl libraries in the install tree · zlib is absent
> PASS: OTLP payload delivered over WinHTTP, HTTP 200
> ```
>
> Two things made this look impossible at the time. The option is **recent**: on v1.24.0 a plain
> `set(WITH_HTTP_CLIENT_CURL ON)` overwrites anything passed on the command line, so curl is
> mandatory and `-D` is silently ignored. And zlib enters *only* through curl — it is gated behind
> `WITH_HTTP_CLIENT_CURL AND WITH_OTLP_HTTP_COMPRESSION` — so removing curl removes the collision
> outright, with no `/FORCE:MULTIPLE` and no vcpkg patching.
>
> This does not reverse the decision below: protobuf and abseil still roughly double the `.asi`, and
> that remains the deciding cost for a binary guildmates download. What it does change is the
> *reason*. The SDK was rejected here for size and dependency weight, not because its HTTP layer
> cannot be replaced — it can, through an extension point upstream provides deliberately
> ("set OFF to supply a custom transport").

Note that problems 3 and 4 both come from **libcurl**, which exists only because the SDK's OTLP HTTP
exporter uses it. The hand-rolled path uses **WinHTTP** — already in the OS, no dependency tail, and
already proven under Wine, where this client actually runs.

## Decision

Keep the hand-rolled exporter. Additionally:

- **`ci/otlp_validate.py`** validates our payload shapes against the official `opentelemetry-proto`
  schema — the same definitions the collector uses — on every build. The bug that motivated this
  evaluation is rejected offline, instantly, with an exact field path.
- The same check enforces **semantic invariants** the schema can't express (trace/span id widths and
  non-zero-ness, `endTime >= startTime`, cumulative sums carrying `startTimeUnixNano`).
- Planned: generate `eq.*` name constants from the semconv registry with **OTel Weaver**, so a typo'd
  attribute becomes a compile error rather than a silently split timeseries.

Together these recover most of what the SDK offered, with no new dependencies.

## Secondary consideration

This repo is a fork that stays mergeable with upstream `coastalredwood/Zeal`. A PR adding vcpkg,
protobuf, abseil and libcurl to a lean injected `.asi` — roughly doubling the binary users download —
is unlikely to be accepted, and reasonably so. ~600 self-contained lines using the already-vendored
`json.hpp` is a plausible contribution.

## If this is ever revisited

The SDK branch is preserved. Revisit if: histograms or exemplars are needed (hand-rolling those is
genuinely hairy), several more serialization bugs slip past the CI validation, or upstream adopts a
package manager and the dependency tree stops being a fork-local cost.
