import argparse
import concurrent.futures as cf
import copy
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib
import gen as G
from run import MODES

EXPR_KINDS = {"var", "lit", "bin", "shc", "shv", "divc", "divv", "un", "cast", "call", "arr", "fld", "sel"}


def expr_variants(e):
    k = e[0]
    t = e[1]
    if k != "lit":
        yield ("lit", t, 0)
    if k == "lit" and e[2] not in (0, 1):
        yield ("lit", t, 1)
    for i, x in enumerate(e):
        if isinstance(x, tuple) and x and x[0] in EXPR_KINDS and x[1] == t and k != "lit":
            yield x
    for i, x in enumerate(e):
        if isinstance(x, tuple) and x and x[0] in EXPR_KINDS:
            for v in expr_variants(x):
                yield e[:i] + (v,) + e[i + 1:]
        elif isinstance(x, tuple) and x and x[0] in ("cmp", "land", "not"):
            for v in cond_variants(x):
                yield e[:i] + (v,) + e[i + 1:]
        elif isinstance(x, list):
            for j, a in enumerate(x):
                for v in expr_variants(a):
                    nl = list(x)
                    nl[j] = v
                    yield e[:i] + (nl,) + e[i + 1:]


def cond_variants(c):
    k = c[0]
    if k == "land":
        yield c[2]
        yield c[3]
        for v in cond_variants(c[2]):
            yield (k, c[1], v, c[3])
        for v in cond_variants(c[3]):
            yield (k, c[1], c[2], v)
    elif k == "not":
        yield c[1]
        for v in cond_variants(c[1]):
            yield ("not", v)
    else:
        for v in expr_variants(c[2]):
            yield (k, c[1], v, c[3])
        for v in expr_variants(c[3]):
            yield (k, c[1], c[2], v)


def stmt_variants(s):
    k = s[0]
    if k in ("decl", "asg"):
        for v in expr_variants(s[3]):
            yield s[:3] + (v,)
    elif k == "astore":
        for v in expr_variants(s[3]):
            yield s[:3] + (v, s[4])
        for v in expr_variants(s[4]):
            yield s[:4] + (v,)
    elif k == "sstore":
        for v in expr_variants(s[3]):
            yield s[:3] + (v,)
    elif k == "ret":
        for v in expr_variants(s[1]):
            yield ("ret", v)
    elif k == "if":
        if s[3] is not None:
            yield ("if", s[1], s[2], None)
        for v in cond_variants(s[1]):
            yield ("if", v, s[2], s[3])
        for v in block_variants(s[2]):
            yield ("if", s[1], v, s[3])
        if s[3] is not None:
            for v in block_variants(s[3]):
                yield ("if", s[1], s[2], v)
    elif k in ("for", "while"):
        if s[3] - s[2] > 1:
            yield s[:3] + (s[2] + 1,) + s[4:]
        for v in block_variants(s[4]):
            yield s[:4] + (v,)
    elif k in ("break", "cont"):
        for v in cond_variants(s[1]):
            yield (k, v)
    elif k == "switch":
        if s[4] is not None:
            yield s[:4] + (None,)
        for cv in s[3]:
            d = dict(s[3])
            del d[cv]
            yield s[:3] + (d, s[4])
        for v in expr_variants(s[2]):
            yield s[:2] + (v,) + s[3:]
        for cv in s[3]:
            for v in block_variants(s[3][cv]):
                d = dict(s[3])
                d[cv] = v
                yield s[:3] + (d, s[4])
        if s[4] is not None:
            for v in block_variants(s[4]):
                yield s[:4] + (v,)
    elif k == "pcall":
        for j, a in enumerate(s[3]):
            for v in expr_variants(a):
                nl = list(s[3])
                nl[j] = v
                yield s[:3] + (nl,)


def inline_of(s):
    k = s[0]
    if k == "if":
        yield s[2]
        if s[3] is not None:
            yield s[3]
    elif k == "switch":
        for cv in s[3]:
            yield s[3][cv]
        if s[4] is not None:
            yield s[4]


def block_variants(b, removal_only=False):
    for i in range(len(b)):
        if b[i][0] not in ("ret", "eret"):
            yield b[:i] + b[i + 1:]
    for i in range(len(b)):
        for inner in inline_of(b[i]):
            yield b[:i] + inner + b[i + 1:]
    if removal_only:
        return
    for i in range(len(b)):
        for v in stmt_variants(b[i]):
            yield b[:i] + [v] + b[i + 1:]


def all_variants(g, removal_only):
    for i in range(len(g.funcs)):
        yield ("dropf", i)
    for v in block_variants(g.main, removal_only):
        yield ("main", v)
    for i, f in enumerate(g.funcs):
        for v in block_variants(f["body"], removal_only):
            yield ("func", i, v)
    for i, f in enumerate(g.folds):
        for v in block_variants(f["body"], removal_only):
            yield ("fold", i, v)


def apply(g, var):
    g2 = copy.copy(g)
    g2.funcs = list(g.funcs)
    g2.folds = list(g.folds)
    if var[0] == "dropf":
        del g2.funcs[var[1]]
    elif var[0] == "main":
        g2.main = var[1]
    elif var[0] == "func":
        f = dict(g2.funcs[var[1]])
        f["body"] = var[2]
        g2.funcs[var[1]] = f
    elif var[0] == "fold":
        f = dict(g2.folds[var[1]])
        f["body"] = var[2]
        g2.folds[var[1]] = f
    return g2


class Tester:
    def __init__(self, comp, mode, work):
        self.comp = comp
        self.mode = mode
        self.work = work
        self.n = 0

    def build_run(self, src, mode, tag):
        path = os.path.join(self.work, f"r_{tag}.mettle")
        exe = os.path.join(self.work, f"r_{tag}_{mode}.exe")
        with open(path, "wb") as f:
            f.write(src.encode())
        args, envx = MODES[mode]
        env = dict(os.environ)
        env.update(envx)
        try:
            p = subprocess.run([self.comp, "--build"] + args + [path, "-o", exe], capture_output=True, env=env, timeout=60)
        except subprocess.TimeoutExpired:
            return "cto"
        if p.returncode != 0 or not os.path.exists(exe):
            return "bf"
        try:
            r = subprocess.run([exe], capture_output=True, timeout=10)
            rc = r.returncode
        except subprocess.TimeoutExpired:
            rc = "rto"
        try:
            os.remove(exe)
        except OSError:
            pass
        return rc

    def interesting(self, g, tag):
        try:
            src, checks = g.render()
        except Exception:
            return False
        if not checks:
            return False
        d = self.build_run(src, "d", tag)
        if self.mode == "d":
            return d not in (0, "bf", "cto")
        if d != 0:
            return False
        m = self.build_run(src, self.mode, tag)
        return m not in (0, "bf", "cto")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seed", type=int)
    ap.add_argument("--mode", default="r")
    ap.add_argument("-j", type=int, default=12)
    ap.add_argument("--comp", default=os.path.join(HERE, "..", "..", "..", "bin", "mettle.exe"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--opt", action="append", default=[])
    ap.add_argument("--gen", default="gen")
    a = ap.parse_args()
    GM = importlib.import_module(a.gen)
    a.comp = os.path.abspath(a.comp)
    opts = {}
    for o in a.opt:
        k, v = o.split("=")
        opts[k] = int(v)
    work = os.path.join(HERE, "rwork", a.gen + str(a.seed))
    os.makedirs(work, exist_ok=True)
    g = {"gen2": getattr(GM, "Gen2", None), "gen3": getattr(GM, "Gen3", None), "gen4": getattr(GM, "Gen4", None)}.get(a.gen, G.Gen)(a.seed, opts)
    g.generate()
    t = Tester(a.comp, a.mode, work)
    if not t.interesting(g, "base"):
        print("not interesting at start")
        return
    with cf.ThreadPoolExecutor(a.j) as ex:
        for removal_only in (True, False, True, False):
            progress = True
            while progress:
                progress = False
                cands = list(all_variants(g, removal_only))
                for off in range(0, len(cands), a.j):
                    batch = cands[off:off + a.j]
                    gs = [apply(g, v) for v in batch]
                    res = list(ex.map(lambda x: t.interesting(x[1], f"c{x[0]}"), enumerate(gs)))
                    hit = next((i for i, r in enumerate(res) if r), None)
                    if hit is not None:
                        g = gs[hit]
                        progress = True
                        src, _ = g.render()
                        print(f"size {len(src)}", flush=True)
                        break
    src, _ = g.render()
    out = a.out or os.path.join(HERE, f"red_{a.gen}_{a.seed}.mettle")
    with open(out, "wb") as f:
        f.write(src.encode())
    print(src)


if __name__ == "__main__":
    main()
