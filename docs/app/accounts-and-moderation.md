# Accounts and moderation

Part of [the App docs](README.md). Built for forums whose readers may be at
risk, for example one mirrored from GitHub issues, which typically ask for:

- the server should not have to hold identifying data such as email addresses;
- people should not reuse names tied to their other activity, so random,
  persistent names are offered;
- moderators must be able to edit or delete posts and block accounts;
- easy sign-up must not hand the forum to trolls and spam.

Every part is a setting. With the defaults nothing changes: sign-up needs an
email, people choose their usernames, nobody is held for review and new
accounts have no daily limits. Moderators, blocks, hidden posts and locks work
on every server, forkmesh.com included, once somebody uses them.

## The setting

One `instance_settings` row, `k = 'accounts'`, JSON parsed by
`app/src/account_policy.py` (unknown values fall back to the default, numbers
are clamped, documents over 4 KB are ignored). The admin console edits it
under People, **Sign-up & moderation** (`app/src/instance_console.py`,
view `accounts-moderation`); every save is CSRF-checked and audited as
`admin.console_save_accounts` with the fields that changed. Each app process
re-reads the row at most every 30 seconds (`CACHE_MS`), and a failed read keeps
the last value.

| Field | Values | Default | example |
| --- | --- | --- | --- |
| `signup` | `required` (email required), `optional` (email optional), `invite` (invite code only, email optional), `closed` | `required` | `optional` |
| `handles` | `off`, `suggest`, `require` | `off` | `suggest` |
| `accountRequired` | only signed-in accounts may open, comment, vote, claim or react | off | on |
| `firstPostApproval` | hold posts of accounts no moderator has approved yet | off | on |
| `approvalSince` | set when `firstPostApproval` is turned on; accounts created earlier are not held | 0 | (automatic) |
| `newAccountDays` | how long an account counts as new (0 = never) | 7 | 7 |
| `newThreadsPerDay` | threads a new account may open per UTC day (0 = no limit) | 0 | 3 |
| `newCommentsPerDay` | comments a new account may write per UTC day (0 = no limit) | 0 | 20 |
| `hideBlockedPosts` | hide the earlier posts of accounts blocked with "hide their posts" | on | on |
| `inviteSetCode` | internal: `invite` switched the Controls invite code on | | |

`invite` reuses the existing one-time sign-up code (Controls, "Require a
referral code"; `admin_signup_referral_code`): choosing it turns that code on,
leaving it turns the code off again only if `invite` was what turned it on.
While `invite` is chosen and the code is off anyway, sign-up refuses with
`invalid_signup_referral_code`, so a later switch in Controls cannot open
sign-up by accident. Email-required plus the Controls code still gives
invite-only with email.

## Sign-up

`_account_signup` (`app/src/entry.py`) keeps every existing gate (the signed
page view, the email honeypot, Turnstile or its backup puzzle, the per-address
throttle, username moderation) and adds, before any account lookup:

- `closed`: refused with `signup_closed` (403), logged like every other
  blocking refusal (`log_blocked_signup`, [signup-failure-pings](signup-failure-pings.md)).
- `optional`/`invite`: the email field may be empty. A given address is
  validated, indexed and verified exactly as before. Without one the account
  stores `email: ""`, no `email_bi`, and gets a recovery code (below).
- `handles: require`: the name must have the random form
  (`random_handle_required` otherwise).

`/signup` carries the setting in `data-signup-policy` on the form (the
`__FORKMESH_SIGNUP_POLICY__` marker, filled by `_serve_signup_page`), with a
suggested name when `handles` is not `off`. `signup.js` marks the email
optional and explains what that means, shows **Suggest a random name** (which
asks `GET /api/accounts/handle-suggestion`), pre-fills the suggestion, makes
the username read-only under `require`, shows "Sign-up is closed" for `closed`,
and after an email-free sign-up shows the recovery code once, with Copy and an
"I saved my recovery code" box before Continue. An account without an address
goes straight to the dashboard: there is nothing to confirm, so the
unverified-email gate (issue #592,
`app/tests/test_unverified_account_gate.py`) does not apply to it, and `openEmailFreeAccount` is the only place the page
links to the dashboard.

### Random names

`app/src/random_handles.py` builds `adjective-animal-xxxx` names, for example
`quiet-otter-7f3k`, from `app/src/handle_words.json` (about 140 adjectives
and 125 animals, chosen to be neutral: no body parts, insults, colours of skin
or words with a second meaning) and a 4-character suffix from
`abcdefghjkmnpqrstuvwxyz23456789` (no `0/o`, `1/l/i`). Choices use the
`secrets` module. That is about 1.6 * 10^10 names (34 bits), enough that a
name says nothing about its owner and rarely collides; the suggestion endpoint
skips names that are taken (accounts or organizations) or hit the username
filter, up to six tries.

`require` accepts any name of that form whose words are in the lists: the
server cannot prove a name was suggested to this browser, but nobody can pick
a name they use elsewhere. Existing accounts keep their names, but a rename
under `require` must also pick such a name (`random_handle_required`). The
copy on the form says why: a random name keeps this account apart from your other
accounts.

### Recovery codes instead of email

`app/src/account_recovery.py`. A code is `RC-` plus 20 base32 characters in
groups of four (100 random bits). Only `recovery_salt`/`recovery_hash`
(PBKDF2 like passwords, `hash_password`) and `recovery_created_at` are kept,
inside the encrypted account record. Typing is forgiving: case, spaces and
dashes are ignored, `0`, `1` and `8` read as `O`, `I` and `B`.

- `POST /api/accounts/recover` `{nodeName, recoveryCode, newPassword}` sets a
  new password, signs every session out, replaces the code and answers the new
  one once. Wrong codes spend the login attempt budgets
  (`_login_attempt_keys` per source and source+name, plus one per name, all
  blind indexes in `login_attempts`); a locked source is refused before any
  hashing, but the per-name budget never blocks a correct code, so nobody can
  lock an owner out by guessing at their name. The forgot-password page has a
  "No email on your account?" section for it.
- `POST /api/accounts/recovery-code` `{password}` (signed in) makes a new code
  after checking the current password; Settings, Account, **Recovery code**.

### What an account without email can and cannot do

Everything that needs a session works. What depends on a confirmed address
explains itself instead of failing:

- No mail of any kind is sent (every sender already skips an empty or
  unverified address): notification email, digests, password-changed notices,
  forgot-password by email (still answers the usual opaque `ok`).
- The unverified gate (`_account_email_unverified`) only holds accounts that
  have an address to confirm, so email-free accounts are never held.
- Renaming needs a verified address (`email_not_verified`); Settings now says
  "An account without an email keeps its username". That keeps the random name
  persistent.
- Features that are reserved for verified addresses stay reserved: video
  uploads in issues (`repo_media_api.py`, `email_unverified`), contributor
  pushes and web forks (`hosted_git.py`). Their existing messages say to verify
  an address. Adding an email later under Settings unlocks them.
- The "first 100 users" badge counts accounts with an indexed address only.

## Moderators

Who moderates a repository (`moderation.can_moderate_repo`):

- platform administrators (`_is_admin`, which needs the admin flag and 2FA);
- the account that owns the repository's node;
- owners and admins of every organization that links the repository
  (`org_repos`, or the organization the URL names);
- **moderators** of those organizations, and **instance moderators**.

A moderator is not a new organization role. Roles
(`organization_rbac.py`) grant repository permissions, World floors, chat
channels and settings; a moderator is a duty next to whatever role the person
has (a member, or someone who is not in the organization at all), so the RBAC
matrix and its published table are unchanged. Moderators are rows in
`moderation_roles` keyed by scope: the organization's `org_bi`, or the instance
scope `blind_index("moderation-scope:instance")`.

- Organization owners and admins add and remove organization moderators from
  the organization's settings page, section **Moderation**
  (`GET/POST /api/orgs/<org>/moderation`). Moderators see the section too but
  cannot change who moderates.
- Administrators add instance moderators in the admin console (Sign-up &
  moderation), which also blocks accounts on the whole server.

What a moderator can do on the repositories they moderate:

- **Edit and delete any post.** The issue inbox accepts `edit`, `delete`,
  `status`, `title`, `labels`, `milestone`, `dates`, `priority`, `progress`
  and `assignees` events from moderators (`MODERATOR_ISSUE_EVENTS`; never
  `bounty` or `agent`), and discussion `status`/`delete`
  (`MODERATOR_DISCUSSION_EVENTS`). These are the same signed events owners
  send: signed by the moderator's browser key, bound to the moderator's
  account by `_bind_session_author`, written into the repository's issue
  record by the git node, and shown as "<moderator> deleted a comment". The
  dashboard gives moderators the issue manager's controls
  (`issueMutationAuthorized`).
- **Hide and unhide** an issue's opening post or any comment (`hide` flags,
  below), for native and mirrored posts alike.
- **Lock and unlock** an issue thread.
- **Block and unblock** accounts in the organization (or, for instance
  moderators and administrators, on the whole server), optionally hiding their
  earlier posts.
- **Approve or reject** posts waiting for review, and mark an account trusted.

## Blocks

`moderation_blocks` (scope, account, `hide_content`, encrypted
`{name, by, reason, scope}`). A blocked account cannot, on any repository of
that scope:

- open, comment, vote, claim or react through the issue inbox
  (`_accept_issue_submission`);
- post any pull request event or open a pull request (`pulls_handler`, three
  paths);
- post any discussion event (`discussions_handler`);
- add a comment reaction (`comment_reactions._write`; taking one away still
  works).

The refusal is `403 account_blocked` with a plain message. Moderators of the
repository are never blocked by the gate; administrators and your own account
cannot be blocked (`cannot_block`). Blocking also drops the account's posts
waiting for review in that scope. Deleting an account removes its roles, blocks, counters
and queued posts.

Blocks bind accounts. With `accountRequired` off, anybody can still post
without signing in (the web key alone, as before), so a blocked person can log
out and post anonymously: forums that block people should turn
`accountRequired` on. Mirrored GitHub authors (`github:<login>`) are not
accounts and cannot be blocked; hide their posts instead.

## Hidden posts

A hide flag (`moderation_flags`, `kind = 'hide'`, target = the event id, or
`body` for the opening post) and a block with "hide their posts" both hide
text in the same way: the post keeps its place, author and time, and its body
reads *This post was hidden by the moderators.* Edits of a hidden post are
hidden too, and its attachments are dropped. The rule is applied to the issue
record before it is parsed, in three copies that tests keep identical:
`moderation.apply_to_record` (server), `repo_moderation.apply` (host first
view) and `applyIssueModeration` (dashboard, `09-moderation.js`;
`test_the_dashboard_hides_exactly_what_the_server_hides` runs it in node
against the server's output).

- `GET /api/repo/<o>/<r>/moderation?kind=issue&number=N` answers `hidden`,
  `hiddenAuthors` (blocked accounts whose posts are hidden), `locked`,
  `placeholder`, and for a signed-in viewer `canModerate` and `pendingMine`.
  Anonymous reads of public repositories are cached publicly for 5 seconds.
  The list of hidden authors is public, like the posts it hides.
- The dashboard waits up to 2 s for it beside the record read. Moderators get
  Hide/Unhide on every post, Lock/Unlock in the sidebar, and a "Show the
  hidden text (moderators only)" reveal under hidden posts.
- The first view (`app/host/forkmesh_host/repo_moderation.py`) reads the same
  answer anonymously beside the Worker request, like reactions (fresh 5 s,
  stale up to 5 min, 150 ms budget, forgotten on a write through this node),
  applies it before rendering and keys the render cache by it. When the state
  hides something and the full issue view cannot be drawn, the host serves the
  plain page instead of the fallback view. If the read times out with nothing
  remembered, the first paint shows the record as stored and the dashboard
  hides the posts a moment later.

Hiding is presentation, not deletion: the text stays in the repository's
issue record, in clones and in the raw blob API. Use a signed delete for a
native post that must go (it is still in git history, as every delete is).

## Locked threads

`moderation_flags` `kind = 'lock'`. A comment on a locked issue is refused
with `403 thread_locked` unless the author moderates the repository. The
dashboard replaces the comment box with "This thread is locked. New comments
are turned off." (moderators keep the box under a notice). Locks and hides
apply to issues; discussions and pull requests have blocks and the existing
close/archive states but no hide or lock yet.

## New accounts

### First-post approval

With `firstPostApproval` on, an `open` or `comment` on an issue from an
account that needs approval goes to `moderation_queue` instead of the issue
inbox (`moderation.hold`), and the author gets `202 {"pending": "review"}`;
the composer says "Sent to the moderators. It appears once a moderator
approves it." and the issue page tells the author how many posts wait. An
account needs approval until an admin or instance moderator approves one of
its posts (or marks it trusted), which stamps `moderation_trusted_at` on the
account record (`moderation.can_trust`). The stamp is server-wide, so an
organization's own moderators can approve a post into their repositories but
do not trust the author anywhere else, and `trust` from an organization scope
answers 403: anyone can create an organization, and before 2026-10-24 a
spammer could make one and trust their own accounts everywhere.
Accounts created before the setting was turned on (`approvalSince`) never
need it, so switching it on does not hold the existing community. Anonymous
posts (when `accountRequired` is off) are always held while approval is on.
Votes, claims and reactions are not held; neither are discussions and pulls.

A held post is not numbered, notified, federated or mirrored: none of that
runs until approval. Approving replays the stored, signed submission through
`_accept_issue_submission` (`_moderation_commit_issue`): the signature is
checked again against the repository the post was written for, a new thread
gets its number then, the author's name is the one bound when it was written
(not the moderator's), and `wantsAgent` is dropped. A queue row is claimed
before it is replayed (`claimed_at`, released after 5 minutes or on failure),
so two moderators cannot approve the same post twice, and a refusal (inbox
full, number unavailable) leaves it waiting. The queue holds at most 2000
posts per organization and 20 per author (`review_queue_full`). Posts on
repositories outside any organization wait in the instance queue, shown in
the admin console (People → Sign-up and moderation → Held posts) with
Approve and Reject.

### Daily limits

For `newAccountDays` after `created_at`, an account may open
`newThreadsPerDay` threads and write `newCommentsPerDay` comments a day on
issues; the next one is refused with `429 new_account_limit`. Counters live in
`moderation_post_counts` (account blind index, UTC day number, kind, count),
older days are deleted on the account's next post, and the table is emptied by
a re-key. Nothing about the request (no address, no browser detail) is stored
or used: the limit binds the account, and the existing per-address sign-up
throttle limits how many accounts one network can make.

## Mirrored GitHub content

example/forum is filled by `tools/forge_sync.py` in mirror mode
([forge-sync](../../www/docs/forge-sync.md)): GitHub wins every field, a
ForkMesh change to a mirrored title, body, state or comment is replaced on the
next pass, and authors are `github:<login>`, not accounts. So:

- a signed edit or delete by a moderator on a mirrored post would be undone by
  the next sync; moderators **hide** mirrored posts instead. The flag lives in
  this server's database only, the record and the mirror are untouched, and
  the next sync cannot bring the text back on screen;
- blocks and limits do not apply to `github:` authors; content on GitHub is
  moderated by the GitHub repository's maintainers;
- native threads and comments written here are ForkMesh-only records the sync
  leaves alone, so signed edits and deletes on them stay.

## Storage and privacy

Migration `0292_accounts_moderation` (written as 0290 and renumbered when it
merged after main's `0290_pull_ai_reviews` and `0291_repo_mirror_quota`; the
newest migration must restamp the schema fingerprint, `test_local_migrations`):

| Table | Keys (blind indexes) | Payload (encrypted) |
| --- | --- | --- |
| `moderation_roles` | `scope_bi`, `account_bi` | `{name, by, scope}` |
| `moderation_blocks` | `scope_bi`, `account_bi`, plain `hide_content` | `{name, by, reason, scope}` |
| `moderation_flags` | `repo_bi`, plain `subject`, `target`, `kind` | `{repo, by, reason}` |
| `moderation_queue` | `repo_bi`, `scope_bi`, `author_bi`, plain `subject`, `kind`, `claimed_at` | the held submission |
| `moderation_post_counts` | `account_bi`, plain `day`, `kind`, `count` | none |
| `moderation_log` | `scope_bi` | `{scope, actor, action, target, detail}` |

No table has an address or browser column. Every payload carries the
plaintext its blind indexes were made from (`scope` as `org:<name>` or
`moderation-scope:instance`, `repo` as `<owner>/<repo>`, the account name), so
`tools/next_rekey.py` recomputes them ([REKEY.md](../REKEY.md)); the counters
are emptied instead. A repository delete purges flags and queue
(`_REPO_SCOPED_TABLES`); an account delete purges roles, blocks, counters and
queued posts.

Every moderation action writes a `moderation_log` row (shown on the
organization page and in the console) and a `sensitive_audit_log` row
(`moderation.<action>`). Neither records an address.

Out of scope here: the sign-up record still keeps the bounded `signup`
evidence (address and user agent, encrypted in `users.data`) that referral
enforcement reads, and `users.ip_bi`; privacy mode for logging and addresses
is a separate change.

## API

| Route | Who | What |
| --- | --- | --- |
| `GET /api/accounts/handle-suggestion` | anyone | `{handle}` |
| `POST /api/accounts/recover` | anyone | `{nodeName, recoveryCode, newPassword}` → `{recoveryCode}` |
| `POST /api/accounts/recovery-code` | signed in | `{password}` → `{recoveryCode}` |
| `GET /api/repo/<o>/<r>/moderation?kind=issue&number=N` | readers | state of one issue |
| `POST /api/repo/<o>/<r>/moderation` | moderators | `{action: hide\|unhide\|lock\|unlock, kind, number, target, reason}` |
| `GET /api/orgs/<org>/moderation` | org moderators | queue, moderators, blocked, log, policy summary |
| `POST /api/orgs/<org>/moderation` | org moderators (moderator changes: owners/admins) | `{action: add_moderator\|remove_moderator\|block\|unblock\|trust, name, reason, hideContent}` or `{action: approve\|reject, id}` |

Refusal codes: `account_required` (401), `account_blocked`, `thread_locked`
(403), `new_account_limit`, `review_queue_full` (429), each with a `message`.

A rename re-keys the account's moderator roles, blocks, new-account post
counts and held posts to the new name (`_rename_account_namespace`), so a
block does not end with a rename and whoever takes the old name inherits no
role. A recovery attempt for a name without a code still spends one PBKDF2
hash, so the answer time does not say which names have one.

## Known limits

- Mirror apps on other servers do not see this server's moderation database;
  on a single-node instance every web submission reaches this one.
- Discussions and pull requests: blocks only (no hide, lock, queue or limits).
- The issue list and search still show the titles of threads whose opening
  post is hidden; delete a native thread to remove it from lists.
- The dashboard shows moderators the label, milestone and assignee pickers
  because the server accepts those events from them.

## Code map

| Path | Role |
| --- | --- |
| `app/src/account_policy.py` | the setting: parse, defaults, form, approval and limit rules |
| `app/src/random_handles.py`, `app/src/handle_words.json` | random names |
| `app/src/account_recovery.py` | recovery codes, recover and regenerate routes, name suggestions |
| `app/src/moderation.py` | roles, blocks, flags, queue, counters, log, the submission gate, both APIs |
| `app/src/instance_console.py` | admin console view `accounts-moderation` |
| `app/src/entry.py` | sign-up, `_accept_issue_submission` gate and hold, pulls and discussions gates, `_moderation_commit_issue`, routes, purge |
| `app/host/forkmesh_host/repo_moderation.py`, `repo_ssr.py` | first view |
| `app/public/signup.*`, `forgot-password.*` | sign-up and recovery pages |
| `app/public/dashboard/js/09-moderation.js` | issue page and organization page |

Tests: `app/tests/test_accounts_moderation.py`,
`app/tests/host/test_repo_moderation_ssr.py`, and the updated contracts in
`test_unverified_account_gate.py`, `test_auth_pages_frontend.py`,
`test_error_group_notifications.py`, `test_account_profile_endpoints.py`.
