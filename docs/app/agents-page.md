# Agents page (`/dashboard/agents`)

Part of [the App docs](README.md). Requested 2026-09-27. Code: the view
`app/public/dashboard/partials/views/agents.html`, the page module
`app/public/dashboard/js/09-agents.js` (a per-page chunk, served as
`/dashboard-page-agents.js`), the route `account_desktop_agents_handler` in
`app/src/route_handlers.py`, and the session deep link
`takeLinkedAgentSession` in `app/public/dashboard/js/06-repo-content.js`.
Tests: `app/tests/test_agents_dashboard.py`. How agent runs themselves work is
in [../agents/README.md](../agents/README.md).

The **Agents** link sits in the Workspace group of the dashboard sidebar,
between Repositories and Apps, for every signed-in account. The page lists
every agent session the account can see, across all its repositories,
organizations, nodes and runners, on one page. Before it, a run could only be
found by opening the Agents tab of the repository it belonged to.

## What is listed

| Kind | Source | What a row shows |
| --- | --- | --- |
| Cloud run | `GET /api/account/agent-sessions`, sessions with `scope: "owner"` | title, `owner/repo`, provider and model, target node, status, started and updated times, who queued it |
| Organization run | the same read, `scope: "organization"`; the route adds an organization's runs only for a member the organization agent route lets read them | as above |
| Desktop | `GET /api/account/desktop-agents` | `node/repo`, the session id, when the desktop last reported |

The account-wide cloud read is the one desktops and phones already use; the
page sends the browser session token. It returns every active run and the 40
most recent finished ones (`ACCOUNT_AGENT_MAX_SESSIONS`). When older finished
runs were left out (`finishedBefore > 0`), the page says they are in each
repository's Agents tab.

### Desktop sessions are sealed

A desktop pushes its sessions for a repository to `/api/repo/<o>/<r>/agents`
as owner-sealed envelopes: only the owner's desktop key opens them, and the
relay stores just `repo_bi`, `agent_id`, the envelope and the push time
(`repo_agents`). The web has no key for that copy, so this page cannot show a
desktop session's title, status, model or conversation, and does not try. It
shows that the session exists, which repository and node it belongs to, and
when the desktop last reported. The row links to the repository's Agents tab,
which, while the desktop is running, reads and manages the desktop's agents
directly over the end-to-end encrypted desktop-control channel
([desktop-agents-web.md](desktop-agents-web.md)). This page does not open
that channel: it would have to reach every desktop for every repository.

`GET /api/account/desktop-agents` (session token only, `no-store`):

1. resolves the signed-in account and its nodes (`_profile_catalog_owner_names`:
   the account itself and every node linked to it);
2. reads each node's catalog keys (`repositories.key_bi`, which is
   `blind_index("<owner>/<repo>")`, the same key `repo_agents` uses), at most
   500 repositories;
3. reads `repo_bi, agent_id, updated_at` from `repo_agents` for those keys in
   chunks of 50, never the `data` column;
4. decrypts the catalog record only of repositories that have sessions, for
   the repository name;
5. returns up to 200 rows, newest report first, with `truncated: true` when
   more exist.

Response: `{ok, account, agents: [{id, owner, repo, node, reportedAt}],
truncated, privacyBoundary: "owner-only-e2ee", serverCanDecrypt: false}`. The
node is the repository's owner namespace, which is the desktop node that
publishes it. `reportedAt` is when the desktop's last changed snapshot for
that repository was stored. The desktop replaces the whole set on every
change, so every session of one repository has the same time.

## Status and the live indicator

The badge uses the relay's queue status: Working (`running`), Queued,
Security review, Completed, Failed, Rejected. A session whose projection says
`displayStatus: "attention"` (a node that has not claimed the job after 90
seconds, a missing provider CLI or sign-in, a failed run) reads **Needs
attention** with the diagnostic message under the row. A stop that has been
asked for but not yet reported reads **Stopping**. A running session has a
pulsing green dot. Desktop rows read **On desktop**.

Rows are ordered running first, then other active runs, then by last update.

## Filters

Three selects: status (All, Working, Needs attention, Completed, Failed, On a
desktop), repository and node. Repository and node options come from the
rows loaded. The choices are kept in the URL (`?status=&repo=&node=`,
lowercase) so a filtered view can be bookmarked or shared.

## Opening a session

A cloud or organization row links to
`/<owner>/<repo>/agents?session=<id>`. When the repository's Agents tab loads
it, `takeLinkedAgentSession` takes the `session` parameter off the URL
(`history.replaceState`, so going back to the list and reloading does not
reopen it) and opens that session's conversation in the detail pane, the same
view as clicking the row in the tab. If the session is not in the tab's list,
the tab shows the list and the start composer.

Since 2026-10-06 an open session is a page of its own on every screen size:
`syncOrgAgentLayout()` hides the session list (`data-org-agent-sessions`) and
drops the two-column grid while the pane shows a cloud or desktop session, so
the session fills the width. On a phone the list used to stay above the
session, so tapping a row left the other agents on screen. **Back to
sessions** brings the list back. When the layout switches with its top
scrolled out of view, it scrolls back to the top.

## Live updates

Nothing polls. The page re-reads the cloud list on `forkmesh:cloud-agents`
(the account socket's `cloud-agents` and `cloud-agents-live` topics, and every
socket reconnect) and the desktop list on `forkmesh:desktop-agents`, which
`02-helpers.js` now fires for the socket's `agents` topic (rung by
`_ring_agent_readers` when a desktop pushes a changed snapshot). Each list
allows one read in flight and one queued. **Refresh** reloads both.
