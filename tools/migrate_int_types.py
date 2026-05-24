#!/usr/bin/env python3
# Migrate legacy numeric type names to the canonical fixed-width family in
# Glide source. Phase 1 normalises these identically, so the change is purely
# textual (int already == i32, float already == f64 at compile time).
#
# Only `int` and `float` are migrated: a context scan proved they appear only
# in type position in the source, with one carve-out each (the JSON ctor
# methods `fn int(...)` / `fn float(...)`, whose *name* must stay). The other
# legacy names are left alone: `long`/`ulong`/`uint` have NO type uses in the
# source (every `long` is an identifier in argparse), so touching them would
# only corrupt identifiers.
#
# Skips: c_raw! { ... } blocks (C code), string/char literals, // line and
# /* */ block comments, and the identifier right after the `fn` keyword.
#
#   python tools/migrate_int_types.py <file.glide> [--write]
import sys

MAP = {"int": "i32", "float": "f64"}


def migrate(text):
    out = []
    i = 0
    n = len(text)
    prev_word = None  # last identifier token, for the `fn <name>` carve-out
    while i < n:
        c = text[i]

        # ---- c_raw! { ... } : copy verbatim by brace depth ----
        if text.startswith("c_raw!", i):
            j = i + 6
            out.append(text[i:j])
            while j < n and text[j] != "{":
                out.append(text[j])
                j += 1
            if j < n:
                depth = 0
                while j < n:
                    out.append(text[j])
                    if text[j] == "{":
                        depth += 1
                    elif text[j] == "}":
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
            i = j
            prev_word = None
            continue

        # ---- // line comment ----
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(text[i])
                i += 1
            continue

        # ---- /* block comment */ ----
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("/*")
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    out.append("*/")
                    i += 2
                    break
                out.append(text[i])
                i += 1
            continue

        # ---- string / char literal ----
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            prev_word = None
            continue

        # ---- identifier ----
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            w = text[i:j]
            if w in MAP and prev_word != "fn":
                out.append(MAP[w])
            else:
                out.append(w)
            prev_word = w
            i = j
            continue

        # ---- other char ----
        if not c.isspace():
            prev_word = None  # only an immediately-preceding `fn` carves out
        out.append(c)
        i += 1
    return "".join(out)


if __name__ == "__main__":
    path = sys.argv[1]
    # newline="" preserves CRLF/LF exactly (no universal-newline translation),
    # so migration never rewrites line endings.
    text = open(path, encoding="utf-8", newline="").read()
    result = migrate(text)
    if len(sys.argv) > 2 and sys.argv[2] == "--write":
        open(path, "w", encoding="utf-8", newline="").write(result)
    else:
        sys.stdout.write(result)
