import os
import random
import struct
import subprocess
import sys

import numpy as np

from gen import TYPES, wrap, lit

HERE = os.path.dirname(os.path.abspath(__file__))
SC = ["int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32", "float64"]
CT = {"int8": "int8_t", "int16": "int16_t", "int32": "int32_t", "int64": "int64_t", "uint8": "uint8_t",
      "uint16": "uint16_t", "uint32": "uint32_t", "uint64": "uint64_t", "float32": "float", "float64": "double"}
M = 2 ** 64


def f32(x):
    return float(np.float32(x))


def bits(v, t):
    if t == "float32":
        return struct.unpack("<I", struct.pack("<f", v))[0]
    if t == "float64":
        return struct.unpack("<Q", struct.pack("<d", v))[0]
    w = TYPES[t][0]
    return v & ((1 << w) - 1)


def frepr(v):
    r = repr(v)
    if "e" not in r and "." not in r:
        r += ".0"
    return r


class Abi:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.seed = seed
        self.structs = []
        for i in range(self.r.randint(1, 5)):
            n = self.r.choice([1, 1, 2, 2, 3, 4, 5, 6, 8])
            fields = [self.r.choice(SC) for _ in range(n)]
            if self.r.random() < 0.3:
                fields = [self.r.choice(["float32", "float64"]) for _ in range(n)]
            self.structs.append({"name": f"S{i}", "fields": fields})

    def val(self, t):
        r = self.r
        if t == "float32":
            return f32(r.choice([0.5, -1.25, 3.0, 1e10, -7.75, 0.1, 1234.5, 2.0 ** -20]))
        if t == "float64":
            return r.choice([0.5, -1.25, 3.0, 1e100, -7.75, 0.1, 123456789.123, 2.0 ** -40])
        w, s = TYPES[t]
        return wrap(r.randint(-(1 << (w - 1)), (1 << w) - 1), t)

    def ptype(self):
        if self.r.random() < 0.45:
            return self.r.choice(self.structs)["name"]
        return self.r.choice(SC)

    def sfields(self, name):
        return next(s for s in self.structs if s["name"] == name)["fields"]

    def flatten(self, t, v):
        if t in CT:
            return [(t, v)]
        return list(zip(self.sfields(t), v))

    def hash(self, params, vals):
        h = 0
        for t, v in zip(params, vals):
            for ft, fv in self.flatten(t, v):
                h = (h * 1000003 + bits(fv, ft)) % M
        return h

    def derive(self, rt, h):
        out = []
        for j, ft in enumerate(self.sfields(rt)):
            x = (h + j) % M
            if ft == "float64":
                out.append(float(x % 1000) + 0.25)
            elif ft == "float32":
                out.append(f32(float(x % 1000) + 0.25))
            else:
                out.append(wrap(x, ft))
        return out

    def mlit(self, t, v):
        if t == "float64":
            return f"({frepr(v)})"
        if t == "float32":
            return f"((float32)({frepr(v)}))"
        return lit(v, t)

    def clit(self, t, v):
        if t == "float64":
            return frepr(v)
        if t == "float32":
            return frepr(v) + "f"
        if t == "int64" and v == -(1 << 63):
            return "(-9223372036854775807LL - 1)"
        if t.startswith("uint"):
            return f"(({CT[t]}){v}ULL)"
        return f"(({CT[t]}){v}LL)"

    def mhash_lines(self, params, names, ind="  "):
        out = [f"{ind}var h: uint64 = (uint64)0;"]
        for t, n in zip(params, names):
            if t in CT:
                items = [(t, n)]
            else:
                items = [(ft, f"{n}.f{j}") for j, ft in enumerate(self.sfields(t))]
            for ft, expr in items:
                if ft == "float64":
                    b = f"bits_f64({expr})"
                elif ft == "float32":
                    b = f"(uint64)bits_f32({expr})"
                else:
                    b = f"(uint64)(({'u' + ft if not ft.startswith('u') else ft})({expr}))"
                out.append(f"{ind}h = h * (uint64)1000003 + {b};")
        return out

    def chash_lines(self, params, names, ind="  "):
        out = [f"{ind}uint64_t h = 0;"]
        for t, n in zip(params, names):
            if t in CT:
                items = [(t, n)]
            else:
                items = [(ft, f"{n}.f{j}") for j, ft in enumerate(self.sfields(t))]
            for ft, expr in items:
                if ft == "float64":
                    b = f"b64({expr})"
                elif ft == "float32":
                    b = f"(uint64_t)b32({expr})"
                else:
                    b = f"(uint64_t)(u{CT[ft]} if 0 else 0)" if False else f"(uint64_t)({CT['u' + ft if not ft.startswith('u') else ft]})({expr})"
                out.append(f"{ind}h = h * 1000003ULL + {b};")
        return out

    def make(self):
        r = self.r
        c = ["#include <stdint.h>", "#include <string.h>",
             "static uint64_t b64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }",
             "static uint32_t b32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }"]
        m = [f"// seed {self.seed}",
             "@noinline fn bits_f64(x: float64) -> uint64 { var y: float64 = x; return *((uint64*)&y); }",
             "@noinline fn bits_f32(x: float32) -> uint32 { var y: float32 = x; return *((uint32*)&y); }"]
        for s in self.structs:
            c.append("typedef struct { " + " ".join(f"{CT[ft]} f{j};" for j, ft in enumerate(s["fields"])) + f" }} {s['name']};")
            m.append(f"struct {s['name']} {{ " + " ".join(f"f{j}: {ft};" for j, ft in enumerate(s["fields"])) + " }")
        main = ["fn main() -> int32 {"]
        code = 0
        ctype = lambda t: CT.get(t, t)

        def build_vals(params):
            return [self.val(t) if t in CT else [self.val(ft) for ft in self.sfields(t)] for t in params]

        def m_args(params, vals, prefix):
            pre = []
            args = []
            for i, (t, v) in enumerate(zip(params, vals)):
                if t in CT:
                    args.append(self.mlit(t, v))
                else:
                    nm = f"{prefix}_{i}"
                    pre.append(f"  var {nm}: {t};")
                    for j, ft in enumerate(self.sfields(t)):
                        pre.append(f"  {nm}.f{j} = {self.mlit(ft, v[j])};")
                    args.append(nm)
            return pre, args

        def c_args(params, vals, prefix):
            pre = []
            args = []
            for i, (t, v) in enumerate(zip(params, vals)):
                if t in CT:
                    args.append(self.clit(t, v))
                else:
                    nm = f"{prefix}_{i}"
                    pre.append(f"  {t} {nm};")
                    for j, ft in enumerate(self.sfields(t)):
                        pre.append(f"  {nm}.f{j} = {self.clit(ft, v[j])};")
                    args.append(nm)
            return pre, args

        for k in range(r.randint(3, 7)):
            kind = r.choice(["mc_hash", "mc_make", "cm_hash", "cm_make", "mm_hash", "mm_make"])
            params = [self.ptype() for _ in range(r.randint(0, 9))]
            names = [f"a{i}" for i in range(len(params))]
            vals = build_vals(params)
            h = self.hash(params, vals)
            sig_m = ", ".join(f"{n}: {t}" for n, t in zip(names, params))
            sig_c = ", ".join(f"{ctype(t)} {n}" for n, t in zip(names, params)) or "void"
            if kind == "mm_hash":
                m.append(f"export fn m{k}({sig_m}) -> uint64 {{")
                m += self.mhash_lines(params, names)
                m.append("  return h;\n}")
                pre, args = m_args(params, vals, f"v{k}")
                main += pre
                code += 1
                main.append(f"  if (m{k}({', '.join(args)}) != (uint64){h}) {{ return {code}; }}")
                continue
            if kind == "mm_make":
                rt = r.choice(self.structs)["name"]
                m.append(f"export fn m{k}({sig_m}) -> {rt} {{")
                m += self.mhash_lines(params, names)
                m.append(f"  var out: {rt};")
                for j, ft in enumerate(self.sfields(rt)):
                    if ft == "float64":
                        m.append(f"  out.f{j} = (float64)((h + (uint64){j}) % (uint64)1000) + 0.25;")
                    elif ft == "float32":
                        m.append(f"  out.f{j} = (float32)((h + (uint64){j}) % (uint64)1000) + (float32)0.25;")
                    else:
                        m.append(f"  out.f{j} = ({ft})(h + (uint64){j});")
                m.append("  return out;\n}")
                pre, args = m_args(params, vals, f"v{k}")
                main += pre
                main.append(f"  var r{k}: {rt} = m{k}({', '.join(args)});")
                exp = self.derive(rt, h)
                for j, ft in enumerate(self.sfields(rt)):
                    code += 1
                    if ft == "float64":
                        main.append(f"  if (bits_f64(r{k}.f{j}) != (uint64){bits(exp[j], ft)}) {{ return {code}; }}")
                    elif ft == "float32":
                        main.append(f"  if (bits_f32(r{k}.f{j}) != (uint32){bits(exp[j], ft)}) {{ return {code}; }}")
                    else:
                        main.append(f"  if (r{k}.f{j} != {lit(exp[j], ft)}) {{ return {code}; }}")
                continue
            if kind == "mc_hash":
                c.append(f"uint64_t abi_c{k}({sig_c}) {{")
                c += self.chash_lines(params, names)
                c.append("  return h;\n}")
                m.append(f"extern fn c{k}({sig_m}) -> uint64 = \"abi_c{k}\";")
                pre, args = m_args(params, vals, f"v{k}")
                main += pre
                code += 1
                main.append(f"  if (c{k}({', '.join(args)}) != (uint64){h}) {{ return {code}; }}")
            elif kind == "mc_make":
                rt = r.choice(self.structs)["name"]
                c.append(f"{rt} abi_c{k}({sig_c}) {{")
                c += self.chash_lines(params, names)
                c.append(f"  {rt} out;")
                for j, ft in enumerate(self.sfields(rt)):
                    if ft in ("float64", "float32"):
                        cast = "double" if ft == "float64" else "float"
                        c.append(f"  out.f{j} = ({cast})((h + {j}ULL) % 1000ULL) + ({cast})0.25;")
                    else:
                        c.append(f"  out.f{j} = ({CT[ft]})(h + {j}ULL);")
                c.append("  return out;\n}")
                m.append(f"extern fn c{k}({sig_m}) -> {rt} = \"abi_c{k}\";")
                pre, args = m_args(params, vals, f"v{k}")
                main += pre
                main.append(f"  var r{k}: {rt} = c{k}({', '.join(args)});")
                exp = self.derive(rt, h)
                for j, ft in enumerate(self.sfields(rt)):
                    code += 1
                    if ft == "float64":
                        main.append(f"  if (bits_f64(r{k}.f{j}) != (uint64){bits(exp[j], ft)}) {{ return {code}; }}")
                    elif ft == "float32":
                        main.append(f"  if (bits_f32(r{k}.f{j}) != (uint32){bits(exp[j], ft)}) {{ return {code}; }}")
                    else:
                        main.append(f"  if (r{k}.f{j} != {lit(exp[j], ft)}) {{ return {code}; }}")
            elif kind == "cm_hash":
                m.append(f"export fn m{k}({sig_m}) -> uint64 {{")
                m += self.mhash_lines(params, names)
                m.append("  return h;\n}")
                c.append(f"uint64_t m{k}({sig_c});")
                pre, args = c_args(params, vals, f"v")
                c.append(f"uint64_t abi_drive{k}(void) {{")
                c += pre
                c.append(f"  return m{k}({', '.join(args)});\n}}")
                m.append(f"extern fn drive{k}() -> uint64 = \"abi_drive{k}\";")
                code += 1
                main.append(f"  if (drive{k}() != (uint64){h}) {{ return {code}; }}")
            else:
                rt = r.choice(self.structs)["name"]
                m.append(f"export fn m{k}({sig_m}) -> {rt} {{")
                m += self.mhash_lines(params, names)
                m.append(f"  var out: {rt};")
                for j, ft in enumerate(self.sfields(rt)):
                    if ft == "float64":
                        m.append(f"  out.f{j} = (float64)((h + (uint64){j}) % (uint64)1000) + 0.25;")
                    elif ft == "float32":
                        m.append(f"  out.f{j} = (float32)((h + (uint64){j}) % (uint64)1000) + (float32)0.25;")
                    else:
                        m.append(f"  out.f{j} = ({ft})(h + (uint64){j});")
                m.append("  return out;\n}")
                c.append(f"{rt} m{k}({sig_c});")
                pre, args = c_args(params, vals, f"v")
                c.append(f"uint64_t abi_drive{k}(void) {{")
                c += pre
                c.append(f"  {rt} res = m{k}({', '.join(args)});")
                c += self.chash_lines([rt], ["res"])
                c.append("  return h;\n}")
                m.append(f"extern fn drive{k}() -> uint64 = \"abi_drive{k}\";")
                exp = self.hash([rt], [self.derive(rt, h)])
                code += 1
                main.append(f"  if (drive{k}() != (uint64){exp}) {{ return {code}; }}")
        main.append("  return 0;\n}")
        return "\n".join(m + main) + "\n", "\n".join(c) + "\n"


def run(seed, comp, work, flags, linux=False):
    a = Abi(seed)
    ms, cs = a.make()
    base = os.path.join(work, f"a{seed}")
    open(base + ".mettle", "wb").write(ms.encode())
    open(base + ".c", "wb").write(cs.encode())
    if linux:
        return None
    p = subprocess.run(["gcc", "-O1", "-c", base + ".c", "-o", base + "_c.o"], capture_output=True)
    if p.returncode != 0:
        return "cc-fail " + p.stderr.decode()[-300:]
    exe = base + ".exe"
    p = subprocess.run([comp, "--build"] + flags + ["--linker", "internal", base + ".mettle", "-o", exe,
                        "--link-arg", base + "_c.o"], capture_output=True)
    if p.returncode != 0:
        return "build-fail " + (p.stdout + p.stderr).decode(errors="replace")[-400:]
    q = subprocess.run([exe], capture_output=True, timeout=20)
    return None if q.returncode == 0 else f"rc={q.returncode}"


if __name__ == "__main__":
    comp = os.path.abspath(sys.argv[1])
    start, count = int(sys.argv[2]), int(sys.argv[3])
    flagsets = [f.split() for f in sys.argv[4].split(",")] if len(sys.argv) > 4 else [[]]
    work = os.path.join(HERE, "abiwork")
    os.makedirs(work, exist_ok=True)
    import concurrent.futures as cf

    def job(s):
        res = []
        for fl in flagsets:
            e = run(s, comp, work, fl)
            if e:
                res.append((" ".join(fl), e))
        return s, res

    bad = 0
    with cf.ThreadPoolExecutor(8) as ex:
        for s, res in ex.map(job, range(start, start + count)):
            if res:
                bad += 1
                print(s, res, flush=True)
    print(f"done {count}, {bad} bad")
