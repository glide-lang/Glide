#!/usr/bin/env python3
# Migrate `int` -> `i32` inside doc-comment CODE EXAMPLES only (the ```glide
# fenced blocks in `///` / `//` comments), so copy-pasteable examples match the
# canonical type names after the source migration. Prose `int` is left alone
# (reads as English shorthand for "integer"), and method names like
# `JsonValue::int` / `get_int` are preserved via the `::` and word-boundary
# carve-outs. `float` is never touched in comments (it's an English word).
#
#   python tools/migrate_int_docs.py <file.glide> [--write]
import sys
import re

COMMENT = re.compile(r"^\s*(///?|//)")


def repl_int(seg):
    # word-boundary `int` -> `i32`, skipping `::int` (method name).
    out = []
    i = 0
    n = len(seg)
    while i < n:
        c = seg[i]
        if c.isalpha() or c == "_":
            j = i
            while j < n and (seg[j].isalnum() or seg[j] == "_"):
                j += 1
            w = seg[i:j]
            after_colons = i >= 2 and seg[i - 1] == ":" and seg[i - 2] == ":"
            after_fn = re.search(r"\bfn\s+$", seg[:i]) is not None
            if w == "int" and not after_colons and not after_fn:
                out.append("i32")
            else:
                out.append(w)
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def migrate(text):
    out = []
    craw = 0
    in_fence = False
    for line in text.split("\n"):
        if craw > 0:
            craw += line.count("{") - line.count("}")
            out.append(line)
            continue
        if "c_raw!" in line and COMMENT.match(line) is None:
            idx = line.index("c_raw!")
            craw = line[idx:].count("{") - line[idx:].count("}")
            out.append(line)
            continue
        is_comment = COMMENT.match(line) is not None
        if is_comment and "```" in line:
            in_fence = not in_fence
            out.append(line)
            continue
        if is_comment and in_fence:
            out.append(repl_int(line))
        else:
            out.append(line)
    return "\n".join(out)


if __name__ == "__main__":
    path = sys.argv[1]
    text = open(path, encoding="utf-8", newline="").read()
    result = migrate(text)
    if len(sys.argv) > 2 and sys.argv[2] == "--write":
        open(path, "w", encoding="utf-8", newline="").write(result)
    else:
        sys.stdout.write(result)
