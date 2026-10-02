# Register tiles, numerics contracts, contract-safe tuning

Three features, in order. Each is useful without the next. Every choice below
is measured against `docs/ideology.md`: I.1 (the compiler asserts only what it
proved), I.2 (reuse the interpreter), I.4 (intent is written, proofs are
inferred), VII (refusals).

## A. Register tiles

### Types

A view type whose layout is a fragment layout is a tile when it is a kernel
local:

```mettle
var q: float16[16, 256] layout fragment_a;
var s: float32[16, 32] layout fragment_c = 0.0;
var m: float32[16] layout fragment_c = -1.0e30;
```

- `T[M, K] layout fragment_a` and `T[K, N] layout fragment_b` hold MMA operands
  (`float16`, `bfloat16`). `T[M, N] layout fragment_c` holds an accumulator
  (`float32`, `int32`), and with `int32` or `bool` elements a mask or an index
  tile.
- `T[M] layout fragment_c` is a row vector: one value per row of an M-row
  `fragment_c` tile.
- M is a multiple of 16, N of 8, K of 16.
- A tile is a value a subgroup holds in registers. Which work item holds which
  element is the backend's business, and nothing in the language names a lane,
  a register or a vendor instruction.
- A tile has no address. Taking it, indexing `t[r, c]`, storing a tile in
  memory, a global, a field, an array, a closure, a parameter or a return value
  are refused. A tile exists only in a kernel body.

### Operations

Every tile operation is a subgroup collective. It requires `Warp`, so a tile
operation under a branch the subgroup does not agree on is `F0002`, and every
scalar operand must be the same in every work item of the subgroup, refused
naming the term that varies (`P0001`).

- `var t: T = scalar;` and `t = scalar;` fill. A declaration without an
  initializer fills with zero.
- Element-wise, on `fragment_c` tiles and row vectors: `+ - * /`, comparisons,
  `&& || !`, casts between element types, the GPU math intrinsics (`expf`,
  `sqrtf`, `fabsf`, ...), `select(c, a, b)`, `max(a, b)`, `min(a, b)`.
  Operands are tiles of the same shape and layout, a scalar (broadcast to every
  element), or a row vector with the tile's M (broadcast along each row). `max`
  and `min` are PTX `max.f32`: a NaN operand yields the other, and +0.0 is
  above -0.0, so a tree of them gives the same bits in any order. An operand
  tile (`fragment_a`, `fragment_b`) is filled, copied, loaded or produced by a
  cast; arithmetic on one is refused, because its elements are packed pairs.
- The built-in names (`tile_load`, `row_sum`, `select`, `max`, ...) mean the
  tile operation only when an operand is a tile, so a program's own `row_sum`
  keeps working.
- `tile_row(t)` and `tile_col(t)` are `int32` tiles of `t`'s shape holding each
  element's row and column. They cost no registers.
- `row_max(t)` and `row_sum(t)` reduce a `fragment_c` tile to a row vector. The
  order is part of the operation: the N columns fall into four classes by
  `(column / 2) % 4`; each class is folded left to right from its first column;
  the four results combine as `(C0 + C1) + (C2 + C3)`. Every backend must
  produce this order, and the interpreter computes exactly this order.
- Conversion is a cast. `(float16[16, 32] layout fragment_a)p` converts an f32
  `fragment_c` tile to an f16 A operand with round to nearest even, so a softmax
  result feeds the next MMA from registers. Other layout changes are refused,
  naming both layouts.
- `tensor_mma` takes tiles. A may be a `fragment_a` tile; C and D may be
  `fragment_c` tiles; `c_scale` may be a row vector. B stays in memory: a
  pointer as today, or a shaped view whose layout (`row`, `col`, `swizzle64`,
  `swizzle128`) the MMA honors, so the view's extents and layout replace
  `ldb` and `b_swizzle`. From workgroup memory the PTX backend reads B with
  `ldmatrix` (`.trans` for a `row` B). C may also be the constant `0.0` for a
  tile D: the accumulator starts at +0.0 and nothing is loaded (memory D gets
  the same form with B). The descriptor must agree with the tile types; a
  shape, element or layout mismatch is refused naming both.
- `tile_load(t, src, ld: e, rows: r)` and `tile_store(dst, t, ld: e, rows: r)`
  move a tile to and from memory. `src` and `dst` are pointers or shaped views
  with their layout honored. Rows at or past `r` are not read and load as
  zero, and are not written. An element type that differs from the tile's
  converts as a cast would. A row vector stores element `i` to `dst[i * e]`,
  once per row.
- A shaped view over the dynamic workgroup arena is a cast,
  `(uint16 shared[32, 256] layout swizzle128)(s16 + offset)`, accepted where
  the refinement checker proves the address 16-byte aligned and a row is whole
  16-byte groups. `uint16` elements are read as the MMA's `f16` or `bf16`.

### Registers

The backend states each tile's register cost (PTX: an f32 `fragment_c` M x N
costs M*N/32 registers a work item, an f16 `fragment_a` M x K costs M*K/64, a
row vector M/8). The budget is the work item's register limit: 255 on PTX, or
the multiple of 8 below 65536 / block threads when that is smaller.

Tiles never spill. The compiler takes the peak, over the kernel, of the tile
registers live at once; above the budget the build fails (`G0001`) naming the
tiles live at the peak, each one's cost, and the budget. When `ptxas` is on
`PATH` the build also assembles the kernel and refuses it if ptxas reports any
spill (`G0002`, with ptxas's byte counts). Without ptxas the report says the
residency is unconfirmed. `--explain` lists every tile, its cost, its live
range and the peak.

A run of element-wise operations on one shape, ending at most in one row
reduction, is emitted element by element: every operation of the run for
element 0, then element 1, and so on. A tile made and used only inside the run
is scalarized and costs nothing, which is what lets attention's masks, scaled
scores and exponentials sit beside a 128-register output tile.

### Interpreter

Under `mettle test` every work item holds the whole tile and every operation
computes every element. Because the scalar operands are subgroup-uniform the
copies agree, and the grid runner checks they do, which re-asks the uniformity
proof: the first work item of each subgroup records what each tile operation
wrote and the rest compare. The static uniformity analysis does not follow a
value assigned under a divergent branch, and this check is what catches that
case today. Tile arithmetic is emitted with `.rn`, so ptxas cannot fuse it,
and the interpreter's per-element arithmetic is the device's. The approximate
intrinsics are the exception: `expf` is `ex2.approx` on the device (within 2
ulp), so a CPU-to-GPU comparison through it states its tolerance.

### Refused

A tile outside a kernel body or in any storage; a tile operation in divergent
control; a subgroup-varying scalar operand; a layout, shape or element
mismatch; arithmetic on an operand tile; a layout change other than
`fragment_c` to `fragment_a`; a register peak over the budget; a ptxas
spill; SPIR-V, which has no cooperative-matrix profile here.

## B. Numerics contracts

### Declaration

```mettle
import "std/numerics";

const Q4_0_GEMM: Numerics = Numerics { k_order: k_ascending };

@numerics(Q4_0_GEMM) kernel(block = 256) gemm_q4_0_i8(...) { ... }
@numerics(Q4_0_GEMM) kernel(block = 128) gemm_q4_0_i8_s16(...) { ... }

@numerics(Q4_0_GEMM) fn q4_0_gemm_shapes() {
  var x: int8* = (int8*)numerics_input((int64)m * k);
  ...
  dispatch gemm_q4_0_i8[...](...);
  dispatch gemm_q4_0_i8_s16[...](...);
  numerics_same(out_a, out_b, m, d, d);
}
```

A kernel or device function with `@numerics(C)` is a member. A plain function
with `@numerics(C)` is a harness: ordinary Mettle, like a `@test`, that launches
members the way the host does, at the shapes the contract covers.
`numerics_input(bytes)` returns memory whose every byte is a distinct symbol.
`numerics_same(a, b, rows, cols, ld)` is the claim. `numerics_tensor_map_2d`
describes a tensor map to the interpreter the way `cuTensorMapEncodeTiled`
describes it to the device.

### The proof

The harness runs in the compile-time interpreter, on the optimized IR the PTX
emitter consumes, with every input byte a symbol. Each value is a term; equal
terms share one node. The terms are:

- leaves: input bytes and constants;
- integer operations, exact with wrap;
- f32 and f64 `+ - * / fma sqrt` rounded to nearest, and conversions as casts
  round;
- the approximate intrinsics (`ex2.approx`, `lg2.approx`, ...) as opaque
  functions of their operand;
- data movement (memory, shuffles, async copies, tensor-map loads), exact;
- each hardware MMA k-step as an opaque primitive named by the instruction the
  PTX backend selects for that `tensor_mma` (`m16n8k16.f32.f16.f16.f32` applied
  to a row of A, a column of B and the accumulator element); a block-scaled
  int8 step as the exact integer dot of its block followed by the rounding
  sequence the backend emits.

Each output element's term is its canonical computation: the ordered chain of
rounding operations and where every operand came from. `numerics_same` passes
when the two terms are the same node. Otherwise the build fails (`C0001`) at
the first element that differs, naming the first operation where the chains
part: the operation, its line in each kernel, and the operand that differs.

Only bit-exact identities are applied, each only where its side condition is
proven from facts the checker derives for every term (an interval, whether it
can be -0.0, whether it can be infinite or NaN):

- `x * 1.0 = x`; `x + (-0.0) = x`; `x + 0.0 = x` when x cannot be -0.0;
- `a + b = b + a` and `a * b = b * a` (IEEE addition and multiplication are
  commutative bit for bit);
- `max`/`min` trees in any order; `select(c, x, x) = x`; constant folding;
- byte extraction and concatenation, so a value stored and reloaded in pieces is
  the value.

An identity that cannot be proven is a difference. Every NaN is one value: PTX
leaves the bits of a single-precision NaN unspecified, so the claim is bit
identity with any NaN standing for every NaN.

The contract's `k_order` is checked too: with `k_ascending` every tensor
accumulation must visit K in ascending order, and a split-K member or a
reordered K walk is `C0004` naming the step out of order.

When the checker cannot decide, the build fails (`C0002`) naming the site: a
symbolic value reaching a branch (a branch whose arms only assign values merges
into a select), an address, a loop bound or a launch; an operation with no
semantics here (inline asm, an atomic on symbolic data, an extern call); a read
of a destination an asynchronous copy or tensor-map load has not yet completed;
an interpreter limit. A member no harness launches is `C0003`: unproven is said.

What the proof rests on, printed with every result: the PTX backend emits each
operation as the instruction its term names; ptxas does not change the value of
`.rn` arithmetic; an MMA primitive's result for an element depends only on that
element's row of A, column of B and accumulator, the same on every SM.

Members emit f32 `add`, `sub` and `mul` with `.rn`. The PTX ISA lets ptxas fuse
unrounded `mul`/`add` into FMA, and the chain the checker derived would then not
be the chain that runs.

The claim covers the shapes the harnesses launch, all listed in the report;
every other shape is unproven and the report says so. `--verify`'s CPU
differential stays as independent evidence. The inference repo's runtime checks
stay as tests.

### Admitting the inference kernels

As written the twins differ for some inputs, so the kernels change to be equal
by construction:

- the gemm family's first MMA takes C = `0.0` (it read `zero` at
  `(r % 64) * d + c` in one kernel and `(r % 16) * d + c` in another);
- attention_fa2 and attention_fa2w take C = `0.0` and the same all-zero B for
  their P = 0 start and end updates (fa2 multiplied 0 by staged Q rows of two
  heads, which keeps a -0.0 accumulator where fa2w's zero rows make it +0.0, and
  turns an f16-overflowed Q into NaN in one kernel only);
- each fa2 warp runs exactly the chunks of its own tile's key range, and both
  kernels stage real keys up to the launch's last key before repeating it (fa2
  ran fully masked chunks fa2w never ran, and 0 * V there is not a no-op when V
  is infinite or O is -0.0).

The contract then proves fa2 and fa2w equal at the same launch. Split invariance
at an arbitrary row split stays a runtime test: a tile's masked key positions
read keys that exist in one launch and not in the other, so a proof would need
either a premise that V is finite or staging per tile.

## C. Contract-safe tuning

### Declaration

The candidate space is data, and the kernels come from it by ordinary
metaprogramming:

```mettle
struct ShortTile { m: int32; stages: int32; per: int32; warps: int32; }
const SHORT_TILES: ShortTile[9] = [ ... ];

import "kernels.tune";
comptime for t in SHORT_TILES_TUNED.rows {
  @numerics(Q4_0_GEMM) kernel ident("gemm_q4_0_i8_s", textof(t.m))(...) { ... }
}
```

`kernels.tune.mettle` is checked in. It holds `SHORT_TILES_TUNED`, one chosen
row a key, and the measurements the choice came from, as data. A build reads it
like any import, so the same file always produces the same binary. A missing
tuning file fails the build naming the step that writes it.

### The step

`mettle tune kernels.mettle --space SHORT_TILES --key m` is the only place
tuning happens. Nothing in a normal build measures anything.

1. For each row it builds the module with that row as the choice for its key.
2. Each build runs the module's numerics contracts. A row whose kernel the
   contract does not prove equivalent is refused and reported with the
   divergence, and is never timed.
3. The rest are timed on the device. The tuner runs the module's `@tune`
   function, a host workload in Mettle that dispatches at real sizes, in the
   interpreter with every dispatch sent to the GPU on random data. Rounds are
   interleaved, and the median is taken.
4. It writes `kernels.tune.mettle`: the fastest proven row per key, the shipped
   row if nothing beats it, and every row's timing and verdict.

### Refused

A row the contract does not prove; a tuning step without a GPU; a family no
contract covers, since tuning without a proof would choose between results.
