# Forum reach: many doors to one forum

A forum that lives in a ForkMesh repository (its threads are the issue
records in `.forkmesh/issues`) should stay readable when a censor blocks one
domain name or one IP address. A forum that plans a move off GitHub will
typically ask for exactly that:
blocking a domain name or IP address must not be enough to block the
platform, a GitHub Pages mirror would give a somewhat censorship-resistant
read-only view, and pages must stay light enough for DNS tunnels and work
without JavaScript.

No single trick does this, so an instance opens several doors at once:

| Door | Read | Write | How |
| --- | --- | --- | --- |
| The instance domain | yes | yes | the normal site |
| The provider the forum mirrors (GitHub) | yes | yes | kept by forge sync ([provider-mirrors.md](provider-mirrors.md)) |
| A Tor onion address | yes | yes | `onionService: true` ([INSTANCES.md](../INSTANCES.md#tor-onion-service)) |
| Static copies on GitHub Pages, Codeberg Pages, S3, IPFS, a USB stick | yes | no | `tools/forum_static_export.py` + `tools/forum_static_publish.py` (below) |
| `git clone` of the repository or of a pages branch | yes, offline | no | any git host that carries it |
| Mirror nodes that carry the repository | yes | no | the mirror quota panel sets a goal of how many mirrors a repository wants (`mirror-quota.md`, branch `example/mirror-quota`) |

Each door has different blocking costs for a censor: a domain, an IP, all
of github.io or codeberg.page, Tor itself, and every copy anyone hosts. A
reader only needs one to work.

## Static read-only export (`tools/forum_static_export.py`)

```
python3 tools/forum_static_export.py --source <checkout> --out <site> \
  --source-url https://<instance>/<owner>/<repo> --site-id <owner>/<repo>
python3 tools/forum_static_export.py --source <bare.git> --ref master --out <site> ...
python3 tools/forum_static_export.py --verify --out <site>
```

It reads a checkout, or with `--ref` a commit of a bare repository (through
`git ls-tree` and one `git cat-file --batch`, so the server's bare mirror
needs no work tree), and writes:

| Path | What |
| --- | --- |
| `index.html` | the repository README, the 30 latest threads (`--latest`), every label with its thread count |
| `all.html` | every thread, newest activity first; the browser's find (Ctrl+F) is the search |
| `issues/<n>.html` | one whole thread: the opening post and every comment, no paging |
| `labels/<slug>.html` | the threads with one label |
| `feed.atom` | Atom 1.0, the newest 100 posts (`--feed-size`); comments carry RFC 4685 `thr:in-reply-to` pointing at their thread, so rss2email threads them |
| `files/<sha256>.<ext>` | mirrored attachments that a page uses (copied from `.forkmesh/attachments/`) |
| `avatars/<provider>-<login>.png` | mirrored avatars that a page uses (`.forkmesh/contributors/`, `--no-avatars` leaves them out) |
| `repo/<path>` | committed repository files a page shows (a logo); see below |
| `style.css` | one shared stylesheet, about 1.5 KB, light and dark |
| `manifest.json` | SHA-256 and size of every file, the source commit and URL, and the input hashes used for incremental runs |
| `.nojekyll` | GitHub Pages serves the folder as is |

Choices:

- **No JavaScript and relative links only**, so the same folder works on
  GitHub Pages, Codeberg Pages, S3, IPFS, a plain web server under any path,
  or opened from a USB stick. Thread text gets `dir="auto"`, so Persian and
  Arabic posts read right to left.
- **Never an image from GitHub.** An image the instance has mirrored is
  copied and linked as `files/<sha>.<ext>`; one it has not becomes a text
  link `[image: alt]` to the original address. Opening it is the reader's
  choice, and the page loads nothing from a third party. Links to other
  threads go to `issues/<n>.html` when that thread is in the copy, and to
  the live forum (`--source-url`) otherwise; `@mentions` and repository
  links go to the live forum, or become plain text without `--source-url`.
- **Markdown** goes through the server renderer
  (`app/host/forkmesh_host/repo_ssr_issue.py`, `render_markdown`), the same
  escaping and the same HTML-to-Markdown flattening as the instance, then
  the export drops its Tailwind classes, `data-*` attributes and icons, so
  pages stay HN-sized. When `app/host` is not next to the tool, a small
  bundled renderer escapes everything and handles paragraphs, headings,
  lists, quotes, code, links and images.
- **The fold** follows the instance (`_issue_index_fold` in
  `tools/mirror_gateway.py` and forge sync's `fold`): the latest `title`,
  `status` and `labels` events win over the record's fields, `edit` events
  replace bodies, `delete` events hide comments, and a thread its author
  deleted is left out.
- **Deterministic.** Every timestamp comes from the records, files and
  lists are sorted, and the tarball (`--tarball`) has zero mtimes, so an
  unchanged forum gives byte-identical output; anyone can re-run the export
  from the same commit and compare with `manifest.json`.
- **Incremental.** A thread page is rendered again only when its input hash
  changes: the record bytes, the export settings, the attachments and
  avatars it uses, and which `#n` it mentions exist. Unchanged pages and
  attachments are not rewritten, and files of removed threads or labels are
  deleted (only paths the previous manifest lists).
- **No search index.** A JavaScript search would break the no-JS rule and
  an index of every word would be several MB; `all.html` lists every title
  for find-in-page instead, and full-text search stays on the live forum.

### Measured size

A synthetic forum of 600 threads and 6,000 comments (2-6 paragraphs per
post, code blocks, quotes, lists, links; 6.2 MB of issue JSON):

| | Bytes |
| --- | --- |
| Whole site, 614 files | 8.4 MB (1.8 MB gzipped, 1.1 MB as one `.tar.gz`) |
| Average thread page (11 posts) | 13 KB (about 2.6 KB gzipped) |
| `index.html` / `all.html` | 7 KB / 117 KB |
| `feed.atom` (100 entries, full text) | 142 KB |
| `manifest.json` | 131 KB |
| `style.css` | 1.5 KB |

A full export takes 3 s; a re-run with nothing changed takes 0.3 s and
writes nothing. Markup is about 250 bytes per post over its text.

A post or the README can point at a repository file with a relative path,
and the export copies that file into `repo/`. Only a file the source commit
holds is copied: `CheckoutSource.published` takes paths from
`git ls-files -s` (regular files, mode 100644 or 100755) and refuses a
symlink or anything that resolves outside the checkout; `GitSource`
refuses symlink blobs. Before 2026-10-24 the checkout mode copied any file
on disk, so a comment with `![x](.env)` published an untracked secret, or
the file behind a symlink, to every static copy.

## Publishing (`tools/forum_static_publish.py`)

```
python3 tools/forum_static_publish.py --site <site> --targets targets.json [--dry-run]
python3 tools/forum_static_publish.py --site <site> --target pages=https://codeberg.org/<org>/pages.git#main
```

For each target it keeps a bare copy under `--cache` (default
`<site>/../.publish-cache`), fetches the target branch, builds a tree of
exactly the files `manifest.json` lists, and commits it **on top of** the
fetched tip. It pushes `<commit>:refs/heads/<branch>` without force; if the
branch moved in between, the push is refused, the run reports it, and the
next run commits on top of the new tip. An unchanged site is
`up-to-date` and pushes nothing. History keeps every published version.
`--dry-run` builds the commit and reports the changed file count, but
pushes nothing.

The new tree replaces the old one, so a publish refuses a branch whose tip
has no `manifest.json` of this export (`published_before`): pointing a
target at the code branch of a repository would otherwise replace the code
at its tip. `--init` publishes onto such a branch once. `CNAME` and
`.domains` (the custom domain of GitHub Pages and Codeberg Pages) are
carried over from the tip, and a target URL or branch that starts with `-`
is refused before git sees it.

`targets.json`:

```json
{"targets": [
  {"name": "github", "url": "https://github.com/example/forum-mirror.git", "branch": "gh-pages",
   "tokenFile": "/etc/forum-static/github.token"},
  {"name": "codeberg", "url": "https://codeberg.org/example/pages.git", "branch": "main",
   "tokenFile": "/etc/forum-static/codeberg.token", "username": "example-bot"},
  {"name": "backup", "url": "git@example.org:mirrors/forum.git", "branch": "pages",
   "sshKey": "/etc/forum-static/deploy_ed25519", "knownHosts": "/etc/forum-static/known_hosts"}
]}
```

Credentials never appear on a command line or in a URL (a URL with a
password is refused): a token comes from the environment variable
`FORKMESH_PUBLISH_TOKEN_<NAME>` (or the target's `tokenEnv`) or from
`tokenFile`, and reaches git as an `http.<origin>/.extraHeader` Basic
header in `GIT_CONFIG_KEY_n`/`GIT_CONFIG_VALUE_n` environment variables,
with `credential.helper` cleared and prompts off. The user name defaults to
`x-access-token` (GitHub); Codeberg wants the account name. SSH targets use
`sshKey` through `GIT_SSH_COMMAND`. Error messages have the token masked.

### GitHub Pages

1. Create a public repository, for example `example/forum-mirror`.
2. Create a fine-grained token limited to that repository with
   *Contents: read and write*, and save it in a root-only file
   (`install -m 0600 /dev/null /etc/forum-static/github.token`, then write
   it in).
3. Run the export, then the publisher with the `github` target above. The
   first run creates the `gh-pages` branch.
4. In the repository: Settings, Pages, *Deploy from a branch*, `gh-pages`,
   folder `/ (root)`. The copy is at
   `https://example.github.io/forum-mirror/`; pass that as `--base-url` to
   the export so the feed carries it.

### Codeberg Pages

1. Create a public repository named `pages` in the organization (or any
   repository and use its `pages` branch).
2. Create an access token with *repository: write* in the account's
   Settings, Applications, and save it in a root-only file.
3. Publish with the `codeberg` target (`branch` `main` for a `pages`
   repository, `pages` for another repository).
4. The copy is at `https://example.codeberg.page/` (or
   `https://example.codeberg.page/<repo>/` for a `pages` branch).

### Anyone can host a copy

- Clone the pages branch: `git clone --branch gh-pages --single-branch
  https://github.com/example/forum-mirror.git` (or `--depth 1` for the
  newest copy only), then serve the folder with any web server, or open
  `index.html` from disk.
- Or download a tarball: GitHub serves
  `https://github.com/example/forum-mirror/archive/refs/heads/gh-pages.tar.gz`;
  an operator can also publish the export's own reproducible
  `--tarball site.tar.gz`.
- Check a copy against its manifest:
  `python3 tools/forum_static_export.py --verify --out <folder>`, and
  rebuild it from the source commit in `manifest.json` to confirm it is
  byte-identical.
- IPFS: `ipfs add -r --cid-version 1 <folder>` gives a content address
  that any gateway serves; relative links work under `/ipfs/<cid>/`.

### Re-export after each sync (systemd)

`/etc/systemd/system/forum-static.service`:

```ini
[Unit]
Description=Static copy of the forum
After=network-online.target

[Service]
Type=oneshot
Nice=10
ExecStart=/usr/bin/python3 /srv/forkmesh/tools/forum_static_export.py --source /srv/forum/work --out /srv/forum/static --source-url https://forum.example/owner/repo --site-id owner/repo --base-url https://owner.github.io/repo-mirror/
ExecStart=/usr/bin/python3 /srv/forkmesh/tools/forum_static_publish.py --site /srv/forum/static --targets /etc/forum-static/targets.json
```

`/etc/systemd/system/forum-static.timer`:

```ini
[Unit]
Description=Refresh the static copy of the forum every 30 minutes

[Timer]
OnBootSec=10min
OnUnitActiveSec=30min
RandomizedDelaySec=2min

[Install]
WantedBy=timers.target
```

`systemctl enable --now forum-static.timer`. To run right after a sync
instead, add `ExecStartPost=-/usr/bin/systemctl start --no-block
forum-static.service` to the sync service. Exporting from a work checkout
while a sync writes to it can catch a half-written state; `--source
<bare.git> --ref master` reads a committed state only.

## Onion service

`onionService: true` in the instance settings (or in
`/etc/forkmesh-instance/instance.json`, then `install-instance.sh
--update`) adds a v3 onion address that serves the same app over Tor, and
the HTTPS site advertises it with `Onion-Location`, so Tor Browser offers
it. Details, files and the key backup are in
[INSTANCES.md](../INSTANCES.md#tor-onion-service).

Every onion visitor reaches Caddy from Tor's loopback connection, and the
host skips its rate limiter for a loopback address, so the onion site sends
the fixed address `100.64.0.1` as `Cf-Connecting-Ip`. All onion visitors
share that one rate-limit bucket and one probe-ban entry: a flood over Tor
slows every Tor reader rather than going unlimited. A per-circuit address
(`HiddenServiceExportCircuitID haproxy` with Caddy's `proxy_protocol`
listener) would separate them.

## Risks and limits

- A static copy is read-only and as fresh as its last publish. Its
  `manifest.json` shows the source commit and time.
- GitHub can take down a Pages site, and github.io or codeberg.page can be
  blocked as a whole; that is why there are several doors, and why anyone
  can host a copy from the branch or tarball.
- Every published version stays in the pages branch. Attachments are
  content-addressed, so a version costs only its changed pages, but a
  removed post stays in that history; a takedown needs a history rewrite on
  that target by its owner. GitHub Pages sites are limited to 1 GB, and
  attachments over `--max-attachment-bytes` (20 MiB) are linked, not copied.
- The static feed has the newest 100 posts across the forum. Per-thread
  and per-label feeds are left to the instance, which can serve them fresh.

Tests: `app/tests/test_forum_static_export.py`.
