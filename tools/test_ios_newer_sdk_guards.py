import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "native"
NEWEST_SDK = 26
NEWER_SDK_COMPILER = (6, 4)
AVAILABLE = re.compile(r"[#@]available\(([^)]*)\)")
PLATFORM = re.compile(r"\b(?:iOS|iPadOS|watchOS|macOS|tvOS|visionOS|macCatalyst)\s+(\d+)")
COMPILER = re.compile(r"compiler\(\s*>=\s*(\d+)(?:\.(\d+))?")
DIRECTIVE = re.compile(r"^\s*#(if|elseif|else|endif)\b(.*)")


def guards(condition):
    found = COMPILER.search(condition)
    return (bool(found) and not condition[:found.start()].rstrip().endswith("!")
            and (int(found[1]), int(found[2] or 0)) >= NEWER_SDK_COMPILER)


def unguarded(text):
    stack = []
    for number, line in enumerate(text.splitlines(), 1):
        directive = DIRECTIVE.match(line)
        if directive:
            kind, condition = directive.groups()
            if kind == "if":
                stack.append(guards(condition))
            elif kind == "elseif" and stack:
                stack[-1] = guards(condition)
            elif kind == "else" and stack:
                stack[-1] = False
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if any(stack):
            continue
        for call in AVAILABLE.finditer(line.split("//", 1)[0]):
            if any(int(major) > NEWEST_SDK for major in PLATFORM.findall(call[1])):
                yield number, line.strip()


def test_newer_sdk_calls_sit_under_a_compiler_check():
    problems = [
        f"{path.relative_to(ROOT)}:{number}: {line}"
        for path in sorted(NATIVE.rglob("*.swift"))
        for number, line in unguarded(path.read_text(encoding="utf-8", errors="replace"))
    ]
    assert not problems, "\n".join(problems)


def test_the_check_reads_compiler_branches():
    guarded = (
        "#if compiler(>=6.4)\n"
        "if #available(iOS 27.0, *) { a() }\n"
        "#else\n"
        "b()\n"
        "#endif\n"
    )
    assert list(unguarded(guarded)) == []
    assert list(unguarded("if #available(iOS 27.1, *) { a() }\n")) == [
        (1, "if #available(iOS 27.1, *) { a() }")]
    assert list(unguarded("#if compiler(>=6.3)\n@available(watchOS 27, *)\n#endif\n")) == [
        (2, "@available(watchOS 27, *)")]
    assert list(unguarded("#if compiler(>=6.4)\n#else\nif #available(iOS 27, *) {}\n#endif\n")) == [
        (3, "if #available(iOS 27, *) {}")]
    assert list(unguarded("if #available(iOS 26.0, *) { a() }\n")) == []
