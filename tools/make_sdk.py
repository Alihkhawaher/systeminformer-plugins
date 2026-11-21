"""Build a System Informer plugin SDK (headers + import library) without running
System Informer's own build tooling.

Mirrors tools/CustomBuildTool (Build.cs BuildSdk + HeaderGen.cs) at the vendored tag:
  - copies phlib/include and kphlib/include headers
  - generates phapppub.h from SystemInformer/include using the "phapppub" markers
  - copies phdk.h, phappres.h and writes phappresource.h (ID -> PHAPP_ID)
  - makes SystemInformer.lib from the exports of the INSTALLED SystemInformer.exe,
    so the plugin binds to exactly the binary it will load into.

Usage: python tools/make_sdk.py [path\\to\\SystemInformer.exe]
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SI = os.path.join(ROOT, "vendor", "systeminformer")
SDK = os.path.join(ROOT, "sdk")
INC = os.path.join(SDK, "include")
LIB = os.path.join(SDK, "lib")
EXE = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\SystemInformer\SystemInformer.exe"

FILES = [
    "phapp.h", "appsup.h", "phfwddef.h", "phsettings.h", "procprv.h", "srvprv.h", "netprv.h",
    "modprv.h", "thrdprv.h", "hndlprv.h", "memprv.h", "devprv.h", "phuisup.h", "colmgr.h",
    "proctree.h", "srvlist.h", "netlist.h", "thrdlist.h", "modlist.h", "hndllist.h", "memlist.h",
    "extmgr.h", "mainwnd.h", "notifico.h", "phplug.h", "actions.h", "procprp.h", "procprpp.h",
    "phsvccl.h", "sysinfo.h", "procgrp.h", "miniinfo.h", "hndlmenu.h",
]
MODES = ["phapppub"]
NOTICE = ("/*\r\n * Copyright (c) Winsider Seminars & Solutions, Inc.  All rights reserved.\r\n *\r\n"
          " * This file is part of System Informer.\r\n *\r\n */\r\n\r\n")
HEADER = ("#ifndef _PH_PHAPPPUB_H\r\n#define _PH_PHAPPPUB_H\r\n\r\n// This file was automatically generated. "
          "Do not edit.\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n")
FOOTER = "\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif\r\n"


def read_lines(path):
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return f.read().splitlines()


def process(lines):
    out, active, blank = [], set(), False
    markers = ["// " + m for m in MODES]
    for line in lines:
        s = line.strip()
        if not s:
            blank = True
            continue
        low = s.lower()
        if low.startswith("// begin_"):
            active.add(low[len("// begin_"):])
        elif low.startswith("// end_"):
            active.discard(low[len("// end_"):])
        else:
            block = any(m in active for m in MODES)
            linemode = False
            if not block:
                for mk in markers:
                    i = low.rfind(mk)
                    if i != -1 and all(c.isalnum() or c in " /" for c in s[i:].strip()):
                        linemode = True
                        break
            if block or linemode:
                if blank and out:
                    out.append("")
                out.append(line)
                blank = False
    return out


def gen_phapppub():
    base = os.path.join(SI, "SystemInformer", "include")
    headers = {n.lower(): {"name": n, "lines": read_lines(os.path.join(base, n))} for n in FILES}
    for h in headers.values():
        deps, kept = [], []
        for line in h["lines"]:
            s = line.strip()
            m = re.fullmatch(r"#include <(.+)>", s, re.I)
            if m and m.group(1).lower() in headers:
                d = headers[m.group(1).lower()]
                if d not in deps:
                    deps.append(d)
                continue
            kept.append(line)
        h["lines"], h["deps"] = kept, deps
    order, done = [], set()

    def visit(h):
        if h["name"].lower() in done:
            return
        done.add(h["name"].lower())
        for d in h["deps"]:
            visit(d)
        order.append(h)

    for n in FILES:
        visit(headers[n.lower()])
    parts = [NOTICE, HEADER]
    for h in order:
        parts.append("\r\n//\r\n// %s\r\n//\r\n\r\n" % os.path.splitext(h["name"])[0])
        parts.extend(l + "\r\n" for l in process(h["lines"]))
    parts.append(FOOTER)
    with open(os.path.join(INC, "phapppub.h"), "w", encoding="utf-8", newline="") as f:
        f.write("".join(parts))


def find_vs_tool(name):
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    vs = subprocess.check_output([vswhere, "-latest", "-property", "installationPath"], text=True).strip()
    msvc = os.path.join(vs, "VC", "Tools", "MSVC")
    ver = sorted(os.listdir(msvc))[-1]
    return os.path.join(msvc, ver, "bin", "Hostx64", "x64", name)


def make_import_lib():
    # SystemInformer.exe exports by ordinal only (NONAME). CustomBuildTool ExportDefinitions gives
    # every "    Name" line of SystemInformer.def the next ordinal from 1001, in file order.
    lines = read_lines(os.path.join(SI, "SystemInformer", "SystemInformer.def"))
    entries = []
    for line in lines:
        if line.startswith("    ") and line.strip():
            parts = line.split()
            entries.append((parts[0], "DATA" in (p.upper() for p in parts[1:])))
    dump = subprocess.check_output([find_vs_tool("dumpbin.exe"), "/exports", EXE], text=True, errors="replace")
    m = re.search(r"(\d+) number of functions", dump)
    base = re.search(r"(\d+) ordinal base", dump)
    if not m or int(m.group(1)) != len(entries) or int(base.group(1)) != 1001:
        raise SystemExit("export count mismatch: def has %d, exe has %s" % (len(entries), m and m.group(1)))
    deffile = os.path.join(LIB, "SystemInformer.def")
    with open(deffile, "w") as f:
        f.write("LIBRARY SystemInformer.exe\nEXPORTS\n")
        for i, (name, data) in enumerate(entries):
            f.write("    %s @%d NONAME%s\n" % (name, 1001 + i, " DATA" if data else ""))
    subprocess.check_call([find_vs_tool("lib.exe"), "/nologo", "/machine:x64", "/def:" + deffile,
                           "/out:" + os.path.join(LIB, "SystemInformer.lib")])
    print("import lib: %d ordinal exports" % len(entries))

def main():
    if os.path.isdir(SDK):
        shutil.rmtree(SDK)
    os.makedirs(INC)
    os.makedirs(LIB)
    for sub in ("phlib", "kphlib"):
        src = os.path.join(SI, sub, "include")
        for n in os.listdir(src):
            if n.endswith(".h"):
                shutil.copy2(os.path.join(src, n), INC)
    gen_phapppub()
    shutil.copy2(os.path.join(SI, "SystemInformer", "sdk", "phdk.h"), INC)
    shutil.copy2(os.path.join(SI, "SystemInformer", "include", "phappres.h"), INC)
    res = open(os.path.join(SI, "SystemInformer", "resource.h"), encoding="utf-8-sig", errors="replace").read()
    with open(os.path.join(INC, "phappresource.h"), "w", encoding="utf-8") as f:
        f.write(re.sub(r"#define ID", "#define PHAPP_ID", res, flags=re.I))
    make_import_lib()


if __name__ == "__main__":
    main()
