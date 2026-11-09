#!/usr/bin/env python3
"""Read-only integrity, syntax, documentation-link and publication checks.

SPDX-License-Identifier: MIT
Run from any directory with Python 3.9 or newer. Results are printed as JSON.
"""

import argparse
import ast
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote


def repository_files(root):
    ignored = {".git", "build-work", "private", "backups", "__pycache__"}
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and not ignored.intersection(path.relative_to(root).parts))


def check_repository(root, check_git):
    failures = []
    counts = {"files": 0, "json": 0, "plist": 0, "python": 0, "shell": 0,
              "current_doc_links": 0, "git_bytes": 0, "screenshots": 0}
    files = repository_files(root)
    forbidden = [
        ("private key", re.compile(r"-----BEGIN (?:OPENSSH|RSA|EC|DSA) PRIVATE KEY-----")),
        ("GitHub credential", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})")),
        ("private LAN endpoint", re.compile(r"(?<![\d.])(?:192\.168\.\d+\.\d+|10\.(?:\d+\.){2}\d+)(?![\d.])")),
        ("personal home path", re.compile(r"/Users/(?!hackintosh(?:[/\s\"']|$))[A-Za-z0-9_.-]+")),
    ]
    blocked_suffixes = {".kc", ".dmg", ".pkg", ".bin", ".zip", ".tar", ".pem", ".key"}
    for path in files:
        rel = path.relative_to(root).as_posix()
        counts["files"] += 1
        if (path.suffix in blocked_suffixes or path.name.endswith((".tar.gz", ".tgz"))
                or ".private." in path.name or path.name.startswith("._")):
            failures.append({"path": rel, "check": "excluded artifact type"})
        data = path.read_bytes()
        if rel.startswith("assets/screenshots/") and path.suffix.lower() in {".png", ".jpg", ".jpeg"}:
            if not (data.startswith(b"\x89PNG\r\n\x1a\n") or data.startswith(b"\xff\xd8\xff")):
                failures.append({"path": rel, "check": "screenshot signature"})
            if path.suffix.lower() == ".png":
                offset = 8
                while offset + 12 <= len(data):
                    length = int.from_bytes(data[offset:offset + 4], "big")
                    kind = data[offset + 4:offset + 8]
                    if kind in {b"eXIf", b"iTXt", b"tEXt", b"zTXt"}:
                        failures.append({"path": rel, "check": "screenshot metadata"})
                    offset += length + 12
                if offset != len(data):
                    failures.append({"path": rel, "check": "PNG chunk boundaries"})
            counts["screenshots"] += 1
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            failures.append({"path": rel, "check": "unexpected binary or encoding"})
            continue
        for name, pattern in forbidden:
            if pattern.search(text):
                failures.append({"path": rel, "check": name})
        try:
            if path.suffix == ".json":
                json.loads(text)
                counts["json"] += 1
            elif path.suffix == ".plist":
                value = plistlib.loads(data)
                if isinstance(value, dict) and "PlatformInfo" in value:
                    failures.append({"path": rel, "check": "full identity configuration"})
                counts["plist"] += 1
            elif path.suffix == ".py":
                ast.parse(text, filename=rel)
                counts["python"] += 1
            elif path.suffix == ".sh":
                result = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True)
                if result.returncode:
                    failures.append({"path": rel, "check": "bash syntax", "detail": result.stderr[:200]})
                counts["shell"] += 1
        except (ValueError, SyntaxError, plistlib.InvalidFileException) as error:
            failures.append({"path": rel, "check": "parse", "detail": str(error)[:200]})
        # Archived and third-party prose may reference external source trees.
        if path.suffix == ".md" and not rel.startswith("archive/") and not rel.startswith("LICENSES/"):
            for target in re.findall(r"\]\(([^\s)]+)(?:\s+[^)]*)?\)", text):
                if target.startswith(("https://", "http://", "mailto:", "#")):
                    continue
                local = unquote(target.split("#", 1)[0])
                counts["current_doc_links"] += 1
                if local.startswith("/") or not (path.parent / local).exists():
                    failures.append({"path": rel, "check": "relative link", "target": local})
    if check_git:
        result = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True)
        for name in filter(None, result.stdout.split(b"\0")):
            rel = name.decode()
            path = root / rel
            committed = subprocess.run(["git", "cat-file", "blob", "HEAD:" + rel],
                                       cwd=root, capture_output=True, check=True).stdout
            if not path.is_file() or path.read_bytes() != committed:
                failures.append({"path": rel, "check": "committed bytes"})
            counts["git_bytes"] += 1
    return {"status": "PASS" if not failures else "FAIL", "counts": counts,
            "failures": failures,
            "scope": "File format, syntax, documentation links and optional commit consistency."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--git", action="store_true", help="Compare current files with HEAD bytes")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    report = check_repository(root, args.git)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
