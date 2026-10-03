# Metal test apparatus

No Mac is attached to this project yet, so the Metal backend's execution
evidence comes from these pieces:

- `msl_interp.c` / `msl_interp.h`: a CPU interpreter for the Metal Shading
  Language dialect `mettle --emit-metal` produces. It is strict: it refuses
  anything outside the dialect and anything MSL leaves undefined.
- `metal_harness.c`: contracts with CPU oracles. Its default backend runs the
  emitted MSL through `msl_interp`; on macOS the same contracts run on the
  GPU through the Metal framework.

- `metal_host_main.mettle` with `metal_host_kernels.mettle`: a Mettle host
  program built with `--gpu-provider=metal`. Linked with
  `src/runtime/metal_runtime.c` and `metal_provider_interp.c`, which runs the
  interpreter in place of a GPU, it checks argument packing, grids, the
  workgroup arena, and graph capture and replay end to end.

`msl_run.c` is the command-line front end: `msl_run --check file.metal`
parses and type-checks every function; `msl_run --selftest` runs the
interpreter's own unit tests.

## The dialect

Everything below is what the emitter writes. The interpreter accepts exactly
this and reports anything else as an error naming the line.

### File level

```
#include <metal_stdlib>
using namespace metal;
#pragma METAL fp math_mode(safe)      // optional; fast/relaxed/safe
#pragma METAL fp contract(off)        // optional; off/on/fast
struct alignas(A) NAME { uchar bytes[N]; };
static_assert(sizeof(NAME) == N, "text");
struct NAME { FIELD-TYPE FIELD; ... };          // kernel argument structs
static RET NAME(PARAMS);                        // prototypes
static RET NAME(PARAMS) { BODY }                // helpers
[[max_total_threads_per_threadgroup(N)]]        // optional, before a kernel
kernel void NAME(PARAMS) { BODY }
```

Line comments `//` may appear and are ignored. Identifiers are C identifiers.

### Types

- Scalars: `bool char uchar short ushort int uint long ulong half bfloat float`.
  `double` never appears and is an error.
- Vectors: `uint3` (built-in index values), `half2`, `float4`, `uint4`,
  `packed_float4`, `packed_uint4`, `ulong2`. Components `.x .y .z .w`.
- Pointers: `SPACE TYPE*` with SPACE one of `device`, `threadgroup`,
  `thread`, `constant`, optionally preceded by `const` and/or
  `coherent(device)` (both are accepted and otherwise ignored), e.g.
  `device uchar*`, `const device float*`, `coherent(device) device uint*`,
  `threadgroup atomic_uint*`. A pointer to a pointer is written
  `device uchar* device*` (the second space is where the pointer itself
  lives).
- `atomic_uint` (only through pointers).
- Record blobs: `struct alignas(A) NAME { uchar bytes[N]; }`, values with
  `sizeof == N`, alignment A. Copy by assignment, by value in calls and
  returns, `= {}` zero-initializes.
- Kernel argument structs: plain C layout (each field at its natural
  alignment; pointers are 8 bytes, 8-aligned; record blobs at their
  `alignas`).
- `simdgroup_float8x8`, `simdgroup_half8x8`, `simdgroup_bfloat8x8`, and
  arrays of them `simdgroup_float8x8 NAME[N];`.
- `simd_vote` (only as the result of `simd_ballot` / `simd_active_threads_mask`,
  immediately cast with `(simd_vote::vote_t)` to a 64-bit unsigned).

### Declarations and statements

- `TYPE NAME = EXPR;`, `TYPE NAME;` (records/matrices/arrays),
  `TYPE NAME = {};` (records),
  `alignas(16) threadgroup TYPE NAME[N];` and `alignas(16) thread TYPE NAME[N];`
  (arrays, kernel scope only for threadgroup).
- `LVALUE = EXPR;` where LVALUE is a name, `*(PTR-TYPE)(EXPR)`, or a matrix
  array element `NAME[INT]`.
- `LVALUE += EXPR;`, `|=`, `NAME++` (prelude functions only).
- `EXPR;` (calls).
- `{ ... }` nested blocks with their own declarations.
- `if (COND) { ... }`, `if (COND) { ... } else { ... }`.
- `while (true) { ... }`, `for (uint NAME = INIT; COND; NAME++) { ... }`.
- `switch (0) { default: { ... } }` (a block `break` leaves).
- `break;` (innermost `while`, `for` or `switch`), `continue;` (innermost
  loop; it passes through `switch`), `return;`, `return EXPR;`.

### Expressions

Literals: integers with optional `u`, `l`, `ul` suffix (`(-5)` and
`(-2147483647 - 1)` for negatives), floats `1.5f`, `3.00000001e+38f`,
`nullptr`, `true`, `false`.

Operators with C precedence: unary `- ~ ! * &`, `* / %`, `+ -`, `<< >>`,
`< <= > >=`, `== !=`, `&`, `^`, `|`, `&&`, `||`, `?:`. Member access `.`,
indexing `[]`, calls. Casts: C style `(TYPE)EXPR`, `as_type<TYPE>(EXPR)`
(bit reinterpretation, sizes must match), `reinterpret_cast<TYPE>(EXPR)`
(pointer <-> 64-bit integer only), `(simd_vote::vote_t)EXPR`. Constructors:
`half2(a, b)`, `float4(v)`, `uint4(v)`, `packed_float4(v)`, `packed_uint4(v)`,
`ulong2(a, b)`. `sizeof(TYPE)` only inside `static_assert`.

### Built-in functions

- Math: `precise::sqrt rsqrt sin cos log exp`, `fast::` forms of the same,
  `fabs`, `fmod`, `fma` (on `float` or `half2`, rounded once).
- Synchronization: `threadgroup_barrier(FLAGS)`, `simdgroup_barrier(FLAGS)`
  with FLAGS `mem_flags::mem_none`, `mem_flags::mem_threadgroup`,
  `mem_flags::mem_device`, or two of them joined by `|`;
  `atomic_thread_fence(FLAGS, ORDER, SCOPE)`.
- SIMD-group: `simd_sum simd_min simd_max simd_prefix_inclusive_sum
  simd_prefix_exclusive_sum simd_broadcast simd_shuffle simd_ballot
  simd_active_threads_mask simd_any simd_all` on `uint` and `float` (lane
  arguments are `ushort`).
- Atomics on `device` or `threadgroup` `atomic_uint*`:
  `atomic_load_explicit`, `atomic_store_explicit`,
  `atomic_fetch_{add,sub,min,max,and,or,xor}_explicit`,
  `atomic_exchange_explicit`, `atomic_compare_exchange_weak_explicit(p,
  thread uint* expected, desired, success_order, failure_order)`. Orders are
  `memory_order_{relaxed,acquire,release,acq_rel,seq_cst}`; scopes
  `thread_scope_{thread,simdgroup,threadgroup,device}`.
- `os_log_default.log(FORMAT, ...)` and `.log_fault(FORMAT, ...)`: printf
  formats with `%d %u %f %x` and friends.
- Matrices: `make_filled_simdgroup_matrix<T, 8, 8>(VALUE)`,
  `simdgroup_load(M, PTR, ulong ELEMENTS_PER_ROW, ulong2 ORIGIN, bool TRANSPOSE)`,
  `simdgroup_store(M, PTR, ulong ELEMENTS_PER_ROW, ulong2 ORIGIN, bool TRANSPOSE)`,
  `simdgroup_multiply_accumulate(D, A, B, C)`. Element (r, c) of a loaded
  matrix is `PTR[(ORIGIN.y + r) * ELEMENTS_PER_ROW + ORIGIN.x + c]`, or with
  TRANSPOSE `PTR[(ORIGIN.y + c) * ELEMENTS_PER_ROW + ORIGIN.x + r]`. A and B
  may be half, bfloat or float matrices and C, D float or half: D = A * B + C,
  each element summed in double and rounded once to D's type.

## Strictness

These are errors, reported with the source line:

- a binary arithmetic or comparison operator whose operands have different
  types (shifts and pointer + integer excepted);
- signed `int`/`long` `+ - *` that overflows; any shift count at or past the
  operand width; `%` with a negative operand; integer division by zero or
  `MIN / -1`; float to integer conversion out of range;
- a memory access outside a buffer, object or array, or not aligned to its
  type, a write through a `constant` pointer, a pointer from one address space
  used as another;
- reading a variable that was never assigned;
- `simd_broadcast` / `simd_shuffle` naming an inactive or out-of-range lane;
- a barrier not reached by every live thread of its group, or threads waiting
  at different barriers.

## Execution model

`msl_dispatch` runs `grid` threadgroups of `block` threads. Threads of a
threadgroup are interleaved deterministically: each runs until it reaches a
barrier, a SIMD-group function or the end. SIMD-groups are 32 consecutive
linear thread indices (x fastest); the last may be partial. A SIMD-group
function executes when every unfinished lane of its SIMD-group waits at the
same call site; those lanes are the active set. A thread that has finished
takes part in no barrier. Device memory is the buffers the harness registers,
addressed by 64-bit GPU addresses; threadgroup memory (static arrays plus the
dynamic arena) is per threadgroup; thread memory is per thread.
