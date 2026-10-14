# Agents page (`/dashboard/agents`)

Part of [the App docs](README.md). Lists every agent session an account can see
across its repositories, organizations, nodes and runners. How agent runs work
is in [../agents/README.md](../agents/README.md).

Code: view `app/public/dashboard/partials/views/agents.html`, module
`app/public/dashboard/js/09-agents.js` (served as `/dashboard-page-agents.js`),
route `account_desktop_agents_handler` in `app/src/route_handlers.py`, deep link
`takeLinkedAgentSession` in `06-repo-content.js`. Tests:
`app/tests/test_agents_dashboard.py`.

## Sources

| Kind | Source |
| --- | --- |
| Cloud and organization runs | `GET /api/account/agent-sessions` (`scope: "owner"` / `"organization"`); all active runs plus the 40 most recent finished (`ACCOUNT_AGENT_MAX_SESSIONS`) |
| Desktop sessions | `GET /api/account/desktop-agents` |

Desktop sessions are owner-sealed envelopes in `repo_agents`; the relay cannot
decrypt them, so the page shows only repository, node, session id and last
report time, and links to the repository's Agents tab, which reads the desktop
live over the encrypted control channel ([desktop-agents-web.md](desktop-agents-web.md)).

`GET /api/account/desktop-agents` (session only, `no-store`) resolves the
account's nodes, reads their catalog keys (max 500 repositories), reads
`repo_bi, agent_id, updated_at` from `repo_agents` (never `data`), and returns
up to 200 rows: `{ok, account, agents: [{id, owner, repo, node, reportedAt}],
truncated, privacyBoundary: "owner-only-e2ee", serverCanDecrypt: false}`.

## Behaviour

- Status badges map the queue status; `displayStatus: "attention"` reads
  **Needs attention** with the diagnostic; desktop rows read **On desktop**.
- Filters (status, repository, node) persist in the URL (`?status=&repo=&node=`).
- A row links to `/<owner>/<repo>/agents?session=<id>`; the tab opens that
  session full-width and strips the parameter with `history.replaceState`.
- Nothing polls: lists re-read on the account socket events
  `forkmesh:cloud-agents` and `forkmesh:desktop-agents`, one read in flight.
