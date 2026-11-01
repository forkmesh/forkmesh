# Discourse Mirror

Part of [the marketplace docs](README.md). Package:
`marketplace/packages/discourse-mirror/`. Code:
`content/mirror/discourse_mirror.py` (one standard-library Python file, the
`mirror` entry point). Tests: `tests/test_discourse_mirror.py`.

The package keeps a read-only copy of a public Discourse forum in a Git
repository. Pushed to ForkMesh, that repository is replicated by the mirror
fleet like any other, so the forum stays readable from every mirror and every
clone when the forum itself is down or blocked where a reader lives.

It writes one of two layouts (`layout` in `forums.json`):

- `forum` (`add --forum --repository <checkout>`): the forum format v1 in
  `.forkmesh/forum/` of an existing ForkMesh repository checkout, which the
  repository's **Forum tab** shows with the Discourse look
  ([repo-forum](../app/repo-forum.md)). Only `.forkmesh/forum` is staged and
  committed (`git add -A -- .forkmesh/forum`, `git commit -- .forkmesh/forum`),
  so other work in the checkout is never swept into the commit. The checkout
  must exist; the tool never creates or re-clones it.
- `site` (the default): a repository of its own holding the topics as JSON and
  offline HTML pages, described below.

## Why it is a tool and not a Worker module

Package Worker modules may only fetch from the `outboundOrigins` written in
their manifest. A mirror's forums are the operator's choice, and the package
names no forum, so the copy runs as an operator tool on a box the operator
controls (a mirror node, the deploy box or a desktop), like
`tools/hosted_repository_sync.py`. ForkMesh carries the result as an ordinary
repository; nothing on the App or the desktop changes.

## Starting a mirror from the web (instance admins)

An instance admin starts a mirror without a shell from
`/start-discourse-mirror` ([repo-forum](../app/repo-forum.md#starting-a-mirror)).
The copy then runs in the repository itself, as the scheduled workflow
`.forkmesh/discourse-mirror.yml` on the instance's own Actions runner
([instance-actions](../server/instance-actions.md)):

- `.forkmesh/forum/tool/discourse_mirror.py` is this importer, committed into
  the repository by the setup. A clone of the repository can carry on the
  mirror anywhere.
- `.forkmesh/forum/source/settings.json` holds the forum address and options:
  `uploads`, `uploadHosts`, `maxUploadBytes`, `maxUploadBytesPerRun`,
  `revisitPerRun`, `delaySeconds` (never under 1 second), `maxTopicsPerRun`
  (400 by default), `commitName`, `commitEmail`. `ignoreRobots` is not read
  from it: a web-started mirror always follows robots.txt.
- The workflow runs twice an hour (minutes chosen at random, so instances do
  not all start at once) and on demand. `discourse_mirror.py ci --max-minutes 45`
  copies until it has done `maxTopicsPerRun` topics or 45 minutes have passed.
  It then saves, rebuilds the list files and commits `.forkmesh/forum`.
  `git push origin HEAD:<branch>` publishes the commit with the job's
  `actions-push` grant (`permissions: contents: write`), rebasing once if the
  branch moved.
- Large forums therefore take several runs for the first copy. The Forum tab
  shows each run's topics when it finishes.

## What a `site` copy holds

```text
README.md            what the copy is, where it came from, when it was taken
mirror.json          source address, title, description, last copy, counts
categories.json      the forum's public categories
topics/index.json    one row per topic: listing fields, when it was fetched, its pictures
topics/<id>.json     the topic and every visible post (cooked HTML as the forum served it)
uploads/<hash>.<ext> copied pictures; uploads/index.json maps each address to its file
site/index.html      categories and the 50 most recently active topics
site/c/<id>.html     a category's topics, 100 a page (<id>-p2.html, …)
site/t/<id>.html     a topic with its posts, each anchored as #p<number>
site/style.css
```

Readers clone the repository (from any mirror) and open `site/index.html`.
The pages carry a strict Content-Security-Policy (no scripts, styles and images
from the copy only), `noindex` (the copy must not compete with the forum in
search results) and `no-referrer`.

Kept per post: id, number, type, username, display name, created and edited
times, the post it replies to, the small-action code and the cooked HTML.
Avatars, e-mail hashes, user ids, trust levels and like lists are not kept.

## How a run works

`sync` fetches, through the forum's public JSON API, as an anonymous visitor:

1. `/site.json` (categories) and `/about.json` (title, description).
2. `/latest.json?page=N`, newest activity first, until a page has nothing new
   (a first run or `--full` walks every page). A topic is fetched again when
   its `bumped_at`, `posts_count` or `highest_post_number` changed. Pinned
   topics, which head the list whatever their age, do not keep the walk going.
3. `/t/<id>.json` for each changed topic, then `/t/<id>/posts.json` with up to
   20 `post_ids[]` at a time for posts not in the first answer. For a changed
   topic, posts already copied are reused; a **revisit** fetches every post
   again.
4. Revisits: each run re-reads the `revisitPerRun` topics fetched longest ago
   (50 by default), so edits and removals reach the copy over time without a
   full re-read. A `--full` run also revisits every copied topic missing from
   the list.
5. Pictures in posts (`<img src>`) on the forum's host or an `uploadHosts`
   entry, PNG, JPEG, GIF, WebP or AVIF only (never SVG), at most
   `maxUploadBytes` each and `maxUploadBytesPerRun` per run. A picture skipped
   for the run budget is tried again when its topic is next fetched.
6. Pages are rebuilt for fetched topics, every category page and the index;
   when the first copy completes, every page is rebuilt so links between
   topics point inside the copy. `render <name>` rebuilds everything.
7. `git add -A` and one commit (`Mirror <title>: N topics updated, …`), then
   `git push <pushRemote> HEAD:refs/heads/<branch>` when a remote is set.

A run that stops early (rate limit, network) still saves, renders and commits
what it fetched; the rest is picked up next time. `--max-topics N` (or
`maxTopicsPerRun`) bounds a run, so a large forum's first copy can be spread
over several runs; topics found but not fetched yet are counted as
`pending`.

### Politeness

- One request at a time, `delaySeconds` apart (1 s by default, at least 0.5 s
  from `add`), with the user agent `ForkMeshDiscourseMirror/1.0 (+https://forkmesh.com/market/discourse-mirror)`.
- `robots.txt` is honoured for user agent `ForkMeshDiscourseMirror`; a 401
  or 403 for `robots.txt` counts as "disallow everything". `ignoreRobots: true`
  is only for forums whose operators agreed in writing.
- A 429 or 5xx waits for `Retry-After` (or backs off) and retries up to five
  times; a wait over five minutes ends the run.
- Redirects are followed only to `https` on the forum's host (or an upload
  host for pictures).

### Removing what the forum removed

A topic leaves the copy (its JSON, its page and pictures nothing else uses)
only when the forum answers 403, 404 or 410 **with a Discourse JSON error**.
Any other refusal (a CDN challenge page, a login wall that is HTML) ends the
run without removing anything, and more than 25 removals outnumbering the
topics fetched in one run also stop it: a blocked or misconfigured forum must
not empty the copy. Removed posts are gone from the working tree and the
pages, but stay in the repository's history; erasing them from history is a
history rewrite, which needs a person's decision and the guarded tools
([destructive-actions](../engineering/destructive-actions.md)).

## Operator guide

The forums to copy live in the plugin's data folder,
`~/projects/<project>/data/discourse-mirror/forums.json` (from
`forkmesh_paths.resolve().plugin_data("discourse-mirror")`;
`--data-dir` or `FORKMESH_DISCOURSE_MIRROR_DATA` overrides it). Each copy's
repository defaults to `data/discourse-mirror/forums/<name>`. The never-erase
rule covers this folder; `remove` drops a forum from the list and leaves its
repository where it is.

### Preparing a forum

1. Ask the forum's operators. A copy republishes their members' posts; agree
   on what is copied and how removals are handled, and keep their answer.
2. Check that the forum can be copied, and how long a first copy takes:

   ```sh
   T=marketplace/packages/discourse-mirror/content/mirror/discourse_mirror.py
   python3 $T check https://forum.example.org
   ```

   It prints JSON: `discourse`, `public` (anonymous reads work), `robotsAllow`,
   the topic and post counts from `/about.json`, `firstCopyRequests` and
   `firstCopyHours` at 1 request a second, and `ready`. Exit status 2 means
   not ready.
3. Create the destination repository on ForkMesh and a Git remote for it in
   the copy's checkout (or pass `--repository` to an existing checkout).
4. Add the forum:

   ```sh
   python3 $T add https://forum.example.org --name example --forum --repository ~/projects/example/src
   ```

   With `--forum` the forum goes into that repository's Forum tab. Point
   `--repository` at the checkout the repository's owner publishes from (the
   owner's desktop project) and leave the push to that node; then turn the
   tab on under the repository's Settings, Features. Without `--forum`,
   `--push-remote forkmesh` names the Git remote of a repository that holds
   only the copy.

   `--no-uploads` skips pictures, `--delay 2` slows the copy down. Other
   settings (`revisitPerRun`, `maxTopicsPerRun`, `uploadHosts` for a forum
   whose pictures are on a CDN host, `maxUploadBytes`,
   `maxUploadBytesPerRun`, `branch`, `commitName`, `commitEmail`,
   `ignoreRobots`) are edited in `forums.json`.
5. Take the first copy. For a forum of a few thousand topics this is a few
   thousand requests; spread it if the operators prefer:

   ```sh
   python3 $T sync example --max-topics 500 --no-push   # repeat until "pending": 0
   python3 $T sync example                              # then push
   ```

   Each run prints one JSON line per forum: `fetched`, `revisited`,
   `removed`, `uploads`, `pending`, `requests`, `commit` and `error` (the exit
   status is 1 when any forum has one).

### Keeping it current

A user timer every 30 minutes is plenty; runs of the same forum never overlap
(`data/discourse-mirror/locks/<name>.lock`).

```ini
# ~/.config/systemd/user/forkmesh-discourse-mirror.service
[Unit]
Description=Copy Discourse forums for ForkMesh mirrors

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 %h/projects/forkmesh/src/marketplace/packages/discourse-mirror/content/mirror/discourse_mirror.py sync

# ~/.config/systemd/user/forkmesh-discourse-mirror.timer
[Timer]
OnBootSec=10min
OnUnitActiveSec=30min
RandomizedDelaySec=5min

[Install]
WantedBy=timers.target
```

A failed push leaves the commit in place and is retried by the next run; the
tool never resets, re-clones or deletes a checkout.

## Not done yet

- The Forum tab is on the web only; the desktop, iPhone and Android follow
  ([repo-forum](../app/repo-forum.md#roadmap)).
- Posting, likes and operator tools on ForkMesh, and writing back to the
  Discourse forum: phases 2 and 3 of the roadmap.
- Private categories and logged-in copies: only what an anonymous visitor
  sees is copied.
- Polls, reactions and user profile pages.

## Tests

```sh
python3 -m pytest -q marketplace/packages/discourse-mirror/tests
```

The suite runs an in-process fake Discourse server on `127.0.0.1` (plain
`http` is accepted only for loopback) and covers a first copy, incremental
runs, removals, blocked answers, `--full`, capped runs, `robots.txt`, rate
limits, `check`, the HTML sanitizer, the CLI, the `forum` layout (one-read
list files, long topics split into post files, removals, a scoped commit that
leaves other work alone), and that the package names no forum.
