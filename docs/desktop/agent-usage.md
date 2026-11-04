# Desktop: agent accounts and the Usage page

Part of [Desktop](README.md). This page covers the Claude Code and Codex
accounts agents run as, the in-app **Usage** page with its meters, Refresh
and install buttons, the usage menus and hover card in the
[Agents page](agents-ui.md)'s composer, and the `agentops.usage` read that
the web and phone Usage screens use
([usage package](../marketplace/usage.md)).

## Code map

| Part | Files |
| --- | --- |
| Usage page | `src/MainWindowAgentUsagePage.cpp` (`MainWindow::buildAgentUsagePage`) |
| Usage reads | `src/MainWindowAgentUsage.cpp`, `src/CodexRateLimitsProbe.*`, `src/AgentUsageResetClock.*` |
| Accounts | `src/MainWindowChat.cpp`, `src/ClaudeAccountRouting.cpp`, `src/ClaudePeerMessaging.cpp` |
| Remote read | `src/MainWindowLocalControlAgentOps.cpp` |

## The Usage page

Clicking a usage icon (the **Usage** button atop the agents list and the
Claude Code and Codex meters at the bottom right of the prompt) opens the
in-app **Usage** page (`openAgentUsagePage`).

- It lists Claude Code and Codex, the default provider first, and carries
  everything the right-click menus offer: **Open … in a terminal**
  (`openAgentProviderTerminal`), **Launch in system terminal**, **Add
  account…**, **Runner accounts**, **Use … for new agents**, and per account
  **Run new agents as this account**, **Edit account…**, **Log in…**, **Log
  out…**, **Delete…**, the terminal launch button and each usage window's bar
  with its reset countdown button.
- The page reads this desktop's settings, so it works without a ForkMesh
  host, and it rebuilds after usage responses and account changes
  (`scheduleAgentUsagePagesRefresh`).
- When the `usage` Market package is installed its rail entry hosts the same
  page (`NativePackageHost::agentUsagePage`, permission
  `desktop.models.usage.read`; the package's `UsageFeature.cpp` embeds it) and
  a usage icon opens that entry; otherwise the page is a section of its own
  under **Agents**. Right-click still opens the account and terminal menus.

### Installing a missing CLI

When `forkmesh::agentcli::installed("claude")` or `("codex")` finds no CLI,
that provider's section says "Not installed on this desktop" and shows
**Install Claude Code** or **Install Codex** (`agentUsagePageInstallClaude` /
`agentUsagePageInstallCodex`); its terminal buttons are disabled, since they
would only fail.

- The button runs the same install as Settings -> Apps (`installSuggestedApp`,
  npm with `@anthropic-ai/claude-code` or `@openai/codex`, after a
  confirmation).
- Without npm it is disabled and the page shows the reason. While an install
  runs it reads "Installing…"; a failed install leaves its last output line
  (`m_suggestedAppInstallProblems`) under the button.
- `refreshSuggestedApps` redraws the page, so the button goes away once the
  install finishes.

## Refresh

**Refresh** calls `refreshAgentUsageAccounts`, which re-reads every account
one at a time. The button reads "Refreshing…" until the last account answers.

- **Claude Code accounts.** For each account it reads that account's own OAuth
  token from its config folder (`claudeCodeOAuthToken(configDir)`), asks
  `api.anthropic.com/api/oauth/usage`, and stores the answer with
  `applyClaudeUsageResponse(response, accountId)`. The active account goes
  through the full path (top-bar meter, limit reminders, credits); any other
  account only gets its account-scoped settings written, so the meters are a
  real re-read of each account rather than a redraw.
- **Codex accounts** (a config folder holding `auth.json`) come next, skipped
  when the `codex` CLI is not installed. Codex limits only come from
  `codex app-server`, so `CodexRateLimitsProbe` (tested by
  `tests/test_codex_rate_limits_probe.cpp` with the test binary standing in
  for the app-server) starts one with `CODEX_HOME` set to that account's
  folder, sends `initialize`, `initialized` and `account/rateLimits/read`,
  takes `rateLimits` and `rateLimitsByLimitId` (adding the general snapshot
  under its own `limitId`, as `CodexAppServerSession` does), and stops the
  process. It starts no thread and no conversation, so it spends no tokens.
  Each probe gets 10 seconds; a probe that times out, exits early or gets an
  error counts as a failure for that account and the walk moves on.
- **The probe is not a repository agent.** It runs in the account's config
  folder, touches no worktree, and so takes no repository guardrails or KVM
  isolation. It keeps the rest of an agent launch: the widened `PATH`, the
  `agentresources` environment and shell wrapper, and the systemd scope
  (`agentresources::contain`) where that exists. `OPENAI_API_KEY` and
  `CODEX_API_KEY` are removed from its environment so the read is the
  signed-in account's, not whatever key ForkMesh was started with.
- **Storing Codex limits.** `applyCodexRateLimits(rateLimits,
  rateLimitsByLimitId, accountId)` takes the usual path for the active account
  (top-bar meter, limit reminders, the global keys). For any other account
  `applyInactiveCodexRateLimits` writes only that account's scoped settings
  (usage windows, 5-hour and weekly percent, reset and window start), the
  same split Claude gets.
- **Backoff.** Each account keeps its own poll backoff key
  (`claude-usage-<id>`, `codex-usage-<id>`), so a rate-limited account is
  skipped until its backoff ends, and a failed Claude read keeps the server's
  `Retry-After`, as the regular poll does (`claudeUsageRetryAfterMs`).
- **Coalescing.** A call while a refresh runs only joins its waiters
  (`m_agentUsageRefreshWaiters`), and a call within 30 seconds of the last
  finished one (`kAgentUsageRefreshMinMs`) answers from the stored readings
  without asking Anthropic again. Several clients refreshing at once (or a
  paired client calling `agentops.usage {refresh: true}` in a loop) therefore
  cannot earn a 429 that also stalls the regular poll.

## Reset times

Each meter shows the reset as a 12-hour clock time in the desktop's time
zone, next to the remaining time: "Resets 3:45 PM (in 5 h 40 min)", "Resets
Tue 9:00 AM (in 22 h 55 min)" when it is not today, and "Resets Oct 26,
10:00 AM" a week or more out. `forkmesh::usageclock` (tested by
`tests/test_agent_usage_reset_clock.cpp`) formats it; the menus and top bar
keep their relative "resets in" text.

## Web and phones

Remote surfaces get the same re-read through `agentops.usage {refresh: true}`:
the desktop runs `refreshAgentUsageAccounts` and answers with the new summary
once every Claude Code and Codex account has answered, or after 20 seconds
with whatever it has. Without `refresh` the call answers the stored snapshot
at once; older desktops ignore the flag and answer the snapshot.

The web page (`/dashboard/usage`), iPhone and Android send `refresh: true`
from their Refresh button and pull-to-refresh, and a plain read when the page
first opens. Each formats the reset clock locally:

| Surface | Formatter | Test |
| --- | --- | --- |
| Web | `usagePageResetClock` (`marketplace/packages/usage/content/web/page.js`) | `app/tests/usage_page_web.test.mjs` |
| iOS | `UsageFormat.resetClock` (`FMAgents/Remote/AgentOpsModels.swift`) | `UsageResetClockTests` |
| Android | `UsageFormat.resetClock` (`agents/ops/AgentOpsModels.kt`) | `UsageResetClockTest` |

`relativeTimeLabel` only formats past times, so it must not be used for a
reset. On Android the reset clock moves to the line under the meter, beside
the hour/day and pace text, so the bar keeps its width.

## Composer usage meter and menus

- The composer's bottom bar shows the usage meter of the composer's provider.
  Its menu (`showAgentAccountMenu`) has per-account **Edit**, **Log in**,
  **Log out**, **Delete** and a terminal button that opens the CLI as that
  account (`CLAUDE_CONFIG_DIR` / `CODEX_HOME`).
- **Usage** (`agentUsageButton`) opens the Usage page; its right-click menu
  (`showAgentUsageMenu`) shows the default provider's bars and offers
  terminals, **Run new agents as**, account management and the default
  provider switch. **Runner accounts** (`fillRunnerAccountsMenu`) switches a
  runner's account.
- Both account popups start with **Open the Usage page**
  (`addAgentUsagePageAction`).
- Usage rows (`TokenUsageMenuRow`) segment the bar per hour or day of the
  window, mark even pace and add a pace note (`usagePaceNote`).
- **Hover card.** Hovering the Claude Code or Codex usage chart in the bottom
  bar, or the agents toolbar's **Usage** button, shows a card
  (`agentUsageHoverCard`, `MainWindow::attachAgentUsageHoverCard` and
  `fillAgentUsageHoverCard`) instead of the plain tooltip. It holds the
  active account's usage bars (`TokenUsageMenuRow`, with the 12-hour reset
  clock and the reset countdown button), the session stats line, and the
  Usage page's controls for that account: Refresh, Open the Usage page, Open
  in a terminal, Launch in system terminal, Edit account…, Log in…, Log out…,
  Add account…, a switch to each other account, Runner accounts… and **Use …
  for new agents**. The card is a `Qt::ToolTip` window that takes clicks; it
  opens 350 ms after the pointer enters, stays while the pointer is on the
  chart or the card, and closes about 360 ms after it leaves both, or on a
  click on the chart. It refills from `scheduleAgentUsagePagesRefresh` when
  usage changes, and skips the rebuild when nothing it shows changed, so a
  click is not lost to a refill. The chart's tooltip text is kept for
  accessibility and the toolbar button. `forkmesh-window-tests
  --usage-hover-only` checks both providers' cards
  (`checkAgentUsageHoverCard`).

## Accounts

- **Delete** (`confirmDeleteAgentAccount`, `deleteAgentAccount`) asks first,
  switches new agents to the default account if the deleted one was
  selected, drops the profile, identity, label and cached usage settings, and
  removes the login file (`.credentials.json` / `auth.json`). The account
  folder stays: it holds the account's agent history, and running agents and
  peer messaging (`ClaudePeerMessaging.cpp`) still read it. A login still
  parked under `claude-login/` is moved to `agent-accounts/<provider>/<id>`
  so the next sign-in does not re-file it as a new account. The default
  account is the CLI's own home folder (`~/.claude`, `~/.codex`) and cannot
  be deleted; sign it out with **Log out**. On macOS a Claude login kept in
  the Keychain is not touched.
- **Claude account routing.** Each account is tied to one Claude login
  (`accountUuid`/`organizationUuid`, saved under
  `agents/accountIdentity/claude/<account id>`). Logins run in a fresh folder
  under `<data>/agent-accounts/claude-login/`. **Log in** on an account always
  files the login on that account, even as a different Claude identity: it
  replaces that account's login and rebinds it, and never adds an account.
  **Add account** reuses the account that already holds the identity and only
  adds one for an identity no account has. A login made outside ForkMesh in
  an account's folder (`/login` in its terminal) stays there:
  `reconcileClaudeAccountLogins` rebinds the account to it. Files move by copy
  then remove, never by deleting folders.
- **Names.** Accounts are shown by the Claude display name
  (`oauthAccount.displayName` in the account's `.claude.json`), never the
  email; a new account with no typed label is named after it.

## Tests

`tests/test_claude_account_routing.cpp`, `tests/test_runner_login.cpp`,
`tests/test_codex_rate_limits_probe.cpp`,
`tests/test_agent_usage_reset_clock.cpp`, `forkmesh-window-tests
--usage-hover-only`.
