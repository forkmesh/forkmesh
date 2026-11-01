# Usage

Part of [Marketplace](README.md). The `usage` package shows the provider usage
of the coding-agent accounts on the desktops linked to the signed-in ForkMesh
account: an in-app page on the desktop, native screens on iPhone and Android,
and a web page.

The Usage package reads only the current, already-authenticated provider usage snapshots from desktops linked to the signed-in ForkMesh account. It does not receive provider credentials or agent transcripts. Its Refresh button (and pull-to-refresh on the phones) asks each desktop to re-read its Claude Code accounts from Anthropic first (`agentops.usage {refresh: true}`); the desktop makes those requests with its own stored tokens, coalesces overlapping refreshes and answers from its stored readings within 30 seconds of the last one ([desktop/agent-usage.md](../desktop/agent-usage.md)).

## Usage on the desktop

Usage is an in-app page (`UsageFeature.cpp` embeds the window's own Usage page through `NativePackageHost::agentUsagePage`, see [package-system.md](package-system.md#desktop-page-contract)) with this desktop's accounts, usage bars, terminals and account controls; the usage icons open the same page ([desktop/agent-usage.md](../desktop/agent-usage.md)).

## Usage on iOS

`MarketPackage.nativeDestinations` maps `usage` to `Route.usage`, which `/dashboard/usage` also parses to; it sits in the hub's Insights group. `UsageScreen` (`native/ios/ForkMesh/Features/Packages/UsageScreen.swift`) talks to the desktop the control hub reaches, not to every account desktop as the web page does, and names it with its connection (on this network, through the relay, or why it is not reachable). Its body is `AgentAccountsPanel` (`Features/Agents/AgentAccountsPanel.swift`), which holds everything the accounts sheet held:

- every provider (Claude Code, Codex) and every account with its signed-in identity, usage windows (segmented pace bars), usage credits and this month's API spend, with "Used for new agents" on the provider and the account that new agents use;
- New terminal per provider, and per account a terminal button and a menu with Use for new agents, New terminal as this account, Log in, Log out, Rename and Delete (not offered for the default account; the desktop also refuses it with `default_account`);
- Add account, which creates and selects the profile on the desktop and then opens its sign-in terminal;
- runner accounts, pull to refresh and a refresh button, and the states for loading, an error, a desktop too old to report usage or account actions, and no desktop paired.

The Agents screen's usage bars and the usage card on Desktop agents push this screen, sharing their usage model, whether or not the Usage package is installed, because the meters belong to Agents.

## Usage on Android

`MarketPackage.NATIVE_DESTINATIONS` maps `usage` to `Route.Usage` (kind `usage`), which `/dashboard/usage` and `forkmesh://usage` parse to; `PackageGate` gates it on the `usage` package and it sits in the hub's Insights group for signed-in users. `UsageScreen` (`native/android/app/.../feature/usage/UsageScreen.kt`) reads `agentops.usage` from the desktop the control hub reaches (named with its connection, like iOS) through `AgentUsageModel`, and offers what the desktop's account menu did:

- every provider and every account with its signed-in identity, its own usage windows (the active account falls back to the provider's windows, as the web page does), credits, API spend and "Used for new agents" on the provider new agents use;
- per provider, New agent (the agent-in-a-terminal form) and Add account, which creates and selects the profile and then opens its sign-in terminal;
- per account, Terminal as this account (the same form, started with `account`), Use for new agents, and a menu with Log in, Log out, Rename and Delete (not offered for the `default` account); Log in and Log out run `terminal.start` with `mode` `login` or `logout` for that account, show an Open sign-in page button for the link the provider prints, and close that terminal when the screen is left;
- a New agent in terminal button, runner accounts, pull to refresh and a refresh button, usage reset reminders, and the states for loading, an error, a desktop too old to report usage, and no desktop connected.

The Usage card on Desktop agents opens this screen when tapped, whether or not the package is installed.
