# Differential harnesses for the Mettle compiler

Oracles for refactors the test suite cannot see. The suite asserts behaviour;
an emitter, encoder or allocator can be rewritten into something behaviourally
identical but textually different, and the suite stays green. These compare
compiler *output* between two binaries instead.

Established the hard way: a SPIR-V refactor that reordered id allocation
changed every emitted module and the suite still passed 1821/1821.

## Read this first: none of these is an external oracle

Every script here compares **one compiler against another compiler**. That is
structurally blind to a wrong answer that the baseline and every mode agree on.
Reach first for a self-checking oracle: compute the expected answer outside the
compiler (Python, with exact declared-width semantics), bake it into the
generated program as a comparison it must pass, and carry the verdict in the
**exit code** rather than printing it — printing routes the verdict through the
standard library, the linker and the runtime before you see it, and puts all
three in the trust path.

These differentials are the cheap complement. They are not the primary evidence.

**And the index line matters more than the body.** A note that is perfect and
sits four items deep in a list nobody opens has no effect. This project had
written the self-checking-oracle lesson down a week before two agents spent an
afternoon rediscovering it; the body was excellent and its index entry was a
fragment at the end of an unrelated line.

**Write the mechanism, not the conclusion.** A memory written mid-learning
freezes whatever you understood at that moment. "Sweep it with `modediff.ps1`"
and "950 identical is the bar" are conclusions and both turned out false.
"The knobs are env vars and argv cannot reach them" and "every differential
here compares the compiler to itself" are mechanisms, and they stay true as
understanding advances. If you are still learning the thing, say what you do
not yet know.

## The trap that fools you hardest: oracle breadth

**Three independent oracles over one configuration is one oracle.**

A fact-index cache once answered "assigned once" for a symbol assigned eight
times, handing a vectorizer recognizer a false invariant. It survived 788
byte-identical objects, 171 identical `--explain` reports, and identical call
sequences. All three compiled at plain `-O`. The defect lived in the
`no_accum` / `no_hoist` / `no_scan` knob modes, which none of the diffs ran.

Each oracle answered its own question correctly. They were all answering a
narrower question than the one being asked. Agreement between oracles feels
like independent confirmation and is not, when they share an input axis.

The division to hold onto: **the suite covers the knob matrix, the diffs cover
byte-level equivalence at one configuration. They are complements, and a clean
diff does not subsume the suite.** Quote a diff result with its flag matrix
attached, because "951 objects identical" without "at `--release` only"
overstates it. `modediff.ps1` takes extra args precisely so the knob modes can
be swept.

## Three ways an oracle fails you, listed together

They are one family and easier to spot side by side than singly.

1. **It lies.** Everything differs by construction. The PowerShell ErrorRecord
   trap below: two differently-named binaries differ on every line of stderr.
   Loud, and therefore the least dangerous.
2. **It answers a narrower question than you asked.** Three diffs that all ran
   at `-O` while the defect lived in the knob modes. Each answer was correct.
   Agreement between them felt like independent confirmation and was not.
3. **It is disconnected from what it claims to observe.** A staleness check
   whose subsystem has no reachable call site reports green whether the thing
   it guards is sound, unsound, or absent. The quietest of the three, because
   it produces exactly the output you expected and nothing looks wrong.

The limiting case of all three: **a check that can no longer fail is
indistinguishable from a check that passes.** Before trusting a clean run,
confirm the checker still fires against a known-bad state. A checker that has
never failed is not yet evidence.

## The traps that make these lie rather than fail

**1. `cmd /c` around the redirect is load-bearing. Do not "clean it up".**

PowerShell 5.1 wraps a native executable's stderr in ErrorRecords stamped with
the executable's own filename and the invoking script line. Compare two
binaries with different names that way and you get:

    A: mettle-baseline.exe : proven by type
    B: mettle.exe          : proven by type

Every file differs by construction. `--explain` writes to stderr, so this hits
the explain oracle squarely. Letting `cmd` own the redirect
(`cmd /c "exe args > file 2>&1"`) sidesteps it. A real shell redirect under
Bash is fine too.

**2. Compile both binaries to the same `-o` path.**

The compiler prints the output path in its log. Give the two runs different
paths and every file "differs" on the log comparison alone. Write to one path
and copy the artifact aside between runs.

A third, narrower one: `--explain` writes a `.explain.base` sidecar, and the
second run reports "no optimization changes since the last explain build". The
first run is therefore not representative. `explaindiff.ps1` deletes the
sidecar and runs each compiler twice, comparing the second outputs.

And a fourth, specific to stdlib: `build_private_import_name` mangles private
symbols with `fnv1a(module_path)`, so two stdlib trees at different paths
produce different `__import_<hash>_` names for identical code. Put both trees
at the same path, one at a time.

## What is here

| Script | Oracle |
|---|---|
| `objdiff.ps1` | object bytes, `--release` |
| `explaindiff.ps1` | `--explain` text: catches a recognizer that silently stops firing |
| `modediff.ps1` | parameterised by extra args, e.g. `-ExtraArgs @("--emit-spirv") -Tag spirv` |
| `mldiff.ps1` | `--ml-opt` objects plus the `METTLE_ML_ACTIONS` per-instruction decision dump |
| `stdlibdiff2.ps1` | two stdlib trees through one path. **Moves `stdlib/` out of the worktree; never run it while another agent shares the tree** |
| `callseq.py` | static, no build: ordered call sequence of one function before and after a split |
| `scaling.py` | compile-time blowups: eight pathological shapes (else-if chain, one big switch, sequential loops, deep nesting, thousands of live locals, many functions, straight-line code, a big enum) built at n and 2n; fails when time or peak memory grows more than 3x for a doubling. Ratios, not seconds, so runner speed cancels out |
| `twocopies.py` | nondeterminism: one compiler copied to two paths of different length, the second run's environment padded, every test and example compared byte for byte. `--selftest` flips a byte to prove the comparison reads bytes. A heap use-after-free once showed up only this way |

Comment tooling: `comments.py` (string-aware scanner, `--show FILE` lists them),
`stripsrc.py` / `striphdr.py` / `stripmettle.py` (strip with token-stream
verification for C; `.mettle` has no preprocessor so verify those by diffing
compiler output instead).

## callseq.py

    python callseq.py <file> <old-git-ref> <entry-function>

Diffs the ordered call sequence of a function before and after a refactor,
inlining only the helpers the refactor introduced so a split function compares
against the single function it replaced. Runs in a second, needs no build.

Limits, stated plainly: it compares call names and order, not arguments, so a
changed displacement or a swapped register operand is invisible to it. It only
inlines newly-introduced helpers, so moving code into a helper that already
existed reads as a deletion. It goes in front of the build, not instead of the
output diffs.

## A false oracle validates clean on the trivial test

The PowerShell ErrorRecord trap above does not reproduce when both binaries
share a filename. Anyone testing the harness by diffing `bin/mettle.exe`
against a copy of itself in another directory gets a clean run and concludes
the rewrite was safe. It only appears on a real before/after pair, which is
exactly when you are least willing to disbelieve the harness rather than your
own change.

## One more, for anything scripted on Windows

A **quoted** bash heredoc still eats backslashes here. A two-character escape
sequence inside a `<<'EOF'` block reaches the file as the character it denotes,
breaking a string across two lines. Use an editor write for anything containing
escapes rather than a heredoc.

Both agents on this task walked into it despite it being in the project's
institutional memory, and this very paragraph was mangled by it on first
writing.

## Running them

From the repository root. Each takes a `-Prev` binary to compare against, so
copy `bin/mettle.exe` aside before you change anything:

    Copy-Item bin/mettle.exe "$env:TEMP/baseline.exe"
    # ... make your change, rebuild ...
    ./tools/ci/oracles/objdiff.ps1  -Prev "$env:TEMP/baseline.exe"
    ./tools/ci/oracles/knobdiff.ps1 -Prev "$env:TEMP/baseline.exe"

`knobdiff.ps1` is the one to reach for on anything touching the optimizer; it
is the only script here that sweeps the `METTLE_SKIP_PASS` modes.

A harness that moved two stdlib trees through one path used to live here. It is
deliberately absent: it relocated `stdlib/` out of the working tree, which is
unsafe whenever anything else is building. Compare stdlib changes by their
effect on emitted objects instead.

## Shared-tree note

`obj/` and `bin/` are shared mutable state with no lock. Two overlapping
`build.bat` runs produce a `mettle.exe` that is a mixture of both, reproducible
once linked and belonging to no commit. If more than one person or agent is
working the tree, build in a `git worktree` with its own `obj/` and `bin/`, and
print `git status --short && git log --oneline -1` before every build rather
than trusting that the tree is where you left it.
