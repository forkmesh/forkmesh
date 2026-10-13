# Worker footprint

`world/tools/build_worker_footprint.py` measures the App's deployed Python
source and the World's static assets, enforces the World's static budgets, and
writes `worker-footprint.json` for the desktop's Logs → Build tab.

## Cloudflare startup budget retired (2026-09-30)

The App used to run as a Python Worker on Workers Free. Cloudflare validates a
Python Worker at upload inside a fixed memory allowance, and going over it
failed the deploy with error **10021** ("Python startup exceeded memory
limits"). The tool guarded that with `startupSourceBytesSoft` (1,730,000
bytes), set below the 1.786 MB staged Worker Cloudflare rejected on 2026-08-21,
and carried the other Workers limits in `WORKER_LIMITS` (`memoryBytes`,
`compressedBundleFreeBytes`, `compressedBundlePaidBytes`,
`uncompressedBundleBytes`, `startupTimeMs`, `dynamicRequestsFreeDaily`).

Production now runs on the self-hosted core nodes (ForkMesh Next,
[../NEXT_ADOPTION.md](../NEXT_ADOPTION.md)), not on Cloudflare Workers, so on
2026-09-30 the startup budget and `WORKER_LIMITS` were removed:

- `validate_budgets` no longer fails on startup source;
- the report no longer carries `workerLimits`;
- the desktop's Build tab lists "Estimated Python startup source" as a plain
  measurement in the Deployed footprint table instead of a bar against a
  budget.

`estimatedStartupSourceBytes`, the module list with load phases, and
`LAZY_MODULES` stay, because the report and the app tests still use them.
Keeping modules lazy still trims what every core-node App process imports at
start, but nothing fails when startup source grows.

## What ships

`app/tools/build_worker_python.py` writes `app/src_build/`: `app/src`,
`world/src` and every catalogued package's `content/worker/` directory,
round-tripped through Python's AST with comments and docstrings removed.
`app/wrangler.toml` points `main` at `src_build/entry.py`. The `[build]`
command runs, in order: `build_dashboard_assets.py`, `build_worker_python.py`,
`build_site_assets.py app`, and `world/tools/build_worker_footprint.py
--staged --report dist/worker-footprint.json`.

## Budgets

`STATIC_LIMITS` in `world/tools/build_worker_footprint.py` holds the budgets
that remain. `validate_budgets` fails the build (and the pytest that calls it)
when one is exceeded:

- `initialWorldModuleBytesSoft` (2,650,000): the initial World ES-module
  graph. This is a project budget, not a Cloudflare cap: the World is still
  served, and it keeps the first visit from pulling in every optional module.
- `assetCount` (20,000) and `maxAssetBytes` (25 MiB): World static asset count
  and the largest single asset.

Startup source (reported, no longer budgeted) is `entry.py`, `app_runtime.py`, `world_durable_objects.py`,
and, in CI, the workers SDK that pywrangler vendors into `python_modules`
(a local `--staged` run leaves it out, so CI measures higher). Every other
module must be **on-demand**: listed in `LAZY_MODULES`, or a marketplace
package worker module (derived by `_marketplace_worker_modules`).

## Adding a module

1. Import it in `entry.py` through `_LazyModule("name")`, never at module top level.
2. Add `name.py` to `LAZY_MODULES` in `world/tools/build_worker_footprint.py`.
3. Run `python3 app/tools/build_worker_python.py && python3 world/tools/build_worker_footprint.py --staged`.

`app/tools/extract_lazy_module.py` moves a family of functions out of
`entry.py`; `app/tools/patch_entry_test_loaders.py` fixes the source-text test
loaders that slice `entry.py`.

## Budget bookkeeping (2026-09-27)

The footprint check was failing at 1,906,702 bytes of startup source. The
growth was not new startup code. Seventeen modules that are reached only
through a `_LazyModule` proxy, or only from inside such a module, were never
added to `LAZY_MODULES`:

- `apps_runtime`, which pulls in `apps`, `apps_api`, `apps_commands`,
  `apps_core`, `apps_directory` and `apps_events`;
- `runner_leases`, imported inside the `/api/runners` route and by
  `hosted_repos`, which pulls in `runner_logins`;
- `core_push`, `mail_budget`, `mail_log`, `mail_tracking`,
  `marketplace_reviews`, `node_control`, `smtp_mail` and `turnstile`.

With those modules listed, startup source is 1,654,437 bytes. The footprint
test no longer asserts that the World's static assets outweigh the App's
Python. Those files belong to two different Workers, so the old comparison
proved nothing and broke once the App grew.

A checkout that has run `pywrangler sync` has `app/python_modules`, and the
check counts that vendored workers SDK (95 KB) toward startup. The estimate is
then 1,749,435 bytes, and three footprint tests still fail. Getting under
budget there means moving about 20 KB or more out of `entry.py` with
`extract_lazy_module.py`, or retiring the Free-plan budget now that the served
stack no longer deploys to Cloudflare. Neither was done on 2026-09-27; the
budget was retired on 2026-09-30 (see above).

## Diagnosing startup-source growth

- Confirm production is still on the old rev: `https://app.forkmesh.com/api/version`.
- Stage the last good rev and `main` in two scratch worktrees, run
  `build_worker_python.py` in each, and compare
  `footprint(from_stage=True)["modules"]` from `build_worker_footprint.py` by
  module and phase. The growth is usually a new module missing from
  `LAZY_MODULES` or new top-level code in `entry.py`.

## Related

- [App Worker home](README.md)
- `www/docs/operations/cloudflare-free-tier-audit.md`
