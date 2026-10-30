# New issue pings

Every new issue should raise an `issue_opened` ping ("New issue in owner/repo").
`issue_opened` is in `NOTIFICATION_IMMEDIATE_KINDS`, so it is also emailed
straight away, not held for the hourly digest (see
[notification-email](notification-email.md)). Email is on by default.

## Two ways an issue appears

1. **Through the relay inbox.** The web composer, the phone apps, a desktop
   filing on someone else's repo, ForkBot, the Discord bug watcher, apps and
   microjobs all write to `issue_inbox` and call
   `notify_pending_inbox(..., opened=True)`. That pings the hosting account and
   the owners, admins and maintainers of an organization that fronts the repo
   (issue #714). Each of these paths takes its number from the relay's
   `issue_seq` counter (`_forkbot_next_issue_number`).
2. **Directly on a node.** Some issues never touch the inbox: one created in
   the hosting desktop's Issues tab, one filed by an agent or crash-repair
   dispatch on that desktop, or one taken by a mirror's direct intake
   (`tools/mirror_gateway.py` → `server/intake.go`). Until 2026-10-20 these
   pinged nobody, so organization teammates never heard about issues the
   hosting node made itself.

## Catching the second kind: `notify_catalog_issues_opened`

Every catalog publication, from the desktop or from `server/catalog.go`,
carries `issueMaxNumber`. When an accepted update raises it,
`catalog_routes.py` calls `notify_catalog_issues_opened` (in `entry.py`):

- Only numbers at or above `issue_seq.next_number` count. Anything lower was
  handed out by the relay, and the inbox has already pinged it. If the counter
  can't be read, nothing is sent: a missed ping is better than a duplicate
  banner.
- Recipients are the organization owners, admins and maintainers
  (`_org_repo_maintainers`) of the publishing owner and repo, leaving out the
  publishing node. A mirror publishing someone else's repo matches no
  `org_repos` row, so it pings no one. A personal repo with no organization
  pings no one either: on that path the only person is the node's owner, who
  made the issue.
- The actor is the publishing node. The catalog doesn't say who wrote the
  issue, and an inline gateway fetch to find out would hold up the
  publication, so the body reads only `Issue #N` and links to the issue.
- At most `PING_CATALOG_ISSUES_MAX` (5) of the newest numbers are pinged per
  publication. A big import or a renumbering can't flood anyone's phone.
- The dedupe key is `issue-opened:<owner>/<repo>:<N>`. A republish at the same
  maximum sends nothing, because the maximum didn't rise.
- The first publication of a repo, or a previous record with no
  `issueMaxNumber`, sends nothing.

A known gap: if the relay reserves N while the desktop has already made a
local N, the desktop moves the web issue to N+1. The catalog then pings N+1
(already pinged through the inbox as N), and the local N goes unpinged.

Tests: `app/tests/test_catalog_issue_pings.py`.

## Where a ping's card links

`repo_record_href` (`app/src/entry.py`) links a notification to the thing it
is about: one issue or pull request (`/issues/<n>`, `/pull/<n>`) when its
number is known, otherwise a repository section such as `/releases` or
`/commits`, otherwise the repository. Older pings stored only the repository
root although they were about one issue, pull request, release or push; the
home feed (`homeFeedHref` in `05-repo-list-explorer.js`) sends those cards to
that record instead of the repository.
