#!/usr/bin/env python3
"""quality_scan.py — levers for the ub-mode "Then Make it Right" phase.

Read-only static audit for the RDR2 co-op codebase:
  * function length / branch complexity (CRAP proxy, no coverage available)
  * structural duplication (dryer-lite: repeated 6-line shingles)
  * magic-number density
  * duplicate natives.h hashes/names
  * TODO/FIXME/HACK markers, disabled warnings, long lines
  * file inventory with LOC

Usage:
  python tools/quality_scan.py [--root E:/rdr2-coop] [--json out.json]
                               [--md docs/UB_MODE_BASELINE.md]
                               [--top 25] [--min-lines 25]

Exit code: 0 always (report tool), 2 on hard error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field, asdict

SOURCE_DIRS = ["src", "net", "server"]
EXTRA_FILES = ["tests"]
CPP_EXTS = {".cpp", ".h", ".hpp", ".cc"}

BRANCH_TOKEN = re.compile(
    r"\b(if|for|while|case|catch)\b|&&|\|\||\?"
)
FUNC_START = re.compile(
    r"^\s*(?:(?:static|inline|virtual|explicit|constexpr|const|extern|unsigned|signed|"
    r"struct|class|enum|typename|friend|template\b[^{;]*|[\w:]+(?:<[^;{]*>)?[\s*&:]+)+)"
    r"([A-Za-z_]\w*)\s*\(([^{;]*)\)\s*(?:const)?\s*(?:noexcept)?\s*\{?\s*$"
)
TODO_RE = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b")
WARN_DISABLE_RE = re.compile(r"#pragma\s+warning\s*\(\s*disable")
NUMBER_RE = re.compile(r"(?<![\w.])(-?\d+(?:\.\d+)?f?)(?![\w.])")


@dataclass
class Func:
    file: str
    name: str
    line: int
    lines: int = 0
    branches: int = 0
    params: int = 0

    @property
    def crap_proxy(self) -> float:
        comp = self.branches + 1
        return round((comp * comp) + comp, 1)


@dataclass
class FileStats:
    path: str
    loc: int = 0
    code_loc: int = 0
    long_lines: int = 0
    magic: int = 0
    todos: list = field(default_factory=list)
    warn_disables: int = 0
    funcs: list = field(default_factory=list)


def norm_path(root: str, path: str) -> str:
    return os.path.relpath(path, root).replace("\\", "/")


def is_code_line(line: str) -> bool:
    s = line.strip()
    return bool(s) and not s.startswith("//") and not s.startswith("*") and not s.startswith("/*")


def strip_comments_for_norm(line: str) -> str:
    s = line.split("//", 1)[0]
    s = re.sub(r"/\*.*?\*/", "", s)
    s = s.strip()
    s = re.sub(r"\s+", " ", s)
    return s


def scan_cpp(root: str, path: str, top: int) -> FileStats:
    rel = norm_path(root, path)
    fs = FileStats(path=rel)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    fs.loc = len(lines)
    depth = 0
    cur: Func | None = None
    cur_start_depth = 0
    cur_entered = False
    pending: Func | None = None
    pending_age = 0
    for idx, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        code = strip_comments_for_norm(line)
        if is_code_line(line):
            fs.code_loc += 1
            if len(line) > 120:
                fs.long_lines += 1
            fs.magic += len([n for n in NUMBER_RE.findall(line) if n not in ("0", "1", "2", "0.0", "1.0", "0.0f", "1.0f")])
        m = TODO_RE.search(line)
        if m:
            fs.todos.append((idx, m.group(1), line.strip()[:160]))
        if WARN_DISABLE_RE.search(line):
            fs.warn_disables += 1

        # A function candidate: header line with (...) that is not a control statement,
        # not a declaration ending with ';', and starting at file/namespace/class scope.
        if cur is None and pending is None and depth <= 2 and "(" in code and ")" in code:
            head = code.rstrip()
            is_ctrl = head.startswith(("if", "for", "while", "switch", "return", "else", "case", "&&", "||"))
            is_decl = head.endswith(";")
            fm = FUNC_START.match(line)
            if fm and not is_ctrl and not is_decl:
                params = [p for p in fm.group(2).split(",") if p.strip()]
                pending = Func(file=rel, name=fm.group(1), line=idx, params=len(params))
                pending_age = 0
                if "{" in line:
                    cur, pending = pending, None
                    cur_start_depth = depth
                    cur_entered = True
        elif pending is not None:
            pending_age += 1
            if "{" in code:
                cur, pending = pending, None
                cur_start_depth = depth
                cur_entered = True
            elif pending_age > 3 or (code and code.endswith(";")):
                pending = None

        if cur is not None:
            cur.lines += 1
            cur.branches += len(BRANCH_TOKEN.findall(code))

        depth += line.count("{") - line.count("}")
        if cur is not None and cur_entered and depth <= cur_start_depth:
            fs.funcs.append(cur)
            cur = None
            cur_entered = False
    return fs


def inventory(root: str, top: int):
    files = []
    for d in SOURCE_DIRS:
        base = os.path.join(root, d)
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if os.path.splitext(fn)[1].lower() in CPP_EXTS:
                    files.append(os.path.join(dirpath, fn))
    for d in EXTRA_FILES:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # skip build outputs / third-party copies
            dirnames[:] = [x for x in dirnames if x.lower() in ("", "src", "tests") or not x.startswith("build")]
            for fn in filenames:
                if os.path.splitext(fn)[1].lower() in CPP_EXTS:
                    files.append(os.path.join(dirpath, fn))
    return files


def duplication(files: list[str], root: str, window: int = 6, top: int = 15):
    shingles = defaultdict(list)  # hash -> list[(file, startline)]
    for path in files:
        rel = norm_path(root, path)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                raw = f.readlines()
        except OSError:
            continue
        norm = [strip_comments_for_norm(l) for l in raw]
        # drop blank/brace-only lines to make duplicates robust
        keep = [(i + 1, n) for i, n in enumerate(norm) if n and n not in ("{", "}", "};")]
        for i in range(0, len(keep) - window):
            chunk = "\n".join(k[1] for k in keep[i:i + window])
            if len(chunk) < 120:
                continue
            h = hashlib.sha1(chunk.encode("utf-8")).hexdigest()
            shingles[h].append((rel, keep[i][0]))
    dups = []
    for h, locs in shingles.items():
        if len(locs) < 2:
            continue
        by_file = defaultdict(list)
        for rel, line in locs:
            by_file[rel].append(line)
        dups.append({
            "hash": h[:12],
            "count": len(locs),
            "locations": [f"{f}:{min(ls)}" for f, ls in sorted(by_file.items())][:8],
        })
    dups.sort(key=lambda d: (-d["count"], d["locations"][0]))
    return dups[:top]


def natives_check(root: str):
    path = os.path.join(root, "src", "natives.h")
    if not os.path.exists(path):
        return {"missing": True}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    decls = re.findall(
        r"constexpr\s+unsigned\s+long\s+long\s+([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+)",
        text,
    )
    by_hash = defaultdict(list)
    by_name = defaultdict(list)
    for name, h in decls:
        by_hash[h.lower()].append(name)
        by_name[name].append(h.lower())
    dup_hashes = {h: names for h, names in by_hash.items() if len(names) > 1}
    dup_names = {n: hs for n, hs in by_name.items() if len(hs) > 1}
    return {
        "count": len(decls),
        "duplicate_hash_groups": len(dup_hashes),
        "duplicate_name_groups": len(dup_names),
        "examples": [{"hash": h, "names": n} for h, n in list(dup_hashes.items())[:10]],
    }


def render_md(report: dict, top: int) -> str:
    out = []
    out.append("# ub-mode quality baseline\n")
    out.append("Сгенерировано `tools/quality_scan.py`. Read-only статический аудит; "
               "CRAP-прокси = (branches+1)^2+(branches+1) без покрытия (худший случай).\n")
    out.append("## Inventory\n")
    inv = report["inventory"]
    out.append("| файл | LOC | code LOC | строк>120 | magic | TODO | warning-disable | функций |")
    out.append("|---|---|---|---|---|---|---|---|")
    for f in sorted(inv, key=lambda x: -x["loc"]):
        out.append(f"| `{f['path']}` | {f['loc']} | {f['code_loc']} | {f['long_lines']} | "
                   f"{f['magic']} | {len(f['todos'])} | {f['warn_disables']} | {len(f['funcs'])} |")
    out.append("")
    funcs = report["functions"]
    out.append(f"## Топ {top} функций по CRAP-прокси\n")
    out.append("| CRAP | branches | lines | params | функция |")
    out.append("|---|---|---|---|---|")
    for fn in funcs[:top]:
        out.append(f"| {fn['crap_proxy']} | {fn['branches']} | {fn['lines']} | {fn['params']} | "
                   f"`{fn['file']}:{fn['line']}` {fn['name']}` |")
    out.append("")
    longf = report["long_functions"][:top]
    out.append(f"## Топ {top} функций по длине\n")
    out.append("| lines | branches | CRAP | функция |")
    out.append("|---|---|---|---|")
    for fn in longf:
        out.append(f"| {fn['lines']} | {fn['branches']} | {fn['crap_proxy']} | "
                   f"`{fn['file']}:{fn['line']}` {fn['name']}` |")
    out.append("")
    out.append(f"## Дублирование (6-строчные шинглы, топ {top})\n")
    out.append("| повторений | места |")
    out.append("|---|---|")
    for d in report["duplication"]:
        out.append(f"| {d['count']} | {', '.join('`' + x + '`' for x in d['locations'])} |")
    out.append("")
    nat = report["natives"]
    out.append("## natives.h\n")
    if nat.get("missing"):
        out.append("не найден")
    else:
        out.append(f"- объявлений: {nat['count']}")
        out.append(f"- групп с одинаковым хешем: {nat['duplicate_hash_groups']}")
        out.append(f"- групп с одинаковым именем: {nat['duplicate_name_groups']}")
        for ex in nat["examples"][:10]:
            out.append(f"  - `{ex['hash']}`: {', '.join(ex['names'])}")
    out.append("")
    marks = report["markers"]
    out.append("## TODO/FIXME/HACK\n")
    out.append(f"Всего маркеров: {marks['todos_total']}; warning-disable: {marks['warn_disables_total']}")
    for t in marks["todos"][:40]:
        out.append(f"- `{t['file']}:{t['line']}` {t['kind']}: {t['text']}")
    out.append("")
    out.append("## Зачем это\n")
    out.append("Baseline нужен, чтобы каждая единица шлифовки (ub-mode) имела проверяемую "
               "цель: уменьшить CRAP/длину/дублирование, убрать мёртвый код, не меняя поведение "
               "(build + CI 41/41). Повторный запуск после шлифовки даёт дельту.")
    return "\n".join(out) + "\n"


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="ub-mode static quality audit")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--json", default="")
    ap.add_argument("--md", default="")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    files = inventory(root, args.top)
    stats = [scan_cpp(root, p, args.top) for p in files]
    all_funcs = [fn for st in stats for fn in st.funcs]
    def fn_dict(fn):
        d = asdict(fn)
        d["crap_proxy"] = fn.crap_proxy
        return d

    report = {
        "root": root,
        "inventory": [asdict(st) for st in stats],
        "functions": [fn_dict(fn) for fn in sorted(all_funcs, key=lambda x: -x.crap_proxy)],
        "long_functions": [fn_dict(fn) for fn in sorted(all_funcs, key=lambda x: -x.lines)],
        "duplication": duplication([p for p in files], root, top=args.top),
        "natives": natives_check(root),
        "markers": {
            "todos_total": sum(len(st.todos) for st in stats),
            "warn_disables_total": sum(st.warn_disables for st in stats),
            "todos": [{"file": st.path, "line": ln, "kind": k, "text": t}
                      for st in stats for (ln, k, t) in st.todos][:80],
        },
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print(f"json: {args.json}")
    md = render_md(report, args.top)
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(md)
        print(f"md: {args.md}")
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
