# Dependency history & security audit — 2026-09-13

> Snapshot. The maintained summary, including what this audit does not cover,
> is [`www/docs/security/dependency-security-audit.md`](../www/docs/security/dependency-security-audit.md).

Every third-party component the ForkMesh repository resolves, across all ecosystems, with who publishes it, how it has been maintained, and a security score. Data was pulled live from pub.dev, registry.npmjs.org, PyPI, go.dev, Google's and Maven Central's plugin indexes and the OSV vulnerability database on 2026-09-13.

## Scope

| Ecosystem | Manifest | Components |
|---|---|---|
| Flutter / Dart (mobile app) | `mobile/pubspec.yaml`, `mobile/pubspec.lock` | 128 packages (16 direct, 2 dev, 110 transitive) |
| npm (browser tests, VS Code extension) | 3 `package-lock.json` files | 12 packages |
| Python (CI) | `tools/requirements-ci.txt` | 1 package |
| Python Worker (Cloudflare) | `app/pyproject.toml` | 0 packages — standard library only |
| Go (mirror node) | `server/go.mod` | 0 modules — standard library only |
| Android build plugins | `mobile/android/settings.gradle.kts` | 3 |
| iOS pods | `mobile/ios/Podfile.lock` | 1 |
| Vendored browser libraries | `world/public/world/vendor` | 2 |
| Desktop native libraries | `desktop/CMakeLists.txt` (system packages) | 4 |
| Toolchains | Flutter, Dart, Go, Node, wrangler | 5 |

## Scoring rubric

Each package starts at 100 and loses points for measurable risk signals. Grades: A ≥ 90, B 80–89, C 70–79, D 60–69, F < 60.

| Signal | Deduction |
|---|---|
| Open advisory (OSV / pub.dev) matching the pinned version | −40 |
| Advisory in the package's history, pinned version not affected | −5 |
| Discontinued / deprecated | −30 |
| No verified publisher | −15 |
| Verified community publisher (not a Google-run one) | −5 |
| No release in > 24 months / > 12 months | −20 / −10 |
| Pinned version > 18 months old and a major version behind | −5 |
| pub.dev points < 100 / < 140 (of 160), or analysis pending | −10 / −5 |
| Ships native code (Flutter plugin or FFI) — larger, harder-to-review surface | −5 |
| npm: fewer than two maintainers | −10 |

Publisher tiers: *core* = dart.dev, flutter.dev, tools.dart.dev, material.io, google.dev (Google-run). *community* = any other pub.dev verified publisher.

## Headline findings

1. **No open advisory matches any pinned version** in any ecosystem (OSV queried per package and version). Four pub.dev advisories exist in package histories; all were fixed years before the versions in use (see below).
2. **The Go toolchain is the one real exposure.** The mirror node (`server/`, pure standard library) is built with go1.24.4. OSV lists **47 open standard-library advisories** for that version, covering net/http, crypto/tls, crypto/x509, net/url, os and archive/tar, i.e. code the node uses to serve git over HTTPS. go1.24.13 (the last 1.24 patch) still carries 29; go1.27.1 carries 0. Upgrading the build toolchain closes all of them with no code change.
3. **Everything is stale, nothing is abandoned.** 52 Dart packages have newer releases (see the `flutter pub outdated` list you pasted). The oldest pinned versions are `js` 0.6.7 (2023-01), `path_provider_linux` 2.2.1 (2023-08) and `flutter_secure_storage_web/windows` 1.2.1/3.1.2 (2024-05). No package in use has gone more than 20 months without an upstream release; the least active is `flutter_secure_storage_macos` (last release 2025-01).
4. **Highest-value upgrades:** `flutter_secure_storage` 9.2.4 → 11.1.1 (16 releases behind; it holds the account keys), `file_picker` 10.3.10 → 12.3.0 (19 behind), `archive` 4.2.0 → 4.3.0, `@types/node` 18 → 26 in the VS Code extension, Flutter 3.47.0 → 3.47.4, Kotlin 2.4.0 → 2.4.20, AGP 9.1.0 → 9.4.0.
5. **Supply-chain concentration is healthy:** 106 of 128 Dart packages come from five Google-run publishers; the community publishers (steenbakker.dev, fluttercommunity.dev, loki3d.com, victorcarreras.dev, halildurmus.dev) are long-standing, verified, and all released within the last five months.

## Top 5 publishers (Dart, by number of packages in the lock file)

| # | Publisher | Packages | Tier | Latest release across their packages | Notes |
|---|---|---|---|---|---|
| 1 | tools.dart.dev | 36 | core | 2026-09-11 | Dart team utility packages (analyzer, test, yaml…) |
| 2 | flutter.dev | 34 | core | 2026-08-28 | Flutter team first-party plugins (url_launcher, shared_preferences, local_auth, path_provider) |
| 3 | dart.dev | 29 | core | 2026-09-07 | Dart SDK team (intl, http, crypto, test, hooks, jni) |
| 4 | steenbakker.dev | 6 | community | 2026-09-11 | Julian Steenbakker — flutter_secure_storage family (holds the app's key material) |
| 5 | google.dev | 5 | core | 2026-09-09 | Google (webdriver) |

All publishers seen: tools.dart.dev (36), flutter.dev (34), dart.dev (29), steenbakker.dev (6), google.dev (5), fluttercommunity.dev (3), loki3d.com (2), dash-overflow.net (2), (unverified) (2), lukas-renggli.ch (2), halildurmus.dev (2), dint.dev (1), canonical.com (1), victorcarreras.dev (1), material.io (1), onepub.dev (1).

Top npm maintainers: Microsoft Playwright team (pavelfeldman, yurys, dgozman-ms; 3 packages ×2 projects), DefinitelyTyped `types` bot (2), Microsoft TypeScript team (weswigham, andrewbranch, jakebailey), mrdoob & mugen87 (three.js), matteo.collina (undici-types), paulmillr (fsevents). Python: the pytest core team. Vendored: mrdoob (three.js 0.184.0), shashwatak (satellite.js 7.1.0).

## Advisory history (pinned versions not affected)

| Package | Pinned | Advisory | Affected range | Verdict |
|---|---|---|---|---|
| archive | 4.2.0 | GHSA-9v85-q87q-g4vg, GHSA-r285-q736-9v95 (CVE-2023-39139/39137, zip path traversal & filename spoofing; fixed 3.3.8) | <3.3.8 | not affected |
| http | 1.6.0 | GHSA-4rgh-jx4f-qfcq (CVE-2020-35669, CRLF header injection; fixed 0.13.3) | <0.13.3 | not affected |
| shared_preferences_android | 2.4.23 | GHSA-3hpf-ff72-j67p (arbitrary deserialization; only 2.3.3 affected, fixed 2.3.4) | 2.3.3 only | not affected |

## Flutter / Dart packages (mobile)

### Direct dependencies

| Package | Use | Pinned (published) | Latest (published) | Behind | Publisher | First release / total releases | Pub points | Advisories in history | Score | Deductions |
|---|---|---|---|---|---|---|---|---|---|---|
| cryptography | direct | 2.9.0 (2025-11-21) | 2.9.0 (2025-11-21) | 0 | dint.dev | 2019-09-24 / 49 | 130/160 | 0 | **85 B** | community publisher; pub points 130/160; native code surface |
| flutter_launcher_icons | dev | 0.14.4 (2025-06-10) | 0.14.4 (2025-06-10) | 0 | fluttercommunity.dev | 2017-12-04 / 41 | 150/160 | 0 | **85 B** | community publisher; no release in 15 months |
| flutter_secure_storage | direct | 9.2.4 (2025-01-09) | 11.1.1 (2026-09-11) | 16 | steenbakker.dev | 2017-10-31 / 64 | 160/160 | 0 | **85 B** | community publisher; pinned version >18 months old and a major behind; native code surface |
| pqcrypto | direct | 0.4.1 (2026-08-11) | 0.4.1 (2026-08-11) | 0 | — | 2025-12-12 / 7 | 150/160 | 0 | **85 B** | no verified publisher |
| provider | direct | 6.1.5+1 (2025-08-19) | 6.1.5+1 (2025-08-19) | 0 | dash-overflow.net | 2018-10-19 / 70 | 150/160 | 0 | **85 B** | community publisher; no release in 13 months |
| url_launcher | direct | 6.3.2 (2025-07-10) | 6.3.2 (2025-07-10) | 0 | flutter.dev | 2017-04-20 / 129 | 160/160 | 0 | **85 B** | no release in 14 months; native code surface |
| archive | direct | 4.2.0 (2026-08-22) | 4.3.0 (2026-09-13) | 1 | loki3d.com | 2013-12-31 / 100 | 150/160 | 2 | **90 A** | past advisory (pinned version not affected); community publisher |
| device_info_plus | direct | 12.4.0 (2026-03-30) | 13.2.0 (2026-06-26) | 3 | fluttercommunity.dev | 2020-04-20 / 77 | 150/160 | 0 | **90 A** | community publisher; native code surface |
| file_picker | direct | 10.3.10 (2026-01-28) | 12.3.0 (2026-09-11) | 19 | victorcarreras.dev | 2018-09-09 / 240 | 160/160 | 0 | **90 A** | community publisher; native code surface |
| flutter_lints | dev | 6.0.0 (2025-05-27) | 6.0.0 (2025-05-27) | 0 | flutter.dev | 2021-05-05 / 17 | 160/160 | 0 | **90 A** | no release in 16 months |
| web_socket_channel | direct | 3.0.3 (2025-04-17) | 3.0.3 (2025-04-17) | 0 | tools.dart.dev | 2016-03-03 / 33 | 150/160 | 0 | **90 A** | no release in 17 months |
| http | direct | 1.6.0 (2025-11-10) | 1.6.0 (2025-11-10) | 0 | dart.dev | 2012-11-30 / 130 | 160/160 | 1 | **95 A** | past advisory (pinned version not affected) |
| local_auth | direct | 3.0.2 (2026-07-09) | 3.0.2 (2026-07-09) | 0 | flutter.dev | 2017-08-17 / 62 | 160/160 | 0 | **95 A** | native code surface |
| path_provider | direct | 2.1.6 (2026-06-15) | 2.1.6 (2026-06-15) | 0 | flutter.dev | 2017-05-03 / 69 | 160/160 | 0 | **95 A** | native code surface |
| shared_preferences | direct | 2.5.5 (2026-03-25) | 2.5.5 (2026-03-25) | 0 | flutter.dev | 2017-05-09 / 91 | 160/160 | 0 | **95 A** | native code surface |
| crypto | direct | 3.0.7 (2025-11-04) | 3.0.7 (2025-11-04) | 0 | dart.dev | 2013-05-29 / 79 | 160/160 | 0 | **100 A** | — |
| cupertino_icons | direct | 1.0.9 (2026-03-25) | 1.0.9 (2026-03-25) | 0 | flutter.dev | 2017-09-18 / 18 | 160/160 | 0 | **100 A** | — |
| intl | direct | 0.20.2 (2025-01-24) | 0.20.3 (2026-06-25) | 1 | dart.dev | 2012-11-30 / 140 | 160/160 | 0 | **100 A** | — |

### Transitive dependencies

| Package | Use | Pinned (published) | Latest (published) | Behind | Publisher | First release / total releases | Pub points | Advisories in history | Score | Deductions |
|---|---|---|---|---|---|---|---|---|---|---|
| flutter_secure_storage_macos | transitive | 3.1.3 (2025-01-03) | 4.0.0 (2025-01-06) | 1 | steenbakker.dev | 2021-07-30 / 13 | 130/160 | 0 | **40 F** | community publisher; no release in 20 months; pinned version >18 months old and a major behind; pub points 130/160; native code surface; DISCONTINUED |
| js | transitive | 0.6.7 (2023-01-26) | 0.7.2 (2025-02-11) | 4 | dart.dev | 2012-09-24 / 57 | 140/160 | 0 | **60 D** | no release in 19 months; DISCONTINUED |
| node_preamble | transitive | 2.0.2 (2023-03-01) | 2.0.2 (2023-03-01) | 0 | — | 2015-08-24 / 21 | 140/160 | 0 | **65 D** | no verified publisher; no release in 42 months |
| nested | transitive | 1.0.0 (2021-02-17) | 1.0.0 (2021-02-17) | 0 | dash-overflow.net | 2019-11-26 / 7 | 150/160 | 0 | **75 C** | community publisher; no release in 67 months |
| path_provider_windows | transitive | 2.3.0 (2024-07-09) | 2.3.0 (2024-07-09) | 0 | flutter.dev | 2020-06-06 / 31 | 160/160 | 0 | **75 C** | no release in 26 months; native code surface |
| shared_preferences_linux | transitive | 2.4.1 (2024-08-09) | 2.4.1 (2024-08-09) | 0 | flutter.dev | 2020-06-30 / 22 | 160/160 | 0 | **75 C** | no release in 25 months; native code surface |
| shared_preferences_windows | transitive | 2.4.1 (2024-08-09) | 2.4.1 (2024-08-09) | 0 | flutter.dev | 2020-09-21 / 21 | 150/160 | 0 | **75 C** | no release in 25 months; native code surface |
| shelf_packages_handler | transitive | 3.0.2 (2023-05-03) | 3.0.2 (2023-05-03) | 0 | tools.dart.dev | 2016-07-21 / 10 | 135/160 | 0 | **75 C** | no release in 40 months; pub points 135/160 |
| sync_http | transitive | 0.3.1 (2022-06-15) | 0.3.1 (2022-06-15) | 0 | google.dev | 2017-04-25 / 10 | 120/160 | 0 | **75 C** | no release in 51 months; pub points 120/160 |
| webkit_inspection_protocol | transitive | 1.2.1 (2023-08-31) | 1.2.1 (2023-08-31) | 0 | google.dev | 2015-05-06 / 37 | 130/160 | 0 | **75 C** | no release in 37 months; pub points 130/160 |
| cli_config | transitive | 0.2.0 (2024-03-12) | 0.2.0 (2024-03-12) | 0 | tools.dart.dev | 2023-04-04 / 4 | 150/160 | 0 | **80 B** | no release in 30 months |
| frontend_server_client | transitive | 4.0.0 (2024-03-26) | 4.0.0 (2024-03-26) | 0 | tools.dart.dev | 2020-11-30 / 10 | 150/160 | 0 | **80 B** | no release in 30 months |
| image | transitive | 4.9.2 (2026-08-19) | 4.10.0 (2026-09-13) | 1 | loki3d.com | 2014-01-11 / 123 | pending | 0 | **80 B** | community publisher; pub.dev analysis pending (0/0); not null-safe |
| plugin_platform_interface | transitive | 2.1.8 (2024-01-04) | 2.1.8 (2024-01-04) | 0 | flutter.dev | 2019-12-11 / 19 | 150/160 | 0 | **80 B** | no release in 32 months |
| shelf | transitive | 1.4.2 (2024-06-21) | 1.4.2 (2024-06-21) | 0 | tools.dart.dev | 2014-01-02 / 78 | 160/160 | 0 | **80 B** | no release in 27 months |
| source_map_stack_trace | transitive | 2.1.2 (2024-08-21) | 2.1.2 (2024-08-21) | 0 | tools.dart.dev | 2015-04-29 / 20 | 150/160 | 0 | **80 B** | no release in 25 months |
| url_launcher_platform_interface | transitive | 2.3.2 (2024-02-15) | 2.3.2 (2024-02-15) | 0 | flutter.dev | 2019-10-22 / 27 | 150/160 | 0 | **80 B** | no release in 31 months |
| flutter_secure_storage_web | transitive | 1.2.1 (2024-05-08) | 2.1.1 (2026-05-06) | 5 | steenbakker.dev | 2021-07-30 / 13 | 150/160 | 0 | **85 B** | community publisher; pinned version >18 months old and a major behind; native code surface |
| flutter_secure_storage_windows | transitive | 3.1.2 (2024-05-08) | 4.2.2 (2026-05-22) | 5 | steenbakker.dev | 2021-07-30 / 17 | 150/160 | 0 | **85 B** | community publisher; pinned version >18 months old and a major behind; native code surface |
| leak_tracker_testing | transitive | 3.0.2 (2025-04-21) | 3.0.2 (2025-04-21) | 0 | tools.dart.dev | 2023-06-06 / 12 | 120/160 | 0 | **85 B** | no release in 17 months; pub points 120/160 |
| posix | transitive | 6.5.2 (2026-07-24) | 6.5.2 (2026-07-24) | 0 | onepub.dev | 2020-09-19 / 35 | 130/160 | 0 | **85 B** | community publisher; pub points 130/160; native code surface |
| shared_preferences_web | transitive | 2.4.3 (2025-02-18) | 2.4.3 (2025-02-18) | 0 | flutter.dev | 2019-12-03 / 26 | 150/160 | 0 | **85 B** | no release in 19 months; native code surface |
| win32_registry | transitive | 2.1.0 (2025-02-19) | 3.0.3 (2026-04-11) | 4 | halildurmus.dev | 2022-01-17 / 19 | 160/160 | 0 | **85 B** | community publisher; pinned version >18 months old and a major behind; native code surface |
| args | transitive | 2.7.0 (2025-03-12) | 2.7.0 (2025-03-12) | 0 | dart.dev | 2012-11-30 / 136 | 150/160 | 0 | **90 A** | no release in 18 months |
| boolean_selector | transitive | 2.1.2 (2024-10-22) | 2.1.2 (2024-10-22) | 0 | tools.dart.dev | 2016-02-18 / 14 | 160/160 | 0 | **90 A** | no release in 23 months |
| characters | transitive | 1.4.1 (2025-06-25) | 1.4.1 (2025-06-25) | 0 | dart.dev | 2019-10-22 / 18 | 160/160 | 0 | **90 A** | no release in 15 months |
| checked_yaml | transitive | 2.0.4 (2025-06-05) | 2.0.4 (2025-06-05) | 0 | google.dev | 2019-05-02 / 10 | 160/160 | 0 | **90 A** | no release in 15 months |
| collection | transitive | 1.19.1 (2024-10-21) | 1.19.1 (2024-10-21) | 0 | dart.dev | 2013-12-19 / 53 | 140/160 | 0 | **90 A** | no release in 23 months |
| convert | transitive | 3.1.2 (2024-10-16) | 3.1.2 (2024-10-16) | 0 | dart.dev | 2015-10-08 / 16 | 160/160 | 0 | **90 A** | no release in 23 months |
| dbus | transitive | 0.7.15 (2026-08-20) | 0.7.15 (2026-08-20) | 0 | canonical.com | 2020-08-07 / 62 | 150/160 | 0 | **90 A** | community publisher; native code surface |
| fake_async | transitive | 1.3.3 (2025-01-28) | 1.3.3 (2025-01-28) | 0 | dart.dev | 2014-07-21 / 16 | 150/160 | 0 | **90 A** | no release in 20 months |
| file | transitive | 7.0.1 (2024-10-08) | 7.0.1 (2024-10-08) | 0 | tools.dart.dev | 2015-06-06 / 47 | 150/160 | 0 | **90 A** | no release in 23 months |
| flutter_secure_storage_linux | transitive | 1.2.3 (2025-04-25) | 3.0.3 (2026-09-11) | 7 | steenbakker.dev | 2021-07-30 / 16 | 140/160 | 0 | **90 A** | community publisher; native code surface |
| flutter_secure_storage_platform_interface | transitive | 1.1.2 (2024-05-15) | 2.1.0 (2026-09-09) | 5 | steenbakker.dev | 2021-07-30 / 11 | 150/160 | 0 | **90 A** | community publisher; pinned version >18 months old and a major behind |
| http_multi_server | transitive | 3.2.2 (2024-12-17) | 3.2.2 (2024-12-17) | 0 | tools.dart.dev | 2014-06-05 / 22 | 160/160 | 0 | **90 A** | no release in 21 months |
| http_parser | transitive | 4.1.2 (2025-01-02) | 4.1.2 (2025-01-02) | 0 | dart.dev | 2014-04-03 / 32 | 160/160 | 0 | **90 A** | no release in 20 months |
| leak_tracker_flutter_testing | transitive | 3.0.10 (2025-04-21) | 3.0.10 (2025-04-21) | 0 | flutter.dev | 2023-08-10 / 27 | 150/160 | 0 | **90 A** | no release in 17 months |
| logging | transitive | 1.3.0 (2024-10-17) | 1.3.0 (2024-10-17) | 0 | dart.dev | 2012-11-30 / 104 | 160/160 | 0 | **90 A** | no release in 23 months |
| path | transitive | 1.9.1 (2024-10-17) | 1.9.1 (2024-10-17) | 0 | dart.dev | 2013-07-16 / 76 | 160/160 | 0 | **90 A** | no release in 23 months |
| shared_preferences_android | transitive | 2.4.23 (2026-03-25) | 2.4.28 (2026-08-28) | 5 | flutter.dev | 2021-11-19 / 53 | 160/160 | 1 | **90 A** | past advisory (pinned version not affected); native code surface |
| shelf_static | transitive | 1.1.3 (2024-09-06) | 1.1.3 (2024-09-06) | 0 | tools.dart.dev | 2014-04-25 / 34 | 160/160 | 0 | **90 A** | no release in 24 months |
| shelf_web_socket | transitive | 3.0.0 (2025-01-30) | 3.0.0 (2025-01-30) | 0 | tools.dart.dev | 2014-05-20 / 26 | 160/160 | 0 | **90 A** | no release in 20 months |
| stream_channel | transitive | 2.1.4 (2025-01-07) | 2.1.4 (2025-01-07) | 0 | tools.dart.dev | 2016-01-28 / 31 | 140/160 | 0 | **90 A** | no release in 20 months |
| string_scanner | transitive | 1.4.1 (2024-12-17) | 1.4.1 (2024-12-17) | 0 | tools.dart.dev | 2014-04-02 / 28 | 150/160 | 0 | **90 A** | no release in 21 months |
| term_glyph | transitive | 1.2.2 (2024-12-17) | 1.2.2 (2024-12-17) | 0 | tools.dart.dev | 2017-01-06 / 10 | 140/160 | 0 | **90 A** | no release in 21 months |
| typed_data | transitive | 1.4.0 (2024-10-17) | 1.4.0 (2024-10-17) | 0 | dart.dev | 2013-12-19 / 21 | 150/160 | 0 | **90 A** | no release in 23 months |
| web | transitive | 1.1.1 (2025-02-26) | 1.1.1 (2025-02-26) | 0 | dart.dev | 2015-07-31 / 19 | 150/160 | 0 | **90 A** | no release in 19 months |
| web_socket | transitive | 1.0.1 (2025-05-06) | 1.0.1 (2025-05-06) | 0 | dart.dev | 2024-03-20 / 9 | 160/160 | 0 | **90 A** | no release in 16 months |
| win32 | transitive | 5.15.0 (2025-10-07) | 6.4.0 (2026-08-05) | 6 | halildurmus.dev | 2020-03-27 / 152 | 160/160 | 0 | **90 A** | community publisher; native code surface |
| xdg_directories | transitive | 1.1.0 (2024-10-04) | 1.1.0 (2024-10-04) | 0 | flutter.dev | 2020-03-06 / 15 | 160/160 | 0 | **90 A** | no release in 23 months |
| _fe_analyzer_shared | transitive | 99.0.0 (2026-04-08) | 108.0.0 (2026-09-11) | 9 | tools.dart.dev | 2019-10-31 / 112 | 125/160 | 0 | **95 A** | pub points 125/160 |
| analyzer | transitive | 12.1.0 (2026-04-08) | 14.4.0 (2026-09-11) | 9 | tools.dart.dev | 2013-10-25 / 389 | 130/160 | 0 | **95 A** | pub points 130/160 |
| cli_util | transitive | 0.4.2 (2024-10-29) | 0.6.0 (2026-08-21) | 4 | tools.dart.dev | 2015-02-05 / 25 | 150/160 | 0 | **95 A** | native code surface |
| coverage | transitive | 1.15.1 (2026-06-09) | 1.15.1 (2026-06-09) | 0 | tools.dart.dev | 2014-02-27 / 101 | 115/160 | 0 | **95 A** | pub points 115/160 |
| device_info_plus_platform_interface | transitive | 7.0.3 (2025-06-16) | 8.1.0 (2026-04-21) | 2 | fluttercommunity.dev | 2020-10-02 / 30 | 150/160 | 0 | **95 A** | community publisher |
| flutter_plugin_android_lifecycle | transitive | 2.0.35 (2026-06-02) | 2.0.35 (2026-06-02) | 0 | flutter.dev | 2019-10-17 / 47 | 160/160 | 0 | **95 A** | native code surface |
| jni | transitive | 1.0.3 (2026-07-30) | 1.0.3 (2026-07-30) | 0 | dart.dev | 2022-09-12 / 34 | 160/160 | 0 | **95 A** | native code surface |
| jni_flutter | transitive | 1.0.2 (2026-07-28) | 1.0.3 (2026-08-31) | 1 | dart.dev | 2026-04-01 / 4 | 160/160 | 0 | **95 A** | native code surface |
| local_auth_android | transitive | 2.0.9 (2026-06-02) | 2.0.10 (2026-08-28) | 1 | flutter.dev | 2022-04-12 / 66 | 150/160 | 0 | **95 A** | native code surface |
| local_auth_darwin | transitive | 2.0.3 (2026-02-25) | 2.0.4 (2026-08-28) | 1 | flutter.dev | 2024-02-09 / 17 | 150/160 | 0 | **95 A** | native code surface |
| local_auth_windows | transitive | 2.0.1 (2025-11-17) | 2.0.2 (2026-08-28) | 1 | flutter.dev | 2022-05-17 / 15 | 150/160 | 0 | **95 A** | native code surface |
| objective_c | transitive | 9.5.0 (2026-07-27) | 9.6.0 (2026-08-20) | 1 | dart.dev | 2024-05-09 / 28 | 150/160 | 0 | **95 A** | native code surface |
| path_provider_android | transitive | 2.3.1 (2026-04-08) | 2.3.1 (2026-04-08) | 0 | flutter.dev | 2021-11-10 / 49 | 160/160 | 0 | **95 A** | native code surface |
| path_provider_foundation | transitive | 2.6.0 (2026-01-15) | 2.6.0 (2026-01-15) | 0 | flutter.dev | 2023-01-19 / 20 | 160/160 | 0 | **95 A** | native code surface |
| path_provider_linux | transitive | 2.2.1 (2023-08-30) | 2.2.2 (2026-06-24) | 1 | flutter.dev | 2020-05-21 / 21 | 150/160 | 0 | **95 A** | native code surface |
| petitparser | transitive | 7.0.2 (2026-02-14) | 7.0.2 (2026-02-14) | 0 | lukas-renggli.ch | 2012-10-23 / 92 | 160/160 | 0 | **95 A** | community publisher |
| shared_preferences_foundation | transitive | 2.5.6 (2025-11-17) | 2.5.7 (2026-08-28) | 1 | flutter.dev | 2023-01-10 / 24 | 160/160 | 0 | **95 A** | native code surface |
| url_launcher_android | transitive | 6.3.32 (2026-06-02) | 6.3.33 (2026-08-28) | 1 | flutter.dev | 2021-11-23 / 64 | 160/160 | 0 | **95 A** | native code surface |
| url_launcher_ios | transitive | 6.4.1 (2026-02-13) | 6.4.2 (2026-08-28) | 1 | flutter.dev | 2021-11-23 / 28 | 150/160 | 0 | **95 A** | native code surface |
| url_launcher_linux | transitive | 3.2.2 (2025-11-17) | 3.2.3 (2026-08-28) | 1 | flutter.dev | 2020-07-07 / 25 | 150/160 | 0 | **95 A** | native code surface |
| url_launcher_macos | transitive | 3.2.5 (2025-11-17) | 3.2.6 (2026-08-28) | 1 | flutter.dev | 2019-12-10 / 32 | 150/160 | 0 | **95 A** | native code surface |
| url_launcher_web | transitive | 2.4.3 (2026-05-05) | 2.4.3 (2026-05-05) | 0 | flutter.dev | 2019-11-19 / 53 | 150/160 | 0 | **95 A** | native code surface |
| url_launcher_windows | transitive | 3.1.5 (2025-11-17) | 3.1.6 (2026-08-28) | 1 | flutter.dev | 2020-09-12 / 26 | 160/160 | 0 | **95 A** | native code surface |
| webdriver | transitive | 3.1.0 (2025-01-28) | 3.2.0 (2026-09-09) | 1 | google.dev | 2012-11-30 / 85 | 130/160 | 0 | **95 A** | pub points 130/160 |
| xml | transitive | 7.0.1 (2026-04-25) | 7.0.1 (2026-04-25) | 0 | lukas-renggli.ch | 2012-09-25 / 90 | 160/160 | 0 | **95 A** | community publisher |
| async | transitive | 2.13.1 (2026-03-26) | 2.13.1 (2026-03-26) | 0 | dart.dev | 2013-05-09 / 66 | 150/160 | 0 | **100 A** | — |
| clock | transitive | 1.1.2 (2024-10-28) | 1.1.3 (2026-08-28) | 1 | tools.dart.dev | 2014-10-06 / 14 | 140/160 | 0 | **100 A** | — |
| code_assets | transitive | 1.2.1 (2026-06-01) | 2.0.0 (2026-08-18) | 1 | dart.dev | 2025-04-01 / 17 | 160/160 | 0 | **100 A** | — |
| cross_file | transitive | 0.3.5+5 (2026-08-25) | 0.3.5+5 (2026-08-25) | 0 | flutter.dev | 2020-11-19 / 29 | 160/160 | 0 | **100 A** | — |
| ffi | transitive | 2.2.0 (2026-02-11) | 2.2.0 (2026-02-11) | 0 | dart.dev | 2019-09-09 / 32 | 160/160 | 0 | **100 A** | — |
| glob | transitive | 2.1.3 (2025-01-23) | 2.2.0 (2026-08-28) | 1 | tools.dart.dev | 2014-09-23 / 24 | 150/160 | 0 | **100 A** | — |
| hooks | transitive | 2.0.2 (2026-06-02) | 2.2.0 (2026-08-18) | 2 | dart.dev | 2013-08-08 / 25 | 160/160 | 0 | **100 A** | — |
| io | transitive | 1.0.5 (2024-12-10) | 1.1.0 (2026-08-28) | 1 | tools.dart.dev | 2014-01-31 / 15 | 160/160 | 0 | **100 A** | — |
| jni_util | transitive | 1.0.0 (2026-07-27) | 1.0.0 (2026-07-27) | 0 | dart.dev | 2026-07-27 / 1 | 150/160 | 0 | **100 A** | — |
| json_annotation | transitive | 4.12.0 (2026-05-15) | 4.12.0 (2026-05-15) | 0 | google.dev | 2017-09-27 / 41 | 150/160 | 0 | **100 A** | — |
| leak_tracker | transitive | 11.0.2 (2025-09-10) | 11.0.2 (2025-09-10) | 0 | tools.dart.dev | 2022-12-06 / 56 | 160/160 | 0 | **100 A** | — |
| lints | transitive | 6.1.0 (2026-01-30) | 6.1.0 (2026-01-30) | 0 | dart.dev | 2021-04-27 / 17 | 160/160 | 0 | **100 A** | — |
| local_auth_platform_interface | transitive | 1.1.0 (2025-09-30) | 1.1.0 (2025-09-30) | 0 | flutter.dev | 2022-02-14 / 12 | 150/160 | 0 | **100 A** | — |
| matcher | transitive | 0.12.20 (2026-04-27) | 0.12.20 (2026-04-27) | 0 | dart.dev | 2014-03-22 / 51 | 150/160 | 0 | **100 A** | — |
| material_color_utilities | transitive | 0.13.0 (2025-06-04) | 0.13.1 (2026-08-10) | 1 | material.io | 2021-10-28 / 20 | 150/160 | 0 | **100 A** | — |
| meta | transitive | 1.19.0 (2026-07-09) | 1.19.0 (2026-07-09) | 0 | dart.dev | 2012-11-30 / 119 | 150/160 | 0 | **100 A** | — |
| mime | transitive | 2.0.0 (2024-09-30) | 2.1.0 (2026-08-28) | 1 | tools.dart.dev | 2013-06-27 / 64 | 160/160 | 0 | **100 A** | — |
| package_config | transitive | 2.2.0 (2025-03-11) | 3.0.0 (2026-06-08) | 1 | tools.dart.dev | 2015-05-28 / 29 | 150/160 | 0 | **100 A** | — |
| path_provider_platform_interface | transitive | 2.1.3 (2026-06-15) | 2.1.3 (2026-06-15) | 0 | flutter.dev | 2020-02-26 / 17 | 140/160 | 0 | **100 A** | — |
| platform | transitive | 3.1.6 (2024-10-17) | 3.2.0 (2026-09-07) | 1 | dart.dev | 2017-01-25 / 25 | 160/160 | 0 | **100 A** | — |
| pool | transitive | 1.5.2 (2025-09-17) | 1.5.3 (2026-08-28) | 1 | tools.dart.dev | 2014-07-24 / 25 | 160/160 | 0 | **100 A** | — |
| process | transitive | 5.0.5 (2025-07-30) | 5.0.6 (2026-08-28) | 1 | tools.dart.dev | 2017-01-21 / 45 | 150/160 | 0 | **100 A** | — |
| pub_semver | transitive | 2.2.0 (2025-03-07) | 2.2.1 (2026-08-28) | 1 | tools.dart.dev | 2014-09-26 / 30 | 160/160 | 0 | **100 A** | — |
| record_use | transitive | 0.6.0 (2026-03-25) | 1.1.1 (2026-08-18) | 3 | dart.dev | 2024-09-06 / 11 | 160/160 | 0 | **100 A** | — |
| shared_preferences_platform_interface | transitive | 2.4.2 (2026-03-25) | 2.4.2 (2026-03-25) | 0 | flutter.dev | 2019-11-27 / 15 | 150/160 | 0 | **100 A** | — |
| source_maps | transitive | 0.10.13 (2024-12-11) | 0.10.14 (2026-08-20) | 1 | tools.dart.dev | 2013-02-06 / 107 | 150/160 | 0 | **100 A** | — |
| source_span | transitive | 1.10.2 (2026-02-05) | 1.10.2 (2026-02-05) | 0 | tools.dart.dev | 2014-07-24 / 40 | 160/160 | 0 | **100 A** | — |
| stack_trace | transitive | 1.12.1 (2024-12-20) | 1.12.2 (2026-08-28) | 1 | tools.dart.dev | 2013-04-03 / 124 | 160/160 | 0 | **100 A** | — |
| test | transitive | 1.31.1 (2026-04-27) | 1.32.0 (2026-09-02) | 2 | dart.dev | 2015-04-01 / 288 | 150/160 | 0 | **100 A** | — |
| test_api | transitive | 0.7.12 (2026-04-27) | 0.7.14 (2026-09-02) | 2 | tools.dart.dev | 2018-10-26 / 71 | 150/160 | 0 | **100 A** | — |
| test_core | transitive | 0.6.18 (2026-04-27) | 0.6.20 (2026-09-02) | 2 | tools.dart.dev | 2018-11-03 / 129 | 150/160 | 0 | **100 A** | — |
| vector_math | transitive | 2.4.2 (2026-07-29) | 2.4.2 (2026-07-29) | 0 | flutter.dev | 2012-10-18 / 48 | 150/160 | 0 | **100 A** | — |
| vm_service | transitive | 15.2.0 (2026-04-28) | 15.3.0 (2026-08-18) | 1 | tools.dart.dev | 2019-07-25 / 99 | 150/160 | 0 | **100 A** | — |
| watcher | transitive | 1.2.1 (2026-01-08) | 1.2.1 (2026-01-08) | 0 | tools.dart.dev | 2013-07-16 / 70 | 150/160 | 0 | **100 A** | — |
| yaml | transitive | 3.1.3 (2024-12-20) | 3.1.4 (2026-08-28) | 1 | tools.dart.dev | 2013-02-05 / 101 | 160/160 | 0 | **100 A** | — |

## npm packages

| Project | Package | Pinned (published) | Latest (published) | Behind | Maintainers | License | OSV | Score | Deductions |
|---|---|---|---|---|---|---|---|---|---|
| world/browser_tests | fsevents | 2.3.2 (2021-02-05) | 2.3.3 (2023-08-21) | 1 | paulmillr, pipobscure | MIT | 0 | **70 C** | no release in 37 months; pinned version >18 months old |
| marketplace forkmesh-ide (vscode) | @types/node | 18.19.130 (2025-10-09) | 26.5.1 (2026-09-09) | 136 | types | MIT | 0 | **85 B** | 136 releases behind; single maintainer |
| marketplace forkmesh-ide (vscode) | undici-types | 5.26.5 (2023-10-23) | 8.10.2 (2026-09-04) | 121 | matteo.collina, ethan_arrowood | MIT | 0 | **85 B** | pinned version >18 months old; 121 releases behind |
| marketplace forkmesh-ide (vscode) | @types/vscode | 1.125.0 (2026-06-17) | 1.137.0 (2026-09-09) | 3 | types | MIT | 0 | **90 A** | single maintainer |
| app/browser_tests | @playwright/test | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, dgozman-ms, microsoft-oss-releases, microsoft1es | Apache-2.0 | 0 | **100 A** | — |
| app/browser_tests | playwright | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, microsoft1es, dgozman-ms, microsoft-oss-releases | Apache-2.0 | 0 | **100 A** | — |
| app/browser_tests | playwright-core | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, microsoft1es, dgozman-ms, microsoft-oss-releases | Apache-2.0 | 0 | **100 A** | — |
| marketplace forkmesh-ide (vscode) | typescript | 5.9.3 (2025-09-30) | 7.0.2 (2026-07-08) | 3 | microsoft1es, typescript-bot, weswigham, andrewbranch, typescript-deploys, microsoft-oss-releases, jakebailey | Apache-2.0 | 0 | **100 A** | — |
| world/browser_tests | @playwright/test | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, dgozman-ms, microsoft-oss-releases, microsoft1es | Apache-2.0 | 0 | **100 A** | — |
| world/browser_tests | playwright | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, microsoft1es, dgozman-ms, microsoft-oss-releases | Apache-2.0 | 0 | **100 A** | — |
| world/browser_tests | playwright-core | 1.61.0 (2026-06-15) | 1.63.0 (2026-09-04) | 4 | pavelfeldman, yurys, microsoft1es, dgozman-ms, microsoft-oss-releases | Apache-2.0 | 0 | **100 A** | — |
| world/browser_tests | three | 0.184.0 (2026-04-16) | 0.186.0 (2026-09-08) | 3 | mrdoob, mugen87 | MIT | 0 | **100 A** | — |

`@types/node` 18.19.130 in the VS Code extension is 136 releases behind (the extension targets VS Code ≥ 1.75 whose runtime is Node 18+, so this is a typing choice, not a runtime risk). `fsevents` 2.3.2 is an optional macOS-only Playwright dependency, last released 2023-08.

## Python

| Package | Pinned (published) | Latest | Authors | Releases | OSV | Score |
|---|---|---|---|---|---|---|
| pytest (CI only) | 9.1.1 (2026-06-19) | 9.1.1 — current | pytest core team (Holger Krekel, Bruno Oliveira, Ronny Pfannschmidt, Florian Bruhin, Floris Bruynooghe) | 192 | 0 | **100 A** |

The Cloudflare Python Worker (`app/`) declares no third-party packages: `pyproject.toml` has an empty dependency list and `disable_python_external_sdk` keeps it on the bundled Pyodide standard library. Nothing to audit beyond the runtime Cloudflare ships.

## Go (mirror node)

`server/go.mod` declares no modules; the binary is standard library only, so its whole third-party surface is the toolchain it is compiled with.

| Component | In use | Latest | Open stdlib advisories (OSV) | Score |
|---|---|---|---|---|
| Go toolchain | go1.24.4 (June 2025) | go1.27.1 (1.24 line ends at go1.24.13) | **47** at 1.24.4, 29 at 1.24.13, 0 at 1.27.1 | **40 F** |

Representative open advisories at go1.24.4: GO-2026-4918 (HTTP/2 transport infinite loop), GO-2026-4870 (TLS 1.3 KeyUpdate connection retention), GO-2026-4341 / GO-2026-6218 (net/url memory exhaustion & quadratic paths), GO-2026-4602 / GO-2026-4864 / GO-2026-4970 (os.Root escapes), GO-2026-4869 (archive/tar unbounded allocation), GO-2025-4013 (crypto/x509 panic on DSA keys). The mirror node serves git over HTTPS with net/http and crypto/tls, so these are reachable from the network. **Action:** build with go1.27.1 (or at minimum go1.26.8) and rebuild the fleet daemon.

## Android and iOS build components

| Component | In use | Latest | Publisher | Score |
|---|---|---|---|---|
| Android Gradle Plugin | 9.1.0 | 9.4.0 (2026-09-10) | Google | 85 B (three minors behind) |
| Kotlin Gradle plugin | 2.4.0 | 2.4.20 (2026-09-07) | JetBrains | 90 A |
| Flutter Gradle plugin loader | 1.0.0 | 1.0.0 | Flutter team | 100 A |
| CocoaPod flutter_secure_storage | 6.0.0 | tracks the Dart package | steenbakker.dev | 85 B (upgrade with the Dart package) |

## Vendored browser libraries (world)

| Library | Vendored | Latest | Author | OSV | Score |
|---|---|---|---|---|---|
| three.js | 0.184.0 (2026-04-16) | 0.186.0 (2026-09-08) | mrdoob, mugen87 | 0 | 95 A |
| satellite.js | 7.1.0 (2026-07-23) | 7.1.0 — current | shashwatak, ezze | 0 | 100 A |

The world and www pages load no scripts from CDNs at runtime; three.js is served from `world/public/world/vendor`. The Worker's Content-Security-Policy allow-lists `https://cdn.jsdelivr.net` for scripts (`app/src/app_runtime.py:36`, `app/src/entry.py:17882`); if nothing still needs it, dropping that origin from `script-src` would tighten the policy. The vendored three.js 0.184.0 example addons reference skypack/jsdelivr URLs for optional physics and loader modules, but the world imports only `three.module.min.js`.

## Desktop native libraries (Debian 13 system packages)

| Library | Installed | Source | Score |
|---|---|---|---|
| Qt 6 (Widgets, Network, Svg, Concurrent, DBus) | 6.8.2+dfsg-9+deb13u2 | Debian security-maintained | 95 A |
| OpenSSL | 3.5.7 (deb13u2) | Debian security-maintained | 95 A |
| libwebp | 1.5.0 | Debian | 95 A |
| Python 3 (build-time only) | 3.13.5 | Debian | 100 A |

These are patched through Debian's security channel rather than pinned by the repository; the `+deb13u2` suffixes show the security updates are applied.

## Toolchains

| Toolchain | In use | Latest | Score |
|---|---|---|---|
| Flutter SDK | 3.47.0 (2026-08-11) | 3.47.4 (2026-09-11) | 90 A |
| Dart SDK | 3.13.0 | 3.13.3 | 90 A |
| Go | 1.24.4 | 1.27.1 | 40 F — see above |
| Node.js (tests, wrangler) | 22.23.1 | 22.23.2 on the 22 line; 24.21.0 newest LTS | 90 A |
| wrangler | 4.131.1 | 4.131.1 — current | 100 A |

## Score distribution (Dart + npm packages)

| Grade | Count |
|---|---|
| A | 108 |
| B | 21 |
| C | 8 |
| D | 2 |
| F | 1 |

Lowest ten: flutter_secure_storage_macos (40), js (60), node_preamble (65), fsevents (70), nested (75), path_provider_windows (75), shared_preferences_linux (75), shared_preferences_windows (75), shelf_packages_handler (75), sync_http (75).

## Recommended actions, in order

1. Rebuild `forkmesh-mirror-node` with go1.27.1 and roll the mirror fleet (closes 47 standard-library advisories; no code change expected for a stdlib-only module).
2. `flutter pub upgrade --major-versions` for `flutter_secure_storage` (11.x), `file_picker` (12.x), `device_info_plus` (13.x) and `archive` (4.3.0); run the mobile test suite and re-check the iOS pod.
3. Bump Flutter to 3.47.4 (patch), Kotlin to 2.4.20 and AGP to 9.4.0 in `mobile/android/settings.gradle.kts`.
4. `npm update` in `app/browser_tests`, `world/browser_tests` (Playwright 1.63, three 0.186) and the VS Code extension (`@types/node` 26, TypeScript 7 — check the extension still compiles under its `engines.vscode` floor).
5. Re-run this audit after each round: `python3` scripts used for this report live in the session scratchpad; the data sources are pub.dev `/api/packages/<name>{,/publisher,/score,/advisories}`, registry.npmjs.org, PyPI JSON, go.dev/dl and OSV `v1/query`.
6. Consider narrowing the Worker CSP `script-src` if the jsdelivr allowance is no longer used.

