# Large file split plan

Status: plan only (2026-10-13). No file has been split yet. Each split below is
a separate branch made of move-only commits; nothing here changes behaviour.

Sizes are from `git ls-tree -r -l HEAD` at `ae985af229`. Vendored three.js and
lucide bundles, `conformance/*/fixture.json`, `desktop_go_new/internal/qtjson/testdata/golden.json`,
`tools/ecosystem_third_parties.json` and font codepoint maps are data or third
party code and are out of scope.

## Ranked table

Rank is payoff divided by effort and risk. Effort is in agent-days, including
fixing the tests that read source text.

| # | File | Size | Generated? | Proposed pieces | Effort | Payoff |
|---|---|---|---|---|---|---|
| 1 | `app/public/dashboard/js/06-repo-content.js` (+ `04-account.js`, `07-repo-compose-branch.js`) | 760 KB (+283, +233) | No (input to `dashboard.js`) | 06 → 7 fragments, 04 → 4, 07 → 3 | 1–1.5 | High: built `dashboard.js` stays byte-identical, so the split is provably a no-op; the hottest web file stops conflicting |
| 2 | `docs/world/README.md` | 356 KB | No | Reference page + `docs/world/changes/2026-09.md`, `2026-10.md` | 0.25 | Medium: nothing reads it, so it carries zero risk |
| 3 | `desktop/tests/test_window_resize.cpp` | 1.98 MB | No | `main()` driver + ~14 `tests/window/*.cpp` run functions + support header | 3–4 | High: one 23.6k-line `main()` is the slowest single TU in the window-test target, and every desktop feature edits it |
| 4 | `desktop/src/MainWindowIssues.cpp` | 587 KB | No | Move the misplaced chat and frame code out, then 4 parts | 1.5 | High: shorter critical-path TUs, fewer conflicts |
| 5 | `desktop/src/MainWindowPulls.cpp` | 580 KB | No | 4 parts | 1.5 | High |
| 6 | `desktop/src/MainWindowRepoDetail.cpp` | 633 KB | No | 5 parts | 2 | High |
| 7 | `desktop/src/MainWindowBranches.cpp` | 545 KB | No | 4 parts | 1.5 | Medium-high |
| 8 | `desktop/src/MainWindowSettings.cpp` | 493 KB | No | 4 parts (network log first) | 1.5 | Medium-high |
| 9 | `desktop/src/MainWindowActions.cpp` | 420 KB | No | 3 parts | 1 | Medium |
| 10 | `world/browser_tests/tests/world.spec.js` | 427 KB | No | Helpers module + ~8 spec files | 1 | Medium: runner picks new files up on its own |
| 11 | `app/src/entry.py` | 2.31 MB | No (source; `src_build/entry.py` is generated from it) | Continue `_LazyModule` extraction: ~8 domain modules, ~25k lines | 6–10, in rounds | High for conflicts and review, but costly because ~312 test files read entry.py as text |
| 12 | `marketplace/packages/crm/content/desktop/CrmFeature.cpp` | 530 KB | No | `CrmFeature*.cpp` × 4 + `CrmCommon.h` (the Marketing precedent) | 1.5 | Medium: one owner, but no precompiled header, so every edit recompiles all 11.6k lines |
| 13 | `desktop/tests/test_crypto.cpp` | 677 KB | No | `main()` + ~8 `test_crypto_*.cpp` run functions | 2 | Medium |
| 14 | `world/public/world/world.js` | 1.31 MB | No (staged as `world-runtime.js`) | Core class + ~10 prototype-mixin modules | 4–6 | Medium-high: conflicts; later, lazy-loading parts cuts first-load bytes |
| 15 | `world/public/world/world.css` | 465 KB | No | 5 contiguous sheets linked in order | 1 | Medium-low |
| 16 | `marketplace/packages/marketing/content/desktop/MarketingFeature.cpp` | 388 KB | No | 2–3 more parts beside `MarketingPages.cpp` | 1 | Low-medium |
| 17 | `desktop/src/MainWindowInternal.h` | 420 KB | No | Feature-only widget classes → per-feature headers outside the precompiled header | 2–3 | Medium: fewer changes to the precompiled header |
| 18 | `tools/mirror_gateway.py` | 439 KB | No | Package `tools/mirror_gateway_pkg/` — **deferred** | 3+ | Low: deploy lists in 7 places and the user installer copy a single file |
| 19 | `app/src/schema.py` | 250 KB | No | Leave as is | — | None: pure data, pinned by the fingerprint test |
| 20 | `desktop/src/MainWindow.h` | 788 KB | No | **Do not split** (measured and rejected on 2026-09-22) | — | — |

Generated large files. These need a build change, not a split:

| File | Size | Built from | Kept honest by |
|---|---|---|---|
| `app/public/dashboard.js` | 2.08 MB | `app/public/dashboard/js/*.js` via `app/tools/build_dashboard_assets.py` → `app/src/dashboard_bundle.py` (`FRAGMENTS` tuple, plain `"".join`) | `app/tests/test_dashboard_bundle.py::test_static_dashboard_js_is_built_from_fragments` (byte-equal) |
| `app/public/dashboard-page-*.js` | small | the 12 `LAZY_CHUNKS` fragments | same test |
| `www/public/*.html` (`index.html` 338 KB, `market.html` 304 KB) | — | `www/templates/pages/**` via `app/tools/site_templates.py`; market data from `catalog.json` via `app/tools/build_market_catalog.py` | `www/tests/test_site_templates.py`, `build_site_assets.py`, `tools/www_push_check.py`, `test_market_landing.py` |
| `app/src_build/entry.py` | — | `app/tools/build_worker_python.py` (comments and docstrings stripped) | not committed |
| `desktop/src/forkmesh_autogen.cpp` | small | hand-kept list of `moc_*.cpp` includes | link errors |

Optional build change for `dashboard.js`: stop committing it and the `?v=`
stamps in the 8 built pages, and build them in deploy and in the test fixture
instead. That would remove the stamp merge conflict that nearly every
dashboard branch hits. It is a separate decision: the host, the `www`
wrangler configs and ~50 tests read the committed bundle by path. It is not
part of this plan.

## Rules for every split

- **Move-only commits.** One commit moves code with no edits to the moved
  lines. A second commit fixes the build lists and tests. Review the first
  with `git diff --color-moved=dimmed-zebra --color-moved-ws=allow-indentation-change`;
  blame follows with `git blame -C -C`. Never reformat while moving: the JS
  tests pin indentation.
- **Cut only at top-level boundaries.** That means a top-level function in
  JS/Python, a `MainWindow::` definition in C++, or a `main()` block in the C++
  tests. Never cut inside an anonymous namespace.
- **No comments or docstrings in new files** (AGENTS.md). Put the reason for
  each split in the subsystem's doc page and link this plan. Note that
  `app/tools/extract_lazy_module.py`'s `HEADER` template still writes a module
  docstring and has to lose it first.
- **Find text readers before moving, not after.** Run
  `grep -rln '<file name>' app/tests world/tests www/tests desktop/tests marketplace/packages/*/tests deploy/tests app/host`,
  then `grep -rn '<moved literal>'` for each pinned string. Run the suites as a
  diff of `FAILED` lists against a detached `main` worktree. The suites fail
  some tests on an untouched main, so only new failures count.
- **One file per branch, landed fast.** Parallel agents edit these files all
  day. A split branch that lives longer than a few hours collects conflicts in
  code it moved. Pre-announce, split, merge main, and land the same day.
  Splitting during a quiet window (e.g. right after a release) helps.
- **After landing, conflicts on old branches resolve by re-applying the edit
  to the new file.** Git's rename detection does not follow a function into a
  new file. Tell the dispatcher to rebase long-lived agent branches after
  each split lands.

## 1. Dashboard fragments (`06-repo-content.js`, `04-account.js`, `07-repo-compose-branch.js`)

**How it loads.** All non-lazy fragments are slices of one IIFE opened in
`01-state.js` and closed in `10-boot.js`, so they share closure scope. A new
fragment is just a new file plus a new entry in `FRAGMENTS` in
`app/src/dashboard_bundle.py`, in the same position. Nothing checks for files
that are not listed, so a fragment left out of `FRAGMENTS` silently vanishes.

**Pieces**

- 06 (14,239 lines, 587 functions):
  - `06a-markdown-tree` (1–800)
  - `06b-issue-records` (800–2400)
  - `06c-pull-diff-review` (2400–5600)
  - `06d-issue-detail` (5600–8700, including projects/Gantt)
  - `06e-repo-settings` (8800–10450)
  - `06f-repo-agents` (10450–13450)
  - `06g-desktop-control` (13450–end)

  Use letter suffixes so `06-repo-actions` keeps its place.
- 04 (5,969 lines):
  - `04a-account-settings` (1–1270)
  - `04b-orgs` (1270–2310)
  - `04c-profile` (2310–5530)
  - `04d-node-claim` (5530–end)
- 07 (4,128 lines):
  - `07a-compose` (1–1186)
  - `07b-mirrors-releases-insights` (1186–2530)
  - `07c-branches` (2530–end)

**Method.** Write a small script that cuts the file at the listed function
names, writes the parts, and replaces the entry in `FRAGMENTS` with the new
names in order. Run `build_dashboard_assets.py`. It must report **no change**
to `dashboard.js` or the built pages. That is the correctness proof.

**Tests that break.** These read the fragment by path:
- 06: 36 files
- 04: 21 files
- 07: 12 files
- `app/browser_tests/tests/agent-mobile-input.spec.js`

Point each one at `assembled_dashboard_js()` from `app/tests/_dashboard_bundle.py`.
That helper survives any future split. Do not repoint them at the new part
files. The ~50 tests that read the built `dashboard.js` are unaffected.

**Gates**
- `pytest app/tests/test_dashboard_bundle.py` plus the 70 repointed files
- `python3 tools/quality_gate.py run --profile core`
- a dashboard browser smoke test

**Traps**
- The tailwind scanner reads `./public/dashboard.js`, so it is unaffected.
- Do not re-indent the fragments.

## 2. `docs/world/README.md`

Lines 1–601 are reference material: code map, architecture, protocols,
configuration, build and test, performance gate, operations, security, traps
and recent changes. Lines 602–5749 are ~50 dated round write-ups
(2026-09-26 … 2026-10-09).

Move those write-ups to `docs/world/changes/2026-09.md` and `2026-10.md`. Leave
a one-line index under "Recent changes" and fold durable traps into "Traps".

No tool or test reads the file, and `www/docs` is generated from a different
tree. Gate: a link check of `docs/README.md`.

## 3. `desktop/tests/test_window_resize.cpp`

**Shape**
- 36,883 lines, with no QTest and no `Q_OBJECT`.
- An anonymous namespace (151–13300) holds `check()`, the probe classes and
  ~71 `checkXxx(MainWindow&)` / `testXxx()` functions.
- `int main` (13304–36883) parses 50 `--*-only` flags and runs inline blocks.
- The file has 2,359 `check(` calls.
- Target: `forkmesh-window-tests` in `desktop/CMakeLists.txt` (~2817). It
  recompiles all of `FORKMESH_APP_SOURCES` without the precompiled header.
- CTest registers ~25 flag runs at ~3951.
- Precedent: `tests/workspace_snapshots.cpp` exposes
  `int runWorkspaceSnapshots(MainWindow&)`, declared in the main file.

**Pieces**
- `tests/window/WindowTestSupport.h/.cpp`: `check()`, the failure counter,
  `captureMessages`, the probe classes, the DeferredDelete flush helper and the
  `forkmesh::ui` forward declarations. Move them out of the anonymous
  namespace into `namespace windowtest`.
- One file per area: `tests/window/{Market,Teammates,MirrorFleet,DiffStream,Agents,Chat,Issues,Pulls,Branches,RepoDetail,Settings,Ledger,Pings,Layout}Checks.cpp`.
  Each exports `int run<Area>(MainWindow&, const WindowTestOptions&)`.
  Derive the area list from the `--*-only` flags and the existing `checkXxx`
  prefixes.
- `test_window_resize.cpp` keeps flag parsing and the call order.

**Order**
1. Move the support code (one commit).
2. Move one subset that has its own flag, e.g. `--package-only` (Market).
3. Move the remaining subsets one area per commit.
4. Move the inline blocks of `main`, keeping their order. Shared locals
   between blocks become `WindowTestOptions` or `WindowTestContext` fields.

**Tests that break**
- `test_window_resize.cpp:13477` asserts a ticket source `.endsWith("test_window_resize.cpp")`.
  Keep that block in the main file, or change the literal.
- `app/tests/test_offline_ci_workflow.py:165` pins the order
  `MainWindow window;` → `if (mirrorFleetOnly)` → `return runMirrorFleetOnly(window);`
  in this file. Keep that sequence in the main file.
- `app/tests/test_qt_repo_detail_git_off_gui.py:257` wants
  `forkmesh::ui::prefetchDiffImageBlobs(` in this file. Repoint it to a glob
  over `tests/window/*.cpp`.
- The market counts (`marketCards.size() == 5`-style pins) move with their
  block. No change is needed.

**Gates**
- Configure `build-ci` with `FORKMESH_BUILD_TESTS=ON`.
- Run `ninja forkmesh-window-tests` with `PIPESTATUS` checked, not piped
  through `tail`.
- Run the binary in an isolated `HOME`/`XDG_*` once in full and once per
  `--*-only` flag. Diff the per-check output against main; the check count
  must stay 2,359.

**Traps**
- Helpers with internal linkage must become `inline` or move into the
  support `.cpp`, or the link fails with duplicate symbols.
- Static initialisation order: build the probes inside the functions, not as
  globals.
- New `.cpp` files go into the explicit list for the target. They need no moc,
  because the target has AUTOMOC on and has no `Q_OBJECT` classes.

**Payoff**
- One 23.6k-line function is the worst case for the optimiser. The parts
  compile in parallel and only the edited part recompiles.
- Nearly every desktop feature branch appends here, so conflicts drop the
  most of any desktop split.

## 4–9. `MainWindow` feature `.cpp` files

**Mechanics shared by all six**
- New units go into `FORKMESH_MAIN_WINDOW_SOURCES` (`desktop/CMakeLists.txt`
  447–564; keep it alphabetical). All three targets (`forkmesh`,
  `forkmesh-window-tests`, `forkmesh-perf-tests`) pick them up.
- No moc work is needed. None of these files has `Q_OBJECT` or `.moc`
  includes.
- Anonymous-namespace helpers used by more than one part move into
  `MainWindow<Feature>Internal.h`, which is not in the precompiled header
  (precedents: `MainWindowAgentsInternal.h`, `MainWindowReposInternal.h`).
  - Keep them `static`/`inline` or give them one definition in
    `MainWindow<Feature>Internal.cpp`.
  - Helpers used by only one part move with that part.
- Do **not** touch `MainWindow.h` or `MainWindowInternal.h`. A split that
  edits either one recompiles ~120 units and conflicts with everyone.

**Precedent for the tests.** `MainWindowChat.cpp` and `MainWindowRepos.cpp`
were split this way before, and their tests read a tuple of files in the
original order (`MAIN_WINDOW_CHAT_SOURCES` in
`app/tests/test_desktop_error_reporting.py`, and `app/tests/test_notifications.py`).

Generalise that pattern before the first split: add
`app/tests/_desktop_sources.py` with `feature_source("Issues")`, which returns
the concatenation of `MainWindowIssues*.cpp` plus its internal header in a
declared order. Then repoint the readers:
- Issues 27, Pulls 20, Actions 15, Settings 14, RepoDetail 12, Branches 6
- `desktop/tests/validate_*.py` (`validate_pull_refresh_responsiveness.py`,
  `validate_issue_*_avatar.py`)

**Other path-keyed data that must follow the moves**
- `desktop/resources/network/endpoint-catalog.json`: entries name
  `desktop/src/<File>.cpp: MainWindow::fn` (Issues 13, Pulls 9, RepoDetail 4,
  Settings 3, Actions 3). `test_crypto.cpp` (~3372) checks that the named file
  exists, and `test_admin_repo_purge.py` reads the catalog.
- `app/tests/test_no_unguarded_destructive_deletes.py` keys its allow-list by
  path. Branches has 4 entries (`branch -D`, `remove --force`,
  `reset --hard`), and the cap is 12 per file.
- `tools/code_health.py:component_for` labels by stem.
  `MainWindowIssuesBoard` becomes "UI · Issues Board". Either accept the new
  labels or map `MainWindow<Feature><Part>` to the feature, then update
  `desktop/tests/test_code_health.cpp:401`.

**Gates after each move**
- `ninja forkmesh` in a Debug agent build. Check `PIPESTATUS`, and set
  `CCACHE_DISABLE=1` once to prove the include resolution.
- Compile the window-test objects for the touched units:
  `ninja CMakeFiles/forkmesh-window-tests.dir/src/MainWindow<File>.cpp.o`.
- `pytest` on the repointed readers, then the destructive-delete and
  endpoint-catalog tests.
- `forkmesh-tests` (it runs the catalog check).

**Pieces per file** (cut at clusters; the ranges are approximate)

| File | Parts |
|---|---|
| Issues (13.5k) | First move the misplaced code. `eventFilter`, frameless resize and `handleFooterLogViewportEvent` (~8.3–9k) go to `MainWindowFrame.cpp`, and the Chat section builders plus `updateHomeStats` (12–13.5k) go to the chat files. Then split into `MainWindowIssues.cpp` (builders, load, list, 0–3.3k), `MainWindowIssuesBoard.cpp` (board, 3.3–3.8k), `MainWindowIssuesThread.cpp` (thread, compose, quick-add, AI, 4–8.3k) and `MainWindowIssuesSidebar.cpp` (sidebar editors, collaborators, bounty and inbox, 9–12k). Helper block 34–1197 → `MainWindowIssuesInternal.h`. |
| Pulls (13.0k) | `MainWindowPulls.cpp` (builders, load, conflict, 0–3k), `MainWindowPullsDiff.cpp` (diff, inline review, threads, 3–6k), `MainWindowPullsMerge.cpp` (checks, review, merge, new pull, 6–8k), `MainWindowPullsAi.cpp` (AI review and fix, links, bounties, inbox, 8–12k). Helper block 20–660 → internal header. |
| RepoDetail (15.0k) | `MainWindowRepoDetail.cpp` (binding, tabs, overview, lazy builders), `MainWindowRepoFiles.cpp` (file panel, tree, file ops, Cove explorer, 0–3.5k), `MainWindowRepoCommits.cpp` (5–6k, 10–11k), `MainWindowRepoSearch.cpp` (global search popup, search page, saved searches, history, 6–10k), `MainWindowRepoInsights.cpp` (11–13k). |
| Branches (12.5k) | `MainWindowWorktrees.cpp` (1–2k, 3–4.5k), `MainWindowBranches.cpp` (panel, operations, celebration, 4.5–7k), `MainWindowBranchDiff.cpp` (8–10k, sticky outline, viewed stamps), `MainWindowBranchMerge.cpp` (update and merge editor, conflict fixes, 10–11.5k). The test-hook block (2–3k) stays with the code it tests. Moves the 4 allow-list entries. |
| Settings (11.3k) | `MainWindowNetworkLog.cpp` (7–9.2k; the biggest coherent block, and not really Settings), `MainWindowSettings.cpp` (sections and tabs, 0–4.5k), `MainWindowSettingsSetup.cpp` (suggested apps, quick setup, avatar, rebuild, firewall, voice, 4.5–7k), `MainWindowCrashRepair.cpp` (error log, crash repair, notices, 9–11.3k). It has 67 includes, so trimming each part's includes is a free compile win. |
| Actions (9.8k) | `MainWindowActions.cpp` (init, scans, queue, run lifecycle, 0–4.5k), `MainWindowNotificationsPage.cpp` (4.5–6.5k), `MainWindowWorkflows.cpp` (glyphs, workflows, run controls, variables, 6.5k–end). |

Order: Issues (misplaced code first), Settings (network log first), Pulls,
Actions, RepoDetail, Branches. Each one is one branch landed the same day.

**Payoff**
- Compile cost is codegen, not parsing (`docs/desktop/build-and-test.md`).
  Splitting a 13k-line TU into 3–4 parts lets an incremental edit recompile
  ~3k lines instead of 13k, and lets a full rebuild schedule them in
  parallel. The parts do not get faster in total.
- Merge conflicts drop the most where many agents edit one feature, notably
  Issues and Pulls.
- This does not shrink the precompiled header fan-out, which is the dominant
  full-rebuild cost.

## 10. `world/browser_tests/tests/world.spec.js`

**Shape**
- 12,415 lines: 126 top-level `test(...)` calls and 36 top-level helpers.
- Setup lives in `prepareWorldPage` (103–1380), plus helpers at 1470–2051,
  6506–6566, 8374 and 11186.
- Config: `workers: 1`. `run-browser-test-batches.js` lists every spec through
  `playwright test --list`.

**Pieces**
- `tests/world-helpers.js` (CommonJS `module.exports`) holds the constants and
  helpers.
- About 8 spec files by area (boot, office, repos and pulls, chat, HUD and
  settings, environment, perf, mirror).

**Tests that break**
- `world/tests/test_world_source_layout.py:16` checks that the file exists.
- `world/tests/test_world_render_workload.py:13–18` reads its text. Make it
  read the spec glob.
- `world.spec.js-snapshots/` holds 9 PNGs that no assertion uses. Delete them
  in a separate commit after confirming that.

Gate: `playwright test --list` count equals 126+ before and after, and a full
`FORKMESH_BROWSER_FULL=1` run.

## 11. `app/src/entry.py`

**Shape**
- 54,077 lines, 1,212 functions and 28 classes. It still imports `js`,
  `pyodide.ffi` and `workers`. The CPython host runs it via
  `app/host/forkmesh_host/runtime.py` with `app/host/shims/js.py`.
- Routing lives in `app_runtime.py:_route`, so entry.py is a library of
  handlers and helpers.
- 73 modules already split out use `_bind_runtime(globals())`, which takes a
  one-time snapshot of entry's names. They are reached through
  `_LazyModule`/`.export`.
- Tooling: `app/tools/extract_lazy_module.py` and
  `app/tools/patch_entry_test_loaders.py`.

**Rounds** (largest coherent domains first; each round is one module and one
branch)

1. `https_mirrors_proxy`: `_https_mirror_*` blocks at 44.7–49.5k (~2.6k
   lines). The 2026-08-17 `https_mirrors` split is the template.
2. `org_runtime`: `org_*` at 28.1–29.2k and 40.1–40.8k (~1.7k).
3. `account_runtime`: `account_*` at 20.9–24k (~1.4k). Keep the names
   `app/host` reads (`_account_row`, `_save_account`) reachable as
   `entry.<name>` through `.export`.
4. `contributions`: 14.7–15.5k (~0.9k).
5. `rewards`: 24–27k.
6. `plugin_runtimes`: the `_WorldCommunityRuntime` family at 10.5–12k.
7. `security_scans`: `repository_security_scans_handler` and its neighbours.
8. `durable_objects`: `ForkMeshCronRunner`, `ForkMeshRoom`, `ForkMeshNodes`
   and the other classes from 51.6k to the end. They must stay importable by
   the name wrangler and the host bind. Do this last.

**What stays in entry.py**
- The lazy-proxy preamble.
- `SCHEMA_PRE_CREATE_ALTER_STATEMENTS`, `SCHEMA_ALTER_STATEMENTS` and
  `_SCHEMA_FINGERPRINT`. `test_local_migrations.py` evaluates them from
  entry.py by name.
- Every name `app/host` reads off `runtime.entry` (about 17, listed by
  `grep -rhoE 'entry\.[A-Za-z_]+' app/host | sort -u`). Either keep the def
  or re-export it.
- The 4 functions that use `global`.

**Method per round**
1. Fix `extract_lazy_module.py` first: drop the docstring from `HEADER`.
2. Run it with the def list from an AST scan of the line range.
3. Add the module to `LAZY_MODULES` in `world/tools/build_worker_footprint.py`.
4. Run `patch_entry_test_loaders.py`.
5. Do the four hand fixes recorded on 2026-08-17:
   - a second `read_text` in the same file;
   - slices whose end anchor moved;
   - **silently vacuous slices** that now span the concatenation (grep
     `index(` for the moved names);
   - tests that pin moved *content* without naming the moved symbol (found
     only by a full run).
6. Move the destructive-delete allow-list entries that name `entry.py` along
   with the code.

**Gates**
- Before and after each round, run the full `app/tests` and `world/tests` in
  8 shards and diff the `FAILED` lists against a detached main.
- `python3 tools/quality_gate.py run --profile core`.
- `python3 app/tools/build_worker_python.py && python3 world/tools/build_worker_footprint.py --staged`.
- The host unit tests (`app/host/tests_unit`).

**Traps**
- The `_bind_runtime` snapshot: a host patch of an entry global
  (`_log_d1_query`, `_D1_QUERY_CALLER`, `CRON_RUNNER_INTERVAL_MS`) reaches a
  moved function only if the module binds **after** the patch. Today the
  lazy modules bind on first request, which comes after
  `runtime.py:70`'s patches. Keep it that way, and never bind eagerly at
  import.
- Module-level memo dicts are shared by reference, which is fine. Rebinding
  them is not.

**Payoff**
- entry.py is the most-edited Python file, and every round removes a
  conflict domain.
- Startup import cost on core nodes drops slightly.
- There is no Cloudflare budget pressure left (retired 2026-09-30,
  `docs/app/worker-footprint.md`).

**Cost.** A 2026-08-17 round broke 6 tests across ~500 files, 4 of them
found only by a full run. Plan about a day per round.

## 12 and 16. `CrmFeature.cpp`, `MarketingFeature.cpp`

**How they build.** `marketplace/tools/generate_desktop_packages.py` reads
each package's `content/desktop/build.json` (`sources`, `mocHeaders`). Both
packages have `mocHeaders: []`, and neither uses `Q_OBJECT` or touches
`MainWindow`. Package units get **no precompiled header**.

**Precedent.** `MarketingPages.cpp` with `MarketingCommon.h/.cpp`.

**CRM** (11.6k lines, `namespace forkmesh::crm`)
- Anonymous namespace 91–1245 (`ListStageBar`, `DetailReads`,
  `ContactChoiceDelegate`, `ShareNameCompleter`) → `CrmCommon.h/.cpp`.
- Then `CrmFeature.cpp` (construction, registration at the end),
  `CrmLists.cpp`, `CrmDetail.cpp`, `CrmPipeline.cpp` and `CrmSharing.cpp`,
  cut by `CrmFeature::` prefix clusters.
- Add the sources to `build.json`.
- Update the 30 `endpoint-catalog.json` entries and
  `marketplace/packages/crm/tests/test_qt_plugin_contracts.py:18`.

**Marketing** (7.6k lines)
- Anonymous namespace 62–676 → `MarketingCommon`.
- Two more parts by cluster.
- 6 readers in `marketplace/packages/marketing/tests/test_marketing.py`, and 2
  catalog entries.

**Gates**
- The `-fsyntax-only` check per unit, using flags lifted from `build.ninja`.
- `marketctl validate`.
- `generate_desktop_packages.generate()`.
- Package pytest.
- Link `forkmesh`.

## 13. `desktop/tests/test_crypto.cpp`

**Shape.** 12,117 lines. `main` holds 88 blocks, grouped by comments:
- markdown, search, symbol trace
- identity, RoomCrypto, mirror crypto, signing formats
- NetworkBackoff
- endpoint catalog
- ProjectStore
- AgentStore
- backup, logging, MCP, merge queue

Target: `forkmesh-tests`, with explicit backend sources and no `MainWindow`.

**Pieces**
- `tests/crypto/CryptoTestSupport.h` holds `check()` and the stub network
  classes.
- `test_crypto_{signing,mirror,backoff,catalog,stores,agents,backup,mcp}.cpp`,
  each a `run<Area>()` called from `main`.

**Breaks**
- `test_crypto.cpp:9624` asserts `.endsWith("test_crypto.cpp")`.
- `app/tests/test_issue_self_assignment.py:39` reads the pinned signing
  vectors from this file. Repoint it to `test_crypto_signing.cpp`.

## 14. `world/public/world/world.js`

**Shape**
- 33,213 lines.
- Lines 1–6338 are ~170 top-level helpers.
- `class ForkMeshWorld extends HTMLElement` (6340–33209) has ~598 methods and
  16 class fields. It has no `#private` members, no `super.` calls and no
  getters or setters, so prototype mixins are safe.

**Loading.** `index.html` loads `/world/world.js` as a module.
`app/tools/build_site_assets.py:stage_world()` renames source `world.js` to
`world-runtime.js`, minifies and versions sibling modules, and
modulepreloads static imports. New sibling modules need no pipeline change.

**Pieces**
- First, move the top-level helpers into `world-helpers-*.js` ES modules with
  named exports.
- Then split the methods into `world-ui-bind.js` (`bindUI`, 12139–13476),
  `world-hud.js`, `world-landmarks.js`, `world-notifications.js`,
  `world-repos-pulls.js`, `world-chat.js`, `world-sound-settings.js`,
  `world-perf.js` and `world-mirror.js`.
- Each one is written as `class ChatMethods { …methods byte-identical… }`.
  `world.js` copies the methods with
  `for (const m of [ChatMethods, …]) Object.defineProperties(ForkMeshWorld.prototype, Object.getOwnPropertyDescriptors(m.prototype))`
  (skipping `constructor`) before `customElements.define`.

**Breaks.** About 106 Python test files read `world.js` as text, many pinning
indentation. Add `world/tests/_world_source.py` that concatenates `world.js`
and the method modules in a declared order. Repoint every reader, using the
same `grep` sweep as for the dashboard.

**Gates**
- `world/tests` (diff of `FAILED` lists).
- `build_worker_footprint.py` (`initialWorldModuleBytesSoft` 2,650,000 counts
  static imports).
- The `world.spec.js` suite.
- A headless render of the real page.

**Payoff**
- Fewer conflicts.
- Later, the mixins for rarely used panels can load on demand, which cuts
  first-load bytes. That is a follow-up, not part of the move.

## 15. `world/public/world/world.css`

21,357 lines with few section comments. Regions by selector prefix:
- 0–3k: mastodon, diagnostics, gym
- 3–6k: office, pull, account, repo
- 6–9k: element, shot, profile
- 9–12k: task, activity, error, discord
- 12k–end: the `fm-world-*` HUD

Split it contiguously into `world-panels.css`, `world-office-repo.css`,
`world-profile.css`, `world-activity.css` and `world-hud.css`. Link them in
the same order in `index.html` (23–32). Do not use `@import`. Because the
order is preserved, the cascade is unchanged.

**Before splitting**
- Confirm that `stage_world()` versions extra stylesheets.
- About 50 tests read `world.css`. Use the same concatenating helper.

## 17. `desktop/src/MainWindowInternal.h`

The bodies already moved to `MainWindowInternal1-4.cpp` (2026-09-22). What
remains is 10.6k lines of class declarations in the precompiled header.

Next step: move feature-only classes out of the precompiled header into
per-feature internal headers included only by their users:
- diff rendering structs (308–577)
- the code editor (7537–7553)
- certificate and merge structs (9846–10354)
- charts used by one page

Run `tools/mainwindow_split/run_refs.py` (clang-19) to find classes used by
one or two units. A move that takes a class out of the precompiled header
makes its users add an include. Prove it with `CCACHE_DISABLE=1`.

Payoff: fewer edits to a precompiled-header input, which is the full-rebuild
trigger (29 of 78 commits in 24 h on 2026-09-22).

## 18. `tools/mirror_gateway.py` (deferred)

11,056 lines, standard library only. The big classes are `GatewayApplication`
(2.4k), `GitRepository` (2.1k), `DashboardService` (1.2k) and
`ArchiveMaterializer` (0.8k).

It ships as **one file** named in:
- `deploy/core/self_deploy.py` (`ROLL_FILES`, `DAEMON_TOOLS`)
- `tools/next_autodeploy.py:57`
- `tools/next_fleet.py:112`
- `www/public/install.sh` (2562, 2579, 2625)
- `desktop_go_new/internal/hosts/install.go:173`
- `desktop/CMakeLists.txt:1456`
- the systemd units

Tests monkeypatch module globals (`gateway._run_git` ×8 and others) that
moved classes would no longer see.

Do it only after the file lists become one manifest, e.g.
`tools/mirror_gateway.files` read by all seven places. Until then the risk to
running mirrors outweighs the payoff.

## 19–20. `schema.py` and `MainWindow.h`: leave as is

- `schema.py` is one ordered list of `CREATE` statements. The fingerprint test
  hashes it with the two entry.py lists. Splitting buys nothing.
- `MainWindow.h`: a libclang study on 2026-09-22 (`tools/mainwindow_split/`)
  found 3,845 of 5,908 members movable, but only 19 of 67 recent commits
  touched single-feature members only. The split was rejected. Reduce its
  edit rate instead:
  - new feature state goes into per-feature objects (`m_ledgerFeature`,
    `SecurityAlarmState` style);
  - new shell access goes through `NextAccess`;
  - item 17 above.

## Related

- [destructive-actions.md](destructive-actions.md): allow-list entries move with the code.
- [../desktop/build-and-test.md](../desktop/build-and-test.md): why rebuilds are slow.
- [../desktop/README.md](../desktop/README.md): precompiled header, namespace and moc rules.
- [../app/worker-footprint.md](../app/worker-footprint.md): `_LazyModule`, `LAZY_MODULES`, extraction tools.
