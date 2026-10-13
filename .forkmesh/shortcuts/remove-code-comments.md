# name: Remove comments from code
# description: Strip every comment out of every code file in the repo, leaving the code itself untouched.

Go through all code files in this repository and remove every comment from them.

Source is code only (`AGENTS.md`): explanations live in `docs/`. Before you
delete a comment that carries knowledge (why something is done, a protocol
detail, a trap, a bug history), move that knowledge into the subsystem's home
page or a topic page next to it. [`docs/README.md`](../../docs/README.md) maps
each directory to its page; every home page has Architecture, Protocols and
interfaces, Traps and Recent changes sections ready to receive it. Cite files,
not line numbers. Comments that only restate the code can go without a trace.

- Cover every language present in the checkout (C++/headers, Python, JavaScript,
  Dart, shell, CMake, YAML, …) and use each language's own comment syntax:
  line comments, block comments, and doc comments alike.
- Delete the comment text only. Do not change any executable code, string
  literals, or regexes that merely look like comments (`"# not a comment"`,
  `"// not a comment"`, URLs such as `https://…`).
- Keep lines that are load-bearing even though they start with a comment
  marker (the exceptions `AGENTS.md` lists): shebangs (`#!/usr/bin/env bash`), encoding/pragma lines, licence
  headers required for compliance, linter/compiler directives
  (`# noqa`, `// eslint-disable`, `# type:`, `// clang-format off`, `#pragma`),
  build tags and `//go:embed`, build markers in templates, and comment-only
  markers a build or test asserts on. User-facing help or UI text is not a
  comment. Some tests assert on comment text in source files; update those
  tests in the same change rather than keeping the comment.
- Tidy up what the removal leaves behind: drop the now-empty comment lines and
  any trailing whitespace, but keep the surrounding blank-line structure so the
  diff stays readable.
- Do not touch generated files, vendored third-party trees, or anything ignored
  by git.

When you are done, build the project and run the test suite to prove nothing
broke, then report which files were changed, which docs pages received the
knowledge, and anything you deliberately left in place.
