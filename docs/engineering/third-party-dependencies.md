# Third-party dependencies: what we keep and why

A review on 2026-10-29 of every manifest and vendored library, looking for
packages we could drop or replace with our own code. The runtime was already
lean: the Go mirror node (`server/`) and the Worker app (`app/pyproject.toml`)
use only their standard libraries, the iOS packages resolve nothing remote, and
the desktop links Qt, OpenSSL and (optionally) libnghttp3 from the system.
`marketplace/packages/docs/data/dependencies.json` is the full inventory
(`marketplace/tools/docs_dependencies.py`).

## Removed

| Dependency | Where | Why it could go |
|---|---|---|
| `three` (npm) | `world/browser_tests` | No test imported it. The browser tests load the vendored `world/public/world/vendor/three-0.184.0` that World serves, and `app/tools/render_call_backgrounds.cjs` maps `three` to the same file with an import map. `world/tests/test_world_source_layout.py` keeps it out. |
| `watchfiles`, `python-dotenv` | `app/host/requirements.txt` | They came with `uvicorn[standard]`, for `--reload` and `--env-file`, which `forkmesh_host serve` never uses. `watchfiles` was a compiled Rust wheel on every core node. The host now pins `uvicorn` plus the three extras it does use: `uvloop`, `httptools`, `websockets`. |
| `numpy` | `app/tools/call_video_metrics.py` | It only took means, medians, a 95th percentile, point distances and mean grey-level differences. `math`, `statistics` and Pillow's `ImageChops`/`ImageStat` give the same numbers (checked against the numpy version on random landmarks and frames). The tool now needs only Pillow. |

## Kept

| Dependency | Why |
|---|---|
| `cryptography` | Ed25519, X25519, P-256, RSA, AES-GCM and HKDF in 28 source files (60 with tests); we do not write our own cryptography. |
| `httpx`, `httpcore`, `h2` | The host's outbound HTTP with the egress guard (`egress.py`); `h2` because APNs only speaks HTTP/2. |
| `uvicorn`, `uvloop`, `httptools`, `websockets` | The ASGI server, its event loop, its HTTP parser and the WebSocket protocol the Durable Object rooms and the mesh listener use. |
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
