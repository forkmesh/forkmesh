# Profile links for usernames and mentions

Part of [the App docs](README.md). Every place that shows a ForkMesh account
name, and every `@name` mention in text people write, opens that account's
profile (`/@name` on the public site). This page lists the shared helpers per
surface and the rules they all follow. Avatars on the profile page itself are
in [profile-avatar.md](profile-avatar.md).

## Rules

- **Which names link.** After a leading `@` is stripped and the name is
  lowercased, it must match the account name pattern:

  ```
  ^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$
  ```

  The text keeps the casing it was written with
  ([username-casing.md](username-casing.md)); the link target is always
  lowercase.
- **Which names stay plain.** Bots (`name[bot]`) and system identities (the
  instance's own git node, Actions, runners and mirror nodes), agents and
  model names, Discord senders and fediverse authors (a display name such as
  "Alice" would open the wrong ForkMesh account), organizations where the row
  is about the organization, and anything that is not a valid account name. A
  bot with its own account page links once the lookup confirms the page.
- **Git history names.** A commit author is a Git identity, not an account. It
  links only when the author name is itself the name of a public user or node
  account (the desktop commits as `user.name = <account>`, mirror intake as
  `user.name = <node>`). Any other identity ("Jane Doe", "ForkMesh Deploy")
  stays plain.
- **Where a mention is not a mention.** Inside code spans and fenced code,
  inside existing links and URLs (`https://x.social/@bob`), in emails
  (`bob@example.com`), and before `.x`, `/x`, `@` or `[bot]`.
- **No nested links.** A name inside a row that is already a link or a button
  (issue and pull lists, the home feed, commit rows, autocomplete options)
  stays plain text.

## Web dashboard

`02-helpers.js` and `06-repo-content.js` hold the helpers:

- `issuePersonNameHtml(name, classes, options)` is the one helper every
  renderer uses for a person's name. It renders
  `<a href="/@name" data-issue-person>` (display casing from `userNameHtml`;
  the platform name for the instance's own account). The hydration
  (`hydrateIssuePersonLink`) first asks `/api/accounts/name-case?names=…`, in
  one request for every name on the page, which are public accounts
  (`issueActorKnown`), then asks `/api/accounts/<name>` through
  `issueActorAccount` only for those, which saves a request per GitHub login
  on a mirror. A private profile is missing from the batch answer and so not
  linkable. The link stays only when the account is `linkable`: it exists, the
  name is taken (`available: false`), the profile is not private, and its kind
  is `user` or `node`. Otherwise the name loses its `href`, or points at the
  repository roster's profile when the roster knows it. Options:
  - `{ git: true }` is for Git history names (commit rows, commit detail, pull
    commits, Insights). It starts without an `href`, gains `/@name` only when
    the account is linkable, and never falls back to the roster.
  - `{ inLink: true }` is for a name inside a row that is already a link or a
    button. It renders plain text (no link, no lookup), so no `<a>` is nested.

  Bots and system identities render as plain spans. A bot whose name is a
  valid handle (not Actions, a runner or a node) carries
  `data-issue-bot-account`, and the hydration swaps it for a link when that
  account is linkable.
- `mdMentionHtml(login)` is the markdown mention link; `renderMarkdown` uses it
  for every body (issues, pulls, discussions, advisories, notes).
- `textWithMentionsHtml(text)` escapes plain text and links its mentions; the
  notification detail title and body use it.

Rendered with `git: true`: the Insights contributor table and the activity
views (Pulse lists, the committer chart, Leaderboard, podium, first
contributions, Contributors, Recent commits), commit authors on the commit
detail page and in a pull request's commit list.

Rendered as links: the reaction "who" list (agents stay plain), the owner in a
repository card (`repositoryCard`, a `role="link"` card whose handler skips
nested links) and in the fork popover when the fork has no published profile
URL, the home feed actor when the card has no link of its own, pull and
discussion hero lines, the issue hero, assignees, reviewers, inline review
comment authors, the pull badge, advisory reporters, openers, comment and
event authors, credits and collaborators, Actions run actors, organization and
team member lists, note sharing, version authors and activity, challenge
owners and members, hackathon team members and squad owners, agent creators
(when the row is not a link) and Apps installers, developers and requesters
(`appsPersonLink`, DOM-built).

Rendered with `inLink` (plain): issue, pull and discussion list rows, commit
list rows, ping rows in the bell menu and the pings window, home feed cards
that are links, and the owner and fork rows of both Top repositories lists.

Tasks (`09-tasks.js`) render mentions with `taskMentionLinkHtml`, an
`<a class="task-mention">` that keeps the mention chip style and the `data-me`
highlight. It is styled in `partials/views/tasks.html` and its packaged copy
`marketplace/packages/tasks/content/web/view.html`, which the page is built
from and must stay identical. `taskPersonHtml` links creators, assignees,
completers, reviewers, reminder senders and timeline actors.

## Server-rendered pages

`app/host/forkmesh_host/repo_ssr_issue.py` holds `profile_href`,
`person_name_html`, `card_author_html` and `mention_html`. `repo_ssr.py`
(`_inline`, README and other markdown) and `repo_views.py` (`_person`, the
light pull, issue and discussion views; the frame's owner link goes to
`/@owner`) reuse them. `person_name_html(name, classes, git=True)` mirrors the
dashboard's `git` option: an `<a data-issue-person data-issue-person-git>`
with no `href`, which the dashboard's hydration links once the account is
confirmed; the code page's last-commit summary uses it. A bot name carries
`data-issue-bot-account` as in the dashboard. Names ending in `-actions` or
`[bot]` never get one, by the same name test as the dashboard: there is no
bot-kind lookup on either side.

`app/src/hackathon_runtime.py` (`_profile_html`) links the invite page's
inviter, "Built by" members, squad cards, the squad heading and challenge
owners. `www/public/hackathon.js` (`profileLink`) links the leaderboards;
`hackathon.css` (`.board-row .who a`) keeps the board's ink colour and
underlines on hover.

Notification emails need absolute links: `chat_format.to_html(source,
profile_origin)` turns mention nodes into links to `{origin}/@name`
(`PROFILE_ORIGIN` when none is given; a mention inside a link label stays
`<strong>`), and `_notification_actor_html` links the "by" name. The origin is
the one `_notification_item_url` uses for the item link
([notification-email.md](notification-email.md)).

## Chat and the world

Message mentions in `app/public/dashboard-chat.js` link through
`mentionProfilePath`. An author links only when the row's `accountKind` is
`"user"`, the message is not from Discord, and the name is a valid account
name that is not ForkBot, an agent or a bot; guests, Discord members and
display names stay plain. The main row, the side rail
(`chat-side-message-author`) and thread cards link, and hovering a linked
author shows the same profile card as a mention. The standalone `/chat` page
(`app/public/chat.js`) links the thread header's sender as its main row does.

The world modules cannot reach `worldPublicProfileUrl` (world.js loads them
with `import()` and exports nothing), so each builds the same URL itself:

- `world-floor-notes.js` (`floorNoteProfileUrl`, `noteTextHTML`): mentions are
  `<a class="mention">`, the note author links.
- `world-office-tasks.js` (`officeTaskProfileUrl`, `officeTaskProfileHTML`,
  `mentionHTML`): mentions, reply authors, assignees, bidders and reviewers
  link; `@agent` stays plain. `mentionHTML` does not trim the text around a
  mention, so "ping @dana" keeps its space.
- `world.js`: the member panel title links to the profile unless the member is
  a guest, and the initials in error actor chips link.

These links keep the surrounding colour and underline only on hover
(`world.css`: `#world-detail-title a`, `.world-error-actor-stack a`,
`.world-task-profile-link`, `.world-task-mention-link`).

## Desktop

`MainWindowInternal.h` defines the `forkmesh-user:` link scheme
(`kUserLinkScheme`) and `MainWindowShared.cpp` the helpers:

- `userProfileLinkHtml(account, shown)` wraps `shown` in a `forkmesh-user:`
  link when `account` is a valid account name, else escapes it. Callers pass
  the event's `authorName`, never the public-key fallback (`author.left(10)`
  can look like a name).
- `linkifyReferenceText` (through `autolinkReferences`) turns `@name` in
  issue, pull and discussion markdown into a markdown link to
  `forkmesh-user:name`.
- `TaskMarkdown.cpp` links mentions after its link sanitizer runs, so a typed
  markdown link to a `forkmesh-user:` target is still dropped while the
  `@label` itself links to its own account.

`openBodyReference` routes `forkmesh-user:` to `MainWindow::openUserProfile`,
which always opens the in-app profile card (`showChatUserProfile`), never a
browser: through the chat directory entry when there is one, otherwise from a
`user:<name>` member that the card fills from `/api/accounts/<name>`. Chat
mention clicks go through the same function. Linked labels: issue and pull
timeline headers, issue activity lines, the issue reporter and assignees, the
pull meta line, pull and discussion conversation cards, the discussion meta
line, task activity, note sharing rows and a package's creator.

## Phone apps

Both apps link mentions to `forkmesh://profile/@name`, which `RouteParser`
opens as `Route.profile` / `Route.Profile`; the app's own scheme keeps a tap
from switching project. `ProfileLinks.swift` (`FMApi/Navigation`) and
`ProfileLinks.kt` hold the account-name check, which also refuses `forkbot`,
`here`, `channel` and `everyone`. Chat mentions and sender names, and issue,
pull and discussion timeline authors and body mentions, are tappable. Discord
messages, fediverse replies and agent transcripts (where `@MainActor` is not a
person) keep plain text, and an author inside a tile that already opens
something stays plain.

## Avatars and the profile card

An avatar opens the same profile as the name next to it, under the same
rules. Wherever the profile opens, it shows the full profile and a QR code of
the account's Solana tip address (`solana:<address>`), the address as text
with a copy button, or "@name has not entered a Solana address for tips."
when none is published. Nodes and organizations have no tip card. The address
comes from the public `solana` field of `/api/accounts/<name>`, which the
server fills only with a valid base58 address; every client checks the shape
again before drawing a code.

- **Web.** `/@name` carries the tip card (`test_profile_tips.py`). The
  dashboard chat rail and side rail wrap the avatar in
  `<a class="chat-avatar-link">` through `linkAvatarToProfile` in
  `dashboard-chat.js`, with the same `authorProfileName` check as the author
  link, so the World's chat gets it too. The standalone `/chat` page links the
  thread header avatar. The Actions run "Triggered by" avatar passes
  `link = true` to `issueActorAvatarHtml`; task detail "Assignee" and
  "Created by" avatars go through `taskAvatarLinkHtml` (the read-only assignee
  name through `taskPersonHtml`); note share and presence avatars go through
  `noteAvatarTagHtml`, which renders the avatar as an `<a>` itself so
  `.notes-face:first-child` still overlaps the faces.
- **Desktop.** `MainWindow::linkAvatarToProfile(widget, account)` gives a
  label a pointing cursor and a click filter that calls `openUserProfile`;
  linking again replaces the old filter. Issue timeline and activity avatars,
  pull and discussion conversation cards (the account is read from the
  header's `forkmesh-user:` link, so a public-key fallback never links), the
  contributor reward recipient and the chat footer's own avatar and name use
  it. The profile card adds the fetched `avatarPng` when no local picture is
  cached, the account's links (`profileLinks`, Mastodon) and a Tips section
  (`chatUserProfileTips`, QR from `QrCode::encodeToImage`, address checked
  with `forkmesh::tips::isValidTipAddress`).
- **Phones.** `ProfileTipContent` (`Features/People/ProfileTips.swift`,
  `feature/people/ProfileOpen.kt`) draws the tip card on the full profile
  screen (`AccountProfileScreen`), the People sheet and, on iOS, the chat
  person sheet (Android's chat opens the full profile directly). The full
  profile screen shows the real avatar, links and Mastodon, the Tips card,
  nodes and the identity QR on both apps. `.opensProfile(name)` (iOS) and
  `Modifier.openProfile(name)` (Android) make a standalone name or avatar open
  `Route.profile` with the `ProfileLinks.account` name check: organization
  and team members, the inbox item's "From", the pull's "opened by" line, and
  on Android review thread authors, office room members and note presence
  avatars; on iOS the thread's root author opens the profile like its
  replies. `AccountProfile` parses `solana`, `profileLinks`, `mastodon` and
  `mastodonUrl` (http and https links only) and `PeopleProfile` keeps a valid
  `tipAddress`.
