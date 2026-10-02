#!/usr/bin/env python3
import json
import re
from pathlib import Path

TRANSCRIPT = Path(
    r"C:\Users\Marquis\.cursor\projects\g-Projects-Mettle\agent-transcripts"
    r"\98597754-fe35-4e44-8d45-8a8f013d787a\98597754-fe35-4e44-8d45-8a8f013d787a.jsonl"
)
OUT = Path(__file__).resolve().parents[1] / "src/ir/recovery/helpers_indexed.c"

FUNCS = [
    "ir_match_forward_i32_index",
    "ir_resolve_indexed_address_temp",
    "ir_ptr_induction_iv_start_value",
    "ir_binary_is_unit_increment_of_iv",
    "ir_find_loop_unit_increment",
]

def extract_function(text: str, name: str) -> str | None:
    pat = rf"(static int {re.escape(name)}\([^{{]*\{{)"
    m = re.search(pat, text, re.DOTALL)
    if not m:
        return None
    start = m.start()
    depth = 0
    for i in range(m.end() - 1, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    return None

best: dict[str, tuple[int, str]] = {}
for line_no, line in enumerate(TRANSCRIPT.read_text(encoding="utf-8").splitlines()):
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("role") != "assistant":
        continue
    for part in obj.get("message", {}).get("content", []):
        if part.get("type") != "tool_use" or part.get("name") != "StrReplace":
            continue
        ns = part.get("input", {}).get("new_string", "")
        for fn in FUNCS:
            body = extract_function(ns, fn)
            if body and (fn not in best or len(body) > len(best[fn][1])):
                best[fn] = (line_no, body)

chunks = []
for fn in FUNCS:
    if fn in best:
        print(f"{fn}: line {best[fn][0]}, {len(best[fn][1])} chars")
        chunks.append(best[fn][1])
    else:
        print(f"MISSING {fn}")

OUT.write_text("\n\n".join(chunks) + "\n", encoding="utf-8", newline="\n")
print(f"Wrote {OUT}")
