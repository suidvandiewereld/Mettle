import argparse
import concurrent.futures as cf
import importlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MODES = {
    "d": ([], {}),
    "O": (["-O"], {}),
    "r": (["--release"], {}),
    "sr": (["-s", "--release"], {}),
    "d0": ([], {"METTLE_MIR": "0"}),
    "r0": (["--release"], {"METTLE_MIR": "0"}),
    "rv": (["--release"], {"METTLE_REGALLOC_VERIFY": "1", "METTLE_RA_COALESCE_CHECK": "1"}),
    "nossa": (["--release"], {"METTLE_IR_SSA": "0"}),
}


def one(comp, gen, seed, modes, work, opts):
    try:
        src, checks = gen.make(seed, opts)
    except RuntimeError as e:
        return seed, [("gen", str(e))]
    path = os.path.join(work, f"s{seed}.mettle")
    with open(path, "wb") as f:
        f.write(src.encode())
    bad = []
    for m in modes:
        args, envx = MODES[m]
        exe = os.path.join(work, f"s{seed}_{m}.exe")
        env = dict(os.environ)
        env.update(envx)
        try:
            p = subprocess.run([comp, "--build"] + args + [path, "-o", exe], capture_output=True, env=env, timeout=120)
        except subprocess.TimeoutExpired:
            bad.append((m, "compile-timeout"))
            continue
        if p.returncode != 0 or not os.path.exists(exe):
            err = (p.stdout + p.stderr).decode(errors="replace")
            lines = [l for l in err.splitlines() if "error" in l.lower() or "fault" in l.lower() or "assert" in l.lower()]
            bad.append((m, "build-fail " + " | ".join(lines[:3])[:300]))
            continue
        try:
            r = subprocess.run([exe], capture_output=True, timeout=20)
            if r.returncode != 0:
                bad.append((m, f"rc={r.returncode}"))
        except subprocess.TimeoutExpired:
            bad.append((m, "run-timeout"))
        try:
            os.remove(exe)
        except OSError:
            pass
        for ext in (".pdb", ".obj", ".o"):
            try:
                os.remove(exe[:-4] + ext)
            except OSError:
                pass
    if not bad:
        try:
            os.remove(path)
        except OSError:
            pass
    return seed, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default="gen")
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--modes", default="d,O,r,sr,d0,r0")
    ap.add_argument("-j", type=int, default=10)
    ap.add_argument("--comp", default=os.path.join(HERE, "..", "..", "..", "bin", "mettle.exe"))
    ap.add_argument("--work", default=os.path.join(HERE, "work"))
    ap.add_argument("--opt", action="append", default=[])
    a = ap.parse_args()
    a.comp = os.path.abspath(a.comp)
    gen = importlib.import_module(a.gen)
    opts = {}
    for o in a.opt:
        k, v = o.split("=")
        opts[k] = int(v)
    os.makedirs(a.work, exist_ok=True)
    modes = a.modes.split(",")
    nbad = 0
    with cf.ThreadPoolExecutor(a.j) as ex:
        futs = [ex.submit(one, a.comp, gen, s, modes, a.work, opts) for s in range(a.start, a.start + a.count)]
        for f in cf.as_completed(futs):
            seed, bad = f.result()
            if bad:
                nbad += 1
                print(seed, bad, flush=True)
    print(f"done {a.count} seeds, {nbad} with failures", flush=True)


if __name__ == "__main__":
    main()
