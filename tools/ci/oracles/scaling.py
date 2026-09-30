import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PICK = "@noinline fn pick(k: int64) -> int64 { return k; }\n"


def elseif(n):
    arms = ["  if (x == 0) { r = 1; }"]
    arms += ["  else if (x == %d) { r = %d; }" % (k, k * 3 + 1) for k in range(1, n)]
    return (PICK + "fn main() -> int32 {\n  var x: int64 = pick(7);\n  var r: int64 = 0;\n"
            + "\n".join(arms) + "\n  return (int32)(r & 127);\n}\n")


def switch(n):
    cases = ["    case %d: { r = r + %d; }" % (k, k * 5 + 3) for k in range(n)]
    return (PICK + "fn main() -> int32 {\n  var r: int64 = 0;\n  switch (pick(3)) {\n"
            + "\n".join(cases) + "\n    default: { r = r - 1; }\n  }\n  return (int32)(r & 127);\n}\n")


def loops(n):
    body = []
    for k in range(n):
        body.append("  var i%d: int64 = 0;\n  while (i%d < x) { r = r + i%d * %d; i%d = i%d + 1; }"
                    % (k, k, k, k + 1, k, k))
    return (PICK + "fn main() -> int32 {\n  var x: int64 = pick(3);\n  var r: int64 = 0;\n"
            + "\n".join(body) + "\n  return (int32)(r & 127);\n}\n")


def nest(n):
    opens = "".join("  " * (k + 1) + "if (x > %d) {\n" % (k - n) for k in range(n))
    closes = "".join("  " * (n - k) + "r = r + %d;\n" % k + "  " * (n - k) + "}\n" for k in range(n))
    return (PICK + "fn main() -> int32 {\n  var x: int64 = pick(3);\n  var r: int64 = 0;\n"
            + opens + closes + "  return (int32)(r & 127);\n}\n")


def locals_(n):
    decls = ["  var v%d: int64 = pick(%d);" % (k, k) for k in range(n)]
    sums = ["  r = r + v%d;" % k for k in range(n)]
    return (PICK + "fn main() -> int32 {\n" + "\n".join(decls)
            + "\n  var r: int64 = 0;\n" + "\n".join(sums) + "\n  return (int32)(r & 127);\n}\n")


def functions(n):
    fns = ["@noinline fn f%d(a: int64) -> int64 { return a * %d + 1; }" % (k, k + 2) for k in range(n)]
    calls = ["  r = r + f%d(r & 15);" % k for k in range(n)]
    return (PICK + "\n".join(fns) + "\nfn main() -> int32 {\n  var r: int64 = pick(1);\n"
            + "\n".join(calls) + "\n  return (int32)(r & 127);\n}\n")


def straight(n):
    body = ["  r = r * 3 + %d;\n  r = r ^ (r >> 7);" % k for k in range(n)]
    return (PICK + "fn main() -> int32 {\n  var r: int64 = pick(1);\n"
            + "\n".join(body) + "\n  return (int32)(r & 127);\n}\n")


def enum(n):
    variants = "\n".join("  V%d(int64)," % k for k in range(n))
    cases = "\n".join("    case V%d(b): { return b + %d; }" % (k, k) for k in range(n))
    return ("enum E {\n" + variants + "\n}\n" + PICK
            + "@noinline fn take(e: E) -> int64 {\n  match (e) {\n" + cases + "\n  }\n}\n"
            + "fn main() -> int32 {\n  var e: E = E.V%d(pick(5));\n  return (int32)(take(e) & 127);\n}\n"
            % (n // 2))


SHAPES = {
    "elseif": (elseif, 1500),
    "switch": (switch, 3000),
    "loops": (loops, 600),
    "nest": (nest, 200),
    "locals": (locals_, 2500),
    "functions": (functions, 1500),
    "straight": (straight, 2500),
    "enum": (enum, 1000),
}
MODES = {"d": [], "O": ["-O"], "r": ["--release"]}


class Counters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def measure(args, timeout):
    start = time.perf_counter()
    proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if os.name == "nt":
        try:
            _, err = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            return None, None, "timeout"
        seconds = time.perf_counter() - start
        counters = Counters()
        counters.cb = ctypes.sizeof(Counters)
        ctypes.windll.psapi.GetProcessMemoryInfo(int(proc._handle), ctypes.byref(counters), counters.cb)
        peak = counters.PeakPagefileUsage
        code = proc.returncode
    else:
        err = proc.stderr.read()
        _, status, usage = os.wait4(proc.pid, 0)
        proc.returncode = os.waitstatus_to_exitcode(status)
        seconds = time.perf_counter() - start
        peak = usage.ru_maxrss * 1024
        code = proc.returncode
    if code != 0:
        return None, None, err.decode("utf-8", "replace")[-300:]
    return seconds, peak, None


def run_shape(compiler, work, name, n, flags, timeout):
    make = SHAPES[name][0]
    path = os.path.join(work, "%s_%d.mettle" % (name, n))
    with open(path, "w") as handle:
        handle.write(make(n))
    obj = os.path.join(work, "out.obj")
    best = None
    peak = None
    for _ in range(2):
        seconds, used, why = measure([compiler, "--stdlib", os.path.join(ROOT, "stdlib")] + flags
                                     + ["-o", obj, path], timeout)
        if why:
            return None, None, why
        best = seconds if best is None else min(best, seconds)
        peak = used if peak is None else max(peak, used)
    return best, peak, None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.path.join(ROOT, "bin", "mettle.exe"))
    parser.add_argument("--shapes", default=",".join(SHAPES))
    parser.add_argument("--modes", default="d,r")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--max-ratio", type=float, default=3.0)
    parser.add_argument("--min-seconds", type=float, default=1.0)
    parser.add_argument("--min-bytes", type=float, default=200e6)
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix="scaling_")
    failures = []
    print("%-10s %-4s %7s %9s %9s %6s %9s %9s %6s"
          % ("shape", "mode", "n", "t(n)", "t(2n)", "ratio", "mem(n)", "mem(2n)", "ratio"))
    for name in args.shapes.split(","):
        n = max(2, int(SHAPES[name][1] * args.scale))
        for mode in args.modes.split(","):
            flags = MODES[mode]
            t1, m1, why = run_shape(args.compiler, work, name, n, flags, args.timeout)
            if why:
                failures.append("%s [%s] n=%d did not compile: %s" % (name, mode, n, why))
                continue
            t2, m2, why = run_shape(args.compiler, work, name, 2 * n, flags, args.timeout)
            if why:
                failures.append("%s [%s] n=%d did not compile: %s" % (name, mode, 2 * n, why))
                continue
            tr = t2 / max(t1, 1e-3)
            mr = m2 / max(m1, 1)
            print("%-10s %-4s %7d %8.2fs %8.2fs %6.2f %8.0fM %8.0fM %6.2f"
                  % (name, mode, n, t1, t2, tr, m1 / 1e6, m2 / 1e6, mr))
            if t2 >= args.min_seconds and tr > args.max_ratio:
                failures.append("%s [%s] time grows %.2fx when the input doubles (%.2fs -> %.2fs)"
                                % (name, mode, tr, t1, t2))
            if m2 >= args.min_bytes and mr > args.max_ratio:
                failures.append("%s [%s] memory grows %.2fx when the input doubles (%.0fM -> %.0fM)"
                                % (name, mode, mr, m1 / 1e6, m2 / 1e6))
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
