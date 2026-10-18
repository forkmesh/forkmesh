# Username capitalization

An account name is one identity in every capitalization: `AliceSmith`,
`alicesmith` and `ALICESMITH` sign in to the same account, open the same
`/@alicesmith` profile, and can never be registered twice. The stored name,
the blind indexes (`blind_index(name)`), the `users.username` column,
organization roles, permissions, routing and every comparison stay
lowercase, as before.

What changes is the text people read. An account may keep the capitalization
its owner chose, so the dashboard shows `AliceSmith` where it used to show
`alicesmith`.

## Where the casing is stored

`account_names.py` (`app/src`) holds the rules. The encrypted account record
gets an optional `nameCase` field. It is only ever a case variant of the
record's `name` (`clean_case`); anything else is refused with
`bad_name_case`. Equal to the lowercase name, the field is removed instead of
stored. A record without it, or whose `nameCase` no longer matches its name
after a rename, falls back to the stored name (`display_name`).

- **Signup** (`_account_signup`) keeps the capitalization typed into the
  name field. The name itself is still lowercased before any check.
- **Settings → Public profile → Name capitalization**
  (`data-profile-page-name-case`) sends `nameCase` to
  `POST /api/accounts/profile`. Accounts created before this change start
  without a casing and show their stored name until their owner sets one.

## Where it is returned

| Answer | Field |
| --- | --- |
| session, login and profile saves (`_account_public_payload`) | `nameCase` |
| `GET /api/accounts/<name>` (any capitalization in the path) | `nameCase` |
| `GET /api/orgs/<org>/members` (roster readers) | `nameCase` per member |
| `GET /api/accounts/name-case?names=a,b,…` | `{"names": {"a": "A"}}` |
| the `/@name` page title | `@AliceSmith · …` |

`/api/accounts/name-case` takes up to 50 names, lowercases and validates
them, and answers only for active accounts with a public profile (a private
profile keeps its casing to itself). Names that are not accounts, such as
Git author names, are simply missing from the answer. Each answer is kept
for five minutes per process (`account_names.cached`); a profile save clears
the account's entry. The route only answers when `names` is in the query, so
a lookup of an account literally named `name-case` still works.

## In the dashboard

`02-helpers.js` keeps one map of known casings (`state.userNameCases`):

- `rememberUserNameCase(name, nameCase)` records a casing from any answer
  that carries one (`sessionFromAccountPayload`, the organization roster);
- `userDisplayName(name)` returns the known casing (keeping a leading `@`)
  or the name as given;
- `userNameHtml(name)` renders a person's name as
  `<span data-user-name="alicesmith">…</span>`;
- `watchUserNames` (started at boot) watches the page for such spans, asks
  `/api/accounts/name-case` once for names it has not seen, and rewrites the
  span text. A span whose text was changed by something else is left alone.

The header account menu, the sidebar user name, the profile page (own and
public), organization and team rosters, issue, pull request and discussion
authors, comments and review threads, the repository Insights contributor
list, the activity feed actor, note activity, referral lists and the Actions
run actor use these helpers. Profile links keep the lowercase
`/@alicesmith`; every other capitalization of the address resolves to the
same page.

Not covered yet: the network page's contributor leaderboard (it reads
`users.username` in SQL), the World and chat rosters, and mentions inside
Markdown, which keep the capitalization the author typed.
