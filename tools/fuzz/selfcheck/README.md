# Self-checking generators

Each generator writes a Mettle program that carries its own expected answer,
computed by a Python model of the language's exact integer and IEEE float
semantics. A nonzero exit in any build mode is a miscompile, and the exit code
names the first wrong value. No second compiler build is needed to say which
side is wrong.

| Generator | What it produces |
| --- | --- |
| `gen.py` | integers of every width, loops, `switch`, arrays, structs by value, pointer helpers |
| `gen2.py` | adds float32/float64, globals written by helpers, early returns, struct makers, recursion |
| `gen3.py` | adds tagged enums with `match`, and `defer` blocks |
| `gen4.py` | adds closures and function pointers |
| `loopk.py` | loop kernels (sums, dots, maps, finds, prefix sums, strided and downward loops) at every element type, offset and trip count |
| `matrix.py` | constant div/mod, multiply, shifts and compares for every integer type |
| `abifuzz.py` | C interop: Mettle calls C, C calls exported Mettle, Mettle calls its own exports; structs and scalars in both directions |

Run a sweep over several build modes:

```
python run.py --gen gen4 --start 1 --count 300 -j 8 --modes d,O,r,sr,s,sfr --verify
python abifuzz.py ../../../bin/mettle.exe 1 200 ",-O,--release"
python matrix.py ../../../bin/mettle.exe divmod,shift,cmp d,O,r
```

Modes: `d` debug, `O`, `r` release, `sr` `-s --release`, `s` `--safe`, `sfr`
`--safe --release`, `rv` release with the register allocation checks,
`nossa` release with `METTLE_IR_SSA=0`. `--verify` sets `METTLE_MIR_VERIFY`,
`METTLE_REGALLOC_VERIFY` and `METTLE_RA_COALESCE_CHECK` for every mode.
`--check-overflow` is not a mode: the generators rely on signed wraparound,
which that flag traps by design.

All three scripts exit nonzero when any program fails, so CI can gate on them.
A seed the generator itself declines is reported and does not count.

Reduce a failing seed (the reducer re-runs the model after every edit, so the
expected values stay right):

```
python reduce.py 120 --gen gen4 --mode O
```

`linered.py <file> <old compiler> <new compiler> <out>` shrinks a program by
lines while the old compiler fails at `-O` and the new one passes.

`METTLE_DUMP_IR_PASSES=<function>` prints that function's IR after every pass
that changed it, which is usually the fastest way from a reduced program to
the pass at fault.
