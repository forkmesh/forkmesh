#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ("src", "tests", "packages")
RAW_STRING = re.compile(r'R"([^(\s]*)\((.*?)\)\1"', re.S)
STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
DECLARATION = re.compile(
    r"\b(?!return\b|else\b|case\b|throw\b|extern\b)[A-Za-z_][\w:<>]*\s*[&*]*\s+[&*]*environ\b\s*[,)=;({\[]")


def code_only(text: str) -> str:
    text = RAW_STRING.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = STRING.sub('""', text)
    return LINE_COMMENT.sub("", text)


def offenders() -> list[str]:
    found = []
    for folder in SOURCES:
        for path in sorted((ROOT / folder).rglob("*")):
            if path.suffix not in {".cpp", ".h", ".hpp", ".cc", ".mm"} or not path.is_file():
                continue
            lines = code_only(path.read_text(errors="replace")).splitlines()
            for number, line in enumerate(lines, 1):
                if re.search(r"\bextern\s+char\s*\*\*\s*environ\s*;", line):
                    continue
                if DECLARATION.search(line):
                    found.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")
    return found


def self_check() -> None:
    bad = [
        "QString environValue(const QByteArrayList &environ, const QByteArray &name)",
        "const QByteArrayList environ = splitNul(data);",
        'QByteArray environ("HOME=/home/x");',
        "QByteArray instanceFromEnviron(const QByteArray &environ);",
    ]
    good = [
        "extern char **environ;",
        "for (char **e = environ; e && *e; ++e)",
        "environ = envp.data();",
        "return environ;",
        'readProc(pid, "environ")',
        'const char *script = R"(env = os.environ)";',
        "QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();",
    ]
    for line in bad:
        assert DECLARATION.search(code_only(line)), line
    for line in good:
        cleaned = code_only(line)
        assert not DECLARATION.search(cleaned) or "extern" in cleaned, line


def main() -> int:
    self_check()
    found = offenders()
    if found:
        print("MinGW defines environ as a macro; rename these identifiers "
              "(docs/desktop/build-and-test.md):", file=sys.stderr)
        for entry in found:
            print(f"  {entry}", file=sys.stderr)
        return 1
    print("no C++ identifier is named environ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
