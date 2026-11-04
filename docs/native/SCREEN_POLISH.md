# Screen polish (November 2026)

Part of [the native docs](README.md). A pass over the phone screens in both
apps to remove the habits that made them look generated: shadows on content,
capitalised eyebrows, icon-only controls, red text for signed-out states and
tinted icon squares on every row. The glass theme, the bottom bar and the
sign-in and sign-up screens are unchanged. Navigation is in
[NAVIGATION.md](NAVIGATION.md). Android follows the same rules
([ANDROID.md](ANDROID.md)).

## Rules

- **Depth.** Only the floating layer casts a shadow: the bottom bar, the new
  agent button, drawers and sheets. Content cards and lists sit flat, separated
  by their fill and a large corner. `FmGlassSurface` takes `floating:` for the
  few floating glass views. The Projects drawer's shadow was drawn under every
  child because it had no `compositingGroup()`, which gave its card the large
  smudge.
- **Headings.** `FmSectionHeader` is sentence case, `labelLarge` in secondary
  ink, with the count as quiet text after the title. Home group headings and
  the Chat list headings follow the same style. NEXUS keeps its capitals, as
  it is the package's name.
- **Large titles.** Agents, Inbox, Repos and the Projects drawer start with a
  title in `displaySmall` (28 pt semibold). The type scale has no bold and no
  34 pt step, on purpose ([THEMES.md](THEMES.md)).
- **Rows.** Repository rows lead with the owner's avatar, and Desktop and
  Projects rows lead with a bare symbol. A description is one line with an
  ellipsis; the full text is on the repository page and in the long-press
  preview. Meta is one plain line ("16 mirrors · Public · 6m ago").
- **Selection.** Segmented choices use a lighter pill with no colour, like the
  bottom bar. The accent marks actions, links and unread.
- **Empty states.** `FmEmptyState` takes optional art and one action. Signed
  out is an empty state with a Sign in button, never red text
  (`InboxModel.signedOut`).

## Screens

- **Agents.** Each tool that shows (sort, list view, runners, terminal, watch
  and the rest) is its own small glass square (`AgentToolChip`, 30 pt; 36 dp on
  Android). On iPhone the row sits under the title and scrolls sideways when it
  is wider than the screen; one capsule beside the title squeezed the tools
  under the title and clipped them (2026-11-04). Android keeps the rest in More,
  so its row fits beside the title. When there are no
  agents, `AgentPipelineArt` draws prompt → mirror → branch → pull request →
  merged, with a dot that travels the path in 1.6 s on appear and on tap
  (the final frame with Reduce Motion). Two "Try one" rows open the composer
  with a starter prompt (`FmMessageHandoff.compose`). The no-desktop note is
  one footnote.
- **Inbox.** The title is the filter: a native `Menu` on "Inbox ⌄" lists All
  activity, then Approvals, Reviews, Assigned, Mentions and Pings with their
  unread counts, and grows out of the title as glass on iOS 26. The subtitle
  names the filter and the unread count. Mark all read, Unread only, pause,
  Discard unsynced changes, Ping settings and Delete all pings are in one "…"
  button, which shows a dot while Unread only or a pause is on.
- **Repos.** All, Online and Offline are a glass segmented control with
  counts. Public, Private and sort are in the filter menu; a "Private ×" pill
  shows a visibility filter that is on. New repository, Import from Files,
  Mirror a repository and Sync are one New menu on "On this device", which
  replaces three buttons that ran off the screen. The storage location row
  shows only once there are repositories on the device.
- **Repository page.** Destinations are two grouped lists (README first) with
  counts as trailing text instead of sixteen tiles with count bubbles. Agent is
  the one filled action.
- **Projects drawer.** One list: projects, then Add project as the last row;
  Invite people and Manage projects in a second group.
- **Header.** For guests, the Hub button and Sign in share one glass capsule.
  The new agent button's shadow is neutral; its stars still count running
  agents.
- **Chat.** Recent hides when empty and its online count moves to People.
  Empty direct messages show one line. Rows you cannot tap are drawn plainly,
  not dimmed like disabled buttons.
- **Telemetry strip.** Off by default (`diagnostics/showStrip`); the latency
  pill was already debug-only.

Genie keeps clear of the new title rows and search fields (`genieKeepsClear`);
it can still rest over list rows.
