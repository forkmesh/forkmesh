# Worker footprint

`world/tools/build_worker_footprint.py` measures the App's deployed Python
source and the World's static assets, enforces the World's static budgets,
and writes `worker-footprint.json` for the desktop's Logs → Build tab.
Startup source size is reported as a measurement only; it is not budgeted,
since the App runs on the core nodes rather than a hosted Worker runtime.

## What ships

`app/tools/build_worker_python.py` writes `app/src_build/`: `app/src`,
`world/src` and every catalogued package's `content/worker/` directory,
round-tripped through Python's AST with comments and docstrings removed. The
build runs, in order: `build_dashboard_assets.py`, `build_worker_python.py`,
`build_site_assets.py app`, and
`world/tools/build_worker_footprint.py --staged --report dist/worker-footprint.json`.

## Budgets

`STATIC_LIMITS` holds the budgets; `validate_budgets` fails the build (and
the pytest that calls it) when one is exceeded:

- `initialWorldModuleBytesSoft` (2,650,000): the initial World ES-module
  graph, so a first visit does not pull in every optional module.
- `assetCount` (20,000) and `maxAssetBytes` (25 MiB): World static asset
  count and the largest single asset.

Startup source is `entry.py`, `app_runtime.py`, `world_durable_objects.py`
and the vendored SDK. Every other module must be **on-demand**: listed in
`LAZY_MODULES`, or a marketplace package worker module
(`_marketplace_worker_modules`).

`test_every_lazily_proxied_module_is_budgeted_as_on_demand` derives the
expected list from the `_LazyModule("name")` proxies. A proxied module that a
startup module also imports at top level (`hidden_unicode`, imported by
`app_runtime.py`) is parsed at startup anyway, so it stays a startup module.
On 2026-10-24, 37 proxied modules added since the list was last kept (account
names, comment reactions, instance console, instance home and privacy, pull
rules, self-update and others) were added to `LAZY_MODULES`.

## Adding a module

1. Import it in `entry.py` through `_LazyModule("name")`, never at top level.
2. Add `name.py` to `LAZY_MODULES` in `world/tools/build_worker_footprint.py`.
3. Run `python3 app/tools/build_worker_python.py && python3 world/tools/build_worker_footprint.py --staged`.

`app/tools/extract_lazy_module.py` moves a family of functions out of
`entry.py`; `app/tools/patch_entry_test_loaders.py` fixes the source-text
test loaders that slice `entry.py`.

## Diagnosing growth

Stage two revisions, run `build_worker_python.py` in each, and compare
`footprint(from_stage=True)["modules"]` by module and phase. Growth is usually
a module missing from `LAZY_MODULES` or new top-level code in `entry.py`.
