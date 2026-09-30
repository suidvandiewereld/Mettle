import math
import struct
import sys
import warnings

import gen as G
from gen import TYPES, TNAMES, wrap, lit, Env, Scope, Return

warnings.filterwarnings("ignore")

FT = ["float32", "float64"]
ALL = TNAMES + FT

FLITS = [0.0, 1.0, -1.0, 0.5, 0.25, 0.125, 2.0, 3.0, 10.0, 100.0, 1000000.0, 0.1, 0.2, 0.3, 1.5e-7,
         1e30, -1e30, 3e38, 1e300, -2.5, 7.0, 65536.0, 4294967296.0, 9007199254740993.0, 1e18, 1.8e19, -0.0]
FDIVS = [2.0, 4.0, 0.5, 3.0, 10.0, -8.0, 0.1, 7.0]


def f32(x):
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def f32_toward_zero(x):
    b = struct.unpack("<I", struct.pack("<f", x))[0]
    if b & 0x7FFFFFFF == 0:
        return x
    return struct.unpack("<f", struct.pack("<I", b - 1))[0]


def i2f32(n):
    if n == 0:
        return 0.0
    s = -1 if n < 0 else 1
    m = abs(n)
    bl = m.bit_length()
    if bl > 24:
        sh = bl - 24
        q = m >> sh
        r = m & ((1 << sh) - 1)
        half = 1 << (sh - 1)
        if r > half or (r == half and (q & 1)):
            q += 1
        m = q << sh
    return f32(float(s * m))


def fround(v, t):
    return f32(v) if t == "float32" else float(v)


def i2f(n, t):
    return i2f32(n) if t == "float32" else float(n)


def bits(v, t):
    if t == "float32":
        return struct.unpack("<I", struct.pack("<f", v))[0]
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def frepr(v):
    if v == 0.0:
        return "-0.0" if math.copysign(1.0, v) < 0 else "0.0"
    r = repr(v)
    if "e" not in r and "." not in r:
        r += ".0"
    return r


def f2i_bounds(it, ft="float64"):
    w, s = TYPES[it]
    if s:
        lo = -float(1 << (w - 1))
        hi = float((1 << (w - 1)) - 1) if w < 64 else 9.2e18
    else:
        lo = 0.0
        hi = float((1 << w) - 1) if w < 64 else 1.8e19
    if ft == "float32":
        h = f32(hi)
        if h > hi:
            h = f32_toward_zero(h)
        hi = h
    return lo, hi


class Gen2(G.Gen):
    def __init__(self, seed, opts=None):
        super().__init__(seed, opts)
        self.f2i_used = set()
        self.globals = {}
        self.f2i_deco = self.r.choice(["", "@noinline ", "@inline "])

    def rtype(self):
        if self.r.random() < 0.3:
            return self.r.choice(FT)
        return self.r.choice(TNAMES)

    def flit(self, t):
        while True:
            v = fround(self.r.choice(FLITS), t)
            if not math.isinf(v):
                return v

    def interesting(self, t):
        if t in FT:
            return self.flit(t)
        return wrap(super().interesting(t), t)

    def expr(self, sc, t, d):
        if t in FT:
            return self.fexpr(sc, t, d)
        if d > 0 and self.r.random() < 0.08:
            ft = self.r.choice(FT)
            self.f2i_used.add((ft, t))
            return ("f2i", t, ft, self.fexpr(sc, ft, d - 1))
        if d > 0 and self.r.random() < 0.05 and self.globals:
            gs = [g for g, gt in self.globals.items() if gt == t]
            if gs:
                return ("var", t, self.r.choice(gs))
        return super().expr(sc, t, d)

    def fexpr(self, sc, t, d):
        vs = [v for v, vt in sc.vars.items() if vt == t]
        if d <= 0 or self.r.random() < 0.2:
            if vs and self.r.random() < 0.75:
                return ("var", t, self.r.choice(vs))
            return ("lit", t, self.flit(t))
        k = self.r.choices(["bin", "divc", "un", "icast", "fcast", "call", "arr", "fld"],
                           [30, 6, 4, 10, 6, 8, 5, 4])[0]
        if k == "bin":
            return ("bin", t, self.r.choice(["+", "-", "*"]), self.fexpr(sc, t, d - 1), self.fexpr(sc, t, d - 1))
        if k == "divc":
            return ("divc", t, "/", self.fexpr(sc, t, d - 1), fround(self.r.choice(FDIVS), t))
        if k == "un":
            return ("un", t, "-", self.fexpr(sc, t, d - 1))
        if k == "icast":
            s = self.r.choice(TNAMES)
            return ("cast", t, s, self.expr(sc, s, d - 1))
        if k == "fcast":
            s = "float32" if t == "float64" else "float64"
            return ("cast", t, s, self.fexpr(sc, s, d - 1))
        if k == "call":
            cands = [f for f in self.funcs if f["ret"] == t and f["idx"] < sc.fidx and not f.get("impure")]
            if cands:
                f = self.r.choice(cands)
                return ("call", t, f["name"], [self.expr(sc, pt, d - 1) for _, pt in f["params"]])
            return self.fexpr(sc, t, d - 1)
        if k == "arr":
            arrs = [a for a, (at, n) in sc.arrs.items() if at == t]
            if arrs:
                a = self.r.choice(arrs)
                return ("arr", t, a, sc.arrs[a][1], self.expr(sc, "int64", d - 1))
            return self.fexpr(sc, t, d - 1)
        if k == "fld":
            cands = [(v, f) for v, st in sc.svars.items() for f, ft in st["fields"] if ft == t]
            if cands:
                v, f = self.r.choice(cands)
                return ("fld", t, v, f)
            return self.fexpr(sc, t, d - 1)

    def cond(self, sc, d):
        if self.r.random() < 0.25:
            t = self.r.choice(FT)
            op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
            return ("cmp", op, self.fexpr(sc, t, max(d - 1, 0)), self.fexpr(sc, t, max(d - 1, 0)))
        return super().cond(sc, d)

    def emit_e(self, e):
        k = e[0]
        t = e[1] if len(e) > 1 else None
        if k == "lit" and t in FT:
            if t == "float32":
                return f"((float32)({frepr(e[2])}))"
            return f"({frepr(e[2])})"
        if k == "divc" and t in FT:
            c = f"((float32)({frepr(e[4])}))" if t == "float32" else f"({frepr(e[4])})"
            return f"({self.emit_e(e[3])} / {c})"
        if k == "f2i":
            return f"f2i_{e[2]}_{t}({self.emit_e(e[3])})"
        return super().emit_e(e)

    def ev(self, e, env):
        k = e[0]
        if k in ("var", "lit"):
            return super().ev(e, env)
        t = e[1]
        if k == "f2i":
            x = self.ev(e[3], env)
            lo, hi = f2i_bounds(t, e[2])
            if x >= lo and x <= hi:
                return wrap(int(x), t)
            return 0
        if t in FT:
            if k == "bin":
                a = self.ev(e[3], env)
                b = self.ev(e[4], env)
                v = {"+": a + b, "-": a - b, "*": a * b}[e[2]]
                return fround(v, t)
            if k == "divc":
                return fround(self.ev(e[3], env) / e[4], t)
            if k == "un":
                return -self.ev(e[3], env)
            if k == "cast":
                v = self.ev(e[3], env)
                if e[2] in FT:
                    return fround(v, t)
                return i2f(v, t)
            if k == "call":
                f = self.fmap[e[2]]
                return self.call(f, [self.ev(a, env) for a in e[3]])
            if k == "arr":
                i = self.ev(e[4], env) & (e[3] - 1)
                return env.get(e[2])[i]
            if k == "fld":
                return env.get(e[2])[e[3]]
            raise RuntimeError("float kind " + k)
        if k == "cast" and e[2] in FT:
            raise RuntimeError("float to int cast outside helper")
        return super().ev(e, env)

    def gen_stmt(self, sc, budget, depth, in_loop):
        r = self.r.random()
        if r < 0.05 and sc.fidx < (1 << 30) and sc.ret:
            return [("eret", self.cond(sc, 1), self.expr(sc, sc.ret, 2))]
        if r < 0.10 and self.globals and sc.impure:
            g = self.r.choice(list(self.globals))
            return [("gasg", g, self.globals[g], self.expr(sc, self.globals[g], 2))]
        if r < 0.16:
            cands = [f for f in self.funcs if f["idx"] < sc.fidx and f.get("impure")]
            mv = [v for v in sc.mutable if v in sc.vars]
            if cands and mv:
                f = self.r.choice(cands)
                tv = [v for v in mv if sc.vars[v] == f["ret"]]
                args = [self.expr(sc, pt, 1) for _, pt in f["params"]]
                if tv:
                    return [("asg", self.r.choice(tv), f["ret"], ("call", f["ret"], f["name"], args))]
        if r < 0.20:
            cands = [f for f in self.funcs if f["idx"] < sc.fidx and f.get("smake")]
            if cands:
                f = self.r.choice(cands)
                v = self.fresh("s")
                st = f["smake"]
                sc.svars[v] = st
                return [("sdecl", v, st), ("smk", v, f["name"], [self.expr(sc, pt, 1) for _, pt in f["params"]])]
        if r < 0.24:
            cands = [f for f in self.funcs if f["idx"] < sc.fidx and f.get("rec")]
            mv = [v for v in sc.mutable if v in sc.vars]
            if cands and mv:
                f = self.r.choice(cands)
                tv = [v for v in mv if sc.vars[v] == f["ret"]]
                if tv:
                    n = ("lit", "int64", self.r.randint(0, 12))
                    return [("asg", self.r.choice(tv), f["ret"], ("call", f["ret"], f["name"], [n, self.expr(sc, f["ret"], 1)]))]
        out = super().gen_stmt(sc, budget, depth, in_loop)
        return out

    def exec_s(self, s, env):
        k = s[0]
        if k == "eret":
            self.steps += 1
            if self.evc(s[1], env):
                raise Return(self.ev(s[2], env))
            return
        if k == "gasg":
            self.steps += 1
            self.genv.set(s[1], self.ev(s[3], env))
            return
        if k == "smk":
            self.steps += 1
            f = self.fmap[s[2]]
            env.set(s[1], self.call(f, [self.ev(a, env) for a in s[3]]))
            return
        return super().exec_s(s, env)

    def call(self, f, args):
        env = Env(self.genv)
        for (pn, pt), a in zip(f["params"], args):
            env.decl(pn, a)
        self.steps += 1
        if self.steps > 300000 or self.depth > 60:
            raise RuntimeError("too many steps")
        self.depth += 1
        try:
            self.exec_block(f["body"], env)
        except Return as r:
            return r.v
        finally:
            self.depth -= 1
        raise RuntimeError("no return")

    def emit_block(self, stmts, ind, out):
        p = "  " * ind
        for s in stmts:
            k = s[0]
            if k == "eret":
                out.append(f"{p}if {self.emit_c(s[1])} {{ return {self.emit_e(s[2])}; }}")
            elif k == "gasg":
                out.append(f"{p}{s[1]} = {self.emit_e(s[3])};")
            elif k == "smk":
                out.append(f"{p}{s[1]} = {s[2]}({', '.join(self.emit_e(a) for a in s[3])});")
            elif k == "sdecl":
                out.append(f"{p}var {s[1]}: {s[2]['name']};")
                for f, ft in s[2]["fields"]:
                    out.append(f"{p}{s[1]}.{f} = {self.emit_e(('lit', ft, 0.0 if ft in FT else 0))};")
            else:
                super().emit_block([s], ind, out)

    def exec_block(self, stmts, env):
        for s in stmts:
            if s[0] == "sdecl":
                self.steps += 1
                env.decl(s[1], {f: (0.0 if ft in FT else 0) for f, ft in s[2]["fields"]})
            else:
                self.exec_s(s, env)

    def gen_struct(self, i):
        n = self.r.randint(1, 5)
        fields = [(f"f{j}", self.rtype()) for j in range(n)]
        return {"name": f"S{i}", "fields": fields}

    def gen_fold(self, st, idx):
        ret = self.rtype()
        e = None
        for fn, ft in st["fields"]:
            if ft in FT and ret not in FT:
                self.f2i_used.add((ft, ret))
                term = ("f2i", ret, ft, ("fld", ft, "s", fn))
            else:
                term = ("cast", ret, ft, ("fld", ft, "s", fn))
            if self.r.random() < 0.5:
                term = ("bin", ret, "*", term, ("lit", ret, fround(3.0, ret) if ret in FT else wrap(self.r.randint(1, 9), ret)))
            e = term if e is None else ("bin", ret, self.r.choice(["+", "-"]) if ret in FT else self.r.choice(["+", "^", "-"]), e, term)
        f = {"name": f"fold{idx}", "idx": -1, "params": [("s", st["name"])], "ret": ret, "ptr": False,
             "body": [("ret", e)], "deco": self.r.choice(["", "@noinline "]), "struct": st}
        st["fold"] = f
        return f

    def gen_func(self, idx, ptr=False):
        kind = self.r.random()
        if kind < 0.12 and self.structs:
            return self.gen_smake(idx)
        if kind < 0.22:
            return self.gen_rec(idx)
        np_ = self.r.choice([0, 1, 2, 3, 4, 5, 6, 8, 10])
        if ptr:
            np_ = max(np_, 1)
        params = [(f"p{j}", self.rtype()) for j in range(np_)]
        ret = params[0][1] if ptr else self.rtype()
        impure = (not ptr) and self.globals and self.r.random() < 0.3
        f = {"name": f"h{idx}", "idx": idx, "params": params, "ret": ret, "ptr": ptr, "impure": impure}
        sc = Scope(None, idx)
        sc.ret = ret
        for pn, pt in params:
            sc.vars[pn] = pt
        body = []
        for _ in range(self.r.randint(0, 3)):
            t = self.rtype()
            v = self.fresh("l")
            body.append(("decl", v, t, self.expr(sc, t, 1)))
            sc.vars[v] = t
            sc.mutable_block.add(v)
        sc.impure = bool(impure)
        body += self.gen_block(sc, self.opts.get("fbudget", 6), 1, False)
        body.append(("ret", self.expr(sc, ret, 3)))
        f["body"] = body
        f["deco"] = self.r.choice(["", "", "@noinline ", "@inline "])
        return f

    def gen_smake(self, idx):
        st = self.r.choice(self.structs)
        np_ = self.r.randint(1, 4)
        params = [(f"p{j}", self.rtype()) for j in range(np_)]
        sc = Scope(None, idx)
        sc.ret = None
        for pn, pt in params:
            sc.vars[pn] = pt
        body = [("sdecl", "r", st)]
        for fn, ft in st["fields"]:
            body.append(("sstore", "r", fn, self.expr(sc, ft, 2)))
        body.append(("ret", ("svar", st["name"], "r")))
        return {"name": f"mk{idx}", "idx": idx, "params": params, "ret": st["name"], "ptr": False,
                "body": body, "deco": self.r.choice(["", "@noinline ", "@inline "]), "smake": st}

    def gen_rec(self, idx):
        t = self.rtype()
        sc = Scope(None, idx)
        sc.vars["x"] = t
        step = self.expr(sc, t, 2)
        body = [("eret", ("cmp", "<=", ("var", "int64", "n"), ("lit", "int64", 0)), ("var", t, "x")),
                ("ret", ("call", t, f"r{idx}", [("bin", "int64", "-", ("var", "int64", "n"), ("lit", "int64", 1)), step]))]
        return {"name": f"r{idx}", "idx": idx, "params": [("n", "int64"), ("x", t)], "ret": t, "ptr": False,
                "body": body, "deco": self.r.choice(["", "@noinline "]), "rec": True}

    def generate(self):
        for i in range(self.r.randint(0, 3)):
            t = self.rtype()
            self.globals[f"g{i}"] = t
        self.ginit = {g: self.interesting(t) for g, t in self.globals.items()}
        self.structs = [self.gen_struct(i) for i in range(self.r.randint(0, 3))]
        folds = [self.gen_fold(st, i) for i, st in enumerate(self.structs)]
        nf = self.r.randint(0, self.opts.get("nfuncs", 7))
        for i in range(nf):
            self.funcs.append(self.gen_func(i, ptr=self.r.random() < 0.15))
        self.fmap = {f["name"]: f for f in self.funcs + folds}
        sc = Scope(None, 1 << 30)
        sc.ret = None
        main = []
        for _ in range(self.r.randint(4, 16)):
            t = self.rtype()
            v = self.fresh("m")
            main.append(("decl", v, t, ("lit", t, self.interesting(t))))
            sc.vars[v] = t
            sc.mutable_block.add(v)
        main += self.gen_block(sc, self.opts.get("mbudget", 14), 0, False)
        self.folds = folds
        self.main = main
        self.sc = sc
        return self.render()

    def render(self):
        self.fmap = {f["name"]: f for f in self.funcs + self.folds}
        self.steps = 0
        self.depth = 0
        self.genv = Env(None)
        for g, v in self.ginit.items():
            self.genv.decl(g, v)
        env = Env(self.genv)
        self.exec_block(self.main, env)
        checks = []
        sc = self.sc
        for v, t in sc.vars.items():
            if v in env.d:
                checks.append((v, t, env.get(v)))
        for a, (t, n) in sc.arrs.items():
            if a in env.d:
                for i, x in enumerate(env.get(a)):
                    checks.append((f"{a}[{i}]", t, x))
        for v, st in sc.svars.items():
            if v in env.d:
                for f, ft in st["fields"]:
                    checks.append((f"{v}.{f}", ft, env.get(v)[f]))
        for g, t in self.globals.items():
            checks.append((g, t, self.genv.get(g)))
        out = [f"// seed {self.seed}"]
        for g, t in self.globals.items():
            out.append(f"var {g}: {t} = {self.emit_e(('lit', t, self.ginit[g]))};")
        out.append("@noinline fn bits_float64(x: float64) -> uint64 { var y: float64 = x; return *((uint64*)&y); }")
        out.append("@noinline fn bits_float32(x: float32) -> uint32 { var y: float32 = x; return *((uint32*)&y); }")
        for ft, it in sorted(self.f2i_used):
            lo, hi = f2i_bounds(it, ft)
            lo_s = frepr(lo) if ft == "float64" else f"((float32)({frepr(lo)}))"
            hi_s = frepr(hi) if ft == "float64" else f"((float32)({frepr(hi)}))"
            out.append(f"{self.f2i_deco}fn f2i_{ft}_{it}(x: {ft}) -> {it} {{ if (x >= {lo_s} && x <= {hi_s}) {{ return ({it})x; }} return ({it})0; }}")
        for st in self.structs:
            out.append(f"struct {st['name']} {{")
            for f, ft in st["fields"]:
                out.append(f"  {f}: {ft};")
            out.append("}")
        for f in self.folds + self.funcs:
            if f.get("ptr"):
                pn0, pt0 = f["params"][0]
                rest = ", ".join(f"{pn}: {pt}" for pn, pt in f["params"][1:])
                out.append(f"{f['deco']}fn {f['name']}({', '.join(f'{pn}: {pt}' for pn, pt in f['params'])}) -> {f['ret']} {{")
                self.emit_block(f["body"], 1, out)
                out.append("}")
                out.append(f"@noinline fn {f['name']}_p(q: {pt0}*{', ' if rest else ''}{rest}) -> void {{")
                args = ", ".join(["*q"] + [pn for pn, _ in f["params"][1:]])
                out.append(f"  *q = {f['name']}({args});")
                out.append("}")
            else:
                out.append(f"{f['deco']}fn {f['name']}({', '.join(f'{pn}: {pt}' for pn, pt in f['params'])}) -> {f['ret']} {{")
                self.emit_block(f["body"], 1, out)
                out.append("}")
        out.append("fn main() -> int32 {")
        self.emit_block(self.main, 1, out)
        for i, (v, t, x) in enumerate(checks):
            code = 1 + (i % 250)
            if t in FT:
                if x != x:
                    out.append(f"  if (!({v} != {v})) {{ return {code}; }}")
                else:
                    out.append(f"  if (bits_{t}({v}) != ({'uint32' if t == 'float32' else 'uint64'}){bits(x, t)}) {{ return {code}; }}")
            else:
                out.append(f"  if ({v} != {lit(x, t)}) {{ return {code}; }}")
        out.append("  return 0;")
        out.append("}")
        return "\n".join(out) + "\n", checks


_orig_ev = G.Gen.ev


def _ev_svar(self, e, env):
    if e[0] == "svar":
        return dict(env.get(e[2]))
    return _orig_ev(self, e, env)


G.Gen.ev = _ev_svar
_orig_emit = G.Gen.emit_e


def _emit_svar(self, e):
    if e[0] == "svar":
        return e[2]
    return _orig_emit(self, e)


G.Gen.emit_e = _emit_svar

_orig_scope_init = Scope.__init__


def _scope_init(self, parent, fidx):
    _orig_scope_init(self, parent, fidx)
    self.ret = parent.ret if parent is not None else None
    self.impure = parent.impure if parent is not None else False


Scope.__init__ = _scope_init


def make(seed, opts=None):
    return Gen2(seed, opts).generate()


if __name__ == "__main__":
    src, checks = make(int(sys.argv[1]))
    sys.stdout.write(src)
