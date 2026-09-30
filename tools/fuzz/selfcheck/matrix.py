import os
import subprocess
import sys
import concurrent.futures as cf

from gen import TYPES, TNAMES, wrap, lit, tdiv

HERE = os.path.dirname(os.path.abspath(__file__))


def vals(t):
    w, s = TYPES[t]
    base = [0, 1, 2, 3, -1, -2, -3, 5, 7, 10, 100, 127, 128, -128, -127, 255, 256, 1000, 32767, -32768, 65535,
            65536, 2 ** 31 - 1, -2 ** 31, 2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 63 - 1, -2 ** 63, 12345678901, -987654321]
    return sorted(set(wrap(v, t) for v in base))


def fam_divmod():
    cases = []
    for t in TNAMES:
        w, s = TYPES[t]
        ds = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 25, 60, 100, 127, 128, 255, 256, 641, 1000, 4096, 65535,
              65536, 1000000, 2 ** 31 - 1, 2 ** 31, 2 ** 32 - 1, 2 ** 32 + 1, 10 ** 12, 2 ** 62, 2 ** 63 - 1]
        if s:
            ds += [-2, -3, -4, -7, -8, -10, -128, -1000, -(2 ** 31)]
        ds = sorted(set(wrap(d, t) for d in ds) - {0})
        for d in ds:
            for op in ("/", "%"):
                if s and d == -1:
                    continue
                name = f"dm_{t}_{'d' if op == '/' else 'm'}_{str(d).replace('-', 'n')}"
                src = f"@noinline fn {name}(x: {t}) -> {t} {{ return x {op} {lit(d, t)}; }}"
                chk = []
                for x in vals(t):
                    if s and d == -1:
                        continue
                    q = tdiv(x, d)
                    r = wrap(q if op == "/" else x - q * d, t)
                    chk.append(([lit(x, t)], lit(r, t), t))
                cases.append((src, name, chk))
    return cases


def fam_cmp():
    cases = []
    ops = ["<", "<=", ">", ">=", "==", "!="]
    for t in TNAMES:
        for u in TNAMES:
            for op in ops:
                name = f"cc_{t}_{u}_{ops.index(op)}"
                src = f"@noinline fn {name}(x: {t}, y: {u}) -> int32 {{ if (({u})x {op} y) {{ return 1; }} return 0; }}"
                chk = []
                vx = vals(t)[::3]
                vy = [wrap(v, u) for v in [0, 1, -1, 127, 128, -129, 255, 65535, 2 ** 31, -5, 2 ** 63 - 1]]
                for x in vx:
                    for y in sorted(set(vy)):
                        a = wrap(x, u)
                        r = {"<": a < y, "<=": a <= y, ">": a > y, ">=": a >= y, "==": a == y, "!=": a != y}[op]
                        chk.append(([lit(x, t), lit(y, u)], lit(1 if r else 0, "int32"), "int32"))
                cases.append((src, name, chk))
    return cases


def fam_widecmp():
    cases = []
    ops = ["<", "<=", ">", ">=", "==", "!="]
    for t in TNAMES:
        for u in TNAMES:
            for op in ops:
                name = f"wc_{t}_{u}_{ops.index(op)}"
                src = f"@noinline fn {name}(x: {t}, y: {u}) -> int32 {{ if ((int64)x {op} (int64)y) {{ return 1; }} return 0; }}"
                chk = []
                for x in vals(t)[::4]:
                    for y in vals(u)[::4]:
                        a = wrap(x, "int64")
                        b = wrap(y, "int64")
                        r = {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "==": a == b, "!=": a != b}[op]
                        chk.append(([lit(x, t), lit(y, u)], lit(1 if r else 0, "int32"), "int32"))
                cases.append((src, name, chk))
    return cases


def fam_shift():
    cases = []
    for t in TNAMES:
        w, s = TYPES[t]
        for op in ("<<", ">>"):
            for c in sorted(set(list(range(0, w, max(1, w // 16))) + [w - 1])):
                name = f"sh_{t}_{'l' if op == '<<' else 'r'}_{c}"
                src = f"@noinline fn {name}(x: {t}) -> {t} {{ return x {op} {lit(c, t)}; }}"
                chk = []
                for x in vals(t):
                    r = wrap(x << c if op == "<<" else x >> c, t)
                    chk.append(([lit(x, t)], lit(r, t), t))
                cases.append((src, name, chk))
            name = f"shv_{t}_{'l' if op == '<<' else 'r'}"
            src = f"@noinline fn {name}(x: {t}, c: {t}) -> {t} {{ return x {op} (c & {lit(w - 1, t)}); }}"
            chk = []
            for x in vals(t)[::2]:
                for c in [0, 1, w - 1, w // 2, w, w + 1, 2 * w - 1, -1]:
                    cc = wrap(c, t) & (w - 1)
                    r = wrap(x << cc if op == "<<" else x >> cc, t)
                    chk.append(([lit(x, t), lit(c, t)], lit(r, t), t))
            cases.append((src, name, chk))
    return cases


def fam_mulconst():
    cases = []
    for t in TNAMES:
        w, s = TYPES[t]
        for k in [0, 1, 2, 3, 5, 7, 9, 10, 15, 16, 17, 24, 31, 100, 255, 257, 1000, 65537, -1, -2, -3, -9, -16]:
            kk = wrap(k, t)
            name = f"mc_{t}_{str(k).replace('-', 'n')}"
            src = f"@noinline fn {name}(x: {t}) -> {t} {{ return x * {lit(kk, t)}; }}"
            chk = [([lit(x, t)], lit(wrap(x * kk, t), t), t) for x in vals(t)]
            cases.append((src, name, chk))
    return cases


FAMS = {"divmod": fam_divmod, "cmp": fam_cmp, "widecmp": fam_widecmp, "shift": fam_shift, "mulconst": fam_mulconst}


def programs(cases, per=120):
    progs = []
    for i in range(0, len(cases), per):
        chunk = cases[i:i + per]
        out = [c[0] for c in chunk]
        out.append("fn main() -> int32 {")
        n = 0
        for src, name, chk in chunk:
            for args, exp, rt in chk:
                n += 1
                out.append(f"  if ({name}({', '.join(args)}) != {exp}) {{ return {1 + (n % 250)}; }}")
        out.append("  return 0;")
        out.append("}")
        progs.append("\n".join(out) + "\n")
    return progs


MODES = {"d": [], "O": ["-O"], "r": ["--release"], "sr": ["-s", "--release"]}


def run_one(comp, path, mode, env):
    exe = path[:-7] + "_" + mode + ".exe"
    e = dict(os.environ)
    e.update(env)
    p = subprocess.run([comp, "--build"] + MODES[mode] + [path, "-o", exe], capture_output=True, env=e, timeout=600)
    if p.returncode != 0:
        return "bf:" + (p.stdout + p.stderr).decode(errors="replace")[-300:]
    r = subprocess.run([exe], capture_output=True, timeout=60)
    return r.returncode


def main():
    comp = os.path.abspath(sys.argv[1])
    fams = sys.argv[2].split(",")
    modes = sys.argv[3].split(",") if len(sys.argv) > 3 else ["d", "O", "r"]
    env = {}
    for kv in sys.argv[4:]:
        k, v = kv.split("=")
        env[k] = v
    work = os.path.join(HERE, "mwork")
    os.makedirs(work, exist_ok=True)
    jobs = []
    for f in fams:
        for i, src in enumerate(programs(FAMS[f]())):
            path = os.path.join(work, f"{f}_{i}.mettle")
            with open(path, "wb") as fh:
                fh.write(src.encode())
            for m in modes:
                jobs.append((path, m))
    with cf.ThreadPoolExecutor(10) as ex:
        res = list(ex.map(lambda j: (j, run_one(comp, j[0], j[1], env)), jobs))
    bad = 0
    for (path, m), r in res:
        if r != 0:
            bad += 1
            print(os.path.basename(path), m, r)
    print(f"{len(jobs)} runs, {bad} bad")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
