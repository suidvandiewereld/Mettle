import sys

import gen2 as G2
from gen import TYPES, TNAMES, wrap, lit, Env, Scope, Return
from gen2 import FT, fround


class Gen3(G2.Gen2):
    def __init__(self, seed, opts=None):
        super().__init__(seed, opts)
        self.enums = []
        self.efolds = []

    def gen_enum(self, i):
        n = self.r.randint(2, 4)
        variants = [(f"V{j}", self.rtype()) for j in range(n)]
        return {"name": f"E{i}", "variants": variants}

    def conv(self, e, src, dst):
        if src == dst:
            return e
        if src in FT and dst not in FT:
            self.f2i_used.add((src, dst))
            return ("f2i", dst, src, e)
        return ("cast", dst, src, e)

    def gen_efold(self, en, idx):
        ret = self.rtype()
        sc = Scope(None, idx)
        arms = []
        for vn, vt in en["variants"]:
            b = self.fresh("b")
            sc2 = sc.child()
            sc2.vars[b] = vt
            e = self.conv(("var", vt, b), vt, ret)
            if self.r.random() < 0.6:
                if ret in FT:
                    e = ("bin", ret, self.r.choice(["+", "*", "-"]), e, self.fexpr(sc2, ret, 1))
                else:
                    e = ("bin", ret, self.r.choice(["+", "^", "*", "-"]), e, self.expr(sc2, ret, 1))
            arms.append((vn, b, e))
        f = {"name": f"ef{idx}", "idx": -1, "params": [("e", en["name"])], "ret": ret, "ptr": False,
             "body": [("ematch", "e", arms)], "deco": self.r.choice(["", "@noinline "]), "enum": en}
        en.setdefault("folds", []).append(f)
        return f

    def gen_stmt(self, sc, budget, depth, in_loop):
        r = self.r.random()
        if r < 0.08 and self.enums:
            en = self.r.choice(self.enums)
            vi = self.r.randrange(len(en["variants"]))
            vn, vt = en["variants"][vi]
            existing = [v for v, t in sc.evars.items() if t is en]
            if existing and self.r.random() < 0.4:
                v = self.r.choice(existing)
                return [("easg", v, en, vi, self.expr(sc, vt, 2))]
            v = self.fresh("e")
            sc.evars[v] = en
            out = [("edecl", v, en, vi, self.expr(sc, vt, 2))]
            return out
        if r < 0.14 and sc.evars:
            v = self.r.choice(list(sc.evars))
            en = sc.evars[v]
            f = self.r.choice(en["folds"])
            tv = [x for x in sc.mutable if sc.vars.get(x) == f["ret"]]
            if tv:
                return [("asg", self.r.choice(tv), f["ret"], ("call", f["ret"], f["name"], [("evar", en["name"], v)]))]
        return super().gen_stmt(sc, budget, depth, in_loop)

    def exec_s(self, s, env):
        k = s[0]
        if k == "edecl":
            self.steps += 1
            env.decl(s[1], (s[3], self.ev(s[4], env)))
            return
        if k == "easg":
            self.steps += 1
            env.set(s[1], (s[3], self.ev(s[4], env)))
            return
        if k == "ematch":
            self.steps += 1
            vi, payload = env.get(s[1])
            vn, b, e = s[2][vi]
            env2 = Env(env)
            env2.decl(b, payload)
            raise Return(self.ev(e, env2))
        if k == "defer":
            self.steps += 1
            self.defers[-1].append(s[1])
            return
        return super().exec_s(s, env)

    def ev(self, e, env):
        if e[0] == "evar":
            return env.get(e[2])
        return super().ev(e, env)

    def emit_e(self, e):
        if e[0] == "evar":
            return e[2]
        return super().emit_e(e)

    def call(self, f, args):
        env = Env(self.genv)
        for (pn, pt), a in zip(f["params"], args):
            env.decl(pn, a)
        self.steps += 1
        if self.steps > 300000 or self.depth > 60:
            raise RuntimeError("too many steps")
        self.depth += 1
        self.defers.append([])
        result = None
        try:
            self.exec_block(f["body"], env)
        except Return as r:
            result = r.v
        finally:
            self.depth -= 1
        pending = self.defers.pop()
        if result is None:
            raise RuntimeError("no return")
        for blk in reversed(pending):
            self.exec_block(blk, env)
        return result

    def emit_block(self, stmts, ind, out):
        p = "  " * ind
        for s in stmts:
            k = s[0]
            if k == "edecl":
                out.append(f"{p}var {s[1]}: {s[2]['name']} = {s[2]['name']}.{s[2]['variants'][s[3]][0]}({self.emit_e(s[4])});")
            elif k == "easg":
                out.append(f"{p}{s[1]} = {s[2]['name']}.{s[2]['variants'][s[3]][0]}({self.emit_e(s[4])});")
            elif k == "ematch":
                out.append(f"{p}match ({s[1]}) {{")
                for vn, b, e in s[2]:
                    out.append(f"{p}  case {vn}({b}): {{ return {self.emit_e(e)}; }}")
                out.append(f"{p}}}")
            elif k == "defer":
                out.append(f"{p}defer {{")
                self.emit_block(s[1], ind + 1, out)
                out.append(f"{p}}}")
            else:
                super().emit_block([s], ind, out)

    def gen_func(self, idx, ptr=False):
        f = super().gen_func(idx, ptr)
        if f.get("impure") and self.globals and self.r.random() < 0.7:
            sc = Scope(None, idx)
            for pn, pt in f["params"]:
                sc.vars[pn] = pt
            for g, gt in self.globals.items():
                sc.vars[g] = gt
            ds = []
            for _ in range(self.r.randint(1, 2)):
                g = self.r.choice(list(self.globals))
                blk = [("gasg", g, self.globals[g], self.expr(sc, self.globals[g], 2))]
                ds.append(("defer", blk))
            f["body"] = ds + f["body"]
        return f

    def generate(self):
        self.enums = [self.gen_enum(i) for i in range(self.r.randint(0, 2))]
        self.efolds = []
        for i, en in enumerate(self.enums):
            for j in range(self.r.randint(1, 2)):
                self.efolds.append(self.gen_efold(en, i * 4 + j))
        return super().generate()

    def render(self):
        self.defers = [[]]
        src, checks = super().render()
        head = []
        for en in self.enums:
            head.append(f"enum {en['name']} {{")
            for vn, vt in en["variants"]:
                head.append(f"  {vn}({vt}),")
            head.append("}")
        lines = src.split("\n")
        return "\n".join(lines[:1] + head + lines[1:]), checks


_orig_render = G2.Gen2.render


def _fmap_patch(self):
    return None


_orig_scope_init = Scope.__init__


def _scope_init(self, parent, fidx):
    _orig_scope_init(self, parent, fidx)
    self.evars = dict(parent.evars) if parent is not None else {}


Scope.__init__ = _scope_init

_orig_g2_render = G2.Gen2.render


def _g2_render(self):
    extra = getattr(self, "efolds", [])
    saved = self.folds
    self.folds = saved + [f for f in extra if f not in saved]
    try:
        return _orig_g2_render(self)
    finally:
        self.folds = saved


G2.Gen2.render = _g2_render
_orig_g2_emit_fold = None


def make(seed, opts=None):
    return Gen3(seed, opts).generate()


if __name__ == "__main__":
    src, checks = make(int(sys.argv[1]))
    sys.stdout.write(src)
