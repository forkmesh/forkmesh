# Dependency security audit

Every third-party component the ForkMesh repository resolves, across all
ecosystems, with who publishes it, how it has been maintained, and a security
score. The full, filterable report is a styled page:

**[Open the dependency audit report](/docs/dependency-audit/)** (all 128 Dart
packages, 12 npm packages, the CI Python pin, Android and iOS build plugins,
vendored browser libraries, desktop system libraries and toolchains).

Data was pulled live on 13 September 2026 from pub.dev, registry.npmjs.org,
PyPI, go.dev, the Google and Maven Central plugin indexes and the OSV
vulnerability database. The method, rubric and raw tables are also kept in the
repository at `docs/dependency-security-audit-2026-09-13.md`.

## What the audit found

| Finding | Detail |
| --- | --- |
| Open advisories on pinned versions | None, in any ecosystem. OSV was queried per package and exact version. |
| Advisories in package histories | Four (archive ×2, http, shared_preferences_android), all fixed years before the versions in use. |
| The one real exposure | The mirror node is built with go1.24.4, which carries 47 open Go standard-library advisories (net/http, crypto/tls, net/url, os). go1.27.1 carries none. |
| Staleness | 52 Dart packages have newer releases; nothing in use has gone more than 20 months without an upstream release. |
| Publisher concentration | 106 of 128 Dart packages come from Google-run publishers; the community publishers are verified and active. |

## Top publishers

| Publisher | Packages | Tier |
| --- | --- | --- |
| tools.dart.dev | 36 | core (Dart team) |
| flutter.dev | 34 | core (Flutter team) |
| dart.dev | 29 | core (Dart SDK team) |
| steenbakker.dev | 6 | community: the flutter_secure_storage family, which holds the app's keys |
| google.dev | 5 | core |

## Scoring rubric

Each package starts at 100 and loses points for measurable risk signals.
Grades: A ≥ 90, B 80–89, C 70–79, D 60–69, F below 60.

| Signal | Deduction |
| --- | --- |
| Open advisory matching the pinned version | −40 |
| Advisory in the package's history, pinned version not affected | −5 |
| Discontinued or deprecated | −30 |
| No verified publisher | −15 |
| Verified community publisher (not Google-run) | −5 |
| No release in more than 24 / 12 months | −20 / −10 |
| Pinned version over 18 months old and a major version behind | −5 |
| pub.dev points below 100 / below 140 of 160, or analysis pending | −10 / −5 |
| Ships native code (Flutter plugin or FFI) | −5 |
| npm: fewer than two maintainers | −10 |

## Recommended actions, in order

1. Rebuild `forkmesh-mirror-node` with go1.27.1 and roll the mirror fleet.
2. Upgrade `flutter_secure_storage` to 11.x, `file_picker` to 12.x,
   `device_info_plus` to 13.x and `archive` to 4.3.0; run the mobile suite and
   refresh the iOS pod.
3. Bump Flutter to 3.47.4, Kotlin to 2.4.20 and the Android Gradle Plugin to
   9.4.0.
4. `npm update` in both browser-test projects and the VS Code extension,
   checking the extension's `engines.vscode` floor.
5. Narrow the Worker CSP `script-src` if the jsdelivr allowance is unused.

## Not covered by this audit

The audit is a snapshot of 13 September 2026. Dependencies added since then
have not been scored:

- the native iOS app's Swift packages, pinned in
  `native/ios/ForkMesh.xcodeproj/project.xcworkspace/xcshareddata/swiftpm/Package.resolved`
  (13 pins, including `mlx-swift`, `mlx-swift-lm`, `swift-transformers` and
  `swift-crypto` for on-device Gemma 4 and cryptography);
- the native Android app's Gradle dependencies in
  `native/android/app/build.gradle.kts` and `native/android/core/build.gradle.kts`
  (AndroidX Compose BOM, kotlinx coroutines).

`server/go.mod` still declares `go 1.24`; the Go standard-library exposure
above depends on the toolchain a release is built with, so check the Go
version of the release host before relying on the finding being fixed.
