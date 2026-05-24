#!/usr/bin/env python3
# Migrate `int`->`i32` / `float`->`f64` inside string-literal CODE EXAMPLES
# (hover docs, doc-gen output). Only rewrites a `"..."` literal when it looks
# like it embeds Glide code (contains `let `, `-> `, `struct `, `fn `, a
# `Vector<`, or `: ` type annotation) so bare recognizer strings such as
# `w.eq("int")` are left untouched. Method names (`::int`) are preserved.
#
#   python tools/migrate_int_doc_strings.py <file.glide> [--write]
import sys
import re

CODE = (re.compile(r"let |-> |struct |fn |Vector<|chan<|: \*?\??!?[A-Za-z]"))


def repl_word(seg):
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
            if w == "int" and not after_colons:
                out.append("i32")
            elif w == "float" and not after_colons:
                out.append("f64")
            else:
                out.append(w)
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def migrate(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            buf = ['"']
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j]); buf.append(text[j + 1]); j += 2; continue
                buf.append(text[j])
                if text[j] == '"':
                    j += 1; break
                j += 1
            body = "".join(buf)
            if CODE.search(body):
                out.append(repl_word(body))
            else:
                out.append(body)
            i = j
            continue
        # line comment: copy to EOL untouched
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(text[i]); i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


if __name__ == "__main__":
    path = sys.argv[1]
    text = open(path, encoding="utf-8", newline="").read()
    result = migrate(text)
    if len(sys.argv) > 2 and sys.argv[2] == "--write":
        open(path, "w", encoding="utf-8", newline="").write(result)
    else:
        sys.stdout.write(result)
