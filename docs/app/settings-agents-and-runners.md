# Settings → Agents & runners

Part of [the App docs](README.md). `/dashboard/settings/agents` manages the
Claude Code and Codex accounts that the account's desktops and runners use,
combining the desktop account menu ([desktop/agents-ui.md](../desktop/agents-ui.md))
and the runners page ([RUNNERS.md](../RUNNERS.md)).

## Desktops

Each desktop gets a card driven over the encrypted control channel
([desktop-agents-web.md](desktop-agents-web.md)). Nothing polls.

| Action | Method | Capability |
| --- | --- | --- |
| Accounts, usage, selected provider | `agentops.usage` | — |
| Select / rename / add account | `agentops.accounts.select`, `.rename`, `.add` | `agentops.accounts` |
| Log in / out | `terminal.start {mode: "login"\|"logout"}` | `terminal.accounts` |
| Agent terminals | `terminal.start {mode: "agent"}`, `.list`, `.attach`, `.close` | `terminal.agents` |

Terminals stream through `createDesktopControlTerminal` in
`app/public/agent-terminal.js`. Closing the dialog ends a sign-in but only
detaches (`terminal.detach`) an agent terminal, which runs with full
permissions. Desktops lacking a capability show read-only rows.

## Runners and code

The lower half reuses `/dashboard/runners` code (`09-runners.js`). The
runner rows carry their own padding and dividers (see
[RUNNERS.md](../RUNNERS.md#the-runners-page)), so the `data-runners-list`
container should be a plain `divide-y` list without padding. Markup
`settings-agents` in `partials/views/settings.html`; logic
`app/public/dashboard/js/09-agent-profiles.js`. Tests:
`app/tests/test_settings_agent_profiles.py`,
`app/browser_tests/tests/dashboard-settings-agent-profiles.spec.js`.

## Notices and states

Desktop and runner states use the shared dashboard components
([dashboard-components.md](dashboard-components.md)): connection state and
"In use" / "New agents" are `fm-label` pills (success, danger, accent), card
actions are `fm-btn fm-btn-sm`, and errors are `text-destructive` so the
shared Copy / Send to prompt actions attach to them. The runner sign-in
banner and the header attention chip tint with the `destructive` (expired)
and `attention` (expiring) tokens instead of raw red and amber. Logging an
account out and ending a terminal still use the browser's `confirm`, which
the browser test answers.
