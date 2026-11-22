# Xbox Series shaders in the AnyPS5 recompiler (experimental fork notes)

Status of the `xbox-series` branch. Everything here comes from one title's `shaders.rpf` (RPF9 container,
26,503 shaders, all DXIL SM 6.6 stubs: 24,804 in the first entry run, 1,699 in a second run starting near file offset 0x10289c85 that was found later; only 4,147 of them were put through the recompiler, see the README). Nothing was executed on hardware: "valid" means `spirv-val` accepts the output.

## Shader format (Xbox side, not handled by AnyPS5)

- Each shader is a DXBC container whose `DXIL` chunk is a stub (bitcode = the 4 bytes `NOBC`).
- The machine code is in an `XDAT` container, section `XBCH` (24-byte header + zlib stream) -> `FSSM` blob.
  Code range = u64 offset at +0xC0, u64 size at +0xC8 of the inflated blob.
- Code is RDNA2 (gfx10.3). 99.89% of all words are opcodes AnyPS5 already decodes; the rest are constant pools.
- 10-dword prologue in front of the code in 15,522 of 24,804 shaders: 4 header dwords, then two 12-byte
  VOP3-encoded words (op 0x180). Strip it (`extract_shaders.py` does) or the decoder reads junk.
- Constant pools / padding after the last `s_endpgm` (and some zero words inside) are NOT instructions;
  the recompiler decodes linearly, so cut the code at the last `s_endpgm`.
- Ray tracing uses the hardware ray accelerator: `image_bvh_intersect_ray` (MIMG op 230) and
  `image_bvh64_intersect_ray` (op 231), NSA form (nsa=3), R128=1. AnyPS5 decodes and emits these.

## Changes on this branch

| File | Change | Why |
|---|---|---|
| `RdnaVectorOpDecoder.cpp` | allow `clamp` on `v_mad_legacy_f32` (VOP3) | Xbox shaders use it; opcode was already supported, only the clamp bit was refused. Clamp semantics not verified. |
| `FloatInstructions.cpp` | `v_mad_legacy_f32` (VOP3 0x140) and `v_mac_legacy_f32` (VOP2 0x06) are fused; `XBOX_NO_FUSED_LEGACY_MAD=1` restores rounding the product first | RDNA2 desktop hardware (RX 6700 XT, gfx1031, measured in boykopovar/AnyPS5#1281) runs 0x140 as `v_fma_legacy_f32`: bit-exact with `v_fma_f32`, with the zero rule and denormal flush of `v_mad_legacy_f32`. The recompiler had the gfx1010 (PS5) rounding. VOP2 0x06 follows the 0x140 measurement and LLVM's gfx10.3 decoding (`v_fmac_legacy_f32`). NaN results are unchanged. **The Series X GPU is not measured.** `tools/xbox/mad_legacy_fused_test.py` |
| `RdnaVectorOpDecoder.cpp` | allow op_sel bits 2-3 on `v_permlane16/x16_b32` | Xbox shaders set op_sel[3]. Assumed to have no effect on a 32-bit op. **Unverified.** |
| `RdnaVectorOpDecoder.cpp` | opcode/modifier numbers in two error messages | diagnostics |
| `RoundingMode.cpp` | ignore `s_setreg*` writes to hwreg 20/21 (`FLAT_SCRATCH_LO/HI`) | Xbox shaders zero these at entry. Ignored, so a shader that really uses scratch via flat_scratch would be wrong. |
| `Recompiler.hpp`, `SpirvModuleEmitter.cpp`, `SpirvFlowEmitter.cpp`, `ShaderInputInfoBuilder.cpp`, ... | legacy (non-NGG) merged ES-GS geometry stage: `GeometryConfiguration`, `ExecutionModelGeometry`, `s_sendmsg` `MSG_GS` `EMIT`/`CUT` and `MSG_GS_DONE` | The title ships 195 such shaders. Each invocation runs single-lane with LDS as `Function` storage (Vulkan has no `Workgroup` storage in geometry shaders). The ESGS/GSVS ring layout and the system SGPR/VGPR assignments are inferred from the shaders, not checked on hardware or against ACO. **Unverified; validity-only (`spirv-val`), never executed.** |
| `Structurizer.cpp` | keep tree post-dominators when some blocks cannot reach the exit; fail early on natural loops with no exit; retry with relaxed return joins only when the first attempt skipped one | 23206/23207 no longer time out; structured CFG is bit-identical on the 903 compute and geometry shaders that finished before |
| `Structurizer.cpp` | fast shared-merge split budget (default 60% of blocks, min 64 splits, `XBOX_SPLIT_PCT`), full 400% cascade as fallback; `XBOX_STRUCT_PROF=1` prints timing and attempts | see below |
| `Recompiler.cpp`, `SrtDescriptorEvaluation.cpp`, `SpirvSpecialization.cpp` | MSVC build fixes (`static constexpr`, no `__builtin_prefetch`, narrowing cast) | upstream builds with MinGW GCC only |

Everything is marked `XBOX-FORK` where it is a behaviour change.

## Results on 300 compute shaders (51 ray tracing + 249 random), wave 32, stubbed bindings

- 182 produce SPIR-V accepted by `spirv-val` (Vulkan 1.3); 0 invalid. (165 on the first pass + 17 of the 24 slow
  ray-tracing shaders once the structurizer fix below let them finish.)
- ~100 fail on image/sampler descriptor resolution: caused by the stub memory, not (known) ISA gaps.
- The 2 ray-tracing shaders that exceeded a 15 minute limit in the structurizer (23206, 23207; 3.7k blocks before structurizing, 27.5k after) now structurize in under a second. When some blocks could not reach the exit, the tree post-dominator pass gave up and the slow fallback ran; it now keeps the tree for the reachable blocks and gives the others the full set. The structurizer also stops early on natural loops with no exit, and only retries with relaxed return joins when the first attempt actually skipped one.
- The valid ray-tracing modules are very large (0.6-1.9M SPIR-V words each).

## Not done / caveats

- Descriptor tables: `tools/xbox/bindings.py` derives a per-shader table layout from the s_load/MIMG/MUBUF instructions (straight-line scan, approximate) and `driver.exe <bin> <wave> <out> <bind>` builds matching fake memory (image dim from the instruction). With this, 172/300 validate at a 30 s limit (+8 slow BVH shaders), 23 gained vs the flat stub, 7 regressed. the bindless (buffer-indexed) cases are handled by the runtime table path, see "Bindless descriptors" below (271/300). Root-signature ABI itself is still not modelled.
- Graphics shaders (PS/VS/GS) not tried.
- No execution or output comparison.

## Bindless descriptors (waterfall loops)

The recompiler already had a runtime descriptor-table path (`TableColumn`: an image/sampler descriptor selected by an
index read from a buffer or pointer). Most Xbox "bindless" shaders did not match it because of how the index reaches the
`s_load`: `v_readfirstlane` inside a waterfall loop (`v_cmpx_eq` narrows exec each iteration), then
`s_load_dwordx8 s[16:23], s[10:11], s16` (heap pointer in a user SGPR, byte offset in an SGPR).

Two changes (marked `XBOX-FORK`):

- `ActiveMaskImplies` now looks through Phis (loop-carried masks): a Phi implies a condition when all incoming values do; a
  path back to a Phi under examination counts as implied. This is sound (the loop only narrows the mask).
- `MatchAffineOffset` takes the "lane was active at the write" arm of a `Select(((exec >> (lane & 31)) & 1) != 0, new, old)`
  under `ReadFirstLane`. **This is an assumption, not a proof**: showing the waterfall mask is a subset of the write-time exec
  needs exec-flow analysis across different exec snapshots. It is the value the shader intends, but a shader that
  deliberately reads a stale lane would be mistranslated. `XBOX_NO_LANE_ASSUME=1` disables it.

- When the heap offset is an opaque wave-uniform value (e.g. a handle-validity test selecting between a loaded index and a
  fallback), an address heap column now uses the descriptor size as record size (32 bytes for an image) with the whole offset
  as key. The emitter already indexes the table by the byte offset, so only the record size was missing. **Assumption**: heap
  entries are size-aligned; an unaligned offset falls outside the table at run time. `XBOX_NO_OPAQUE_HEAP_INDEX=1` disables it.

Result on the 300-shader set with the binding model: 172 -> 200 (Phi + lane-active) -> 271 valid (`spirv-val`), 0 lost,
0 invalid. Remaining: 10 timeouts at the 30 s limit (slow BVH shaders, two more than before because they now get further),
8 `EvaluateRuntimeSources` failures, 4 Phi-chain descriptors, 2 atomic image format 56, 5 single oddities.
Output remains unexecuted; "valid" means only `spirv-val`, and the three assumptions above are untested against hardware.

## Full compute corpus (2,647 shaders, wave 32, 30 s limit, binding model)

`tools/xbox/run_corpus_bind.py` (needs `bindings.py` output for `bins_all`). Two fake-memory models, because the fake memory
decides what the recompiler sees when a shader derives a descriptor from constant-buffer contents:

| model | valid (`spirv-val`) | invalid |
|---|---|---|
| fake buffers unbacked | 2,336 | 0 |
| fake buffers backed by 64 KB of zeros | 2,409 | 0 |
| compiles under either | **2,472 (93.4%)** | 0 |

Each model rejects shaders the other accepts (63 lost / 136 gained going to the backed model): zeros read as a descriptor give
"incompatible image interface" or "unsupported buffer type". So these are properties of the test memory, not the shaders.
Realistic constant-buffer contents are needed to judge shaders that build descriptors from buffer data.
Remaining 175: 74 timeouts at 30 s, 101 failures (image interface/zeros, address-loaded or Phi-chain descriptors,
16 atomic image format 56). Nothing was executed.

Driver fake-buffer contents are selectable (`driver <bin> <wave> <out> <bind> <variant>`): 0 unbacked, 1 zeros, 2 tiled
well-formed 2D image T# records (32 B, format 56), 3 tiled well-formed buffer V# records (16 B). `tools/xbox/run_variants.py`
runs the shaders not yet valid under variants 2 and 3: only 8 more pass (167 still fail, 0 invalid). Union over the four
models: **2,480 of 2,647 (93.7%)**. The remaining failures are structural, not test-memory artefacts: 66 timeouts at 30 s,
descriptors loaded from computed addresses (35) or Phi chains (44), 18 atomic-image format failures.

## Graphics stages (first sample; wave 32, 30 s limit, binding model, fake buffers zero-backed)

`driver <bin> <wave> <out> <bind> 1 <ps|vs|ms> [inputAddr] [interpolators] [fp16mask] [flatmask]`, runners `run_graphics.py`, `run_mesh.py`.
The masks are hex, one bit per interpolant: `fp16mask` sets FP16_INTERP_MODE (bit 19 of the interpolator setting), `flatmask` sets
FLAT_SHADE (bit 10).
**Everything the shader blob does not state is a default** (no register state is parsed; the FSSM header has an attribute table at
+0x130 that is not decoded). Defaults: float mode 0xc0 (fp32 denorm flush, fp16/64 preserve, DX10 clamp; the shader's own
`s_setreg` then ORs in its MODE bits), pixel inputs = perspective center only, export target i -> i, mode 4, no blend/merge state;
interpolant count, FP16 interpolants and flat interpolants (read only with `v_interp_mov_f32` P0, `run_full.py`) are derived from
the `v_interp*` instructions; mesh = triangle list, 128-thread group,
42 primitives / 126 vertices per group, ESGS item size 4. Target capabilities include ClipDistance/CullDistance/MeshShadingEXT.

| stage | total | valid (`spirv-val`) | invalid |
|---|---|---|---|
| **pixel (headline, strict)** | 738 | **375** | 0 |
| pixel, `XBOX_ALLOW_UNMEASURED_F16_INTERP=1` (approximation, off by default) | 738 | 649 | 0 |
| vertex | 114 | 92 | 0 |
| mesh | 640 | 559 | 0 |

The pixel relaxation is an **approximation**: Xbox pixel shaders set fp16/fp64 rounding to round-toward-zero in MODE, and upstream
refuses 16-bit interpolation under non-default rounding ("not measured"). With the opt-in the rounding bits are ignored, so f16
interpolants may differ in the last bit. Remaining failures: pixel 89 (56 atomic-image format, 30 emulated barycentric inputs
over the location limit, 3 descriptor chains); vertex 21 (18 computed `s_swappc`/`s_setpc` targets the recompiler cannot resolve
statically, 2 Phi dominance, 1 sampler chain; 1 timeout); mesh 77 (66 descriptor sources reading through a null pointer, likely
a user-data gap in the harness, 8 sampler chains, 3 Phi dominance; 4 timeouts). None of this was executed, no pipeline state
was checked, and the mesh configuration is a guess, so a mesh shader passing here means only that the code translates.

### FSSM header (partly decoded, `tools/xbox/fssm_header.py`)

Verified against the code: the section directory at 0x120-0x130 and the pixel input table at 0x138 (8-byte entries: semantic,
component mask, index, slot, flags). The table's entry count equals the number of interpolants the code reads in 719 of 738
pixel shaders; the code's `v_interp` attribute number is the entry's position, and flag bit 4 marks 16-bit interpolants
(736/738; bit 3 covers all 738 but also flags some 32-bit ones). Driving the recompiler from the table instead of from the
instructions gives identical results on all 738 pixel shaders (375 valid either way).

Flag bits 0-1 are the interpolation mode: 1 = flat, 2 = interpolated. On all 12,101 carved pixel shaders every entry has exactly
one of them; the 4,295 interpolants the code reads only with `v_interp_mov_f32` P0 all have bit 0, no bit-0 interpolant is read
any other way, and the 49,594 read with `v_interp_p1/p2_f32` or the 16-bit interpolation instructions all have bit 1 (10 shaders
read 12 or 13 interpolants but have no table at 0x138). Until this was decoded the harness passed no FLAT_SHADE bits, so an
interpolant the game declares flat looked like a smooth one read at vertex 0. That is not host interpolation, so the shader fell
back to barycentric emulation, which gives every interpolant three locations; shaders with eleven or more interpolants exceeded
the 32-location limit there and compiled only with `VK_KHR_fragment_shader_barycentric`. `run_full.py` now passes the flat mask,
taken from the code as for the FP16 mask (the two agree on every interpolant the code reads).

Not decoded: the trailing sections (the directory points past the code), the register-like words at 0xd4/0xf8/0xfc/0x110/0x12c,
flag value 10 (0x0a, also 16-bit), the vertex/mesh output tables near 0x1d0. Their code-offset-like fields do not land on instruction
boundaries, so they are not per-instruction annotations. No pipeline state (input enable, export formats, MODE) was found,
so the defaults above for those remain defaults.

## Structurizer speed (ray-tracing loop graphs)

Profiling showed the slow ray-tracing shaders were not hung: `Structurizer` runs a cascade of up to 4 attempts
(8 with `relaxReturnJoins`); on these graphs attempts 1-3 each ran the whole split budget (4x the block count,
~1 block cloned per split, graph grows to 5x) and threw, and the 4th succeeded after ~0.16x splits. Each split
recomputes all analyses.

On a 4,147 shader corpus (all 2,647 compute + 1,500 other) no completed split loop needed more than 0.72x blocks
(0.37x for graphs over ~250 blocks). The cascade now runs with 60% (min 64) first and reruns with the original
400% only if that fails. Measured against the original budget:

- 4,119 shaders finish in both: identical structured-graph hash for all, no success/failure changed;
- 20 shaders that exceeded 90 s now finish (8 still exceed 90 s); slowest finishing shader 69 s -> 2 s;
- total time on shaders finishing in both: 593 s -> 274 s.

A second change targets the large graphs: `joinWithinIteration` and `iterationSelectionMerge` allocated a
count x count bit matrix per call (~24,000 calls on a 1.4k block shader) and converted every set to an id list.
`densePostDominators` runs the same fixpoint over only the blocks that reach the join point (dense indices; the
sets are subsets of that domain, so the greatest fixpoint is identical). Same graph hash on all 4,119 shaders;
a 1,372-block shader went 15.3 s -> 8.1 s.

A per-scan cache of reachability sets was tried and removed: identical output, no measurable speedup.

Remaining cost per large shader is spread out: analysis recompute (~0.8 ms per split at 1.8k blocks), the
`findSelectionMerge` scan over all selection headers (~2 s of 8 s) and `splitOneLoopMerge`.
`tools/xbox/structurizer_corpus.py` + `stages.exe` reproduce the comparison (`stages` prints a graph hash).

## Ray-tracing traversal shaders

64 of the 2,647 compute shaders contain `image_bvh_intersect_ray` / `image_bvh64_intersect_ray` (counted by decoding instructions, not by scanning raw words, which gives false positives). AnyPS5 already emulates that instruction in SPIR-V by reading the BVH nodes from memory (triangle nodes 0/1, box16 type 4, box32 type 5), and its node layouts match what the Xbox builder's rehydration shaders write.

| Result | Shaders |
|---|---|
| valid at the 30 s limit | 55 |
| valid with a 15 min limit (100 to 456 s each) | 7 |
| error: descriptor chain (`CS_RTShadowTrace32HQ_Tiled`) | 1 |
| error: sampler chain (`CS_RTShadowTrace32TemporalHQ_OneOver8RPP`) | 1 |

62 of 64 pass `spirv-val`. The traversal loop has never been run, so this says nothing about correctness. A BVH in the hardware node format has to exist in memory; `XboxBvhBuilder.dll` ships its 52 builder shaders as DXIL only (no RDNA2 code), so they would need a separate DXIL path.

The stored BVH format is described in [BVH_CONTAINER.md](BVH_CONTAINER.md).

## Second pass (fixing the recompiler-side failures)

| Stage | Before | After |
|---|---|---|
| Compute | 2,480 / 2,647 | 2,559 / 2,647 |
| Pixel, strict (default host) | 375 / 738 | 395 / 738 |
| Pixel, strict + `VK_KHR_fragment_shader_barycentric` host | n/a | 410 / 738 |
| Pixel, with host extension and f16 approximation | 649 | 735 |
| Vertex | 92 / 114 | 92 / 114 |
| Mesh | 559 / 640 | 559 / 640 |

All still `spirv-val` only; nothing executed. Details of each change are in the README. Descriptor chains are the main remaining recompiler-side group (about 82 compute): the descriptor index is a value loaded from a buffer at run time and used as a waterfall key, so there is no static table to build; each shape differs. The vertex `s_swappc`/`s_setpc` cases and the mesh null-pointer cases depend on the real vertex fetch shader / user-data layout and are not recompiler problems. (Later fixed for most vertex shaders, see "Vertex fetch subroutine" below.)

## Compile time: structurizer

Profile of the slowest shader (`CS_ScreenProbes_TraceRays…`, 486 KB, 14,346 blocks to 19,831): the structurizer is about 95% of the compile. It does 5,483 shared-merge splits and, after each one, re-scans every selection header from the deepest one (22.9 million header evaluations) and recomputes all analyses.

Changes (all in `ControlFlow/src/Structurizer.cpp`, marked XBOX-FORK):
- The header scan and the loop-merge scan resume at the item that acted last. When a resumed scan finds nothing, both restart from 0 once, so the stop condition is still "no header has an action left". The scan order can differ from before, so the output graph can differ (`XBOX_NO_SPLIT_CURSOR=1` restores the old order).
- `splitOneLoopMerge` skips loops where `splitSharedMergeBlock` cannot act (no predecessor, one predecessor without force, or no predecessor dominated by the header). That part is exact.
- The header order is built from integer keys instead of a comparator that walks the dominator sets; `constructsFollowDominance` is computed lazily.

Result on 281 shaders (the 25 slowest plus 250 random): total structurizer time 1,795 s to 881 s, nothing slower, 238 of 281 graphs identical, one output 3 blocks bigger. The 43 graphs that changed were run end to end through the recompiler: all 43 have the same status (40 valid, 3 identical errors). The slowest shader went from 91 s to 59 s, with an identical graph.

Stable block ids (second step): while splitting shared merges, block ids no longer change; the layout order lives in `ControlFlowGraph::layoutOrder` and is applied once at the end (`XBOX_NO_STABLE_IDS=1` disables it). This removes the per-split renumbering (13 s of the 59 s on the slowest shader). Same 281-shader set: total 881 s to 488 s, 280 of 281 graphs identical to the previous step (the other has the same size and is valid end to end); slowest shader 91 s originally, 41 s now.

What is left: `recomputeAnalyses` (dominators, post-dominators, SCC from scratch after every split) is now about 34 of the 41 s on the slowest shader. Making it incremental is the next step.

## Descriptor Phis (third pass)

Of the 157 compute shaders that threw in the first full run, 139 still failed after the second pass. Two changes in `ResourceTracker.cpp` fix 45 of them, with no regression on 559 re-run shaders (all 157 plus every sixth previously valid one):

- Image stores are split over descriptor Phis the same way reads already were. Each arm gets its own copy of the store, and EXEC is narrowed to the lanes of that arm (`LogicalAnd(exec, arm condition)`), so only one arm writes per lane. 30 shaders; this was the largest remaining single shape (a Phi of two SRT descriptor loads feeding an `image_store`).
- A Phi inside the pointer of an address load (`GetAddressResource` / `LoadAddressU32`) is now collected, so the load is rebuilt per arm. These are the flag-structurizer joins: one path carries the user-data high dword and the other a constant. 15 shaders.

Descriptor-chain failures went from 82 to 36. The others in that group are: Phi chains over ballot/lane masks (4), webs over 512 states (2), loop-carried indices loaded from a buffer (4), and a few single cases. The remaining 50 throws are not matcher problems: the harness's fake T# format is incompatible with the image interface the shader uses.

Not done: a general "heap base + index * stride" lowering to a runtime-sized Vulkan descriptor array kept in sync by the host. The existing table mechanism already does this when the index is a function of register state; Phi splitting extends it to merged heap bases. A buffer-loaded index that varies per loop iteration needs the host side described in the design note and is not covered by validity-only testing.

## Execution harness

Everything above is validity-only: the output passes `spirv-val`. `tools/xbox/exec.cpp` adds real execution. It allocates device-address buffers on the host GPU, builds the SRT and V#/T# descriptors with the real addresses, recompiles the shader against that memory, fills the bindings the recompiler asks for (guest buffers, shader data, flattened SRT, an identity BDA page table, a fault buffer, images, samplers), dispatches, and reads everything back. Test programs are assembled with `rdna_asm.py` and checked against a reference written in Python.

Nine tests pass on an RX 7800 XT (RDNA3 host, wave32):
- store of per-lane values through a V# loaded from the SRT
- `v_readfirstlane` under an exec mask narrower than the one the VGPR was written under (hardware answer: the first active lane's old value)
- a scalarization (waterfall) loop with `s_and_saveexec` / `s_cbranch_execnz` over four distinct per-lane values
- `image_store` through a T# that is loaded on two branches and merged by a Phi, for both selector values (the selected image gets the data, the other stays untouched; with `XBOX_NO_STORE_SPLIT=1` the shader no longer compiles)
- `image_bvh_intersect_ray`: box32 node unsorted and with box sort, ray extent clipping, triangle hit and triangle miss, compared with a slab test and Moller-Trumbore

What this found: the harness's own mistakes (T# format 56 is `8_8_8_8_UNORM`, so stored floats were clamped; the BDA page table must be exactly `header + n * 32` bytes; the `rdna2.py` VOPC table is wrong for the non-cmpx compares), not recompiler bugs. The lane-active assumption did not produce a wrong result in the two shapes tested.

Still open: box16 nodes, 64-bit node pointers, `a16` rays, multi-wave and multi-group dispatch, image formats other than RGBA32F, and any shader that needs image tables. The BVH tests use nodes laid out the way the emulation reads them, so they check the emulation against the ISA's traversal maths, not against the Xbox's own compressed BVH format.

### Real shaders on the GPU

`tools/xbox/run_real.py` builds a scenario from each shader's `.bind` file (SRT nodes with low 32-bit guest addresses behind a BDA page table, a 1 MiB buffer of seeded random data per V#, a 16x16 RGBA32F image per 2D T#, a dummy image or buffer for unpopulated array elements), dispatches 2x2 workgroups and compares runs. There is no reference for what a real shader should compute, so the checks are: did it run, is it deterministic (same build twice), and is the output identical with the structurizer optimisations switched off (`XBOX_NO_STABLE_IDS`, `XBOX_NO_SPLIT_CURSOR`).

The 1,683 compute shaders of 20 KB or less (`docs/xbox/real_run.json`):

| result | shaders |
|---|---|
| ran to completion on the GPU | 961 |
| ... of which changed a buffer or image | 481 |
| harness cannot build the inputs (images other than 2D: 3D, cube, array, 1D; atomic image formats) | 519 |
| recompiler or harness failure | 178 |
| BDA fault (unmapped address from random data used as a pointer) | 25 |

Of the 961 that ran, 925 were deterministic and identical with and without the structurizer optimisations. The other 36 give different bytes between repeated runs of the same build (floating-point atomics whose order varies, and reads of workgroup memory that no thread wrote, because the real workgroup size is unknown and the harness uses one wave). Two of them (23165, 24311) looked like deterministic differences between the two builds, but repeating the same build also changes them by the same amount, so they are flaky, not an equivalence failure. No shader gives a different result because of the structurizer changes.

What this does not show: that the results are right. Random inputs exercise code paths but a shader that runs and writes something has not been checked against the hardware's answer. The workgroup size and the number of user SGPRs are guesses (one wave; 10, or more when the `.bind` file uses a higher register). The 178 failures are mostly shaders whose descriptors sit behind pointers held in data the harness cannot invent (119), the `ImageTableMap` role (31), and the descriptor-chain shapes listed above.

### A CPU reference for small real shaders

`tools/xbox/cpu_ref.py` is a small RDNA2 interpreter (one wave64 workgroup: SALU, the common VALU, SMEM, MUBUF, DS) and `tools/xbox/cpu_compare.py` runs the same scenario through it and through the recompiled SPIR-V on the GPU, then compares every output buffer byte for byte. Anything the interpreter does not implement is reported as a skip, never guessed. Results are in `docs/xbox/cpu_compare.json`.

The 872 compute shaders of 3,000 bytes or less: **183 give byte-identical results** on the CPU and the GPU (105 of them change memory), **none differ**, 687 are skipped (mostly image instructions, D16 buffer ops, VOP3P, `s_wqm`, image classes the harness cannot build, or unmapped addresses reached through random data) and 2 failed to run on the GPU. Each shader runs with all-float inputs, and also with all-integer inputs when it has no float arithmetic, because small integers are denormal floats and the GPU may flush denormals where the reference does not.

Getting there found no recompiler bug, but it found several tooling facts worth keeping:

* `out_disasm/rdna2.py` names VOPC opcodes with the PC gfx10 table. The Xbox shaders use the GCN-style numbering the recompiler uses (0xC2 is `v_cmp_eq_u32`, 0xD2 `v_cmpx_eq_u32`, 0x82/0x92 the i32 forms). The corpus histogram confirms it. `cpu_ref.py` takes the VOPC names from the recompiler's table.
* `rdna2.py` reads a 7-bit MUBUF opcode; the recompiler reads 8 bits (bit 25 is opcode bit 7), so the D16 forms (0x80 to 0x87, half-precision data in packed registers) were shown as their 32-bit twins.
* Its operand text is not faithful for VOP3 (a compare's SGPR destination prints as `v0`, `v_readfirstlane` and `v_readlane` print an SGPR destination as a VGPR). The interpreter decodes operands from the raw words.
* RDNA2 `v_cmpx_*` writes EXEC only (plus the explicit destination of the VOP3 form); the recompiler does the same.
* A buffer load at a byte address that is not 4-byte aligned makes the recompiler read the containing aligned dword. D3D forbids such loads, so this is not treated as a bug, but the reference skips shaders that do it.
* The harness V# now uses OOB_SELECT 1 (only the index is range checked), which suits both structured and raw uses of the same buffer; mode 3 checks a 32-bit byte offset that wraps for huge indices.



### Images in the CPU reference, and a recompiler finding

`cpu_ref.py` now models 2D RGBA32F images (`image_load`, `image_store`, `image_sample_lz`, `image_sample_l`; out-of-range access, a16/d16, other dimensions are skipped). Over the shaders up to 1500 bytes: 223 match byte for byte, 3 mismatch, 2 fail in the harness (descriptor role 10), the rest are skipped for unsupported instructions. The 3 mismatches (22186, 22570, 23442) use LDS and give a different GPU value on every run, so they are flaky shaders (unwritten workgroup memory), not recompiler results.

Finding (fixed): a shader that stores to a texel and loads it back (read-modify-write, e.g. 23448) got stale data. The tracker keeps a read and a write of one descriptor as two resources, so the load was emitted as `OpImageFetch` on the sampled binding while the store went through the storage binding; the two are not coherent. `EmitReadOp` in `SpirvImageEmitter.cpp` now reads through the storage binding when a plain written storage resource of the same source exists (`XBOX_NO_STORAGE_RMW_READ=1` restores the old behaviour). `exec_tests.py::test_image_load_after_store_same_texel` fails without the fix and passes with it.

Reference fixes found on the way: VOP2 madmk/madak/fmamk/fmaak literals, and image texels kept as raw bits (a float round trip quieted signalling NaNs).


### Executing pixel shaders (first run of a graphics stage)

Until now the PS/VS/mesh results were `spirv-val` only. `exec.exe` now has `stage ps`: it recompiles the pixel shader with the same descriptor model as the compute runs, draws one triangle covering an 8x8 target with a host-written vertex shader, and reads back the eight RGBA32F colour attachments (and an optional D32 depth attachment). `tools/xbox/cpu_compare_ps.py` runs the same shader through `cpu_ref.py` and compares every written export channel, the depth export and the output buffers bit for bit. Results: `docs/xbox/cpu_compare_ps.json`.

Setup, shared by both sides (nothing comes from the game): the vertex shader writes a constant vector per interpolant at every vertex, so interpolation is the identity; export format follows the code (compressed exports mean FP16_ABGR, plain ones 32_ABGR); the input VGPR set is the smallest of {persp-center, + pos x/y, + pos z/w} for which the CPU result does not change when the unknown VGPRs are filled with different junk; the lane to pixel layout on the CPU keeps quads together; `VK_KHR_fragment_shader_barycentric` is enabled on the host (the hardware has it). 2D and 2D-array RGBA32F images with a nearest/clamp sampler.

Result for all 738 pixel shaders: **157 give bit-identical results, none differ, 581 are skipped**. Skip reasons: 16-bit interpolants (201), image classes the harness cannot build (1D, 3D, cube: 190), reads through pointers the harness cannot invent (119), out-of-range image access (28), `s_setreg` mode changes (24), gather4 (6), a few single instructions the reference lacks (about 15).

What a MATCH covers and what it does not: arithmetic, exports, kills, depth export, buffer and image loads, quad swizzles, one input vector. The median matching shader is 38 instructions long and only 12 of them sample a texture. It does not cover real interpolation (every attribute is constant), derivatives with varying inputs, blending, 16-bit attributes, or any real game data.

Three harness defaults found while doing this (not recompiler bugs, but they made the earlier `spirv-val`-only numbers say nothing about run time behaviour):

* `driver.cpp` set the export mapping to `i` for target `i`; the recompiler's mapping is a swizzle (0xe4 = identity), and `0` made every colour channel a copy of x.
* The depth export is dropped unless `depthExportEnable` is set. Without it the recompiler emits no `gl_FragDepth` at all, so a depth-only shader renders nothing.
* Per-vertex inputs (`v_interp_mov` P0/P10/P20) need the barycentric extension or the geometry-shader emulation; without either the pixel stage's input does not match a plain vertex shader's output.

Reference fixes found on the way: `ds_swizzle_b32`, `s_wqm`, `v_fract/trunc/rndne_f32`, `v_cvt_f32_f16`, `v_mad/mac_f32`, `v_cmp_class_f32`, output modifiers, float inline constants as SGPR sources, `image_sample` and `_b`/`_d` variants, 2D-array images, the MRTZ export, and EXP semantics (in a compressed export `en` bits 1:0 and 3:2 enable the two pairs, not one pair per bit).


Update after the pixel work: with the extra reference instructions (`s_wqm`, `v_fract`, 2D-array images, `v_mad/mac`, ...) the compute comparison over shaders up to 1500 bytes now gives 242 matches, 6 mismatches and 2 harness failures. The 3 flaky LDS shaders are still there; 23306, 23324 and 23441 are new, because they could not be run before. They have not been investigated (23324 differs by about 14 ulp in one float, which could be an approximate transcendental), so they may be reference or recompiler bugs.


## Whole corpus (26,497 shaders)
`tools/xbox/extract_full.py` (blobs and stage for the main run and the second run), `run_full.py` (recompile + `spirv-val`) and `make_full_results.py` (merge into `docs/xbox/results.json`). Stage is the shader kind in the first dword of the DXIL stub (stub files) or in the DXIL part of the container (second run); kind 6/7 is mesh. Counts: 12,098 PS, 2,233 VS, 2,689 CS, 9,282 mesh, 195 GS (run separately with `run_gs.py`: all 195 pass `spirv-val`, not executed); the two runs together are 26,503 shaders, 6 of the second run do not inflate.

| Stage | Shaders | Valid |
|---|---|---|
| Compute | 2,689 | 2,678 |
| Pixel strict | 12,098 | 12,095 |
| Pixel, with the host barycentric extension | 12,098 | 12,095 |
| Pixel, extension and f16 approximation | 12,098 | 12,095 |
| Vertex | 2,233 | 2,233 |
| Mesh | 9,282 | 9,281 |

The pixel, vertex and mesh runs use the same single-guess harness defaults as the sample (float mode, export mapping, mesh configuration); only the compute numbers include the fake-memory variants (default, unbacked, tiled T#, tiled V#; `run_full.py` tried only three of the four until the 2026-11-22 rerun, which cost 57 compute shaders that validate only unbacked). Nothing from these runs was executed.

Rerun on 2026-11-22 at the head after the second round of contributor merges (#86 to #125, 36 PRs; 92 minutes on 12 workers): compute 2,650 -> 2,678, pixel strict 7,046 -> 12,095 (4,800 shaders that threw on "GetImageResource/GetSamplerResource ... not a valid runtime value" now compile, from the descriptor-chain and structurizer work; #104 removes the need for the barycentric extension and #101, measured on hardware, the need for the f16 opt-in), vertex 1,402 -> 1,445, mesh 9,145 -> 9,240; the 195 geometry shaders all pass `spirv-val`. Five mesh shaders (10264, 10267, 10268, 10451, 10452) went from valid to "GetSamplerResource dword 0 is not a valid runtime value". This is not a regression from a merged PR: #93 made the harness wave size follow the program (64 for every mesh shader in the corpus; it used to be a fixed 32), and these five throw at wave 64 on the commit before the round of merges and compile at wave 32. The FSSM header (word 0xD4, bit 5 clear) says they are wave64, as are all mesh, vertex and geometry shaders, so wave 64 is the right size and this is a real limitation: the sampler descriptor chain (a ReadFirstLane over nested Phi/SelectU32) cannot be resolved under wave64. It is open. `XBOX_WAVE=32` reproduces the old harness. A rerun with the per-shader wave sizes read from the headers (2,157 shaders where the old heuristic disagreed: 319 compute, 937 pixel, 901 vertex) changed no outcome except one compute shader, and that one is not a wave effect: 23210 is wave32 in its header and takes about 99 s alone (the output passes `spirv-val`), against the harness limit of 120 s per driver run, so it validates or times out depending on machine load (12 workers this time), and compute is 2,678 with 7 timeouts in this run. Strict pixel is now as high as the old opt-in number, so the opt-in columns are kept only for comparison. Earlier rerun at `b947aae562` (after the contributor merges, including #25, #30, #39, #57, #75, #78): compute 2,601 -> 2,650, pixel strict 7,037 -> 7,046, pixel with both opt-ins 11,993 -> 12,063, vertex 1,371 -> 1,402, mesh 8,904 -> 9,145; the 195 geometry shaders all pass `spirv-val`. Two compute shaders time out (22618 and 23207) and 23206 takes about 48 s before it throws, so #39's under-a-second claim for 23206 and 23207 does not reproduce with this harness.

Rerun on 2026-11-22 after #138 to #145 (86 minutes on 12 workers, header wave sizes): mesh 9,240 -> 9,281 (#138: equivalent leaves in descriptor webs share a state, and the state and depth limits are separate; this includes the five wave64 shaders 10264, 10267, 10268, 10451, 10452; 650 still throws on sampler-pair heap capacity), vertex 1,445 -> 2,233. The vertex gain is not a recompiler change: the earlier corpus folder had no `vin_full/` tables (the FSSM input tables `extract_full.py` writes since #90), and 788 vertex shaders need them. 785 shaders have a table; the other 3 were already covered by the ESGS export halves. Compute (2,678, 7 timeouts, 23210 borderline as above), pixel and geometry are unchanged. `spirv-val` only, nothing executed.

(Pixel numbers above are after regenerating the new shaders' bindings with the current `bindings.py`; the first full run used an old copy without atomic-image marking, which failed 953 shaders on "atomic image descriptor uses an unsupported format 56".)

## The six compute mismatches, resolved
- **23306, 23441, 22186, 22570, 23442: not comparable.** Eight GPU runs of each give eight different outputs. The CPU reference shows why: each reads LDS dwords that no thread wrote earlier in the run (336 of 504 reads in 23306). On a GPU that memory holds whatever was left there, on the CPU it is zero. The cause is the harness (one wave of 64 threads with thread id = lane, while these shaders index LDS for a different workgroup shape), not the recompiler. `cpu_ref.py` now raises Unsupported on a read of never-written LDS, so such shaders are SKIP instead of MISMATCH. No previously matching shader was lost by this.
- **23324: explained by reciprocal precision.** Deterministic, 11 floats differ by 5 to 14 ulp. The only approximate instructions are three `v_rcp_f32` (hardware: within 1 ulp; reference: exact). `tools/xbox/rcp_envelope.py` reruns the reference 300 times with random +-1 ulp on every reciprocal: the reference results then spread up to 63 ulp, and every GPU value lies inside that envelope. This shows the difference is consistent with rcp rounding, not that the recompiler is exact.

Compute comparison now (shaders up to 1500 bytes): 242 match, 1 mismatch (23324, above), 2 harness failures, 435 skipped.

## Comparison speed
`cpu_compare.py` and `cpu_compare_ps.py` ran the pure-Python interpreter in threads, so a run used one core; a few shaders that loop on junk input ran to the 2,000,000-step limit, and GPU hangs waited 240 s. They now use worker processes, a 100,000-step limit (the largest compared shader needs about 10,000) and a 30 s GPU timeout, and record each shader's time. Compute (680 shaders): about an hour to 56 s with the same outcomes; pixel (738): 9 s, identical outcomes.

## Harness safeguards (before any corpus run)
Three harness bugs moved the published numbers after long runs (an 8-bit image format for atomics, fake user data, bindings from a stale
`bindings.py`). Two checks now run first and stop the run if they fail:
- **Version-stamped bindings.** Every `.bind` starts with `version <hash of bindings.py>`; `harness_guard.py` refuses a directory with any
  file from another generator or without a stamp. When it was introduced, both binding directories were regenerated and compared with the
  old files: identical apart from the stamp (26,497 and 4,147 files), so no other stale bindings had leaked into the results.
- **Canary suite** (`canary.py`, list in `canary.json`, 31 cases, about 1 s): atomic-image compute and pixel shaders, the barycentric
  extension path, the f16 policy (16-bit interpolants must THROW strict and be VALID with the opt-in), plain VS/mesh/pixel/compute and
  second-run shaders, plus byte-exact CPU/GPU MATCHes (compute, compute with images, pixel). `run_full.py` runs the recompile canaries,
  `cpu_compare*.py` the compare canaries; `--no-canary` skips them.
- **Self-test:** with bindings regenerated by the old generator, 4 of the 22 recompile canaries change (all pixel atomic ones, VALID to
  THROW) and the suite exits 1; the guard refuses the old unstamped directory outright. The single compute atomic canary passes either
  way (its atomic does not depend on the format), so the pixel ones carry that check.
- `cpu_compare_ps.py` now uses `exec_ps.exe` by itself (`XBOX_EXEC_PS` overrides) instead of relying on the caller to set `XBOX_EXEC`,
  which silently ran the compute harness when forgotten.
Not covered: a wrong harness default that makes *everything* pass or fail the same way is caught only if it changes a canary; when a
reviewed run changes outcomes on purpose, re-pick with `canary.py --select`.

## Mesh user data above s15 (fourth harness gap)
996 mesh shaders failed with "EvaluateDescriptorSources ... LoadAddressU32(GetAddressResource(Void(0)...))": a descriptor read through a null
pointer. Mesh shaders are merged NGG programs: s0-s7 hold system values (the shader reads the wave index from s3 bits 24-27) and user data
runs from s8 up to s39. The AgcDriver does the same (merged programs get 8 hidden words in front of the user data, `firstUserSgpr` 0).
The harness passed 16 user words and `bindings.py` tracked only s0-s15, so every table reached from s16 or above was null.
- `driver.cpp`: 40 user words for mesh (16 for the other stages, unchanged). Built as `driver_gfx2.exe`; an unmodified rebuild matched
  `driver_fb.exe` on 500 mesh shaders and the canaries, and `driver_gfx2.exe` matches it on 1,100 PS/VS jobs.
- `bindings.py`: tracks s0-s39 for mesh (stage from the manifest). Regenerated: only the mesh bindings changed.
- Result: mesh 8,035 -> 8,904 of 9,282. 969 shaders went from THROW to VALID; 100 went from VALID to THROW (runtime-indexed descriptor
  table, see the README); 0 of the 350 remaining THROWs pass under fake-memory variants 0, 2 or 3.
- Canary: three `ms-hiuser` shaders; with the old 16-word driver all three fail and the suite exits 1.

## Vertex fetch subroutine (fifth harness gap)
Every Xbox vertex shader that reads vertex inputs calls a fetch subroutine that is not in the shader blob. The runtime builds it from
the input layout and passes its address in user data. 630 of the 2,236 carved VS open with `s_swappc_b64 s[4:5], s[4:5]`. That is
the same shape as a PS5 external fetch call. The 158 merged VS do the same call after their wave setup, through s[12:13]. The 1,448
VS without inputs make no call. 1,371 of those were already valid. The recompiler already models an external fetch call: if the
vertex context lists the VGPRs the subroutine fills (`resourcesDst`, `fetchEmbedded`), the call becomes host vertex attribute reads.
The AGC driver's static-ABI path gets the same list from the shader's input semantics. The harness passed an empty vertex context,
and two recompiler rules rejected the Xbox call shape:
- **Input table.** The FSSM header of a VS has a 5-byte-entry table at 0x138 (`fssm_header.vertex_inputs`): input register, read mask,
  first VGPR, highest component read. On all 2,236 VS it lists exactly the ISG1 inputs with a non-zero read mask, in order. The first
  VGPRs are 4, 8, 12 ... for the 630 and 9, 13 ... for the merged ones. In the 630, every VGPR above v3 that the code reads before
  writing it lies in the table. An approximate decoder confirms this for 626; it cannot settle the other 4. `carve_shaders.py` (and
  `extract_full.py`, not run here) writes the table as `vin/<idx>.vin`. `driver.cpp` takes it as the `vs` input layout (argv[7]), and
  `run_full.py` passes `vin_full/<idx>.vin` when it exists. Host input locations are table positions, as in the AGC path. The buffer
  formats are pipeline state and are not modelled: `spirv-val` only, nothing executed.
- **Target written after the call** (`isFetchCall`, GraphBuilder.cpp). PS5 shaders were checked for writes to the target pair across
  the whole program; PS4 shaders only before the call. The Xbox body reloads s4-s7 right after the call, so the target counted as
  computed (407 shaders, "computed/data-dependent s_swappc_b64 call target"). Now only writes before the call count, for every
  guest. A later write cannot change the address the call already read.
- **Link written after the call** (`analyzeSwappcCalls`, `requireReturnAddressesStayInCalls`). Any later touch of a fetch call's link
  pair threw ("escapes the constant-offset call/return model"). Now the link is tracked by the same dataflow as other call links (as
  for PS4): reading the return address after the subroutine returns still throws, overwriting it does not. One shader (113) really
  reads it (`s_or_b64 s[2:3], s[2:3], s[4:5]` while s[4:5] still holds the return address). It stays a failure until the Xbox fetch
  ABI is known (the subroutine may leave a value there). Unit tests: `core/libs/tests/ScalarSwappc.cpp`.

Result (Mac build, own VS runner equal to `run_full.py` graphics, 120 s): VS 1,371 -> 2,000 valid of 2,236 (626 of the 862 upstream
failures, plus 3 carved VS that are not in results.json), 0 valid shaders lost. Still failing: 30 descriptor chains, 5 buffer
slots that the bindings fill with an image T#, 5 timeouts, 113, and the 195 merged VS (158 with inputs, 37 without).

**Merged VS (195): ES halves of the legacy GS pipelines.** All 195 have the same shape. They take the wave mask from s3 like a merged
program. The 158 with inputs then call the fetch subroutine through s[12:13], after `s_and_b32 s13, s13, 0xff` (the same user word
carries other data above bit 7). They write their outputs to LDS (`ds_write*`, no `exp`), run `s_barrier`, and end with one
`s_setpc_b64` to a copy of s[6:7]: a jump into a second program whose address the runtime supplies. The corpus has 195 GS shaders,
compiled as merged ES-GS programs that read the ESGS data from LDS, so these are most likely their ES halves (the pairing is not
established). An experiment that allowed the masked high word and an `s_inst_prefetch` before the call only moved the 158 from the
call error to the tail jump. It was not kept. Translating them needs an ES-half model: the LDS writes become the stage's outputs in
the GS input layout, and the jump ends the stage. Both halves must agree on the ESGS layout, so this is a pipeline-level change,
not a per-shader fix.

## Vertex export halves of legacy GS pipelines (sixth harness gap)
The 195 merged VS above are now compiled as a vertex stage that feeds a geometry stage. 190 write an 8-dword ESGS item and 5 a
4-dword one, at `lane * item bytes` plus a per-wave offset from s3 bits 24-27 (0 in the harness), then run `s_barrier` and
`s_setpc_b64` into the GS half.
- **Request.** A `Vertex` request with a `GeometryConfiguration` in `graphics` is the export half of that geometry pipeline
  (`ExportsToGeometry`, Recompiler.cpp). Its decoded program ends at the barrier before the jump. The barrier only orders the item
  writes before the GS half reads them, and the host pipeline orders the two stages itself.
- **Item writes.** The tessellation lowering already turns a domain program's ESGS writes into `GeometryOutput` attributes when it
  feeds the mesh-path geometry stage. It now does the same for a vertex stage, with `geometryItemDwords` from the configuration's
  `esgsItemSize`, and the same check: an item write must be at `lane * 4 * esgsItemSize + c`. The emitter writes item dword `d` to
  the slot the geometry stage reads it from: with `inputPosition`, dwords 0-3 go to `gl_Position`, and dword `d` >= 4 to component
  `d % 4` of the vec4 output at location `d / 4 - 1`, bit-cast to float (the geometry stage reads `in_gs_param_<n>` the same way).
  So the two halves agree on the ESGS layout without being paired: both follow the configuration's item size and slots.
- **Fetch call.** The 158 with inputs call the fetch subroutine through s[12:13] after `s_inst_prefetch` and
  `s_and_b32 s13, s13, 0xff`: the high user word carries other data above the 40-bit address. `isFetchCall` now accepts
  `s_inst_prefetch` before the call, and a write to the target's high word that only masks it with a constant of 16 bits or less.
- **Harness.** `run_full.esgs_item` recognizes the shape (last instructions `s_barrier`, `s_setpc_b64`) and takes the item size from
  the lane multiplier of the LDS address (`v_mad_u32_u24 v, lane, <bytes>, s`). `driver.cpp` gets it as argument 8 and compiles the
  VS as a merged program with user data from s8 (`userDataBaseRegister` 8, 32 words), so the entry sets s2/s3 as for the other
  merged vertex programs (ES thread count = lanes), and with `inputAttributeCount` = item dwords / 4. `bindings.py` tracks s0-s39
  for these VS (`export_half`); before, the table each of them reaches through s17 had no node.
- Tests: `tools/tests/test_xbox_geometry_export.py` (synthetic programs: the outputs, the masked fetch call, a wrong item stride, and
  the same program without a geometry stage).

Result (on top of the fetch subroutine change, own corpus, 120 s): VS 2,031 -> 2,226 of 2,236, all 195 export halves, and no valid
shader lost. Still failing: 5 buffer slots that the bindings fill with an image T#, 4 sampler chains, and 113. `spirv-val` only:
nothing was executed, and pairing these with the 195 GS shaders for execution needs the pipeline state that says which export half
goes with which GS.

## Corpus run time
A cold mesh run (9,282 shaders, 14 workers) takes about 10 minutes and is bound by total CPU time, not by scheduling: about 8,200
worker-seconds, of which the ~30 shaders that hit the 120 s limit are 44%; the median shader takes 0.16 s (driver start + spirv-val).
Running the slowest first did not help (588 s against 520 s).
`run_full.py` therefore caches results under a hash of every input (the result-deciding functions of the script, driver and spirv-val
builds, shader code, bindings, stage, f16 opt-in, XBOX_* switches); a re-run only recompiles shaders whose inputs changed. Warm mesh
re-run: 5 s, identical outcomes. A cached TIMEOUT is re-run only when `--limit` is raised. Timeouts within a few seconds of the limit
can flip between runs under different CPU load (28 vs 30 in two cold runs).
The remaining lever for cold runs is the recompiler itself: the slow shaders spend their time in the structurizer (recomputeAnalyses).

## DXIL feasibility (Mesa spirv_to_dxil)
`dxil_probe.py` runs our recompiled SPIR-V through Mesa's `spirv2dxil` (built with `-Dspirv-to-dxil=true` plus the microsoft-experimental
Vulkan driver option, which is what turns the real NIR build on; `-m 6 -x 8` = shader model 6.6, validator 1.8) and validates with dxil.dll.
Sample of 220 shaders our recompiler already makes valid SPIR-V for (random, plus an oversample of raw-pointer shaders; mesh is not tried,
spirv_to_dxil has no mesh stage): **151 convert to validator-clean DXIL (69%)**. Of 69 failures, 58 crash the converter and 11 fail DXIL
validation with overlapping resource bindings. Every one of the 58 crashes declares RuntimeDescriptorArray + SampledImageArrayDynamicIndexing
(0 of 151 passing shaders do): bindless image arrays are the blocker, not raw pointers (shaders with raw pointers: 15 of 40 convert).
Sample numbers, not corpus numbers. Per-shader outcomes: docs/xbox/dxil_probe.json.

## Image atomics over descriptor Phis
Reads and stores were split over descriptor Phis; image atomics were not, so an atomic through a Phi-selected T# always failed with
"GetImageResource dword 0 is not a valid runtime value". 32-bit atomics are now split like stores (`SplittableImageRead`,
`ReplaceWithArmCopies` in `ResourceTracker.cpp`): each arm's copy runs with EXEC narrowed to that arm's lanes, so a lane performs its
atomic once, and the returned pre-op value is selected from the lane's arm. `XBOX_NO_ATOMIC_SPLIT=1` turns it off. 64-bit atomics are
not split. Result at 6a9417f8a6 (Mac, `run_full.py` classification): compute 24774 and pixel 19845, 21239, 21396, 21416, 21436, 21450,
21464 THROW -> VALID, no other shader changes. `spirv-val` only, not executed.

## Descriptor webs: equivalent reloads, states and depth
36 mesh and 3 vertex shaders choose a sampler per lane: `ReadFirstLane` of a Phi web whose selects test the lane's bit of an exec
mask or ballot (`SelectU32(ULessThan32(LaneId, 32), low word, high word) >> (LaneId & 31) & 1`). Their descriptor webs gave up at
512 states ("descriptor web: more than 512 descriptor states") while choosing among only 3 to 40 descriptors. The web keyed its
states on the exact IR values of a descriptor tuple, but merged its arms with `EquivalentValue`, so two reloads of the same SRT
dwords made two states. Leaf values are now replaced by an equivalent representative before the state lookup (`CanonicalLeaf` in
`ResourceTracker.cpp`), which leaves about a third of the states (the largest mesh web: 6,481 -> 2,233).
- The state limit is 8,192. The old limit of 512 also bounded the recursion depth of `WebSelector`; with only the state limit
  raised, a linear chain of 2,500 joins overflowed the stack. The depth is now its own limit of 512 joins ("descriptor web: nested
  more than 512 joins deep"). The largest web in the corpus needs 3,966 states at a depth of 68.
- Tests: `tools/tests/test_xbox_descriptor_webs.py` (`ShaderMemory.cpp`'s split-reload program: 260 joins plan their 4 T#s, 600
  joins are refused at the depth limit). `ShaderMemory.cpp` itself now expects the same.

Result (whole corpus at `3dab00a088`, 20 workers, 120 s): mesh 9,245 -> 9,281 of 9,282, vertex 2,233 -> 2,236 of 2,236, no other
change and no valid shader lost; no shader compiles more than twice as slowly. The one compute shader whose failure also
mentions `ReadFirstLane` (24211) fails on an image address chain, a different class. `spirv-val` only, not executed.
