#!/usr/bin/env python3
"""Generate the AudioPlayer.reds declaration file from main.cpp.

Parses the `CClassStaticFunction::Create` / `AddParam` / `SetReturnType` /
`RegisterFunction` calls and emits a native-class `.reds` wrapper that
matches the registered RTTI signatures exactly.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

CLASS_RE = re.compile(r'TTypedClass<\s*(\w+)\s*>\s+s_type\s*\(\s*"([^"]+)"\s*\)')
CREATE_RE = re.compile(
    r'auto\*\s*(\w+)\s*=\s*CClassStaticFunction::Create\s*\(\s*'
    r'&s_type\s*,\s*"([^"]+)"\s*,\s*"[^"]+"\s*,'
)
PARAM_RE = re.compile(r'(\w+)->AddParam\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')
RET_RE = re.compile(r'(\w+)->SetReturnType\(\s*"([^"]+)"\s*\)')
REG_RE = re.compile(r's_type\.RegisterFunction\(\s*(\w+)\s*\)')


def parse(source: str):
    m = CLASS_RE.search(source)
    if not m:
        raise SystemExit("gen_reds: could not find TTypedClass<...> s_type definition")
    class_name = m.group(2)

    funcs = {}
    order = []
    for m in CREATE_RE.finditer(source):
        var, name = m.group(1), m.group(2)
        funcs[var] = {"name": name, "params": [], "ret": None, "registered": False}
        order.append(var)
    for m in PARAM_RE.finditer(source):
        var, typ, pname = m.group(1), m.group(2), m.group(3)
        if var in funcs:
            funcs[var]["params"].append((typ, pname))
    for m in RET_RE.finditer(source):
        var, typ = m.group(1), m.group(2)
        if var in funcs:
            funcs[var]["ret"] = typ
    for m in REG_RE.finditer(source):
        var = m.group(1)
        if var in funcs:
            funcs[var]["registered"] = True

    return class_name, [funcs[v] for v in order if funcs[v]["registered"]]


def emit(class_name: str, funcs) -> str:
    lines = [f"public native class {class_name} extends IScriptable {{"]
    for f in funcs:
        params = ", ".join(f"{n}: {t}" for t, n in f["params"])
        sig = f"\tpublic static native func {f['name']}({params})"
        if f["ret"]:
            sig += f" -> {f['ret']}"
        lines.append(sig)
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        print("usage: gen_reds.py <main.cpp> <out.reds>", file=sys.stderr)
        return 2
    src = Path(sys.argv[1]).read_text(encoding="utf-8")
    out = Path(sys.argv[2])
    class_name, funcs = parse(src)
    if not funcs:
        raise SystemExit("gen_reds: no registered native functions found")
    content = emit(class_name, funcs)
    if out.exists() and out.read_text(encoding="utf-8") == content:
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
