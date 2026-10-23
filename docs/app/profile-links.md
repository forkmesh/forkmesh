# Profile links for usernames and mentions

Every place that shows a ForkMesh account name, and every `@name` mention in
text people write, opens that account's profile (`/@name` on the public site).
This page lists the shared helpers per surface and the rules they all follow.

## Rules

- **Which names link.** After a leading `@` is stripped and the name is
  lowercased, it must match the account name pattern
  `^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`. The text keeps the casing it was
  written with ([username-casing.md](username-casing.md)); the link target is
  always lowercase.
- **Which names stay plain.** Bots (`name[bot]`) and system identities (the
  instance's own git node, Actions, runners and mirror nodes), agents and
  model names, Discord senders and fediverse authors (a display name such as
  "Alice" would open the wrong ForkMesh account), organizations where the row
  is about the organization, and anything that is not a valid account name. A
  bot that has its own account page is the exception: it links once the
  lookup confirms the page.
- **Git history names.** A commit author is a Git identity, not an account. It
  links only when the commit-to-account resolution maps it: the author name is
  itself the name of a public user or node account (the desktop commits as
  `user.name = <account>`, mirror intake as `user.name = <node>`). Any other
  identity ("Jane Doe", "ForkMesh Deploy") stays plain text.
- **Where a mention is not a mention.** Inside code spans and fenced code,
  inside existing links and URLs (`https://x.social/@bob`), in emails
  (`bob@example.com`), and before `.x`, `/x`, `@` or `[bot]`.
- **No nested links.** A name that sits inside a row that is already a link or
  a button (issue and pull lists, the home feed, commit rows, autocomplete
  options) stays plain text.

## Web dashboard

`02-helpers.js` and `06-repo-content.js` hold the helpers:

- `issuePersonNameHtml(name, classes, options)` is the one helper every
  renderer uses for a person's name. It renders
  `<a href="/@name" data-issue-person>` (with the display casing from
  `userNameHtml`, and the platform name for the instance's own account). The
  person hydration (`hydrateIssuePersonLink`) asks `/api/accounts/<name>`
  through `issueActorAccount`. The link stays only when the account is
  `linkable`: it exists, the name is taken (`available: false`), the profile is
  not private, and its kind is `user` or `node`. Otherwise the name loses its
  `href`, or points at the repository roster's profile when the roster knows
  it. Options:
  - `{ git: true }` is for Git history names (commit rows, commit detail, pull
    commits, Insights). It starts without an `href`, gains `/@name` only when
    the account is linkable, and never falls back to the roster.
  - `{ inLink: true }` is for a name inside a row that is already a link or a
    button. The name renders as plain text (no link, no lookup), so no `<a>`
    is ever nested.
  Bots and system identities render as plain spans. A bot whose name is a
  valid handle (not Actions, a runner or a node) carries
  `data-issue-bot-account`, and the hydration swaps it for a link when that
  account is linkable. Use the helper for every standalone name.
- `mdMentionHtml(login)` is the markdown mention link; `renderMarkdown` uses it
  for every body (issues, pulls, discussions, advisories, notes).
- `textWithMentionsHtml(text)` escapes plain text and links its mentions; the
  notification detail title and body use it.

The Insights contributor table, the activity views (Pulse lists, the
committer chart, Leaderboard, podium, first contributions, Contributors and
Recent commits) all go through `issuePersonNameHtml` with `git: true`. The
older `linkRepoInsightsContributors` / `resolveContributorProfile` pass, which
rewrote those rows to links after rendering, is gone. Its rule is now
`issueActorAccount`'s `linkable`.

Also routed through the helper: commit authors on the commit detail page and
in a pull request's commit list, the reaction "who" list (agents stay plain),
the owner in a repository card (`repositoryCard`, a `role="link"` card whose
handler skips nested links) and in the fork popover when the fork has no
published profile URL, and the home feed actor when the card has no link of
its own. Names inside link or button rows go through the helper with
`inLink` and stay plain: issue, pull and discussion list rows, commit list
rows, ping rows in the bell menu and the pings window, home feed cards that
are links, and the owner and fork rows of both Top repositories lists.

Names now linked this way: pull and discussion hero lines, the issue hero,
issue assignees, pull reviewers, inline review comment authors, the pull
badge, security advisory reporters, openers, comment and event authors,
credits and collaborators, Actions run actors, organization and team member
lists, note sharing and version authors, note activity, challenge owners and
members, hackathon team members and squad owners, agent creators (when the row
is not itself a link) and Apps installers, developers and requesters
(`appsPersonLink`, DOM-built).

Tasks (`09-tasks.js`) render mentions with `taskMentionLinkHtml`, an
`<a class="task-mention">` that keeps the mention chip style
(`partials/views/tasks.html` and its packaged copy
`marketplace/packages/tasks/content/web/view.html`, which the page is built
from and must stay identical; hover underline) and the `data-me` highlight.
`taskPersonHtml` links creators, assignees, completers, reviewers, reminder
senders and timeline actors.

## Server-rendered pages

`app/host/forkmesh_host/repo_ssr_issue.py` holds `profile_href`,
`person_name_html`, `card_author_html` and `mention_html`. `repo_ssr.py`
(`_inline`, README and other markdown) and `repo_views.py` (`_person`, the
light pull, issue and discussion views; the page frame's owner link goes to
`/@owner`) reuse them. `person_name_html(name, classes, git=True)` mirrors the
dashboard's `git` option: an `<a data-issue-person data-issue-person-git>`
with no `href`, which the dashboard's hydration links once the account is
confirmed. The code page's last-commit summary uses it for the Git author. A
bot name carries `data-issue-bot-account` as it does in the dashboard.

`app/src/hackathon_runtime.py` (`_profile_html`) links the invite page's
inviter, "Built by" members, squad cards, the squad heading and challenge
owners. `www/public/hackathon.js` (`profileLink`) links the leaderboards;
`hackathon.css` (`.board-row .who a`) keeps the board's ink colour and
underlines on hover.

Notification emails need absolute links: `chat_format.to_html(source,
profile_origin)` turns mention nodes into `<a href="{origin}/@name">`
(`PROFILE_ORIGIN` when none is given; a mention inside a link label stays
`<strong>`), and `_notification_actor_html` links the "by" name. The origin is
the one `_notification_item_url` already uses for the item link.

## Chat and the world

Message mentions in `app/public/dashboard-chat.js` already linked through
`mentionProfilePath`. Authors now link too, but only for real accounts: rows
carry the message's `accountKind`, and an author links only when it is
`"user"`, the message is not from Discord, and the name is a valid account name
that is not ForkBot, an agent or a bot. Guests, Discord members and display
names stay plain. The main row, the side rail (`chat-side-message-author`) and
thread cards link, and hovering a linked author shows the same profile card as
a mention. The standalone `/chat` page (`app/public/chat.js`) links the thread
header's sender the way its main row already did.

The world modules cannot reach `worldPublicProfileUrl` (world.js loads them
with `import()` and exports nothing), so each builds the same URL itself:

- `world-floor-notes.js` (`floorNoteProfileUrl`, `noteTextHTML`): mentions are
  `<a class="mention">`, the note author links.
- `world-office-tasks.js` (`officeTaskProfileUrl`, `officeTaskProfileHTML`,
  `mentionHTML`): mentions, reply authors, assignees, bidders and reviewers
  link; `@agent` stays plain. `mentionHTML` used to trim the text around each
  mention ("ping @dana" showed as "ping@dana"); the pieces are no longer
  trimmed.
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
  issue, pull and discussion markdown into `[@name](forkmesh-user:name)`.
- `TaskMarkdown.cpp` links mentions after its link sanitizer runs, so a typed
  `[x](forkmesh-user:…)` link is still dropped while the `@label` itself links
  to its own account.

`openBodyReference` routes `forkmesh-user:` to `MainWindow::openUserProfile`,
which opens the in-app profile card when the chat directory knows the account
and the web profile (`chatMentionProfileUrl`) otherwise. Chat mention clicks go
through the same function. Linked labels: issue and pull timeline headers,
issue activity lines, the issue reporter and assignees, the pull meta line,
pull and discussion conversation cards, the discussion meta line, task
activity, note sharing rows and a package's creator.

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
