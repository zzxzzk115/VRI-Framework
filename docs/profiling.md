# Profiling and benchmarking in vrf

## Decision: GPU cost is measured with VRI's built-in profiler

All benchmark numbers go through **`vrf::GpuProfiler`** (`vrf/gpu/gpu_profiler.hpp`), which sits on
VRI timestamp queries (`vri_ext_query.h`). The older route — libvultra plus Tracy — is gone with
libvultra and should not be reintroduced or referenced in new work.

**One distinction worth keeping straight, because "Tracy is dead" is too strong.** vrf still contains
Tracy, but only as the *optional CPU-side* zone macro in `vrf/core/profiling.hpp`: `VRF_ZONE("name")`
compiles to nothing unless `vrf_with_tracy` / `VRF_WITH_TRACY` is enabled, which it is not by default.
That is a **CPU timeline** tool and it is complementary to `GpuProfiler`, not superseded by it. Do not
remove `VRF_ZONE` on the strength of this decision; what is retired is libvultra's profiling path, not
CPU zones as a concept.

So the two axes are:

| Axis | Tool | Measures |
| --- | --- | --- |
| GPU | `vrf::GpuProfiler`, `VRF_GPU_ZONE` | timestamp deltas on the device, nestable, results lag by `framesInFlight` |
| CPU | `VRF_ZONE` (opt-in Tracy) | wall time on the host: graph compile, pass setup, submission |

Never mix them into one number. Pass *setup* (the framegraph builder lambda) is CPU-only work that
runs at graph-compile time; charging it to a GPU figure would be meaningless.

`GpuProfiler` already handles the parts that are easy to get wrong: it resolves results only after
the frame slot's fence (which `FrameStream::Begin` has already waited on, so it adds no sync), and it
degrades to a no-op with `Enabled() == false` on devices without timestamp queries, so callers never
branch.

## Proposed: automatic pass instrumentation + a profiler panel

Today each call site must wrap its own work in `VRF_GPU_ZONE`. Every framegraph pass already carries
a name (`addCallbackPass<T>("GBufferPass", ...)`), so the executor can open a GPU zone around each
pass's execute callback automatically and the UI layer can render `GpuProfiler::Results()` — already
ordered and carrying `depth` — as a collapsible tree with no per-pass code at all.

**The reason to prefer automatic over opt-in is not convenience, it is completeness.** Hand-placed
zones measure what someone remembered to instrument. A pass that was never wrapped reads as free, and
the passes most likely to go un-instrumented are exactly the cheap-looking ones added late — barriers,
clears, resolves. A cost that hides is worse than a cost that is large.

### Draw-call-only vs pass-inclusive timing

The open question was whether to time only the draw/dispatch rather than the whole pass. **Do both,
nested, with pass-inclusive as the default.** `GpuProfiler` already supports nesting, so an outer zone
per pass and an inner zone around just the draw costs one extra timestamp pair and answers both
questions:

- **Inclusive (outer)** is the number that decides "is the frame faster". It captures layout
  transitions, clears, descriptor writes and barrier stalls — real GPU time that a draw-only figure
  omits.
- **Draw-only (inner)** is the number you want when optimising one shader, where surrounding overhead
  is noise.

Reporting only draw-only time is actively dangerous for a comparison between two *pipeline shapes*
rather than two shaders. A design that replaces one expensive dispatch with nine cheap barrier- and
clear-heavy passes can show a lower total draw time while making the frame slower. The sum of
inclusive per-pass time across the whole chain, boundary to boundary, is what must decide such a
comparison — not the ray dispatch alone.

That case is not hypothetical: it is exactly the sparse/temporal ray-tracing comparison this framework
is being used for, where the requirement is that the sparse path beat the dense one end to end.

### Sketch

```cpp
// executor, per pass
{
    VRF_GPU_ZONE(profiler, cmd, pass.name);          // inclusive
    pass.execute(resources, &renderContext);          // may open its own inner zone(s)
}

// UI
for (const auto& zone : profiler.Results())           // record order, `depth` gives the tree
    DrawRow(zone.depth, zone.name, zone.milliseconds);
```

Watch `maxZonesPerFrame` (default 64) when instrumenting every pass — a real frame graph can exceed
it, and zones past the cap are dropped, which would silently under-report. Size it from the pass count
and surface an overflow rather than truncating quietly.

## Image quality metrics

Performance is only half of a research claim; see `vrf/research/image_metrics.hpp` for RMSE, PSNR,
SSIM and MAE against a reference image. Differing-pixel counts cannot support a "visually lossless"
claim — the threshold is arbitrary and the answer is "did anything change", not "does it look the
same". Compare at **native resolution**: a UI-composited or rescaled capture measures the resampler
alongside the renderer, and that error can exceed the difference under test.
