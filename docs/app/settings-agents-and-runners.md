# Settings → Agents & runners

Part of [the App docs](README.md). `/dashboard/settings/agents` manages the
Claude Code and Codex accounts that the account's desktops and runners sign
agents in with. Everything the desktop's account menu does
([desktop/agents-ui.md](../desktop/agents-ui.md)) and everything the
runners page does ([RUNNERS.md](../RUNNERS.md)) is here on one page.

The user asked (2026-10-06) for the web Settings page to manage every part of
the runner, agent, Claude Code and Codex profiles: log in, log out, add a
profile, open a terminal, and show usage. Before this the web had the runner
half only on `/dashboard/runners`, and the desktop half only on the phones'
"Accounts and usage" sheet ([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md),
"Accounts, usage and terminals from the usage sheet").

## Desktops

Each desktop on the account (`hub.desktops()` from the browser key binding)
gets a card. The page talks to it over the same end-to-end encrypted control
channel as the repository Agents tab ([desktop-agents-web.md](desktop-agents-web.md)).
The channel is opened when the tab is opened. Nothing polls: the card is read
again on Refresh, after a terminal ends, when the terminal window closes, on
`repos.changed`, and (terminal list only) on `agents.changed`, debounced
1.5 s and only while the tab is on screen.

| Shown or done | Method | Needs capability |
| --- | --- | --- |
| Provider and account rows, usage windows, extra-usage credits, API spend, which provider new agents use | `agentops.usage` | — |
| Use for new agents | `agentops.accounts.select {provider, account}` | `agentops.accounts` |
| Rename | `agentops.accounts.rename {provider, account, label}` | `agentops.accounts` |
| Add account (then its sign-in opens at once) | `agentops.accounts.add {provider, label}` | `agentops.accounts` |
| Log in / Log in again / Log out | `terminal.start {provider, mode: "login"\|"logout", account}` | `terminal.accounts` |
| New terminal, Terminal as this account | `terminal.start {provider, mode: "agent", account?, owner, name}` in the repository picked on the card (`repos.list`, checked-out ones) | `terminal.agents` |
| Running terminals: Open / End | `terminal.list`, `terminal.attach`, `terminal.close` | `terminal.agents` |

- The account in use shows the provider's live windows; the others show the
  last windows the desktop stored for them, as on the desktop.
- A desktop without the capability shows the rows without the buttons, and
  says to update the app.
- A desktop that does not answer shows "Not reachable" with Try again.
- There is still no way to delete a desktop profile. The desktop has none
  either.

### The terminal window

Terminals open in the dashboard's existing sign-in dialog
(`[data-agent-login-modal]`, `showAgentLoginModal` in
`06-repo-content.js`). `createDesktopControlTerminal` in
`app/public/agent-terminal.js` streams them:

- It sends `terminal.start` (or `terminal.attach`) with the grid's columns
  and rows.
- It feeds `terminal.output` (and `reset`) into the same VT emulator as the
  node sign-in terminal, and the start answer's `replay` too.
- It sends keystrokes as `terminal.input` and size changes as
  `terminal.resize`.
- Output that arrives before `terminal.start` has answered is kept and
  replayed once the terminal id is known.
- Closing the dialog ends a sign-in (`terminal.close`). An agent terminal is
  only detached (`terminal.detach`): it keeps running on the desktop and is
  listed under Running terminals, where End closes it.
- `mountTerminalView` is the part shared with `createAgentLoginTerminal`
  (keyboard, paste, measuring, resize, rendering).
- The dialog's footer is set per use: for a desktop it says the stream is end
  to end encrypted, and for an agent terminal that the agent has full
  permissions (`--dangerously-skip-permissions` /
  `--dangerously-bypass-approvals-and-sandbox`, as for the phones).

## Runners

The lower half is the runners list from `/dashboard/runners`, rendered by the
same code (`loadRunners`, `renderRunners`, `09-runners.js`) into the same
`data-runners-*` hooks:

- provider status;
- each account with its usage;
- Use this account and Remove;
- Add account / Connect, which opens the runner sign-in panel.

`wireRunnerLoginPanel` wires that panel on either page, once.
`initSettingsRunners` is the Settings entry point. Setting a runner up stays
on `/dashboard/runners` (the Set up a runner link).

## Code

- Markup: `settings-agents` in `app/public/dashboard/partials/views/settings.html`.
- Logic: `app/public/dashboard/js/09-agent-profiles.js`, a core-bundle
  fragment (it calls `desktopControlHub`, `runnerUsageRow` and
  `showAgentLoginModal`), entered from `setSettingsSection` in
  `04-account.js` (`"agents"`).
- Terminal client: `createDesktopControlTerminal` in `app/public/agent-terminal.js`.

## Tests

- `app/tests/test_settings_agent_profiles.py`: the page hooks, the section
  wiring, the methods and capabilities used, and `createDesktopControlTerminal`
  run in node against a fake connection:
  - start parameters, including output that arrives before the start answer;
  - input;
  - exit;
  - close versus detach;
  - attach with replay;
  - a refused start.
- `app/browser_tests/tests/dashboard-settings-agent-profiles.spec.js`: the
  page against a fake `/desktop-control.js` and `/api/runners`:
  - accounts, usage and runners shown;
  - select, rename and log in;
  - add an account, which signs in at once;
  - agent terminal detach and End.

  Set `SETTINGS_AGENTS_SCREENSHOT_DIR` to save screenshots.
