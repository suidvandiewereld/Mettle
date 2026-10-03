# Metal backend

Goal: Mettle kernels run on Apple GPUs through Metal, from the same source and
the same device IR that PTX and SPIR-V take. Later stages make a Mettle host
program run on macOS and launch them.

Constraints that shape it:

- No Mac is available yet. Nothing here has run on Apple hardware.
- Apple's Metal compiler may gate emitted MSL the way `ptxas` gates PTX. On
  Windows that is `metal.exe` from Apple's Metal Developer Tools for Windows,
  when installed; the gate skips without it.
- No other external compiler is an oracle. Execution evidence before a Mac
  exists comes from Mettle's own MSL interpreter.

## Stages

1. `--emit-metal`: kernels to Metal Shading Language source.
2. An MSL interpreter that runs emitted kernels on the CPU, and a harness whose
   contracts run against it today and against real Metal on a Mac later.
3. `simdgroup_matrix` for `tensor_mma` and resident accumulators.
4. `aarch64-macos` host: Mach-O, an owned linker, ad-hoc code signing, the
   Darwin arm64 ABI, the runtime on libSystem.
5. `std/gpu` on Metal, so `dispatch` works unchanged on a Mac.

## Why MSL source

MSL is the documented input the Metal runtime compiles
(`newLibraryWithSource`), the way the CUDA driver JITs PTX. AIR and metallib
are undocumented LLVM bitcode containers. MSL it is.

## Control flow

MSL has no `goto` and no labeled break. `src/codegen/gpu_structure.c` turns a
reducible CFG into nested `if`, `while (true)`, `switch (0) { default: }`
blocks, `break` and `continue`, after Ramsey, "Beyond Relooper" (ICFP 2022):
the code for a node sits inside the code of its immediate dominator, a node
with two or more forward in-edges follows a block its predecessors break out
of, and a loop header wraps its subtree in a loop. `switch` is the block
because `continue` passes through it to the enclosing loop. A branch that has
to leave more than one construct sets `mtl_exit` and breaks; a check after
each construct finishes the exit. Tail breaks and continues are dropped and
blocks nobody breaks out of are unwrapped, so structured source comes back
without flags. Every node is emitted once. Irreducible flow is refused.

`tests/gpu_structure_test.c` checks it by execution: random reducible CFGs
from random structured programs with multi-level breaks and jump threading,
walked directly and through the structured tree with the same branch
decisions; the traces must match.

## Values

Every IR name is an MSL local declared at the top of its function. Integers
narrower than 32 bits live in `int`/`uint`, as PTX keeps them in 32-bit
registers; loads extend and stores truncate. `float64` is refused: Apple GPUs
and MSL have no double.

MSL leaves signed overflow undefined, `%` with a negative operand undefined,
and division by zero unspecified. Mettle wraps. So signed `+ - *` compute in
`uint`/`ulong` and reinterpret with `as_type`, and signed `%` is
`a - (a / b) * b`. Shifts already mask the count in MSL, as Mettle documents.

Floats: the module starts with `#pragma METAL fp math_mode(safe)` and
`#pragma METAL fp contract(off)` from MSL 3.2, so the runtime's default fast
math cannot reassociate or fuse. `sqrtf` is `precise::sqrt` (correctly
rounded in MSL), the other math built-ins use `precise::`.

## Pointers

MSL has no generic address space, so every pointer's space is inferred. A
pointer-valued name is `device`, `threadgroup` or `thread` `uchar*`, IR
pointer arithmetic is in bytes already, and each access casts to the accessed
type: `*(device float*)(p)`. Spaces come from kernel parameters (global),
allocations, address-of, casts that name a space, and flow through assignment
and arithmetic. A name that would hold two spaces is refused, naming it. A
device helper taking a plain `T*` is cloned per space signature of its call
sites. Mettle's `constant` is read-only device memory, `const device`.

## Launch ABI

```
struct mtl_args_vadd { device float* a; device float* b; device float* c; int n; };
[[max_total_threads_per_threadgroup(256)]]
kernel void vadd(constant mtl_args_vadd& mtl_args [[buffer(0)]],
                 threadgroup uchar* mtl_arena [[threadgroup(0)]],
                 uint3 mtl_tid [[thread_position_in_threadgroup]], ...)
```

Every kernel parameter sits in one struct at `buffer(0)`, in declaration order
with natural alignment: the same bytes CUDA's parameter buffer holds. Pointers
are 64-bit GPU addresses (`MTLBuffer.gpuAddress + offset`); the host makes
every allocation resident (`useResources` or a heap). Records cross as their
own bytes, `alignas` blobs whose size is checked with `static_assert`. The
dynamic workgroup arena is `threadgroup(0)`, sized by
`setThreadgroupMemoryLength`. `kernel(block = N)` becomes
`[[max_total_threads_per_threadgroup(N)]]`.

## Mapping

| Mettle | MSL |
|---|---|
| `thread.x`, `block.x`, `block_dim.x`, `grid_dim.x` | `thread_position_in_threadgroup`, `threadgroup_position_in_grid`, `threads_per_threadgroup`, `threadgroups_per_grid` |
| `subgroup_local_id()`, `subgroup_size()` | `thread_index_in_simdgroup`, `threads_per_simdgroup` |
| `barrier(workgroup, ...)` | `threadgroup_barrier(mem_flags)` |
| subgroup barrier | `simdgroup_barrier(mem_flags)` |
| broadcast, shuffle, reductions, scans, ballot, any, all | `simd_broadcast`, `simd_shuffle` (inactive source returns the caller's value), `simd_sum/min/max`, `simd_prefix_*_sum`, `simd_ballot`, `simd_any`, `simd_all` |
| u32 atomics | `atomic_*_explicit` on `atomic_uint`; orders through `atomic_thread_fence` (3.2), native orders from 4.1; device-scope ordering marks pointers `coherent(device)` |
| u64 atomics | refused: Metal has only void `atomic_max`/`atomic_min` on `atomic_ulong` |
| async copies | synchronous copies, ordering kept |
| `tensor_mma` f16/bf16/f32 | `simdgroup_matrix` 8x8 tiles (stage 3) |
| inline PTX | refused |

## Versions

`--metal-version=3.1|3.2|4.0|4.1`, default 3.2 (macOS 15, Apple silicon).
3.1 drops the fp pragmas and ordered atomics.
