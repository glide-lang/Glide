#!/usr/bin/env python3
# Analysis-only: for every legacy numeric-type-name occurrence in code
# (skipping c_raw blocks, string/char literals and // comments), report the
# short preceding context so we can tell type positions from identifier uses.
import sys, os, re

NAMES = {"int", "uint", "long", "ulong", "float"}


def strip_strings(seg):
    # replace string/char literal bodies with spaces so they don't match
    out = []
    i = 0
    n = len(seg)
    while i < n:
        c = seg[i]
        if c == '"' or c == "'":
            q = c
            out.append(" ")
            i += 1
            while i < n:
                if seg[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if seg[i] == q:
                    out.append(" ")
                    i += 1
                    break
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def comment_pos(seg):
    i = 0
    n = len(seg)
    while i < n:
        c = seg[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if seg[i] == "\\":
                    i += 2
                    continue
                if seg[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and seg[i + 1] == "/":
            return i
        i += 1
    return -1


def classify_prefix(prefix):
    p = prefix.rstrip()
    # strip a single leading type-modifier run (* ? !) attached to the name
    pm = p
    while pm.endswith("*") or pm.endswith("?") or pm.endswith("!"):
        pm = pm[:-1].rstrip()
    if pm.endswith(":"):
        return "TYPE :"
    if pm.endswith("->"):
        return "TYPE ->"
    if pm.endswith("<"):
        return "TYPE <"
    if re.search(r"\bas$", pm):
        return "TYPE as"
    if re.search(r"\bsizeof\($", pm):
        return "TYPE sizeof("
    if re.search(r"\bfn\($", pm):
        return "TYPE fn("
    if pm.endswith(","):
        return "AMBIG ,"
    if pm.endswith("("):
        return "IDENT ("
    if pm.endswith("::"):
        return "IDENT ::"
    if pm.endswith("."):
        return "IDENT ."
    if pm.endswith("="):
        return "AMBIG ="
    if pm == "":
        return "AMBIG bol"
    return "OTHER[" + (pm[-3:] if len(pm) >= 3 else pm) + "]"


def process(path, counts, samples):
    text = open(path, encoding="utf-8").read()
    craw = 0
    for ln, line in enumerate(text.split("\n"), 1):
        if craw > 0:
            craw += line.count("{") - line.count("}")
            continue
        cpos = comment_pos(line)
        code = line if cpos < 0 else line[:cpos]
        if "c_raw!" in code:
            idx = code.index("c_raw!")
            craw = code[idx:].count("{") - code[idx:].count("}")
            continue
        scan = strip_strings(code)
        for m in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*", scan):
            w = m.group(0)
            if w in NAMES:
                ctx = classify_prefix(scan[: m.start()])
                key = (w, ctx)
                counts[key] = counts.get(key, 0) + 1
                if ctx.startswith("IDENT") or ctx.startswith("AMBIG") or ctx.startswith("OTHER"):
                    if len(samples.setdefault(key, [])) < 4:
                        samples[key].append(f"{path}:{ln}: {line.strip()[:90]}")


if __name__ == "__main__":
    roots = sys.argv[1:] or ["src", "bootstrap", "examples"]
    files = []
    for r in roots:
        for dp, _, fns in os.walk(r):
            for fn in fns:
                if fn.endswith(".glide"):
                    files.append(os.path.join(dp, fn))
    counts = {}
    samples = {}
    for f in files:
        process(f, counts, samples)
    print("=== counts by (name, context) ===")
    for k in sorted(counts, key=lambda k: (k[0], -counts[k])):
        print(f"  {counts[k]:5d}  {k[0]:6s} {k[1]}")
    print("\n=== samples for IDENT/AMBIG/OTHER (the dangerous ones) ===")
    for k in sorted(samples):
        for s in samples[k]:
            print(f"  [{k[0]} {k[1]}] {s}")
