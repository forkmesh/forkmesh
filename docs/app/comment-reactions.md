# Comment reactions

Every comment-like thing on a repository page can take emoji reactions:

- an issue's description and each issue comment;
- a pull request's description, its conversation comments, review summaries
  with text, inline diff comments (`line-comment`, `thread-comment`) and
  thread replies, shown on the same comment in the Conversation and Files
  changed tabs;
- a discussion's opening post and each reply.

Commit pages have no comments on the web, so they have no reactions. Security
advisory comments have no stable comment id and are not covered. Chat already
has its own reactions (sealed, sent over the relay, `docs/app/chat-stickers.md`);
comment reactions reuse its vocabulary instead of adding a second one.

## Vocabulary

The 18 reactions of the desktop's `ReactionCatalog.h` (`like`, `love`,
`laugh`, `celebrate`, `hot`, `thanks`, `smile`, `wink`, `cool`, `thinking`,
`surprised`, `sad`, `angry`, `dislike`, `eyes`, `star`, `rocket`, `check`),
stored as slugs. The picker's quick row is the chat's six (`like`, `love`,
`laugh`, `celebrate`, `eyes`, `rocket`); the rest are below it and searchable
by name and keywords ("party" finds Celebrate). The server, the dashboard
(`COMMENT_REACTIONS`, `ISSUE_REACTION_EMOJI`), the server-rendered first view
(`REACTION_EMOJI`, `REACTION_LABELS`) and the desktop catalog are pinned to
one list by `app/tests/test_comment_reactions.py`.

## Storage

Comments live in the repository's signed ledger (`.forkmesh/issues`,
`.forkmesh/pulls/<n>/NNNN-*.md`, `.forkmesh/discussions/<n>/NNNN-comment.md`),
written by the owner's node from signed events that arrive through the
inboxes. The issue ledger already has a signed `reaction` event, which the
desktop writes and every view draws. It is not used for web reactions:

- a web event goes through the maintainer's inbox and appears only after the
  owner's node drains it, so a click would take minutes to show and fill the
  inbox with one-tap events;
- pull requests and discussions have no reaction event at all;
- a reaction is a preference, like a star, not part of the record.

So web reactions live in the App database, next to `repo_stars`:

```
comment_reactions (repo_bi, subject, comment, emoji, account_bi, created_at, data)
  PRIMARY KEY (repo_bi, subject, comment, emoji, account_bi)
comment_reaction_rate (account_bi PRIMARY KEY, window_start, count)
```

- `repo_bi` is the repository's blind index (the same key as every other
  repository table, from `_repository_access_context`), `account_bi` the
  reacting account's. `subject` is `issue:<n>`, `pull:<n>` or
  `discussion:<n>`; `comment` is the comment's event id, or `body` for the
  opening post. `data` is the encrypted `{name, agent}` of the reactor.
- The primary key makes a reaction unique per account, comment and emoji and
  is the index of the page read (one range scan per record).
- `idx_comment_reactions_account (account_bi, created_at)` serves the daily
  cap and account deletion.
- Deleting the repository (`_REPO_SCOPED_TABLES`) or the account
  (`_delete_account_namespace`) deletes its rows. `docs/REKEY.md` lists both
  tables (the rate counter is emptied on a re-key).

The `comment_reactions` migration (`app/migrations/*_comment_reactions.sql`)
creates both tables and stamps the schema fingerprint.

The issue view merges both sources: ledger reactions folded as before
(`foldIssueReactions`, deletes retract them) and the stored ones, counting a
name once per emoji. A ledger reaction cannot be taken back from the web; the
desktop that signed it does that.

## API

`GET /api/repo/<owner>/<repo>/reactions?kind=issue|pull|discussion&number=<n>`
returns every comment's tallies for one record in one indexed read:

```json
{"ok": true, "kind": "issue", "number": 7, "signedIn": true,
 "reactions": {"body": [{"emoji": "rocket", "count": 3, "mine": true,
   "people": [{"name": "alice", "agent": false}]}]}}
```

Tallies are ordered by their first reaction; `people` holds the first ten
reactors (only those rows are decrypted). Anonymous reads of a public
repository are `public, max-age=5, stale-while-revalidate=30`; a signed-in
read (it carries `mine`) or any private repository read is
`private, no-store`. Both send `Vary: authorization, cookie`.

`POST` with `{kind, number, comment, emoji, on, author?}` sets one reaction on
(`on: true`) or off (`on: false`). It is explicit, not a toggle, so a double
click or a retried request cannot flip it back. The answer carries the
comment's new tallies.

Checks, in order:

1. A session (`_account_session_record`): a body `sessionToken`, a bearer
   token, or the session cookie with an allowed `Origin` (the CSRF guard every
   cookie mutation has). No session: 401.
2. `kind`, `number` (1 to 10^9), `comment` (`[A-Za-z0-9][A-Za-z0-9._:@+-]{0,159}`),
   `emoji` (a catalog slug, never free text) and a boolean `on`: else 400.
3. Read access to the repository (`_repository_access_context` with the
   account as viewer): a private repository the account cannot read answers
   404, the same as a missing one. Reads apply the same check, so a private
   repository's reactions are invisible to anyone who cannot read it.
4. The record type's feature switch (issues, pull requests, discussions): 403
   when it is off.
5. 60 writes a minute per account (`comment_reaction_rate`): 429 with
   `Retry-After`.
6. For a new reaction: at most 8 reactions per account on one comment (409
   `too_many_reactions`), 200 per account on one record (409
   `too_many_reactions`), 5000 per record (409 `record_full`) and 1000 new
   reactions per account per day (429 `daily_limit`). Together with the
   per-account uniqueness these bound the table.

The comment id is not checked against the ledger (the App does not read
repository content); the caps bound what a made-up id can cost. The
per-account cap on one record means a single account cannot fill a real
record's 5000 rows with invented ids and lock everyone else out.

## Notifications

Adding a reaction pings the comment's author with the `reaction` kind
("Reactions to your comments", Conversations group, off by email by
default, mutable like every kind). The ping is collapsed per comment: its
dedupe key is the repository, record and comment, so every later reaction
rewrites the same inbox row ("dave, carol and 1 other reacted to your comment
on alice/app#7", body "🎉 2 👍 1") instead of adding one, and an already-read
row is not marked unread again. It never rings a phone (`push=False`). No
ping for reacting to your own comment, for taking a reaction back, for a name
that is not an account name, or, on a private repository, for an author who
cannot read it.

The author comes from the dashboard (the loaded record's `authorName` for the
comment), because the App cannot read the ledger. Naming someone else only
files that collapsed ping for that comment, the same reach an @mention has.
Because a made-up comment id gets its own dedupe key, a reaction only pings
while the reacting account has reacted to at most 30 distinct comments in
the last 24 hours (`MAX_PINGED_COMMENTS_PER_DAY`, counted across every
repository from the account's own rows). Past that the reaction is still
stored and shown, but nobody is pinged, so one account cannot flood someone's
inbox by inventing ids.

## Agents

A reaction made with an app token (`fmat_…`) is stored as an agent's. The
chip gets a small accent dot, the "who reacted" card marks the person with an
"agent" badge and a bot avatar, and the accessible label says "(agent)".

## First paint

Issue pages are server-rendered in the dashboard's own markup
(`repo_ssr_issue`), so reaction bars are drawn there with their counts and
the dashboard's render of the same data is byte-identical
(`app/tests/host/test_repo_ssr_issue.py` runs both). Pull request and
discussion first views (`repo_views`) draw the same bars under each comment.

`repo_ssr.fetch` starts the record's reaction read beside the Worker's page
request: an anonymous in-process `GET …/reactions` (no cookie, no
authorization), remembered per record for 5 seconds and served stale for up
to 5 minutes while a refresh runs. `finish` waits at most 150 ms for it. A
write through the node drops the repository's remembered summaries. The page
also carries the summary as `#forkmesh-comment-reactions` JSON
(`{base, subject, reactions}`), which the dashboard takes at boot, so its
first render shows the same counts and nothing moves. The dashboard then
makes its one read for the page (signed in, to learn `mine`) and repaints
only the bars.

Rendered issue articles and first views are cached per bundle and reaction
summary (at most 8 variants a bundle).

## Interface

- One tap or click toggles a chip; the change shows at once and is rolled
  back with a short notice when the server refuses it.
- The smile-plus button opens the picker: search box, quick row, the rest of
  the catalog, the hovered reaction's name under the grid. Arrow keys, Home,
  End and Enter move and pick; Escape closes and returns focus. Under 640 px
  it opens as a bottom sheet with bigger targets and focuses the first
  reaction instead of the search box, so the keyboard does not cover it.
- Hovering a chip (300 ms), focusing it from the keyboard, or long-pressing it
  on a touch screen (450 ms; the tap that follows is swallowed and the
  context menu suppressed) shows who reacted: a small avatar stack, up to
  eight names, "and N more".
- Adding a reaction pops the chip and bursts six small copies of the emoji.
  The first Celebrate on a comment also throws confetti in the theme's
  colours, once per comment per page. With `prefers-reduced-motion: reduce`
  none of this runs (checked in script, and hidden in CSS).
- Signed out, chips and the picker show "Sign in to react" with a link back
  to the page.
- Selection uses the accent colour, never green (`docs/app/dashboard-colors.md`).

Code: `app/src/comment_reactions.py` (API),
`app/public/dashboard/js/06-repo-content.js` (bar rendering, shared with the
first-view parity test), `app/public/dashboard/js/09-comment-reactions.js`
(loading, picker, toggles, effects), `app/public/dashboard/shell.html`
(`.fm-reaction*` styles), `app/host/forkmesh_host/repo_ssr.py`,
`repo_ssr_issue.py`, `repo_views.py` (first view).
