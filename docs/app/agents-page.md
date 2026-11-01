# Agents on the web: the Agents page and desktop agents

Part of [the App docs](README.md). The Agents page (`/dashboard/agents`)
lists every agent session an account can see across its repositories,
organizations, nodes and runners; a repository's Agents tab also lists and
manages the owner's desktop agents while that desktop runs. How agent runs
work is in [../agents/README.md](../agents/README.md).

## The Agents page

Code: view `app/public/dashboard/partials/views/agents.html`, module
`app/public/dashboard/js/09-agents.js` (served as `/dashboard-page-agents.js`),
route `account_desktop_agents_handler` in `app/src/route_handlers.py`, deep link
`takeLinkedAgentSession` in `06-repo-content.js`. Tests:
`app/tests/test_agents_dashboard.py`.

### Sources

| Kind | Source |
| --- | --- |
| Cloud and organization runs | `GET /api/account/agent-sessions` (`scope: "owner"` / `"organization"`); all active runs plus the 40 most recent finished (`ACCOUNT_AGENT_MAX_SESSIONS`) |
| Desktop sessions | `GET /api/account/desktop-agents` |

Desktop sessions are owner-sealed envelopes in `repo_agents`; the relay cannot
decrypt them, so the page shows only repository, node, session id and last
report time, and links to the repository's Agents tab, which reads the desktop
live over the encrypted control channel ([below](#desktop-agents-on-a-repositorys-agents-tab)).

`GET /api/account/desktop-agents` (session only, `no-store`) resolves the
account's nodes, reads their catalog keys (max 500 repositories), reads
`repo_bi, agent_id, updated_at` from `repo_agents` (never `data`), and returns
up to 200 rows: `{ok, account, agents: [{id, owner, repo, node, reportedAt}],
truncated, privacyBoundary: "owner-only-e2ee", serverCanDecrypt: false}`.

### Behaviour

- Status badges map the queue status; `displayStatus: "attention"` reads
  **Needs attention** with the diagnostic; desktop rows read **On desktop**.
- Filters (status, repository, node) persist in the URL (`?status=&repo=&node=`).
- A row links to `/<owner>/<repo>/agents?session=<id>`; the tab opens that
  session full-width and strips the parameter with `history.replaceState`.
- Nothing polls: lists re-read on the account socket events
  `forkmesh:cloud-agents` and `forkmesh:desktop-agents`, one read in flight.

### Layout

The page follows GitHub's Actions list (Primer):

- A page header: title, one muted line, then **Refresh** (default button) and
  **New agent** (`data-agents-new`, the page's one green button). New agent
  opens the global composer (`window.ForkMeshComposer.open()`, falling back to
  the header's `#agentModalToggle`); it is disabled while signed out.
- One setup notice (`data-agents-setup`) with accent links to
  `/dashboard/runners?setup=1` and `/desktop`. It shows only after loading,
  with no read errors, when the account has no sessions and the runner-accounts
  picker stayed hidden (no runner reported a provider), so a new run would have
  nowhere to start. The composer still owns its own disabled-control notice
  ([web-composer.md](web-composer.md)).
- The runner-accounts picker and the session list are flat bordered boxes with
  a subtle header row (`--dashboard-subtle-rgb`); nothing is nested in a box.
  The list header carries the count (`N sessions · M working`) and the filters.
- A row is a status icon, the title (accent on hover), a neutral status label,
  a muted metadata line (repository, kind, provider and model, node, author)
  and the times in a right-hand column (inline on narrow screens).
- Status icons: working `loader-circle` spinning amber, queued `circle-dashed`
  amber, needs attention `triangle-alert` amber, completed `circle-check` green
  (the only green, success), failed or rejected `circle-x` red, stopped
  `circle-slash` grey, stopping `circle-stop` grey, desktop `monitor` grey.
  Amber text switches to amber-700 in the light theme through the
  `[[data-dashboard-theme=light]_&]` variant, since amber-500 on white is too
  faint for label text.
- Empty states are centred blankslates: signed out (accent Sign in link), no
  sessions, and no filter match (with a **Clear filters** button,
  `data-agents-clear-filters`).
- The setup notice is a blue info `fm-flash`. Read problems and partial-load
  notes (`data-agents-notice`) are a danger or neutral flash inside the list
  box, built by `setAgentsNotice`.

## Desktop agents on a repository's Agents tab

When the owner's desktop is running, a repository's Agents tab lists that
desktop's agents and manages them: live transcript, prompt, stop, continue,
diff, merge and clean up, delete.

### Transport

The browser uses the same end-to-end encrypted control protocol as the phone
apps ([desktop networking](../desktop/networking.md)); the relay only forwards
`{"type":"control"}` chunks (`app/src/node_control.py`) and cannot read or
forge them. Browser side: `app/public/desktop-control.js`, UI in
`06-repo-content.js`. Desktop side: `agents.*` (`MainWindowLocalControl.cpp`)
and `agentops.*` (`MainWindowLocalControlAgentOps.cpp`).

### Security rules

- Each browser holds a non-extractable Ed25519 key (IndexedDB) registered via
  `POST /api/accounts/devices/browser-control`, signed over
  `forkmesh-browser-control-v1\n<account>\n<pubkey>\n<ts>`. It becomes an
  `account_devices` row of kind `browser_control` with only `desktop_control`;
  it can never sign owner writes.
- Revocable in Settings → Devices (`device_revoked`); at most
  `BROWSER_CONTROL_MAX_DEVICES` (20); a key bound as another kind is refused
  (`device_key_conflict`). The browser accepts a welcome only from the
  account's listed desktop keys.
- The desktop never persists a web peer, re-checks it on every connect, and
  allows `agentops.*` only to the owner.

### Actions

`agents.list` (re-read on `agents.changed`), `agents.transcript` (deltas via
`since`), `agents.prompt`, `agents.stop`, `agents.continue`, `agentops.diff`
(capped at `kMaxDiffChars`), and `agentops.prepare` followed by
`agentops.merge` / `agentops.delete` with a one-use, one-minute token. Delete
refuses a running agent. New agents still start through the website queue.
[Settings → Agents & runners](settings-agents-and-runners.md) and the phones
([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)) use the same channel.

The Workflows page uses the same channel for desktop steps: `diag.prepare` then
`diag.run` with action `freeDiskSpace` and a `step`, one step at a time
([workflows.md](workflows.md#desktop-steps)). A failed request rejects with the
desktop's own `message` when it sent one (for example the last line a failed
cleanup step printed), otherwise with the text for its error code.

Requires Ed25519 and X25519 in WebCrypto. A relay reconnect ends all channels.

Tests: `app/tests/test_desktop_control_js.py`, `app/tests/test_mobile_node_devices.py`,
`desktop/tests/test_local_control*.cpp`,
`app/browser_tests/tests/dashboard-desktop-agents.spec.js`.
