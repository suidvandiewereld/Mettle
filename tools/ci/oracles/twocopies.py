import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
MODES = {
    "d": [],
    "O": ["-O"],
    "r": ["--release"],
    "safe": ["--safe"],
    "s": ["-s"],
    "co": ["--check-overflow"],
}


def corpus(limit):
    found = []
    for top in ("tests", "examples"):
        for directory, _, names in os.walk(os.path.join(ROOT, top)):
            for name in names:
                if name.endswith(".mettle"):
                    found.append(os.path.join(directory, name))
    found.sort()
    return found[:limit] if limit else found


def compile_one(exe, flags, source, obj, env):
    if os.path.exists(obj):
        os.remove(obj)
    args = [exe, "--stdlib", os.path.join(ROOT, "stdlib")] + flags + ["-o", obj, source]
    try:
        done = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=300)
    except subprocess.TimeoutExpired:
        return None
    if done.returncode != 0 or not os.path.exists(obj):
        return None
    with open(obj, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.path.join(ROOT, "bin", "mettle.exe"))
    parser.add_argument("--modes", default="d,O,r,safe")
    parser.add_argument("-j", type=int, default=8)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix="twocopies_")
    near_dir = os.path.join(work, "a")
    far_dir = os.path.join(work, "a_copy_placed_at_a_much_longer_path_" + "x" * 48)
    os.makedirs(near_dir)
    os.makedirs(far_dir)
    exe_name = os.path.basename(args.compiler)
    near = os.path.join(near_dir, exe_name)
    far = os.path.join(far_dir, exe_name)
    shutil.copy2(args.compiler, near)
    shutil.copy2(args.compiler, far)

    near_env = {k: v for k, v in os.environ.items() if not k.startswith("METTLE_")}
    far_env = dict(near_env)
    far_env["TWOCOPIES_PAD"] = "p" * 8192

    modes = args.modes.split(",")
    jobs = [(src, mode) for src in corpus(args.limit) for mode in modes]

    def job(indexed):
        index, (source, mode) = indexed
        slot = os.path.join(work, "w%d" % threading.get_ident())
        os.makedirs(slot, exist_ok=True)
        obj = os.path.join(slot, "out.obj")
        first = compile_one(near, MODES[mode], source, obj, near_env)
        if first is None:
            return source, mode, "skip"
        second = compile_one(far, MODES[mode], source, obj, far_env)
        if second is None:
            return source, mode, "second copy failed to compile"
        if args.selftest:
            second = second[:-1] + bytes([second[-1] ^ 1])
        return source, mode, "same" if first == second else "differ"

    counts = {}
    bad = []
    with cf.ThreadPoolExecutor(args.j) as pool:
        for source, mode, verdict in pool.map(job, enumerate(jobs)):
            counts[verdict] = counts.get(verdict, 0) + 1
            if verdict not in ("same", "skip"):
                bad.append("%s [%s] %s" % (os.path.relpath(source, ROOT), mode, verdict))
    shutil.rmtree(work, ignore_errors=True)

    for line in bad:
        print(line)
    print("identical=%d differing=%d skipped=%d second-failed=%d"
          % (counts.get("same", 0), counts.get("differ", 0), counts.get("skip", 0),
             counts.get("second copy failed to compile", 0)))
    if args.selftest:
        if counts.get("differ", 0) == 0 or counts.get("same", 0) != 0:
            print("selftest: a flipped byte went unnoticed")
            return 1
        print("selftest: every flipped byte was seen")
        return 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
