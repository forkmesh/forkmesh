# Network-log badge audit — which categories still earn their slot

> Snapshot of 2026-08-06. File references below are to the code as it was
> then and some have moved since (`RepoHost.cpp` no longer exists; helper
> bodies left `MainWindowInternal.h` for `MainWindowInternal1-4.cpp`). Today the
> classifier is `kNetworkLogRules` / `networkLogStyleFor()` in
> `desktop/src/MainWindowSettings.cpp`, the single sink is
> `MainWindow::logSystem()` in the same file, and the live log is
> `~/projects/forkmesh/data/network_log.txt` in the default layout. See
> [docs/desktop/diagnostics.md](../../../docs/desktop/diagnostics.md).

Audit of the 30 log categories in the desktop client's Log view: the coloured
badge on every entry, the quick-filter chip row, and the icon legend in the
footer/header strip. The question asked was "which of these do we no longer
use?"

Three lists have to agree for a category to work, and they do — no category is
missing from one of them:

| List | Where |
| --- | --- |
| classification rules (substring → badge) | `desktop/src/MainWindowSettings.cpp` (`kNetworkLogRules`) |
| quick-filter chip order | `desktop/src/MainWindowSettings.cpp` (`order[]` at the time of the audit; no longer present under that name) |
| legend icons + accents | `desktop/src/MainWindowInternal.h` (`categories()`) |

Every log line reaches the classifier through one sink, `MainWindow::logSystem()`
(`MainWindowSettings.cpp`), so "is this category still produced?" is
answerable by looking at what feeds that function.

## Method

Two independent passes, because either one alone is misleading:

1. **Static** — extract the message text of all 690 distinct string-literal log
   messages passed to `logSystem()` / `flashMessage()` across `desktop/src`,
   and run each through a faithful re-implementation of `networkLogStyleFor()`
   (stall check → error precedence → `[body: …]` stripping → first-matching
   rule). This answers "could this badge ever be produced?"
2. **Empirical** — classify every line of a real retained log,
   `~/.local/share/ForkMesh/ForkMesh/network_log.txt`: 27,165 entries spanning
   2026-08-05 22:52 → 2026-08-06 12:45 on the deploy box. This answers "does
   this badge actually appear?"

The empirical window is only ~14 hours of one mostly-headless machine, so a zero
there is weak evidence on its own. The static pass is what settles whether code
still emits a category at all.

## Verdict

### 1. `HOST` is dead — no producer anywhere in the tree

The rule is `{"host: ", "#76e3ea", "HOST"}`. It was added in `1f0a992a3` to
catch `RepoHost`'s own log lines, which at that commit were:

```
RepoHost.cpp:547   emit log("Host: connection to relay went stale for …")
RepoHost.cpp:700   emit log("Host: serving " + m_owner + "/" + m_name + " live to the web.")
RepoHost.cpp:972   QStringLiteral("Host: served %1 for %2/%3.")
```

`RepoHost` and its desktop server/publisher pipeline have since been deleted.
The managed Go service owns serving, catalog publication, and native
collaboration intake. No string reaching `logSystem()` in the entire
codebase contains `"host: "`, and the badge scored 0 across 27,165 real
entries.

`HOST` now costs a permanently-dark legend cell and a chip that can never
appear. It is also the 30th category, which is exactly what fills the legend's
`10 × 3` compact grid (`kCompactColumns`/`kCompactRows`,
`MainWindowInternal.h:7698`) — dropping it leaves a hole to lay out, so removal
should adjust that grid rather than just delete the row.

Note that "hosting" *events* are not gone, only this badge: host-online /
host-offline lines exist, they just don't use a `Host: ` prefix, so they land in
other categories.

### 2. Two `BOUNTY` rules are redundant — they can never change a badge

| Rule | Status |
| --- | --- |
| `{"escrow", …, "BOUNTY"}` (index 11) | never wins |
| `{"funded", …, "BOUNTY"}` (index 13) | never wins |

Every message containing `escrow` or `funded` also contains an earlier needle:

- `"Bounty escrow for issue #%1 ready to fund …"` → `bounty` (index 10) wins
- `"Bounty for %1 funded and split to the author + treasury …"` → `bounty` wins
- `"Reward escrow for pull #%1 ready to fund …"` → `pull #` (index 8) wins → `PULL`
- `"Pull #%1 merged, but no inbuilt bounty wallet is funded yet …"` → `pull #` wins → `PULL`

Both scored 0 hits statically and 0 hits over the real log. They are harmless,
but they are dead weight in a list whose ordering is already load-bearing.

### 3. `FORK` is 31% of the log and almost none of it means "a fork happened"

This is the biggest problem the audit turned up, and it is a *misclassification*
problem, not a dead-category one.

- `FORK` badged 8,424 of 27,165 entries (31.0%).
- In **8,422 of those 8,424**, the substring `fork` appears *only* inside the
  word `forkmesh` — a URL host, an account name, or the `forkmesh/forkmesh`
  repo name. Two entries were about an actual fork.

Because `{"fork", …, "FORK"}` sits at index 26, it pre-empts all eleven rules
below it — `PROMPT`, `ISSUE`, `ADMIN`, `IDENTITY`, `CRYPTO`, `PEER`, `NETWORK`,
`STATUS`, `NODE`, `CLIP`, `SAVE` — for any message that so much as mentions
ForkMesh. The rule already carries a comment acknowledging it is a catch-all
that other rules must be hoisted above; in practice nearly every line qualifies.

The measurable damage:

- **`NODE` is effectively unreachable.** Its needles `connected` and `peer`
  scored **0** wins in 27,165 entries; `node` won 9 times, and the samples are
  incidental build-output text (`sh-control-tests.dir/src/ControlNode.cpp.o`),
  not node events. 7,288 entries did contain `connected` / `peer` / `node` —
  5,644 went to `FORK`, the rest to `ERROR`/`STATUS`/`ACCOUNT`/`NETWORK`/`MIRROR`.
- **`ISSUE` never won.** 70 entries contained `issue`; all 70 went elsewhere
  (`BGTASK` 45, `ERROR` 17, `FORK` 4, `GIT` 3, `MERGE` 1).

A narrower needle (`fork` as a whole word, or `forked`/`forking`/`fork location`)
would let the eleven shadowed rules fire and would stop a third of the log
reading as a fork.

### 4. `ADMIN` never wins in practice, for a different reason

177 entries contained `admin`; **155** were `GET …/api/accounts/admin-pending`
polls that match `account` (index 16) first, and 20 more were errors. `ADMIN`
has four live producers (`MainWindowSetup.cpp:2088`, `:2352`,
`MainWindowChat.cpp:15148`, `MainWindowMessages.cpp:601`) — the badge works, it
is just drowned by an endpoint name.

### 5. Live but simply not exercised in this window

Every one of these has a confirmed producer feeding `logSystem()`; each scored 0
in the 14-hour deploy-box log only because that box did no such work. **Not**
candidates for removal:

| Badge | Producer |
| --- | --- |
| `PEER` | `MainWindowMessages.cpp:841` — `"Node connected: %1 is online"` |
| `FORKED` | `MainWindowRepoDetail.cpp:1205` — `"Forked into %1/%2."` |
| `PULL` | 33 producers, e.g. `MainWindowBranches.cpp:5266` `"Opened pull #%1 …"` |
| `BOUNTY` | `MainWindowPulls.cpp:7242`, `:7457`, `:7461` |
| `WALLET` | `MainWindowChat.cpp:22341`, `MainWindowSettings.cpp:3332`, + validation paths |
| `CLIP` | 10 producers, e.g. `MainWindowActions.cpp:4762` `"Copied %1 to clipboard."` |
| `ISSUE` | 13 producers (see §3 for why it still loses) |

The log contains no `Issue #`, `Copied`, `Forked into` or `Node connected` line
at all, which is consistent: this machine self-merges agent branches rather than
opening pull requests, and has no interactive clipboard use.

## Observed distribution (27,165 entries)

| Badge | Count | Share |
| --- | --- | --- |
| BGTASK | 9,285 | 34.2% |
| FORK | 8,424 | 31.0% |
| ACCOUNT | 3,161 | 11.6% |
| ERROR | 2,039 | 7.5% |
| INFO | 1,033 | 3.8% |
| PUBLISH | 739 | 2.7% |
| MIRROR | 716 | 2.6% |
| STATUS | 624 | 2.3% |
| GIT | 540 | 2.0% |
| ACTIONS | 309 | 1.1% |
| SYNC | 83 | 0.3% |
| STALL | 52 | 0.2% |
| PIN | 46 | 0.2% |
| NETWORK | 38 | 0.1% |
| SAVE | 24 | 0.1% |
| MERGE | 15 | 0.1% |
| SESSION | 11 | <0.1% |
| PROMPT | 11 | <0.1% |
| NODE | 9 | <0.1% |
| CRYPTO | 3 | <0.1% |
| IDENTITY | 3 | <0.1% |
| PEER, FORKED, HOST, PULL, ISSUE, BOUNTY, WALLET, ADMIN, CLIP | 0 | — |

Two-thirds of the log is `BGTASK` + `FORK`, and `FORK` is noise. `ACCOUNT`'s
11.6% is likewise almost entirely `/api/accounts/*` polling.

## Recommended follow-ups

Nothing here is applied — this is the audit only.

1. Remove the `HOST` rule, its `order[]` entry and its legend row, and re-lay
   the legend's compact grid for 29 categories.
2. Delete the redundant `escrow` and `funded` rules.
3. Narrow the `fork` needle so it stops matching `forkmesh`. This is the change
   that recovers `NODE`, `ISSUE` and nine other categories.
4. Consider whether `ADMIN` should be checked above `account`, so
   admin-ownership lines aren't lost to the polling endpoint's name.

## Note for a future audit: the stored line gained a tail

Since adhoc #1587 every entry ends with the file and line that logged it —
`2026-08-06 12:45:01  Mirror sync finished  [desktop/src/MainWindowRepos.cpp:812]`
— in `network_log.txt` as well as on screen. Anything re-implementing
`networkLogStyleFor()` over a captured log must strip that tail first
(`forkmesh::logMessageBody()`, `desktop/src/LogSource.h`): the path is not
part of what the caller wrote, and a line logged from `MainWindowIssues.cpp`
would otherwise badge as `ISSUE`. The app itself strips it in
`MainWindow::logBadgeFor()` / `logAccentFor()` and never appends it to the text
handed to the classifier.
