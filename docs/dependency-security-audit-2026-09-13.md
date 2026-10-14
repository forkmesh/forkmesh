# Dependency security audit (snapshot)

A one-time audit of every third-party component the repository resolves. The
maintained summary is `www/docs/security/dependency-security-audit.md`.

## Scope

| Ecosystem | Manifest |
|---|---|
| Mobile (Flutter/Dart) | `pubspec.yaml`, `pubspec.lock` |
| npm (browser tests, editor extension) | `package-lock.json` files |
| Python (CI) | `tools/requirements-ci.txt` |
| Python app | `app/pyproject.toml` (standard library only) |
| Go mirror node | `server/go.mod` (standard library only) |
| Android build plugins, iOS pods | Gradle settings, `Podfile.lock` |
| Vendored browser libraries | `world/public/world/vendor` |
| Desktop native libraries | `desktop/CMakeLists.txt` (system packages) |
| Toolchains | Flutter, Dart, Go, Node, wrangler |

## Method

Data was pulled from the public registries and OSV per package and pinned
version. Each package starts at 100 points and loses points for measurable
risk signals:

| Signal | Deduction |
|---|---|
| open advisory matching the pinned version | −40 |
| advisory in history, pinned version not affected | −5 |
| discontinued or deprecated | −30 |
| no verified publisher / community publisher | −15 / −5 |
| no release in over 24 / 12 months | −20 / −10 |
| pinned version over 18 months old and a major version behind | −5 |
| low registry quality score | −10 / −5 |
| ships native code | −5 |
| npm package with fewer than two maintainers | −10 |

Grades: A ≥ 90, B 80–89, C 70–79, D 60–69, F < 60. Scores are a screening
heuristic from public registry signals, not a code review.

## Findings

1. No open advisory matched any pinned version in any ecosystem; historical
   advisories were fixed long before the pinned versions.
2. The main exposure was the Go toolchain version used to build the mirror
   node: its standard library had open advisories, fixed by building with a
   current Go release.
3. Many mobile packages were behind their latest releases but none were
   abandoned; the most valuable upgrades were the secure-storage and
   file-picker plugins.
4. Supply-chain concentration is healthy: most Dart packages come from the
   platform vendors' own publishers.
5. The Python app and the Go mirror node use only their standard libraries
   at runtime; third-party packages are build- and test-time only.

## Practice

- Pin exact versions in lock files and review upgrades.
- Re-run advisory checks per pinned version, not per package name.
- Keep runtime services on standard libraries where practical.
- Build release binaries with a supported toolchain version.
