# Third-party dependencies and their security audit

Every third-party component ForkMesh uses, why it is kept, and how each one is
audited for risk: publisher, maintenance history, known advisories and a score.
The filterable report is at
**[the dependency audit report](/docs/dependency-audit/)**.
Part of [Security](README.md); signed releases are covered on
[Encryption and signatures](../encryption.md#software-distribution).

## Inventory

The Go mirror node (`server/`) and the app source (`app/pyproject.toml`) use
only their standard libraries; the third-party packages the cores need are
pinned in `app/host/requirements.txt` ([docs/host/README.md](../host/README.md));
the iOS packages resolve nothing remote, and the desktop links Qt, OpenSSL and
(optionally) libnghttp3 from the system.
`marketplace/packages/docs/data/dependencies.json` is the full inventory
(`marketplace/tools/docs_dependencies.py`).

Every manifest and vendored library was last reviewed on 2026-10-29 for
packages we could drop or replace with our own code.

## Removed

| Dependency | Where | Why it could go |
|---|---|---|
| `three` (npm) | `world/browser_tests` | No test imported it. The browser tests load the vendored `world/public/world/vendor/three-0.184.0` that World serves, and `app/tools/render_call_backgrounds.cjs` maps `three` to the same file with an import map. `world/tests/test_world_source_layout.py` keeps it out. |
| `watchfiles`, `python-dotenv` | `app/host/requirements.txt` | They came with `uvicorn[standard]`, for `--reload` and `--env-file`, which `forkmesh_host serve` never uses. `watchfiles` was a compiled Rust wheel on every core node. The host pins `uvicorn` plus the three extras it does use: `uvloop`, `httptools`, `websockets`. |
| `numpy` | `app/tools/call_video_metrics.py` | It only took means, medians, a 95th percentile, point distances and mean grey-level differences. `math`, `statistics` and Pillow's `ImageChops`/`ImageStat` give the same numbers (checked against the numpy version on random landmarks and frames). The tool needs only Pillow. |

## Kept

| Dependency | Why |
|---|---|
| `cryptography` | Ed25519, X25519, P-256, RSA, AES-GCM and HKDF in 28 source files (60 with tests); we do not write our own cryptography. |
| `httpx`, `httpcore`, `h2` | The host's outbound HTTP with the egress guard (`egress.py`); `h2` because APNs only speaks HTTP/2. |
| `uvicorn`, `uvloop`, `httptools`, `websockets` | The ASGI server, its event loop, its HTTP parser and the WebSocket protocol the rooms of the cores' Durable Object emulation (`durable.py`) and the mesh listener use. |
| `orjson` | The `JSON.stringify`/`JSON.parse` shim in `forkmesh_host/js.py` runs on every request; it falls back to `json` when missing, so it is a speed choice, not a hard need. |
| `Pillow` | PNG/WebP decode and encode for avatars and pull-request cards. |
| `PyYAML` | Tests and release tools read `codemagic.yaml` and workflow files. |
| `numpy` in `build_call_face_assets.py` | It is the array engine under our own TFLite interpreter; the arithmetic is the point of the tool. |
| `ctranslate2`, `sentencepiece`, `fontTools`, `pycairo`, `brotli` | Single offline tools (forge translation, icon font subsetting, logo rendering, deploy compression with a Node fallback). |
| Vendored `three` (World and code map) | The renderer. The two copies stay separate because an instance can run the app with the World plugin turned off. |
| Vendored `satellite-js` | SGP4 orbit propagation; a rewrite would need its own validation suite. |
| `esbuild`, `@playwright/test`, `typescript` | Build and test tools only. |
| Android: Compose, `activity-compose`, `kotlinx-coroutines-android` | The UI toolkit and the coroutine runtime. |
| Go: `golang.org/x/mobile` | `gomobile bind` for the phone build of the mirror node (`native/fmm`). |

## Next candidate

`www/public/assets/vendor/lucide-0.468.0.min.js` is 358 KB and carries every
Lucide icon. The dashboard uses about 200 literal names, but 264 places build
the name at run time (`data-lucide="${...}"`, `dataset.lucide`), so a
generated subset needs a list of every dynamic name first, and a test that
fails when a page asks for an icon the subset lacks. Until then a missing
icon would fail silently as a blank square.

## Audit method

Package metadata comes from each ecosystem's registry (npm, PyPI, go.dev,
Gradle/Maven plugin indexes) and advisories from the OSV database, queried per
package and exact pinned version. Toolchains (Go, Kotlin, Android Gradle
Plugin) are audited alongside libraries, since a stale toolchain can carry
standard-library advisories even when every package is current.

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
