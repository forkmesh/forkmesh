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

The lower half reuses `/dashboard/runners` code (`09-runners.js`). Markup
`settings-agents` in `partials/views/settings.html`; logic
`app/public/dashboard/js/09-agent-profiles.js`. Tests:
`app/tests/test_settings_agent_profiles.py`,
`app/browser_tests/tests/dashboard-settings-agent-profiles.spec.js`.
