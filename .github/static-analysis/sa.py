#!/usr/bin/env python3
"""Helpers of the static analysis workflow (.github/workflows/static-analysis.yml).

  sa.py compdb    RAW SRC OUT [EXTRA...]  compilation database of the player
  sa.py gcc       DB SRC OUT              gcc -fanalyzer over the database
  sa.py tidy      DB SRC OUT CHECKS       clang-tidy over the database
  sa.py cppdb     DB OUT                  the database for cppcheck
  sa.py cppcheck  XML SRC OUT            cppcheck XML report -> SARIF
  sa.py scanbuild DIR SRC OUT             merge the SARIF files of scan-build
  sa.py summary   DIR [BASE LABEL]        Markdown summary of the SARIF files,
                                          with BASE: new findings first

Every tool ends up as one SARIF file (one run, paths relative to the source
tree) plus a tab-separated list of its findings next to it. Each finding
carries the text of its source line (properties.sourceLine), so that the
same finding of another revision is recognised when lines have moved. The
commands exit non-zero only when a tool itself fails (a file does not
compile, the tool crashes), never because of findings.

Python 3 standard library only.
"""

import collections
import concurrent.futures
import glob
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SARIF_SCHEMA = "https://json.schemastore.org/sarif-2.1.0.json"
# the analysis covers the player itself; the bundled third-party code and
# the tests are compiled but their findings are dropped
SKIP_DIRS = ("external/", "tests/")
JOBS = os.cpu_count() or 2


def log(*args):
    print(*args, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# compilation database

def source_args(args):
    """Drop sources, objects, libraries, output and link flags of a combined
    compile-and-link command, so that it can be used for one file."""
    out, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a in ("-c", "-pthread") or a.startswith(("-l", "-L", "-Wl,")):
            continue
        if re.search(r"\.(c|a|o|so)$", a) and not a.startswith("-"):
            continue
        out.append(a)
    return [args[0]] + out


def absolute_args(args, directory):
    """Make the include paths of a command absolute."""
    out, it = [], iter(args)
    for a in it:
        for opt in ("-I", "-iquote", "-isystem", "-include"):
            if a == opt:
                out += [a, os.path.normpath(os.path.join(directory, next(it)))]
                break
            if a.startswith(opt) and opt != "-include":
                out.append(opt + os.path.normpath(os.path.join(directory, a[len(opt):])))
                break
        else:
            out.append(a)
    return out


def cmd_compdb(raw, src, out, *extra):
    """RAW: database written by bear around tests/build.sh. EXTRA: further
    receiver-only sources, compiled with the flags of the first host entry
    plus EXTRA_FLAGS (environment). Writes OUT with one entry per player
    source (no external/, no tests/), arguments absolute."""
    src = os.path.abspath(src)
    entries = json.load(open(raw))
    db, seen = [], set()
    base = None
    for e in entries:
        args = e.get("arguments") or shlex.split(e["command"])
        f = os.path.relpath(os.path.join(e["directory"], e["file"]), src)
        if f.startswith(SKIP_DIRS) or f.startswith("..") or f in seen:
            continue
        seen.add(f)
        args = absolute_args(source_args(args), e["directory"])
        if base is None:
            base = args
        db.append({"directory": src, "file": os.path.join(src, f),
                   "arguments": args + ["-c", os.path.join(src, f)]})
    if not db:
        log("no player source in", raw)
        return 1
    extra_flags = shlex.split(os.environ.get("EXTRA_FLAGS", ""))
    for f in extra:
        if f in seen or not os.path.isfile(os.path.join(src, f)):
            continue
        args = base + extra_flags
        probe = subprocess.run(args + ["-fsyntax-only", os.path.join(src, f)], cwd=src,
                               capture_output=True, text=True)
        if probe.returncode:
            first = next((l for l in probe.stderr.splitlines() if "error" in l), "?")
            log(f"skipped {f} (does not compile on the host): {first}")
            continue
        seen.add(f)
        db.append({"directory": src, "file": os.path.join(src, f),
                   "arguments": args + ["-c", os.path.join(src, f)]})
    json.dump(db, open(out, "w"), indent=1)
    log(len(db), "sources:", " ".join(sorted(seen)))
    return 0


# --------------------------------------------------------------------------
# SARIF helpers

def rel_uri(uri, src):
    if uri.startswith("file://"):
        uri = uri[7:]
    if os.path.isabs(uri):
        r = os.path.relpath(uri, src)
        if not r.startswith(".."):
            return r.replace(os.sep, "/")
    return uri


def relativize(node, src):
    """Make every artifactLocation of a SARIF tree relative to SRC."""
    if isinstance(node, dict):
        if isinstance(node.get("uri"), str):
            new = rel_uri(node["uri"], src)
            if new != node["uri"]:
                node["uri"] = new
                node["uriBaseId"] = "%SRCROOT%"
        for v in node.values():
            relativize(v, src)
    elif isinstance(node, list):
        for v in node:
            relativize(v, src)


def location(result):
    try:
        pl = result["locations"][0]["physicalLocation"]
        return pl["artifactLocation"]["uri"], pl.get("region", {}).get("startLine", 0)
    except (KeyError, IndexError):
        return "", 0


def keep(result):
    uri, _ = location(result)
    return bool(uri) and not os.path.isabs(uri) and not uri.startswith(SKIP_DIRS)


_LINES = {}


def source_line(src, uri, line):
    """Text of line LINE of SRC/URI, white space collapsed ("" if unknown)."""
    path = os.path.join(src, uri)
    cache = _LINES
    if path not in cache:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                cache[path] = f.read().splitlines()
        except OSError:
            cache[path] = []
    lines = cache[path]
    return " ".join(lines[line - 1].split()) if 0 < line <= len(lines) else ""


def write_sarif(out, name, version, results, src, rules=None, info_uri=None):
    """One run, findings outside the player dropped, duplicates (headers
    seen from several files) merged, the source line added; plus OUT.tsv."""
    uniq, seen = [], set()
    for r in results:
        if not keep(r):
            continue
        k = (r.get("ruleId"), location(r), r.get("message", {}).get("text"))
        if k in seen:
            continue
        seen.add(k)
        # GCC adds related locations without a place (notes); code scanning
        # rejects the whole result for them
        if "relatedLocations" in r:
            r["relatedLocations"] = [l for l in r["relatedLocations"] if "physicalLocation" in l]
            if not r["relatedLocations"]:
                del r["relatedLocations"]
        r.setdefault("properties", {})["sourceLine"] = source_line(src, *location(r))
        uniq.append(r)
    uniq.sort(key=lambda r: (location(r), r.get("ruleId") or ""))
    driver = {"name": name, "version": version or "unknown"}
    if info_uri:
        driver["informationUri"] = info_uri
    ids = sorted({r.get("ruleId") or "" for r in uniq})
    rules = rules or {}
    driver["rules"] = [dict({"id": i}, **({"shortDescription": {"text": rules[i]}} if i in rules else {}))
                       for i in ids]
    sarif = {"$schema": SARIF_SCHEMA, "version": "2.1.0",
             "runs": [{"tool": {"driver": driver},
                       "originalUriBaseIds": {"%SRCROOT%": {"uri": "file:///"}},
                       "results": uniq}]}
    json.dump(sarif, open(out, "w"), indent=1)
    with open(re.sub(r"\.sarif$", "", out) + ".tsv", "w") as t:
        for r in uniq:
            uri, line = location(r)
            msg = r.get("message", {}).get("text", "").replace("\t", " ").replace("\n", " ")
            t.write(f"{name}\t{r.get('ruleId')}\t{r.get('level', 'warning')}\t{uri}:{line}\t{msg}\n")
    log(f"{name}: {len(uniq)} findings -> {out}")


def tool_version(cmd):
    """First line of CMD's output that names a version."""
    try:
        lines = subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()
    except OSError:
        return ""
    return next((l.strip() for l in lines if "version" in l.lower()), lines[0].strip() if lines else "")


# --------------------------------------------------------------------------
# gcc -fanalyzer

def cmd_gcc(db, src, out):
    src = os.path.abspath(src)
    cc = os.environ.get("ANALYZER_CC", "gcc")
    timeout = int(os.environ.get("ANALYZER_TIMEOUT", "1500"))
    entries = json.load(open(db))
    work = tempfile.mkdtemp(prefix="fanalyzer-")

    def one(i, e):
        # own directory per file: -fdiagnostics-format=sarif-file writes
        # <source name>.sarif into the current directory
        d = os.path.join(work, str(i))
        os.mkdir(d)
        args = [cc] + [a for a in e["arguments"][1:] if a not in ("-c", e["file"])]
        args += ["-fanalyzer", "-fdiagnostics-format=sarif-file", "-c", e["file"], "-o", os.devnull]
        try:
            p = subprocess.run(args, cwd=d, capture_output=True, text=True, timeout=timeout)
            rc, err = p.returncode, p.stderr
        except subprocess.TimeoutExpired:
            rc, err = "timeout", ""
        return e["file"], rc, err, glob.glob(os.path.join(d, "*.sarif"))

    results, failed = [], 0
    with concurrent.futures.ThreadPoolExecutor(JOBS) as ex:
        for f, rc, err, files in ex.map(lambda a: one(*a), enumerate(entries)):
            name = os.path.relpath(f, src)
            if rc == "timeout":
                print(f"::warning title=gcc -fanalyzer::{name}: stopped after {timeout} s, no result for this file")
                continue
            for s in files:
                data = json.load(open(s))
                for run in data.get("runs", []):
                    for r in run.get("results", []):
                        if r.get("level") == "error":
                            rc = rc or 1
                        results.append(r)
            if rc:
                failed += 1
                print(f"::error title=gcc -fanalyzer::{name} does not compile (exit {rc})")
                log(err)
            else:
                log(f"{name}: done")
    for r in results:
        relativize(r, src)
    write_sarif(out, "GCC -fanalyzer", tool_version([cc, "--version"]), results, src,
                info_uri="https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html")
    return 1 if failed else 0


# --------------------------------------------------------------------------
# clang-tidy

TIDY_LINE = re.compile(r"^(?P<file>/[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<level>warning|error): "
                       r"(?P<msg>.*?) \[(?P<check>[^\]]+)\]$")


def cmd_tidy(db, src, out, checks):
    src = os.path.abspath(src)
    tidy = os.environ.get("CLANG_TIDY", "clang-tidy")
    entries = json.load(open(db))
    dbdir = os.path.dirname(os.path.abspath(db))
    # findings in the headers of the player, not in those of FFmpeg or libc
    header_filter = "^" + re.escape(src) + "/(include|container|main|manager|output|playback|tools)/"

    def one(e):
        args = [tidy, "-p", dbdir, "--quiet", "--checks=" + checks, "--header-filter=" + header_filter,
                # gcc-only options of the build
                "--extra-arg=-Wno-unknown-warning-option", "--extra-arg=-Wno-unused-command-line-argument",
                e["file"]]
        if os.environ.get("CLANG_TIDY_CONFIG"):
            # check options; the checks themselves come from --checks
            args.insert(1, "--config=" + os.environ["CLANG_TIDY_CONFIG"])
        p = subprocess.run(args, capture_output=True, text=True)
        return e["file"], p.returncode, p.stdout, p.stderr

    results, failed = [], 0
    with concurrent.futures.ThreadPoolExecutor(JOBS) as ex:
        for f, rc, stdout, stderr in ex.map(one, entries):
            name = os.path.relpath(f, src)
            errors = 0
            for line in stdout.splitlines():
                m = TIDY_LINE.match(line)
                if not m:
                    continue
                if m["level"] == "error" or m["check"] == "clang-diagnostic-error":
                    errors += 1
                    log(line)
                    continue
                results.append({
                    "ruleId": m["check"], "level": "warning", "message": {"text": m["msg"]},
                    "locations": [{"physicalLocation": {
                        "artifactLocation": {"uri": rel_uri(m["file"], src), "uriBaseId": "%SRCROOT%"},
                        "region": {"startLine": int(m["line"]), "startColumn": int(m["col"])}}}]})
            if errors or (rc and not stdout):
                failed += 1
                print(f"::error title=clang-tidy::{name}: {errors} compiler errors (exit {rc})")
                log(stderr[-2000:])
            else:
                log(f"{name}: done")
    write_sarif(out, "clang-tidy", tool_version([tidy, "--version"]), results, src,
                info_uri="https://clang.llvm.org/extra/clang-tidy/checks/list.html")
    return 1 if failed else 0


# --------------------------------------------------------------------------
# cppcheck

def cmd_cppdb(db, out):
    """cppcheck cuts -DNAME="string" of the database at the quotes and then
    reports a syntax error where NAME is used. The host build sets such
    names only to point the player at host paths, and the sources have
    receiver defaults for all of them, so they are left out here."""
    entries = json.load(open(db))
    for e in entries:
        e["arguments"] = [a for a in e["arguments"] if not re.match(r'-D\w+=".*"$', a)]
    json.dump(entries, open(out, "w"), indent=1)
    return 0


LEVELS = {"error": "error", "warning": "warning", "portability": "warning", "performance": "note",
          "style": "note", "information": "note"}


def cmd_cppcheck(xml, src, out):
    src = os.path.abspath(src)
    root = ET.parse(xml).getroot()
    version = root.find("cppcheck").get("version") if root.find("cppcheck") is not None else ""
    results, tool_errors = [], 0
    for e in root.iter("error"):
        rid, sev = e.get("id"), e.get("severity")
        locs = e.findall("location")
        if rid in ("syntaxError", "internalError", "cppcheckError", "preprocessorErrorDirective",
                   "internalAstError", "unknownMacro"):
            tool_errors += 1
            where = f"{locs[0].get('file')}:{locs[0].get('line')}" if locs else ""
            print(f"::error title=cppcheck::{rid} {where}: {e.get('msg')}")
            continue
        if not locs:
            # summaries such as checkersReport, no place in the source
            continue
        loc = locs[0]
        r = {"ruleId": rid, "level": LEVELS.get(sev, "warning"),
             "message": {"text": f"{e.get('verbose') or e.get('msg')} [{sev}"
                                 f"{', inconclusive' if e.get('inconclusive') == 'true' else ''}]"},
             "locations": [{"physicalLocation": {
                 "artifactLocation": {"uri": rel_uri(os.path.join(src, loc.get("file")), src),
                                      "uriBaseId": "%SRCROOT%"},
                 "region": {"startLine": max(1, int(loc.get("line", "1"))),
                            "startColumn": max(1, int(loc.get("column", "1")))}}}]}
        if e.get("cwe"):
            r["properties"] = {"tags": ["external/cwe/cwe-" + e.get("cwe")]}
        results.append(r)
    write_sarif(out, "cppcheck", version, results, src, info_uri="https://cppcheck.sourceforge.io/")
    return 1 if tool_errors else 0


# --------------------------------------------------------------------------
# scan-build

def cmd_scanbuild(d, src, out):
    src = os.path.abspath(src)
    results, version = [], ""
    for s in sorted(glob.glob(os.path.join(d, "**", "*.sarif"), recursive=True)):
        data = json.load(open(s))
        for run in data.get("runs", []):
            version = version or run.get("tool", {}).get("driver", {}).get("version", "")
            for r in run.get("results", []):
                relativize(r, src)
                results.append(r)
    write_sarif(out, "Clang Static Analyzer (scan-build)", version, results, src,
                info_uri="https://clang.llvm.org/docs/analyzer/checkers.html")
    return 0


# --------------------------------------------------------------------------
# summary

# new findings listed at most in the summary (all of them in new-findings.tsv)
MAX_NEW_ROWS = 200


def load_results(d):
    """[(tool, version, results)] of the merged file of each tool under D:
    D/<tool>.sarif or D/<artifact>/<tool>.sarif (not the per-file SARIF
    files scan-build leaves further down); plus the analysed commit."""
    files = sorted(glob.glob(os.path.join(d, "*.sarif")) + glob.glob(os.path.join(d, "*", "*.sarif")))
    tools = []
    for s in files:
        run = json.load(open(s))["runs"][0]
        driver = run["tool"]["driver"]
        tools.append((driver["name"], driver.get("version", ""), run["results"]))
    commits = glob.glob(os.path.join(d, "commit")) + glob.glob(os.path.join(d, "*", "commit"))
    commit = open(commits[0]).read().strip() if commits else ""
    return tools, commit


def finding_key(r):
    """What identifies a finding across revisions: check, file, message and
    source line text, but not the line number. Numbers in the message (line
    references, sizes) are left out as well, they shift with the code."""
    msg = re.sub(r"\d+", "#", " ".join(r.get("message", {}).get("text", "").split()))
    return (r.get("ruleId") or "", location(r)[0], msg, r.get("properties", {}).get("sourceLine", ""))


def compare(results, base):
    """Split RESULTS into (new, number of fixed) against the results BASE of
    the same tool. Equal keys are counted, so that one more finding of an
    existing kind on an identical line still shows up as new."""
    left = collections.Counter(finding_key(r) for r in base)
    new = []
    for r in results:
        k = finding_key(r)
        if left[k]:
            left[k] -= 1
        else:
            new.append(r)
    return new, sum(left.values())


def md_cell(text, limit=300):
    text = " ".join(text.split())
    if len(text) > limit:
        text = text[:limit - 1] + "…"
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("|", "\\|")


def place(r, commit):
    """file:line, linked to the analysed commit when known."""
    uri, line = location(r)
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not (commit and repo):
        return f"`{uri}:{line}`"
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    return f"[{uri}:{line}]({server}/{repo}/blob/{commit}/{uri}#L{line})"


def print_new(d, tools, commit, base_tools, base_commit, label):
    """The findings of TOOLS that BASE_TOOLS do not have; returns
    {tool: (new, fixed)} for the tools both sides have results of."""
    base = {name: res for name, _, res in base_tools}
    counts, new = {}, []
    for name, _, res in tools:
        if name in base:
            n, fixed = compare(res, base[name])
            counts[name] = (len(n), fixed)
            new += [(name, r) for r in n]
    shown = f"`{label}`" + (f" ({base_commit[:7]})" if base_commit else "")
    total_new = sum(n for n, _ in counts.values())
    total_fixed = sum(f for _, f in counts.values())
    print(f"#### New findings compared with {shown}: {total_new}"
          f" ({total_fixed} gone, {sum(len(res) for _, _, res in tools)} in total)\n")
    missing = [name for name, _, _ in tools if name not in counts]
    if missing:
        print(f"No result of {shown} for {', '.join(missing)}: not compared.\n")
    with open(os.path.join(d, "new-findings.tsv"), "w") as t:
        for name, r in new:
            uri, line = location(r)
            msg = " ".join(r.get("message", {}).get("text", "").split())
            t.write(f"{name}\t{r.get('ruleId')}\t{r.get('level', 'warning')}\t{uri}:{line}\t{msg}\n")
    if new:
        print("| tool | check | place | message |")
        print("|---|---|---|---|")
        for name, r in new[:MAX_NEW_ROWS]:
            msg = md_cell(r.get("message", {}).get("text", ""))
            print(f"| {name} | `{r.get('ruleId')}` | {place(r, commit)} | {msg} |")
        if len(new) > MAX_NEW_ROWS:
            print(f"\n{len(new) - MAX_NEW_ROWS} more in new-findings.tsv (artifact).")
        print()
    return counts


def cmd_summary(d, base_dir=None, label="base"):
    tools, commit = load_results(d)
    counts = {}
    if base_dir:
        base_tools, base_commit = load_results(base_dir)
        counts = print_new(d, tools, commit, base_tools, base_commit, label)
        print("#### All findings\n")
    if counts:
        print("| tool | findings | new | gone | files |")
        print("|---|---:|---:|---:|---:|")
    else:
        print("| tool | findings | files |")
        print("|---|---:|---:|")
    for name, _, res in tools:
        files = len({location(r)[0] for r in res})
        if counts:
            n, fixed = counts.get(name, ("-", "-"))
            print(f"| {name} | {len(res)} | {n} | {fixed} | {files} |")
        else:
            print(f"| {name} | {len(res)} | {files} |")
    print()
    for name, version, res in tools:
        print(f"<details><summary><b>{name}</b> ({version}): {len(res)} findings</summary>\n")
        print("| check | findings |")
        print("|---|---:|")
        for rid, n in collections.Counter(r.get("ruleId") for r in res).most_common():
            print(f"| `{rid}` | {n} |")
        print("\n</details>\n")
    return 0


def main():
    cmds = {"compdb": cmd_compdb, "gcc": cmd_gcc, "tidy": cmd_tidy, "cppdb": cmd_cppdb, "cppcheck": cmd_cppcheck,
            "scanbuild": cmd_scanbuild, "summary": cmd_summary}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[sys.argv[1]](*sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
