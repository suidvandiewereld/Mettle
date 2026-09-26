import subprocess, sys, os
src = open(sys.argv[1]).read().split("\n")
old, new = sys.argv[2], sys.argv[3]
work = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lr")
os.makedirs(work, exist_ok=True)
def rc(comp, lines, flags, tag):
    p = os.path.join(work, f"{tag}.mettle"); e = os.path.join(work, f"{tag}.exe")
    open(p, "w", newline="\n").write("\n".join(lines))
    if subprocess.run([comp, "--build"] + flags + [p, "-o", e], capture_output=True).returncode != 0:
        return "bf"
    try:
        return subprocess.run([e], capture_output=True, timeout=10).returncode
    except subprocess.TimeoutExpired:
        return "to"
def ok(lines):
    return rc(new, lines, [], "d") == 0 and rc(old, lines, ["-O"], "o") not in (0, "bf", "to") and rc(new, lines, ["-O"], "n") == 0
assert ok(src)
chunk = len(src) // 2
while chunk >= 1:
    i = 0
    progress = False
    while i < len(src):
        cand = src[:i] + src[i + chunk:]
        if ok(cand):
            src = cand
            progress = True
        else:
            i += chunk
    if not progress:
        chunk //= 2
open(sys.argv[4], "w", newline="\n").write("\n".join(src))
print(len(src))
