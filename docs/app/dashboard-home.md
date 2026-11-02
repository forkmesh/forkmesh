# Dashboard home

Part of [the App docs](README.md). The home page (`/dashboard`) is laid out
after GitHub's dashboard (November 2026 redesign): three columns, short rows,
and almost no description text. Markup and its scoped styles are in
`app/public/dashboard/partials/views/home.html`; rendering is in
`app/public/dashboard/js/05-repo-list-explorer.js`.

## Left rail

- The signed-in account (avatar, name, "Your personal account"), linking to
  the profile.
- **Home** and **Feed** switch the centre column. Feed is the activity from
  other people (mentions, pull requests, pushes) that used to sit under the
  repositories.
- **Top repositories**: one line per repository, logo and `owner/name`. The
  description, views and fork count moved into the row's tooltip. A small
  dot shows whether the repository is served (green) or offline; it is left
  out on instances, like before. The search button appears at ten
  repositories and opens the finder.

## Centre column

- A time-of-day greeting with the account name, and no icon.
- The prompt composer (`dashboard-composer.js`) in its inline form: the input
  card sits on a tray that holds the repository picker, the cloud and pull
  request toggles, and clear/expand. The model and mode choices sit next to
  the plus (attach) button, and the slash button is hidden (typing `/` still
  opens the menu). Clicking into it draws no outline or coloured border:
  the caret is enough, and the ring made the calm card look busy.
- **Agent sessions**: the five newest, or "No agent sessions."
- **Pull requests** and **Issues**: the open ones in the account's own public
  repositories (and its organizations' linked aliases), newest first, five
  each. Each section only appears when it has something to show. There is no
  account-wide API for these, so the page reads at most six repositories,
  using the same reads as the repository tabs (the `forkmesh/pulls` ledger and
  `issue-index`), and skips a repository whose catalog row says it has none
  open.

## Right rail

- **Latest from our changelog**: the four newest notes from
  `/changelog.json`, the file every site serves from
  `changelog/changelog.json`.
- **Latest from the blog**: the three newest posts (or the instance's dev.to
  posts), as titles only. The post artwork was dropped to keep the rail
  minimal, so the image-host allowlist it needed is gone too.
