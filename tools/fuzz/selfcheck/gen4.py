import sys

import gen3 as G3
from gen import TYPES, TNAMES, wrap, lit, Env, Scope, Return
from gen2 import FT


class Closure:
    def __init__(self, param, body, env):
        self.param = param
        self.body = body
        self.env = env


class Gen4(G3.Gen3):
    def expr(self, sc, t, d):
        if d > 0 and self.r.random() < 0.07:
            cs = [c for c, (pt, rt) in sc.cvars.items() if rt == t]
            if cs:
                c = self.r.choice(cs)
                pt = sc.cvars[c][0]
                return ("ccall", t, c, self.expr(sc, pt, d - 1))
            fps = [p for p, f in sc.fpvars.items() if f["ret"] == t]
            if fps:
                p = self.r.choice(fps)
                f = sc.fpvars[p]
                return ("fpcall", t, p, [self.expr(sc, pt, d - 1) for _, pt in f["params"]])
        return super().expr(sc, t, d)

    def gen_stmt(self, sc, budget, depth, in_loop):
        r = self.r.random()
        if r < 0.05 and sc.fidx >= (1 << 30):
            pt = self.rtype()
            rt = self.rtype()
            c = self.fresh("c")
            x = self.fresh("x")
            inner = Scope(None, sc.fidx)
            inner.ret = None
            for v, vt in sc.vars.items():
                inner.vars[v] = vt
            inner.vars[x] = pt
            body = self.expr(inner, rt, 2)
            sc.cvars[c] = (pt, rt)
            return [("cdecl", c, pt, rt, x, body)]
        if r < 0.09 and sc.fidx >= (1 << 30):
            cands = [f for f in self.funcs if not f.get("impure") and not f.get("smake") and not f.get("ptr")]
            if cands:
                f = self.r.choice(cands)
                same = [g for g in cands if g["ret"] == f["ret"] and [t for _, t in g["params"]] == [t for _, t in f["params"]]]
                existing = [p for p, g in sc.fpvars.items() if g in same and p in sc.mutable_fp]
                if existing and self.r.random() < 0.5:
                    p = self.r.choice(existing)
                    g = self.r.choice(same)
                    sc.fpvars[p] = g
                    return [("fpasg", p, g)]
                p = self.fresh("fp")
                sc.fpvars[p] = f
                sc.mutable_fp.add(p)
                return [("fpdecl", p, f)]
        return super().gen_stmt(sc, budget, depth, in_loop)

    def sig(self, f):
        return f"fn({', '.join(t for _, t in f['params'])}) -> {f['ret']}"

    def emit_e(self, e):
        k = e[0]
        if k == "ccall":
            return f"{e[2]}({self.emit_e(e[3])})"
        if k == "fpcall":
            return f"{e[2]}({', '.join(self.emit_e(a) for a in e[3])})"
        return super().emit_e(e)

    def ev(self, e, env):
        k = e[0]
        if k == "ccall":
            clo = env.get(e[2])
            arg = self.ev(e[3], env)
            env2 = Env(clo.env)
            env2.decl(clo.param, arg)
            return self.ev(clo.body, env2)
        if k == "fpcall":
            f = env.get(e[2])
            return self.call(f, [self.ev(a, env) for a in e[3]])
        return super().ev(e, env)

    def exec_s(self, s, env):
        k = s[0]
        if k == "cdecl":
            self.steps += 1
            snap = Env(self.genv)
            e = env
            while e is not None and e is not self.genv:
                for name, val in e.d.items():
                    if name not in snap.d and not isinstance(val, (list, dict, tuple, Closure)):
                        snap.d[name] = val
                e = e.parent
            env.decl(s[1], Closure(s[4], s[5], snap))
            return
        if k == "fpdecl":
            self.steps += 1
            env.decl(s[1], s[2])
            return
        if k == "fpasg":
            self.steps += 1
            env.set(s[1], s[2])
            return
        return super().exec_s(s, env)

    def emit_block(self, stmts, ind, out):
        p = "  " * ind
        for s in stmts:
            k = s[0]
            if k == "cdecl":
                out.append(f"{p}var {s[1]}: Fn({s[2]}) -> {s[3]} = fn({s[4]}: {s[2]}) -> {s[3]} {{ return {self.emit_e(s[5])}; }};")
            elif k == "fpdecl":
                out.append(f"{p}var {s[1]}: {self.sig(s[2])} = &{s[2]['name']};")
            elif k == "fpasg":
                out.append(f"{p}{s[1]} = &{s[2]['name']};")
            else:
                super().emit_block([s], ind, out)


_orig_scope_init = Scope.__init__


def _scope_init(self, parent, fidx):
    _orig_scope_init(self, parent, fidx)
    self.cvars = dict(parent.cvars) if parent is not None else {}
    self.fpvars = dict(parent.fpvars) if parent is not None else {}
    self.mutable_fp = set(parent.mutable_fp) if parent is not None else set()


Scope.__init__ = _scope_init


def make(seed, opts=None):
    return Gen4(seed, opts).generate()


if __name__ == "__main__":
    src, checks = make(int(sys.argv[1]))
    sys.stdout.write(src)
