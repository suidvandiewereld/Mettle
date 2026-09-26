import random
import sys

from gen import TYPES, TNAMES, wrap, lit, tdiv

N = 128
NS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100]


def arr_init(t, seed):
    x = seed & 0xFFFFFFFF
    out = []
    for i in range(N):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(wrap(x >> 3, t) if TYPES[t][0] > 8 else wrap((x >> 7) - 100, t))
    return out


class K:
    def __init__(self, r, idx):
        self.r = r
        self.idx = idx
        self.name = f"k{idx}"
        self.kind = r.choice(["sum", "dot", "mapin", "mapto", "minmax", "count", "find", "prefix",
                              "rev", "fill", "sum2", "down", "cond", "hist", "sumsq", "xorr", "shiftmap"])
        self.t = r.choice(TNAMES)
        self.a = r.choice(TNAMES)
        self.lo = r.choice([0, 0, 0, 1, 2, 3])
        self.c = wrap(r.choice([1, 2, 3, 5, 7, 10, -1, -3, 127, 255, 1000]), self.t)
        self.d = wrap(r.choice([0, 1, -1, 7, 100, -100, 65535]), self.t)
        self.op = r.choice(["+", "-", "*", "^", "&", "|"])
        self.cmp = r.choice(["<", ">", "<=", ">=", "==", "!="])
        self.style = r.choice(["range", "while", "cfor"])

    def loop(self, body, lo=None, hi="n", step=1):
        lo = self.lo if lo is None else lo
        if step != 1 or self.style == "cfor":
            return [f"  for (var i: int64 = (int64){lo}; i < {hi}; i = i + (int64){step}) {{"] + ["    " + b for b in body] + ["  }"]
        if self.style == "range":
            return [f"  for i: int64 in {lo}..{hi} {{"] + ["    " + b for b in body] + ["  }"]
        return [f"  var i: int64 = (int64){lo};", f"  while (i < {hi}) {{"] + ["    " + b for b in body] + ["    i = i + 1;", "  }"]

    def src(self):
        t, a, k = self.t, self.a, self.kind
        L = lit
        if k == "sum":
            sig = f"(p: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop([f"s = s + ({a})p[i];"]) + ["  return s;"]
        elif k == "sumsq":
            sig = f"(p: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop([f"s = s + ({a})p[i] * ({a})p[i];"]) + ["  return s;"]
        elif k == "xorr":
            sig = f"(p: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop([f"s = s ^ ({a})p[i];"]) + ["  return s;"]
        elif k == "dot":
            sig = f"(p: {t}*, q: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop([f"s = s + ({a})p[i] * ({a})q[i];"]) + ["  return s;"]
        elif k == "mapin":
            sig = f"(p: {t}*, n: int64) -> void"
            body = self.loop([f"p[i] = (p[i] {self.op} {L(self.c, t)}) + {L(self.d, t)};"])
        elif k == "shiftmap":
            w = TYPES[t][0]
            self.sh = self.r.randint(0, w - 1)
            self.shop = self.r.choice(["<<", ">>"])
            sig = f"(p: {t}*, n: int64) -> void"
            body = self.loop([f"p[i] = (p[i] {self.shop} {L(self.sh, t)}) {self.op} {L(self.c, t)};"])
        elif k == "mapto":
            sig = f"(dst: {t}*, s: {t}*, n: int64) -> void"
            body = self.loop([f"dst[i] = s[i] {self.op} {L(self.c, t)};"])
        elif k == "minmax":
            sig = f"(p: {t}*, n: int64) -> {t}"
            body = [f"  var m: {t} = p[0];"] + self.loop([f"if (p[i] {self.cmp} m) {{ m = p[i]; }}"]) + ["  return m;"]
        elif k == "count":
            sig = f"(p: {t}*, n: int64, key: {t}) -> int64"
            body = [f"  var c: int64 = 0;"] + self.loop([f"if (p[i] {self.cmp} key) {{ c = c + 1; }}"]) + ["  return c;"]
        elif k == "find":
            sig = f"(p: {t}*, n: int64, key: {t}) -> int64"
            body = self.loop([f"if (p[i] == key) {{ return i; }}"]) + ["  return (int64)(-1);"]
        elif k == "prefix":
            sig = f"(p: {t}*, n: int64) -> void"
            body = self.loop([f"p[i] = p[i] + p[i - 1];"], lo=max(1, self.lo))
        elif k == "rev":
            sig = f"(dst: {t}*, s: {t}*, n: int64) -> void"
            body = self.loop([f"dst[i] = s[n - 1 - i];"])
        elif k == "fill":
            sig = f"(p: {t}*, n: int64) -> void"
            body = self.loop([f"p[i] = {L(self.c, t)};"])
        elif k == "sum2":
            sig = f"(p: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop([f"s = s + ({a})p[i];"], step=2) + ["  return s;"]
        elif k == "down":
            sig = f"(p: {t}*, n: int64) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};", "  var i: int64 = n - 1;", f"  while (i >= (int64){self.lo}) {{",
                    f"    s = s * {L(3, a)} + ({a})p[i];", "    i = i - 1;", "  }", "  return s;"]
        elif k == "cond":
            sig = f"(p: {t}*, n: int64, th: {t}) -> {a}"
            body = [f"  var s: {a} = {L(0, a)};"] + self.loop(
                [f"if (p[i] {self.cmp} th) {{ s = s + ({a})p[i]; }} else {{ s = s - {L(1, a)}; }}"]) + ["  return s;"]
        elif k == "hist":
            sig = f"(p: {t}*, n: int64, h: int32*) -> void"
            body = self.loop([f"h[((int64)p[i]) & 15] = h[((int64)p[i]) & 15] + (int32)1;"])
        self.sig = sig
        deco = self.r.choice(["@noinline ", "@noinline ", "@noinline @simd ", "@noinline @inline "]) if False else "@noinline "
        return f"{deco}fn {self.name}{sig} {{\n" + "\n".join(body) + "\n}"

    def model(self, arrs, off, n, extra):
        t, a, k = self.t, self.a, self.kind
        lo = self.lo
        A = arrs
        p = A["x"]
        if k in ("sum", "sumsq", "xorr", "sum2"):
            s = 0
            step = 2 if k == "sum2" else 1
            for i in range(lo, n, step):
                v = wrap(p[off + i], a)
                s = wrap(s + (v * v if k == "sumsq" else v) if k != "xorr" else s ^ v, a)
            return s
        if k == "dot":
            q = A["y"]
            s = 0
            for i in range(lo, n):
                s = wrap(s + wrap(wrap(p[off + i], a) * wrap(q[off + i], a), a), a)
            return s
        if k == "mapin":
            for i in range(lo, n):
                v = p[off + i]
                v = {"+": v + self.c, "-": v - self.c, "*": v * self.c, "^": v ^ self.c, "&": v & self.c, "|": v | self.c}[self.op]
                p[off + i] = wrap(wrap(v, t) + self.d, t)
            return None
        if k == "shiftmap":
            for i in range(lo, n):
                v = p[off + i]
                v = wrap(v << self.sh, t) if self.shop == "<<" else wrap(v >> self.sh, t)
                v = {"+": v + self.c, "-": v - self.c, "*": v * self.c, "^": v ^ self.c, "&": v & self.c, "|": v | self.c}[self.op]
                p[off + i] = wrap(v, t)
            return None
        if k == "mapto":
            doff = off + extra
            for i in range(lo, n):
                v = p[off + i]
                v = {"+": v + self.c, "-": v - self.c, "*": v * self.c, "^": v ^ self.c, "&": v & self.c, "|": v | self.c}[self.op]
                p[doff + i] = wrap(v, t)
            return None
        if k == "minmax":
            m = p[off]
            for i in range(lo, n):
                if {"<": p[off + i] < m, ">": p[off + i] > m, "<=": p[off + i] <= m, ">=": p[off + i] >= m,
                    "==": p[off + i] == m, "!=": p[off + i] != m}[self.cmp]:
                    m = p[off + i]
            return m
        if k == "count":
            key = extra
            c = 0
            for i in range(lo, n):
                v = p[off + i]
                if {"<": v < key, ">": v > key, "<=": v <= key, ">=": v >= key, "==": v == key, "!=": v != key}[self.cmp]:
                    c += 1
            return c
        if k == "find":
            key = extra
            for i in range(lo, n):
                if p[off + i] == key:
                    return i
            return -1
        if k == "prefix":
            for i in range(max(1, lo), n):
                p[off + i] = wrap(p[off + i] + p[off + i - 1], t)
            return None
        if k == "rev":
            q = A["y"]
            for i in range(lo, n):
                q[off + i] = p[off + n - 1 - i]
            return None
        if k == "fill":
            for i in range(lo, n):
                p[off + i] = self.c
            return None
        if k == "down":
            s = 0
            i = n - 1
            while i >= lo:
                s = wrap(wrap(s * 3, a) + wrap(p[off + i], a), a)
                i -= 1
            return s
        if k == "cond":
            th = extra
            s = 0
            for i in range(lo, n):
                v = p[off + i]
                if {"<": v < th, ">": v > th, "<=": v <= th, ">=": v >= th, "==": v == th, "!=": v != th}[self.cmp]:
                    s = wrap(s + wrap(v, a), a)
                else:
                    s = wrap(s - 1, a)
            return s
        if k == "hist":
            h = A["h"]
            for i in range(lo, n):
                j = wrap(p[off + i], "int64") & 15
                h[j] = wrap(h[j] + 1, "int32")
            return None


def make(seed, opts=None, nk=8, calls=40):
    r = random.Random(seed)
    ks = [K(r, i) for i in range(nk)]
    out = [f"// seed {seed}"]
    for k in ks:
        out.append(k.src())
    main = ["fn main() -> int32 {"]
    types = sorted(set(k.t for k in ks))
    state = {}
    for t in types:
        for nm in ("x", "y"):
            main.append(f"  var {nm}_{t}: {t}[{N + 8}];")
            init = arr_init(t, seed * 31 + hash(nm) % 7 + TNAMES.index(t))
            state[(nm, t)] = init + [0] * 8
            for i, v in enumerate(init):
                main.append(f"  {nm}_{t}[{i}] = {lit(v, t)};")
            for i in range(N, N + 8):
                main.append(f"  {nm}_{t}[{i}] = {lit(0, t)};")
    main.append(f"  var hh: int32[16];")
    main.append(f"  for z: int64 in 0..16 {{ hh[z] = (int32)0; }}")
    hstate = [0] * 16
    code = 0
    for c in range(calls):
        k = r.choice(ks)
        t = k.t
        n = r.choice(NS)
        maxoff = N - n - 4
        if maxoff < 0:
            n = N - 8
            maxoff = 0
        off = r.randint(0, min(maxoff, 6))
        arrs = {"x": state[("x", t)], "y": state[("y", t)], "h": hstate}
        extra = None
        if k.kind in ("count", "find", "cond"):
            pool = arrs["x"][off:off + max(n, 1)]
            extra = r.choice(pool + [0, 1, -1]) if r.random() < 0.8 else r.choice([0, 1, 5])
            extra = wrap(extra, t)
        if k.kind == "mapto":
            extra = r.choice([0, 1, 2, -1]) if off >= 1 else r.choice([0, 1, 2])
            if off + extra < 0:
                extra = 0
        res = k.model(arrs, off, n, extra)
        px = f"&x_{t}[{off}]"
        if k.kind in ("sum", "sumsq", "xorr", "sum2", "down", "minmax"):
            if k.kind == "minmax" and n == 0:
                continue
            call = f"{k.name}({px}, (int64){n})"
            rt = k.a if k.kind != "minmax" else t
        elif k.kind == "dot":
            call = f"{k.name}({px}, &y_{t}[{off}], (int64){n})"
            rt = k.a
        elif k.kind in ("count", "find"):
            call = f"{k.name}({px}, (int64){n}, {lit(extra, t)})"
            rt = "int64"
        elif k.kind == "cond":
            call = f"{k.name}({px}, (int64){n}, {lit(extra, t)})"
            rt = k.a
        elif k.kind == "mapto":
            call = f"{k.name}(&x_{t}[{off + extra}], {px}, (int64){n})"
            rt = None
        elif k.kind == "rev":
            call = f"{k.name}(&y_{t}[{off}], {px}, (int64){n})"
            rt = None
        elif k.kind == "hist":
            call = f"{k.name}({px}, (int64){n}, &hh[0])"
            rt = None
        else:
            call = f"{k.name}({px}, (int64){n})"
            rt = None
        code += 1
        if rt is None:
            main.append(f"  {call};")
        else:
            main.append(f"  if ({call} != {lit(res, rt)}) {{ return {code % 200 + 1}; }}")
    code = 200
    for t in types:
        for nm in ("x", "y"):
            s = 0
            for i, v in enumerate(state[(nm, t)]):
                s = (s * 31 + (v & 0xFFFF)) & 0xFFFFFFFF
            main.append(f"  var cs_{nm}_{t}: uint64 = (uint64)0;")
            main.append(f"  for z: int64 in 0..{N + 8} {{ cs_{nm}_{t} = (cs_{nm}_{t} * (uint64)31 + (((uint64)(int64){nm}_{t}[z]) & (uint64)65535)) & (uint64)4294967295; }}")
            code += 1
            main.append(f"  if (cs_{nm}_{t} != (uint64){s}) {{ return {min(code, 250)}; }}")
    for j in range(16):
        main.append(f"  if (hh[{j}] != (int32){hstate[j]}) {{ return 251; }}")
    main.append("  return 0;")
    main.append("}")
    return "\n".join(out + main) + "\n", None


if __name__ == "__main__":
    sys.stdout.write(make(int(sys.argv[1]))[0])
