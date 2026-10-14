# Dependency security audit

A periodic review of every third-party component the repository resolves, in
every ecosystem: publisher, maintenance history, known advisories and a score.
The filterable report is at
**[the dependency audit report](/docs/dependency-audit/)**.

## Method

Package metadata comes from each ecosystem's registry (pub.dev, npm, PyPI,
go.dev, Gradle/Maven plugin indexes) and advisories from the OSV database,
queried per package and exact pinned version. Toolchains (Go, Flutter, Kotlin,
Android Gradle Plugin) are audited alongside libraries, since a stale toolchain
can carry standard-library advisories even when every package is current.

## Scoring rubric

Each package starts at 100 and loses points for risk signals.
Grades: A ≥ 90, B 80–89, C 70–79, D 60–69, F below 60.

| Signal | Deduction |
| --- | --- |
| Open advisory matching the pinned version | −40 |
| Advisory in the package's history, pinned version not affected | −5 |
| Discontinued or deprecated | −30 |
| No verified publisher | −15 |
| Verified community publisher | −5 |
| No release in more than 24 / 12 months | −20 / −10 |
| Pinned version over 18 months old and a major version behind | −5 |
| Registry quality score below threshold, or analysis pending | −10 / −5 |
| Ships native code (plugin or FFI) | −5 |
| npm: fewer than two maintainers | −10 |

## Acting on findings

1. Open advisories on a pinned version or toolchain are fixed first: upgrade,
   rebuild and redeploy the affected component.
2. Packages that hold keys or secrets are kept on current major versions.
3. Stale packages and toolchains are upgraded in batches with the full test
   suite for the affected platform.
4. New dependencies in any ecosystem (including Swift packages and native
   Android Gradle dependencies) are added to the next audit.
