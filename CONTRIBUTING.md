# Contributing to ForkMesh

Thanks for helping build ForkMesh — a peer-to-peer developer platform where
every developer runs a node and the mesh keeps serving code even when the
source machine goes offline. This is early, and that's the point: the people who
show up now help shape the protocol. Human and AI contributors are both
first-class here.

This guide covers the complete change-to-review workflow. If you have not run a
node yet, begin with [Getting started](www/docs/getting-started.md). For desktop
dependencies, navigation, logs, and troubleshooting, use the
[Qt client guide](www/docs/qt-client.md).

## The served stack

forkmesh.com, app.forkmesh.com and world.forkmesh.com run the self-hosted
stack described in [docs/PLAN.md](docs/PLAN.md): the App runs on CPython on
the core nodes (`app/host/`), and `deploy/` and `tools/next_*.py` manage the
fleet. Source files hold code only; explanations
go in `docs/` (`AGENTS.md`, "Source is code only"). Start at
[docs/README.md](docs/README.md).

---

## Ways to contribute

- **Run a node** and mirror a repository you care about — every mirror makes the
  mesh more durable.
- **Fix a bug or ship a feature** in any of the components below.
- **Publish a Market package** under your own creator identity by following the
  [member publishing guide](marketplace/README.md#publish-a-package).
- **Improve docs** — the README, this guide, and the website docs.
- **Report or triage issues.** ForkMesh dogfoods itself: issues and pull
  requests live in-repo under [`.forkmesh/`](.forkmesh/) as signed,
  append-only data, and also appear on the website.

Not sure where to start? Open an issue describing what you'd like to work on,
or pick up a funded issue and hand it to an agent.

---

## Repository layout

```text
forkmesh/
  desktop/           Qt 6 desktop node
  app/               Product UI plus Python API, Git, and realtime room code;
                     app/host/ runs it on CPython on the core nodes
  www/               Marketing, docs, and blog site (forkmesh.com)
  world/             World code and media, served by the core nodes
  server/            Go daemon: mirror, core and runner roles
  deploy/            Fleet file, node renderer, certsync, IDS, systemd units
  native/            Native iOS (Swift) and Android (Kotlin) apps
  marketplace/       Package catalog and packages (CRM, Vault, Analytics, ...)
  conformance/       Versioned contracts, vectors and shared fixtures
  tools/             Release, mirror-host, ledger and agent helpers;
                     tools/next_*.py drive the core fleet
  docs/              Developer documentation, one home page per subsystem
  .forkmesh/         In-repo signed data: issues (the live roadmap),
                     discussions, commit comments, release metadata, workflows
```

Work in the component your change touches. Most changes are self-contained to
one directory. The developer docs for each directory start at
[`docs/README.md`](docs/README.md).

---

## Prepare a working copy

ForkMesh's native workflow is fork → branch → commit → signed pull request:

1. Run the desktop client and sign in or choose your node identity.
2. Find `forkmesh/forkmesh` under **Repos**, choose **Fork**, and select the
   parent folder for your writable checkout. If you already have write access,
   you may clone the repository normally instead.
3. Configure a Git author before your first commit:

   ```sh
   git config user.name "Your Name"
   git config user.email "you@example.com"
   ```

4. Start from an up-to-date `main` and create a focused branch:

   ```sh
   git switch main
   git pull --ff-only
   git switch -c docs/clear-getting-started
   ```

Use a short branch prefix that describes the work, such as `fix/`, `feat/`,
`docs/`, or `test/`. Never develop directly on `main`, and do not mix unrelated
cleanup into the same branch.

You can also create the branch in **Code → Branches → New branch**. Git and the
Qt UI operate on the same working copy, so switching between them is safe.

---

## Building and testing

Please build and test the component you changed before opening a pull request.
For changes to repository transport or Git-facing behavior, run the
[versioned stock-Git conformance suite](www/docs/git-compatibility.md#run-the-suite).

### Desktop node (`desktop/`)

Requirements: Qt 6.4+ (Widgets, Network), CMake 3.16+, OpenSSL dev headers, Git,
a C++17 compiler, and the `openssl` CLI at runtime for the LAN TLS certificate.

```sh
cd desktop
./run.sh          # incremental Release build and launch
./run.sh test     # sub-minute critical trust/mirror/agent contracts
```

Additional test targets live in `desktop/tests/` and are built via CMake. See
the [Qt client guide](www/docs/qt-client.md#build-and-run-from-source) for dependency
commands, manual CMake usage, and focused-test troubleshooting.

Qt 6.4 is a floor, not an aspiration: Ubuntu 24.04 LTS and Debian 12 both ship
Qt 6.4.2, so an API added in 6.5 configures and compiles on a newer development
box and breaks the build on theirs. Guard those behind
`#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)`, and when a change reaches for an
unfamiliar Qt API, confirm it with `tools/qt64-build-check.sh` — that builds
`desktop/` against each distribution's own Qt 6.4.2 and compiler, and needs
neither root nor Docker.

### App, www and World (`app/`, `www/`, `world/`)

App hosts the product UI, API, encrypted WebSockets, Git protocols, and
realtime rooms. It runs on CPython under `app/host/forkmesh_host`; www is a
static-asset site; World's static files are served by the core nodes. See [docs/app/README.md](docs/app/README.md).

```sh
python3 -m pytest app/tests/host             # the CPython host
python3 -m pytest app/host/tests_unit        # host unit tests
python3 -m pytest deploy/tests               # renderer, certsync, IDS, fleet tools
python3 -m pytest app/tests/test_status_page.py   # one App module
```

Backend tests live in `app/tests/`. The bounded default contracts run from
the repository root with `python3 tools/run_critical_tests.py worker` and
`python3 tools/run_critical_tests.py world`; run additional focused modules for
the behavior you change. If you edit a JavaScript dashboard fragment, rebuild
the dashboard bundle so the packaged output stays in sync.

The browser World smoke suite is also bounded to 55 seconds:

```sh
npm --prefix app/browser_tests test
```

`npm --prefix app/browser_tests run test:all` remains available
for focused browser-test maintenance outside the default gate.

### Native apps (`native/`)

```sh
cd native/ios/ForkMeshCore && swift test          # Linux or macOS, swift.org 6.3
cd native/android && ./gradlew :core:coreTest     # JDK 17+
```

See [`native/ios/README.md`](native/ios/README.md) and
[`native/android/README.md`](native/android/README.md) for the app builds;
[`docs/native/README.md`](docs/native/README.md) is the developer home page.

### Mirror server (`server/`)

```sh
cd server
go test ./...
```

### IDE Marketplace plugin

```sh
cd marketplace/packages/forkmesh-ide/content/vscode
npm ci
npm run compile   # tsc -p ./
```

### Tools (`tools/`)

```sh
python3 tools/test_forkmesh_mcp_server.py     # MCP server end-to-end
python3 tools/test_branch_pr_review_guard.py  # PR review guard
```

---

## Coding conventions

- **Source is code only.** Do not add comments or docstrings. Put the reason
  for a change, and anything a reader needs to know, in the matching page
  under [`docs/`](docs/README.md). The exceptions are lines tools read:
  shebangs, encoding lines, build tags and `//go:embed`, lint and type
  directives, license headers, build markers in templates, and user-facing
  help or UI text ([`AGENTS.md`](AGENTS.md)).
- **Match the surrounding code.** Follow the existing naming, formatting and
  idioms of the file you're editing rather than introducing a new style.
- **Keep changes focused.** Make the smallest change that solves the problem;
  avoid unrelated refactors in the same pull request.
- **Respect signing and identity.** Actions that write protocol data (issues,
  pull requests, repository/profile metadata) are signed with a node's Ed25519
  key. Don't add side doors that bypass signing — write the same native
  `.forkmesh/` entries the desktop node produces.

---

## Opening an issue

**Do not report security vulnerabilities in public issues** — ForkMesh issues
are signed and published in-repo, so a public report discloses the problem
before a fix exists. Follow the private process in [SECURITY.md](SECURITY.md)
instead.

For everything else, open an issue that includes:

- what you observed and what you expected,
- the affected component (desktop node, relay, mobile, Marketplace plugin, tools),
- steps to reproduce, and
- the version or commit you tested against.

You can file issues from the desktop node, the website, or the MCP server
(`create_issue`) — they all produce the same signed in-repo record.

---

## Opening a pull request

### 1. Review and commit the branch

Check that only the intended files changed, then commit them:

```sh
git status --short
git diff
git add path/to/file
git diff --cached
git commit -m "docs: add a practical Qt client guide"
```

Prefer explicit paths over `git add -A` when the working tree contains unrelated
work. Do not commit build directories, credentials, tokens, local settings,
diagnostic logs, or private repository data.

If `main` advanced, merge or rebase it into the branch and resolve conflicts
before review:

```sh
git fetch origin
git rebase origin/main
```

Do not rewrite another contributor's published branch without coordinating
with them. Signed `.forkmesh/` event records are append-only protocol data; do
not hand-edit or discard them merely to make a conflict disappear.

### 2. Run the relevant checks

Run the smallest focused tests while iterating, then the component suite before
opening the PR. From the repository root, validate the quality-gate definition:

```sh
python3 tools/quality_gate.py validate
```

Before a code PR, run the core profile:

```sh
python3 tools/quality_gate.py run --profile core
```

For a documentation-only change where runtime suites add no useful evidence,
configuration validation plus local link checks may be sufficient, but explain
that scope in the PR rather than claiming the runtime gates passed.

The release profile includes the real-browser World suite and its pinned
Playwright/Chromium requirements:

```sh
python3 tools/quality_gate.py run --profile release
```

Record every command and result. If a required toolchain is unavailable, say so
plainly in the PR; do not report a skipped suite as passing. The detailed policy
is in the [definition of done](www/docs/engineering/definition-of-done.md).

### 3. Open the native ForkMesh PR

In the Qt client:

1. Open your writable fork.
2. Open **Pulls → New pull request**.
3. Set **Target node** to `forkmesh`, **Base** to `main`, and **Head** to your
   feature branch.
4. Enter a concise, outcome-focused title and a useful description, then
   submit. ForkMesh signs the patch and commit series and queues them in the
   upstream owner's inbox, even while that node is offline.

Base and head must differ, and the branch must contain a committed diff. If the
repository says it is read-only, fork it first. The
[Qt client PR guide](www/docs/qt-client.md#create-a-pull-request) also documents
owner-local PRs, direct one-file mirror proposals, and common failure cases.

MCP-capable agents can use the repository's configured
`open_pr_from_branch` tool. It creates the same signed native PR record; it is
not a privileged review bypass.

Once the PR exists, the owner's node runs the repository's `on: pull_request`
workflows against its head and reports them under **Checks**. If you have not
had work merged here before, those runs wait as *Awaiting approval* until a
maintainer releases them — a check run executes your commands on someone else's
machine, so each of your pull requests is approved individually until one of
them lands. Re-run the same suites locally in the meantime; that evidence is
what the reviewer reads first.

### 4. Complete the PR description

Use [the repository template](.github/PULL_REQUEST_TEMPLATE.md). The **Change**
section should explain the user-visible outcome and how to verify it. Complete
all five evidence sections:

- Threat review.
- Abuse and privacy review.
- Benchmark and budget evidence.
- Regression-test evidence.
- Operational evidence.

For a section that truly does not apply, write a concrete reason, for example
“Not applicable — Markdown-only change; no runtime path or collected data
changed.” Placeholder-only or unexplained `N/A` sections fail CI.

Before submitting, make sure the PR answers three reviewer questions without
requiring them to reconstruct your work: what changed, why this approach is
safe, and exactly how it was tested.

Keep pull requests scoped to a single concern so they're easy to review.

### 5. Get an independent approval

Every pull request needs at least one approval from a peer who is not the pull
request author. An author's self-approval does not count. The latest decisive
review from each signer wins, and any unresolved peer request for changes keeps
the merge locked.

More distinct approvals create a stronger review signal and make a
discretionary merge more likely, but they never bypass tests, security checks,
scope review, or maintainer judgment. See the
[team onboarding guide](https://forkmesh.com/docs/onboarding#reviews) for the
website review flow.

### 6. Respond to review

Add follow-up commits to the same branch, rerun affected checks, and summarize
what changed. Resolve a review thread only after the concern is addressed or
the reviewer agrees with the documented alternative. The maintainer performs
the final merge; opening a PR does not grant merge authority.

### AI agents

ForkMesh treats agents as first-class contributors. Agent-authored commits and
pull requests should be attributable — signed with the agent's key — and the
review still runs through the same human gate before merge. Whether you're a
person or an agent, the checklist above is the same.

Agents working in this repository must also follow [`AGENTS.md`](AGENTS.md):
never erase ForkMesh user data (`~/projects/forkmesh/{data,config,ssh,state,logs}`
and the legacy paths), sandbox `HOME` and `XDG_*_HOME` in tests that run the
installers, and keep explanations in `docs/` rather than in source comments.

---

## Code of conduct

Be respectful and constructive. We're building a network people want to be part
of; assume good faith, keep discussions technical, and help newcomers get their
first node on the mesh.

---

Thanks for contributing. Build a node, mirror a project you love, open an issue,
and hand it to an agent — the mesh is small enough that you'll matter, and far
enough along that you'll ship something real today.
