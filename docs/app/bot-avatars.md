# Avatars for bots and system accounts

Bots and system identities get a built-in glyph instead of a blank circle or
an initial, wherever avatars render: issue and pull request timelines and
comments, assignees, reactions, the AI review card, contributor avatars and
insights, the activity page, the latest-commit row on the code page,
notifications, Actions runs ("Triggered by" and each run row), and the
profile page.

## Kinds

`botAvatarKind(name, email)` (`app/public/dashboard/js/02-helpers.js`),
`bot_avatar_kind` (`app/src/bot_avatars.py`) and the server-rendered issue
page (`app/host/forkmesh_host/repo_ssr_issue.py`) use the same rules, first
match wins:

| Kind | Glyph | Names |
| --- | --- | --- |
| `ai` | sparkles | `AI Review`, `ForkBot`, `AI agent`; agent sessions in the issue sidebar |
| `runner` | server rack | `<anything>-runner`, `<anything> runner` |
| `node` | git graph | the instance's git node account and its platform name (`ForkMesh`, `<platform> mirror`, `<platform> mirror reconciler`) |
| `actions` | gear | `github-actions`, `<owner>-actions` (the instance's Actions bot, also with `[bot]`), `<platform> Actions`, `ForkMesh Deploy`, commit email `actions@forkmesh.local` / `deploy@forkmesh.local` |
| `bot` | robot | `<name>[bot]`, `dependabot`, `renovate`, names ending in `-bot` / ` bot`, a `[bot]@` commit email |

The dashboard also knows the instance's platform name and git node
(`platformName`, `platformAccount`), which the server does not.

The glyphs are 24x24 stroke drawings in `currentColor`, drawn in the
`muted-foreground` role on the usual `bg-secondary` circle, so they follow the
light and dark themes. The same glyph strings live in the three places above;
`app/tests/test_bot_avatars.py` keeps them identical.

## Override

A bot or system name that is a valid account name (for example an
organization's `<owner>-actions` account) is still looked up like a person
(`data-issue-actor-avatar`, `/api/accounts/<name>`). If that account has
uploaded an avatar in its account settings, the upload is drawn over the
glyph. Names that cannot be accounts (`tasks[bot]`) always show the glyph.

## Server endpoint

`GET /api/accounts/<name>/avatar` (`_account_avatar` in
`app/src/account_users.py`) answers a bot or system name that has no account,
or no uploaded picture, with the glyph as a self-contained SVG
(`bot_avatar_svg`: a grey circle with a white glyph, `image/svg+xml`,
`nosniff`, `default-src 'none'`) instead of a 404, for pages and clients that
use plain `<img>` tags. An uploaded picture still wins; other names without a
picture still get 404.
