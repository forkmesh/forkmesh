# Contributing to ForkMesh

ForkMesh is a developer platform where nodes mirror repositories so code stays
reachable when its source host goes offline. Human and AI contributors follow
the same workflow. New to ForkMesh? Start with
[Getting started](www/docs/getting-started.md); desktop dependencies, logs and
troubleshooting are in the [Qt client guide](www/docs/qt-client.md). The
developer documentation for each subsystem starts at
[docs/README.md](docs/README.md); the served stack is described in
[docs/PLAN.md](docs/PLAN.md).

## Ways to contribute

- Run a node and mirror a repository.
- Fix a bug or ship a feature in one of the components below.
- Publish a Market package under your own creator identity
  ([member publishing guide](marketplace/README.md#publish-a-package)).
- Improve the documentation.
- Report or triage issues. Issues and pull requests are signed, append-only
  records under [`.forkmesh/`](.forkmesh/) and also appear on the website.

## Repository layout

```text
desktop/      Qt 6 desktop node
app/          Product UI, Python API, Git and realtime room code; app/host/ runs it on CPython
www/          Marketing, docs and blog site
world/        World code and media
server/       Go daemon: mirror, core and runner roles
deploy/       Fleet file, node renderer, certsync, IDS, systemd units
native/       Native iOS (Swift) and Android (Kotlin) apps
marketplace/  Package catalog and packages
conformance/  Versioned contracts, vectors and shared fixtures
tools/        Release, fleet, ledger and agent helpers
docs/         Developer documentation, one home page per subsystem
.forkmesh/    Signed in-repo data: issues, discussions, releases, workflows
```

## Prepare a working copy

The workflow is fork, branch, commit, signed pull request:

1. Run the desktop client and sign in or choose your node identity.
2. Find `forkmesh/forkmesh` under **Repos** and choose **Fork** (or clone it
   directly if you have write access).
3. Configure a Git author (`git config user.name`, `git config user.email`).
4. Branch from an up-to-date `main`:

   ```sh
   git switch main
   git pull --ff-only
   git switch -c fix/short-description
   ```

Use a prefix such as `fix/`, `feat/`, `docs/` or `test/`. Never develop on
`main`, and keep unrelated cleanup out of the branch. Branches can also be
created in **Code → Branches → New branch**; Git and the desktop UI share the
same working copy.

## Building and testing

Build and test the component you changed. Changes to repository transport or
Git-facing behavior also need the
[stock-Git conformance suite](www/docs/git-compatibility.md#run-the-suite).

| Component | Requirements | Command |
| --- | --- | --- |
| Desktop (`desktop/`) | Qt 6.4+, CMake 3.16+, OpenSSL headers, Git, C++17, `openssl` CLI | `cd desktop && ./run.sh` (build and launch), `./run.sh test` (critical contracts) |
| App, host, fleet | Python 3 | `python3 tools/run_critical_tests.py worker`, `python3 -m pytest app/tests/host app/host/tests_unit deploy/tests` |
| World | Python 3 | `python3 tools/run_critical_tests.py world` |
| Browser smoke tests | Node.js | `npm --prefix app/browser_tests test` (`run test:all` for the full set) |
| Native iOS core | Swift 6.3 (Linux or macOS) | `cd native/ios/ForkMeshCore && swift test` |
| Native Android core | JDK 17+ | `cd native/android && ./gradlew :core:coreTest` |
| Mirror server (`server/`) | Go | `cd server && go test ./...` |
| IDE plugin | Node.js | `cd marketplace/packages/forkmesh-ide/content/vscode && npm ci && npm run compile` |
| Tools | Python 3 | `python3 tools/test_forkmesh_mcp_server.py` |

Notes:

- Qt 6.4 is the floor (the version shipped by current LTS distributions).
  Guard newer APIs with `#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)` and check
  unfamiliar APIs with `tools/qt64-build-check.sh`, which builds `desktop/`
  against each distribution's Qt 6.4 without root or Docker.
- Additional desktop test targets live in `desktop/tests/` and build with CMake.
- After editing a dashboard JavaScript fragment, rebuild the dashboard bundle.
- App runs on CPython under `app/host/forkmesh_host`; see
  [docs/app/README.md](docs/app/README.md).
- Native app builds: [`native/ios/README.md`](native/ios/README.md),
  [`native/android/README.md`](native/android/README.md),
  [`docs/native/README.md`](docs/native/README.md).

## Coding conventions

- **Source is code only.** No comments or docstrings; explanations go in the
  matching page under [`docs/`](docs/README.md). Exceptions are lines tools
  read: shebangs, encoding lines, build tags and `//go:embed`, lint and type
  directives, license headers, template build markers, and user-facing help or
  UI text ([`AGENTS.md`](AGENTS.md)).
- **Match the surrounding code** in naming, formatting and idioms.
- **Keep changes focused**: the smallest change that solves the problem.
- **Respect signing and identity.** Protocol writes (issues, pull requests,
  repository and profile metadata) are signed with a node's Ed25519 key. Write
  the same `.forkmesh/` entries the desktop node produces; never bypass signing.

## Opening an issue

Do not report security vulnerabilities in public issues; follow
[SECURITY.md](SECURITY.md). Other issues should state what you observed and
expected, the affected component, reproduction steps, and the version or commit
tested. Issues can be filed from the desktop node, the website, or the MCP
server (`create_issue`); all produce the same signed record.

## Opening a pull request

### 1. Commit the branch

Review `git status --short` and `git diff`, add explicit paths, and commit.
Never commit build directories, credentials, tokens, local settings, logs or
private repository data. If `main` advanced, rebase or merge it in and resolve
conflicts. Do not rewrite another contributor's published branch, and never
hand-edit signed `.forkmesh/` event records to make a conflict disappear.

### 2. Run the checks

```sh
python3 tools/quality_gate.py validate
python3 tools/quality_gate.py run --profile core      # before a code PR
python3 tools/quality_gate.py run --profile release   # adds the real-browser World suite
```

A documentation-only change may rely on configuration validation and link
checks; say so in the PR. Record every command and result, and never report a
skipped suite as passing. The policy is the
[definition of done](www/docs/engineering/definition-of-done.md).

### 3. Open the pull request

In the desktop client, open your fork, choose **Pulls → New pull request**, set
**Target node** to `forkmesh`, **Base** to `main` and **Head** to your branch,
then submit. The signed patch series is queued in the owner's inbox even while
that node is offline. Base and head must differ and the branch must contain a
committed diff. See the
[Qt client PR guide](www/docs/qt-client.md#create-a-pull-request) for other
cases. MCP-capable agents can use `open_pr_from_branch`, which creates the same
signed record and grants no review bypass.

The owner's node runs the repository's `on: pull_request` workflows and reports
them under **Checks**. For a first-time contributor these runs wait as
*Awaiting approval* until a maintainer releases them, because a check executes
contributed commands on another machine.

### 4. Complete the description

Use [the repository template](.github/PULL_REQUEST_TEMPLATE.md). Describe the
user-visible outcome and how to verify it, and complete all five evidence
sections: threat review; abuse and privacy review; benchmark and budget
evidence; regression-test evidence; operational evidence. A section that does
not apply needs a concrete reason; placeholder or unexplained `N/A` sections
fail CI.

### 5. Review and merge

Every pull request needs at least one approval from someone other than the
author. The latest decisive review from each signer counts, and an unresolved
request for changes blocks the merge. Approvals never bypass tests, security
checks or maintainer judgment
([onboarding guide](https://forkmesh.com/docs/onboarding#reviews)). Address
feedback with follow-up commits on the same branch; the maintainer performs the
final merge.

### AI agents

Agent-authored commits and pull requests must be attributable and go through
the same review. Agents must also follow [`AGENTS.md`](AGENTS.md): never erase
ForkMesh user data, sandbox `HOME` and `XDG_*_HOME` in tests that run the
installers, and keep explanations in `docs/`.

## Code of conduct

Be respectful and constructive, assume good faith, keep discussions technical,
and help newcomers.
