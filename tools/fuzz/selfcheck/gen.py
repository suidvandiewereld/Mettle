import random
import sys

TYPES = {
    "int8": (8, True), "int16": (16, True), "int32": (32, True), "int64": (64, True),
    "uint8": (8, False), "uint16": (16, False), "uint32": (32, False), "uint64": (64, False),
}
TNAMES = list(TYPES)


def wrap(v, t):
    w, s = TYPES[t]
    v &= (1 << w) - 1
    if s and v >= (1 << (w - 1)):
        v -= 1 << w
    return v


def lit(v, t):
    v = wrap(v, t)
    if v < 0:
        return f"({t})({v})"
    return f"({t}){v}"


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


class Break(Exception):
    pass


class Continue(Exception):
    pass


class Return(Exception):
    def __init__(self, v):
        self.v = v


class Gen:
    def __init__(self, seed, opts=None):
        self.r = random.Random(seed)
        self.seed = seed
        self.opts = opts or {}
        self.funcs = []
        self.structs = []
        self.nvar = 0

    def fresh(self, p):
        self.nvar += 1
        return f"{p}{self.nvar}"

    def interesting(self, t):
        w, s = TYPES[t]
        r = self.r.random()
        if r < 0.3:
            return self.r.randint(-8, 8)
        if r < 0.5:
            lo = -(1 << (w - 1)) if s else 0
            hi = (1 << (w - 1)) - 1 if s else (1 << w) - 1
            return self.r.choice([lo, hi, lo + 1, hi - 1, 0, 1, -1])
        if r < 0.7:
            return self.r.choice([1, 2, 4, 8, 16, 3, 5, 7, 255, 256, 65535, 1 << (w - 1)])
        return self.r.randint(-(1 << (w - 1)), (1 << w) - 1)

    def expr(self, sc, t, d):
        r = self.r.random()
        vs = [v for v, vt in sc.vars.items() if vt == t]
        if d <= 0 or r < 0.2:
            if vs and self.r.random() < 0.75:
                return ("var", t, self.r.choice(vs))
            return ("lit", t, wrap(self.interesting(t), t))
        k = self.r.choices(
            ["bin", "shift", "div", "un", "cast", "call", "arr", "fld", "sel"],
            [30, 8, 7, 5, 10, 8, 6, 4, 0])[0]
        if k == "bin":
            op = self.r.choice(["+", "-", "*", "&", "|", "^", "+", "-"])
            return ("bin", t, op, self.expr(sc, t, d - 1), self.expr(sc, t, d - 1))
        if k == "shift":
            w = TYPES[t][0]
            op = self.r.choice(["<<", ">>"])
            if self.r.random() < 0.5:
                return ("shc", t, op, self.expr(sc, t, d - 1), self.r.randint(0, w - 1))
            return ("shv", t, op, self.expr(sc, t, d - 1), self.expr(sc, t, d - 1))
        if k == "div":
            op = self.r.choice(["/", "%"])
            if self.r.random() < 0.5:
                w, s = TYPES[t]
                cands = [1, 2, 3, 4, 5, 7, 8, 10, 16, 100, 255, 1024, 12345]
                c = self.r.choice(cands)
                if s and self.r.random() < 0.4:
                    c = -c
                    if c == -1:
                        c = -2
                c = wrap(c, t)
                if c == 0 or (s and c == -1):
                    c = 3
                return ("divc", t, op, self.expr(sc, t, d - 1), c)
            return ("divv", t, op, self.expr(sc, t, d - 1), self.expr(sc, t, d - 1))
        if k == "un":
            return ("un", t, self.r.choice(["-", "~"]), self.expr(sc, t, d - 1))
        if k == "cast":
            s = self.r.choice(TNAMES)
            return ("cast", t, s, self.expr(sc, s, d - 1))
        if k == "call":
            cands = [f for f in self.funcs if f["ret"] == t and f["idx"] < sc.fidx and not f.get("impure")]
            if cands:
                f = self.r.choice(cands)
                return ("call", t, f["name"], [self.expr(sc, pt, d - 1) for _, pt in f["params"]])
            return self.expr(sc, t, d - 1)
        if k == "arr":
            arrs = [a for a, (at, n) in sc.arrs.items() if at == t]
            if arrs:
                a = self.r.choice(arrs)
                return ("arr", t, a, sc.arrs[a][1], self.expr(sc, "int64", d - 1))
            return self.expr(sc, t, d - 1)
        if k == "fld":
            cands = [(v, f) for v, st in sc.svars.items() for f, ft in st["fields"] if ft == t]
            if cands:
                v, f = self.r.choice(cands)
                return ("fld", t, v, f)
            return self.expr(sc, t, d - 1)
        if k == "sel":
            return ("sel", t, self.cond(sc, d - 1), self.expr(sc, t, d - 1), self.expr(sc, t, d - 1))

    def cond(self, sc, d):
        r = self.r.random()
        if d > 0 and r < 0.2:
            op = self.r.choice(["&&", "||"])
            return ("land", op, self.cond(sc, d - 1), self.cond(sc, d - 1))
        if d > 0 and r < 0.25:
            return ("not", self.cond(sc, d - 1))
        t = self.r.choice(TNAMES)
        op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
        return ("cmp", op, self.expr(sc, t, max(d - 1, 0)), self.expr(sc, t, max(d - 1, 0)))

    def emit_e(self, e):
        k = e[0]
        if k == "var":
            return e[2]
        if k == "lit":
            return lit(e[2], e[1])
        t = e[1]
        if k == "bin":
            return f"({self.emit_e(e[3])} {e[2]} {self.emit_e(e[4])})"
        if k == "shc":
            return f"({self.emit_e(e[3])} {e[2]} {lit(e[4], t)})"
        if k == "shv":
            w = TYPES[t][0]
            return f"({self.emit_e(e[3])} {e[2]} ({self.emit_e(e[4])} & {lit(w - 1, t)}))"
        if k == "divc":
            return f"({self.emit_e(e[3])} {e[2]} {lit(e[4], t)})"
        if k == "divv":
            return f"({self.emit_e(e[3])} {e[2]} (({self.emit_e(e[4])} & {lit(63, t)}) | {lit(1, t)}))"
        if k == "un":
            return f"({e[2]}{self.emit_e(e[3])})"
        if k == "cast":
            return f"(({t})({self.emit_e(e[3])}))"
        if k == "call":
            return f"{e[2]}({', '.join(self.emit_e(a) for a in e[3])})"
        if k == "arr":
            return f"{e[2]}[({self.emit_e(e[4])}) & {e[3] - 1}]"
        if k == "fld":
            return f"{e[2]}.{e[3]}"
        if k == "sel":
            return f"if {self.emit_c(e[2])} {{ {self.emit_e(e[3])} }} else {{ {self.emit_e(e[4])} }}"

    def emit_c(self, c):
        k = c[0]
        if k == "land":
            return f"({self.emit_c(c[2])} {c[1]} {self.emit_c(c[3])})"
        if k == "not":
            return f"(!{self.emit_c(c[1])})"
        return f"({self.emit_e(c[2])} {c[1]} {self.emit_e(c[3])})"

    def ev(self, e, env):
        k = e[0]
        if k == "var":
            return env.get(e[2])
        if k == "lit":
            return e[2]
        t = e[1]
        w, s = TYPES[t]
        if k == "bin":
            a = self.ev(e[3], env)
            b = self.ev(e[4], env)
            op = e[2]
            v = {"+": a + b, "-": a - b, "*": a * b, "&": a & b, "|": a | b, "^": a ^ b}[op]
            return wrap(v, t)
        if k in ("shc", "shv"):
            a = self.ev(e[3], env)
            c = e[4] if k == "shc" else (self.ev(e[4], env) & (w - 1))
            c &= w - 1
            if e[2] == "<<":
                return wrap(a << c, t)
            return wrap(a >> c, t)
        if k in ("divc", "divv"):
            a = self.ev(e[3], env)
            b = e[4] if k == "divc" else (((self.ev(e[4], env) & wrap(63, t)) | 1))
            b = wrap(b, t)
            q = tdiv(a, b)
            if e[2] == "/":
                return wrap(q, t)
            return wrap(a - q * b, t)
        if k == "un":
            a = self.ev(e[3], env)
            return wrap(-a if e[2] == "-" else ~a, t)
        if k == "cast":
            return wrap(self.ev(e[3], env), t)
        if k == "call":
            f = self.fmap[e[2]]
            args = [self.ev(a, env) for a in e[3]]
            return self.call(f, args)
        if k == "arr":
            i = self.ev(e[4], env) & (e[3] - 1)
            return env.get(e[2])[i]
        if k == "fld":
            return env.get(e[2])[e[3]]
        if k == "sel":
            return self.ev(e[3], env) if self.evc(e[2], env) else self.ev(e[4], env)

    def evc(self, c, env):
        k = c[0]
        if k == "land":
            if c[1] == "&&":
                return self.evc(c[2], env) and self.evc(c[3], env)
            return self.evc(c[2], env) or self.evc(c[3], env)
        if k == "not":
            return not self.evc(c[1], env)
        a = self.ev(c[2], env)
        b = self.ev(c[3], env)
        return {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "==": a == b, "!=": a != b}[c[1]]

    def call(self, f, args):
        env = Env(None)
        for (pn, pt), a in zip(f["params"], args):
            env.decl(pn, a)
        self.steps += 1
        if self.steps > 200000:
            raise RuntimeError("too many steps")
        try:
            self.exec_block(f["body"], env)
        except Return as r:
            return r.v
        raise RuntimeError("no return")

    def exec_block(self, stmts, env):
        for s in stmts:
            self.exec_s(s, env)

    def exec_s(self, s, env):
        k = s[0]
        self.steps += 1
        if self.steps > 300000:
            raise RuntimeError("too many steps")
        if k == "decl":
            env.decl(s[1], self.ev(s[3], env))
        elif k == "asg":
            env.set(s[1], self.ev(s[3], env))
        elif k == "adecl":
            env.decl(s[1], [0] * s[3])
        elif k == "astore":
            a = env.get(s[1])
            a[self.ev(s[3], env) & (s[2] - 1)] = self.ev(s[4], env)
        elif k == "sdecl":
            env.decl(s[1], {f: 0 for f, _ in s[2]["fields"]})
        elif k == "sstore":
            env.get(s[1])[s[2]] = self.ev(s[3], env)
        elif k == "scopy":
            env.set(s[1], dict(env.get(s[2])))
        elif k == "if":
            if self.evc(s[1], env):
                self.exec_block(s[2], Env(env))
            elif s[3] is not None:
                self.exec_block(s[3], Env(env))
        elif k == "for":
            for i in range(s[2], s[3]):
                e2 = Env(env)
                e2.decl(s[1], i)
                try:
                    self.exec_block(s[4], e2)
                except Continue:
                    pass
                except Break:
                    break
        elif k == "while":
            env.decl(s[1], s[2])
            while env.get(s[1]) < s[3]:
                try:
                    self.exec_block(s[4], Env(env))
                except Break:
                    break
                env.set(s[1], env.get(s[1]) + 1)
        elif k == "break":
            if self.evc(s[1], env):
                raise Break()
        elif k == "cont":
            if self.evc(s[1], env):
                raise Continue()
        elif k == "switch":
            v = self.ev(s[2], env) & 7
            body = s[3].get(v, s[4])
            if body is not None:
                self.exec_block(body, Env(env))
        elif k == "ret":
            raise Return(self.ev(s[1], env))
        elif k == "pcall":
            f = self.fmap[s[2]]
            cur = env.get(s[1])
            nv = self.call(f, [cur] + [self.ev(a, env) for a in s[3]])
            env.set(s[1], nv)
        elif k == "scall":
            f = self.fmap[s[2]]
            env.set(s[1], self.call(f, [dict(env.get(s[3]))]))
        else:
            raise RuntimeError(k)

    def emit_block(self, stmts, ind, out):
        p = "  " * ind
        for s in stmts:
            k = s[0]
            if k == "decl":
                out.append(f"{p}var {s[1]}: {s[2]} = {self.emit_e(s[3])};")
            elif k == "asg":
                out.append(f"{p}{s[1]} = {self.emit_e(s[3])};")
            elif k == "adecl":
                out.append(f"{p}var {s[1]}: {s[2]}[{s[3]}];")
                out.append(f"{p}for {s[1]}_z: int64 in 0..{s[3]} {{ {s[1]}[{s[1]}_z] = {lit(0, s[2])}; }}")
            elif k == "astore":
                out.append(f"{p}{s[1]}[({self.emit_e(s[3])}) & {s[2] - 1}] = {self.emit_e(s[4])};")
            elif k == "sdecl":
                out.append(f"{p}var {s[1]}: {s[2]['name']};")
                for f, ft in s[2]["fields"]:
                    out.append(f"{p}{s[1]}.{f} = {lit(0, ft)};")
            elif k == "sstore":
                out.append(f"{p}{s[1]}.{s[2]} = {self.emit_e(s[3])};")
            elif k == "scopy":
                out.append(f"{p}{s[1]} = {s[2]};")
            elif k == "if":
                out.append(f"{p}if {self.emit_c(s[1])} {{")
                self.emit_block(s[2], ind + 1, out)
                if s[3] is not None:
                    out.append(f"{p}}} else {{")
                    self.emit_block(s[3], ind + 1, out)
                out.append(f"{p}}}")
            elif k == "for":
                out.append(f"{p}for {s[1]}: int64 in {s[2]}..{s[3]} {{")
                self.emit_block(s[4], ind + 1, out)
                out.append(f"{p}}}")
            elif k == "while":
                out.append(f"{p}var {s[1]}: int64 = {s[2]};")
                out.append(f"{p}while ({s[1]} < {s[3]}) {{")
                self.emit_block(s[4], ind + 1, out)
                out.append(f"{p}  {s[1]} = {s[1]} + 1;")
                out.append(f"{p}}}")
            elif k == "break":
                out.append(f"{p}if {self.emit_c(s[1])} {{ break; }}")
            elif k == "cont":
                out.append(f"{p}if {self.emit_c(s[1])} {{ continue; }}")
            elif k == "switch":
                out.append(f"{p}switch ((({s[1]})({self.emit_e(s[2])})) & {lit(7, s[1])}) {{")
                for cv in sorted(s[3]):
                    out.append(f"{p}  case {cv}: {{")
                    self.emit_block(s[3][cv], ind + 2, out)
                    out.append(f"{p}  }}")
                if s[4] is not None:
                    out.append(f"{p}  default: {{")
                    self.emit_block(s[4], ind + 2, out)
                    out.append(f"{p}  }}")
                out.append(f"{p}}}")
            elif k == "ret":
                out.append(f"{p}return {self.emit_e(s[1])};")
            elif k == "pcall":
                args = ", ".join(self.emit_e(a) for a in s[3])
                out.append(f"{p}{s[2]}_p(&{s[1]}{', ' if args else ''}{args});")
            elif k == "scall":
                out.append(f"{p}{s[1]} = {s[2]}({s[3]});")

    def gen_block(self, sc, budget, depth, in_loop):
        out = []
        n = self.r.randint(1, max(1, budget))
        for _ in range(n):
            out.extend(self.gen_stmt(sc, budget, depth, in_loop))
        return out

    def gen_stmt(self, sc, budget, depth, in_loop):
        ed = self.r.randint(1, 3)
        ks = ["decl", "asg", "asg", "if", "loop", "arr", "astore", "struct", "switch", "pcall", "brk"]
        ws = [10, 20, 10, 8, 6 if depth < 3 else 0, 3, 6, 3, 3 if depth < 3 else 0, 3, 5 if in_loop else 0]
        k = self.r.choices(ks, ws)[0]
        if k == "decl":
            t = self.r.choice(TNAMES)
            v = self.fresh("v")
            e = self.expr(sc, t, ed)
            sc.vars[v] = t
            sc.mutable_block.add(v)
            return [("decl", v, t, e)]
        if k == "asg":
            mv = [v for v in sc.mutable if v in sc.vars]
            if not mv:
                return self.gen_stmt(sc, budget, depth, in_loop) if self.r.random() < 0.5 else []
            v = self.r.choice(mv)
            return [("asg", v, sc.vars[v], self.expr(sc, sc.vars[v], ed))]
        if k == "if":
            c = self.cond(sc, 2)
            a = self.gen_block(sc.child(), budget // 2, depth + 1, in_loop)
            b = self.gen_block(sc.child(), budget // 2, depth + 1, in_loop) if self.r.random() < 0.6 else None
            return [("if", c, a, b)]
        if k == "loop":
            lo = self.r.randint(0, 3)
            hi = lo + self.r.randint(0, 9)
            iv = self.fresh("i")
            inner = sc.child()
            inner.vars[iv] = "int64"
            if self.r.random() < 0.5:
                body = self.gen_block(inner, budget // 2, depth + 1, "for")
                return [("for", iv, lo, hi, body)]
            body = self.gen_block(inner, budget // 2, depth + 1, "while")
            sc.vars[iv] = "int64"
            return [("while", iv, lo, hi, body)]
        if k == "arr":
            t = self.r.choice(TNAMES)
            n = self.r.choice([4, 8, 16])
            a = self.fresh("a")
            sc.arrs[a] = (t, n)
            return [("adecl", a, t, n)]
        if k == "astore":
            if not sc.arrs:
                return []
            a = self.r.choice(list(sc.arrs))
            t, n = sc.arrs[a]
            return [("astore", a, n, self.expr(sc, "int64", 1), self.expr(sc, t, ed))]
        if k == "struct":
            if not self.structs:
                return []
            st = self.r.choice(self.structs)
            v = self.fresh("s")
            sc.svars[v] = st
            out = [("sdecl", v, st)]
            for f, ft in st["fields"]:
                if self.r.random() < 0.8:
                    out.append(("sstore", v, f, self.expr(sc, ft, ed)))
            if st.get("fold") and self.r.random() < 0.7:
                fv = [x for x in sc.mutable if sc.vars.get(x) == st["fold"]["ret"]]
                if fv:
                    out.append(("scall", self.r.choice(fv), st["fold"]["name"], v))
            if self.r.random() < 0.3:
                v2 = self.fresh("s")
                sc.svars[v2] = st
                out.append(("sdecl", v2, st))
                out.append(("scopy", v2, v))
                f, ft = self.r.choice(st["fields"])
                out.append(("sstore", v2, f, self.expr(sc, ft, ed)))
            return out
        if k == "switch":
            t = self.r.choice(["int32", "int64", "uint8", "uint32"])
            e = self.expr(sc, t, ed)
            cases = {}
            for cv in self.r.sample(range(8), self.r.randint(1, 8)):
                cases[cv] = self.gen_block(sc.child(), budget // 3, depth + 1, None)
            dflt = self.gen_block(sc.child(), budget // 3, depth + 1, None) if self.r.random() < 0.6 else None
            return [("switch", t, e, cases, dflt)]
        if k == "pcall":
            cands = [f for f in self.funcs if f["idx"] < sc.fidx and f.get("ptr")]
            if not cands:
                return []
            f = self.r.choice(cands)
            pt = f["params"][0][1]
            mv = [v for v in sc.mutable if sc.vars.get(v) == pt]
            if not mv:
                return []
            return [("pcall", self.r.choice(mv), f["name"], [self.expr(sc, t, 1) for _, t in f["params"][1:]])]
        if k == "brk":
            if in_loop == "for":
                return [(self.r.choice(["break", "cont"]), self.cond(sc, 1))]
            return [("break", self.cond(sc, 1))]
        return []

    def gen_struct(self, i):
        n = self.r.randint(1, 5)
        fields = [(f"f{j}", self.r.choice(TNAMES)) for j in range(n)]
        return {"name": f"S{i}", "fields": fields}

    def gen_func(self, idx, ptr=False):
        np_ = self.r.choice([0, 1, 2, 3, 4, 5, 6, 8, 10])
        if ptr:
            np_ = max(np_, 1)
        params = [(f"p{j}", self.r.choice(TNAMES)) for j in range(np_)]
        ret = params[0][1] if ptr else self.r.choice(TNAMES)
        f = {"name": f"h{idx}", "idx": idx, "params": params, "ret": ret, "ptr": ptr}
        sc = Scope(None, idx)
        for pn, pt in params:
            sc.vars[pn] = pt
        body = []
        for _ in range(self.r.randint(0, 3)):
            t = self.r.choice(TNAMES)
            v = self.fresh("l")
            body.append(("decl", v, t, self.expr(sc, t, 1)))
            sc.vars[v] = t
            sc.mutable_block.add(v)
        body += self.gen_block(sc, self.opts.get("fbudget", 6), 1, False)
        body.append(("ret", self.expr(sc, ret, 3)))
        f["body"] = body
        f["deco"] = self.r.choice(["", "", "@noinline ", "@inline "])
        return f

    def gen_fold(self, st, idx):
        ret = self.r.choice(TNAMES)
        e = None
        for fn, ft in st["fields"]:
            term = ("cast", ret, ft, ("fld", ft, "s", fn))
            if self.r.random() < 0.5:
                term = ("bin", ret, "*", term, ("lit", ret, wrap(self.r.randint(1, 9), ret)))
            e = term if e is None else ("bin", ret, self.r.choice(["+", "^", "-"]), e, term)
        f = {"name": f"fold{idx}", "idx": -1, "params": [("s", st["name"])], "ret": ret, "ptr": False,
             "body": [("ret", e)], "deco": self.r.choice(["", "@noinline "]), "struct": st}
        st["fold"] = f
        return f

    def generate(self):
        self.structs = [self.gen_struct(i) for i in range(self.r.randint(0, 3))]
        folds = [self.gen_fold(st, i) for i, st in enumerate(self.structs)]
        nf = self.r.randint(0, self.opts.get("nfuncs", 6))
        for i in range(nf):
            self.funcs.append(self.gen_func(i, ptr=self.r.random() < 0.2))
        self.fmap = {f["name"]: f for f in self.funcs + folds}
        sc = Scope(None, 1 << 30)
        main = []
        nseed = self.r.randint(4, 16)
        for _ in range(nseed):
            t = self.r.choice(TNAMES)
            v = self.fresh("m")
            main.append(("decl", v, t, ("lit", t, wrap(self.interesting(t), t))))
            sc.vars[v] = t
            sc.mutable_block.add(v)
        main += self.gen_block(sc, self.opts.get("mbudget", 14), 0, False)
        self.folds = folds
        self.main = main
        self.sc = sc
        return self.render()

    def render(self):
        folds = self.folds
        main = self.main
        sc = self.sc
        self.fmap = {f["name"]: f for f in self.funcs + folds}
        self.steps = 0
        env = Env(None)
        self.exec_block(main, env)
        checks = []
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
        out = [f"// seed {self.seed}"]
        for st in self.structs:
            out.append(f"struct {st['name']} {{")
            for f, ft in st["fields"]:
                out.append(f"  {f}: {ft};")
            out.append("}")
        for f in folds + self.funcs:
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
        self.emit_block(main, 1, out)
        for i, (v, t, x) in enumerate(checks):
            out.append(f"  if ({v} != {lit(x, t)}) {{ return {1 + (i % 250)}; }}")
        out.append("  return 0;")
        out.append("}")
        return "\n".join(out) + "\n", checks


class Scope:
    def __init__(self, parent, fidx):
        self.parent = parent
        self.fidx = fidx if parent is None else parent.fidx
        self.vars = dict(parent.vars) if parent else {}
        self.arrs = dict(parent.arrs) if parent else {}
        self.svars = dict(parent.svars) if parent else {}
        self.mutable_block = set()

    @property
    def mutable(self):
        s = set()
        p = self
        while p:
            s |= p.mutable_block
            p = p.parent
        return [v for v in self.vars if v in s]

    def child(self):
        return Scope(self, self.fidx)


class Env:
    def __init__(self, parent):
        self.parent = parent
        self.d = {}

    def decl(self, k, v):
        self.d[k] = v

    def get(self, k):
        e = self
        while e:
            if k in e.d:
                return e.d[k]
            e = e.parent
        raise KeyError(k)

    def set(self, k, v):
        e = self
        while e:
            if k in e.d:
                e.d[k] = v
                return
            e = e.parent
        raise KeyError(k)


def make(seed, opts=None):
    return Gen(seed, opts).generate()


if __name__ == "__main__":
    src, checks = make(int(sys.argv[1]))
    sys.stdout.write(src)
