# Website page templates

Part of [the website docs](README.md). Added 2026-10-06 (issue 1490): the user
asked for the website to be DRY, with template inheritance and base files, so
a change to the shared page head or chrome is one edit instead of one per page.

## Layout

| Path | What it holds |
| --- | --- |
| `www/templates/base.html` | The document every page extends: doctype, `<html>`, the shared `<head>` (charset, `api-client.js`, `analytics.js`, viewport, title, identity and favicons), the default header and footer chrome, the Genie spider, and `<body>` |
| `www/templates/partials/identity.html` | `application-name`, `theme-color`, `color-scheme`, the favicons and the web manifest |
| `www/templates/partials/site-header.html`, `site-footer.html` | The stylesheet and script pair of each piece of shared chrome |
| `www/templates/partials/genie-spider.html` | The spider's script tag ([genie-spider.md](genie-spider.md)) |
| `www/templates/layouts/blog-post.html` | Blog posts: Open Graph and Twitter tags built from `heading`, `slug`, `image` and `description`, the feature-post and social assets, and the `.shell` / `<main>` wrapper |
| `www/templates/pages/**` | One child template per page, at the same path as its output in `www/public/` |
| `app/tools/site_templates.py` | The renderer (standard library only) |

The rendered pages stay checked in under `www/public/`. Tests, the App
(`BACKEND_ASSETS`, `SHARED_FILES`), the Worker and other generators read
`www/public` directly, so rendering only at deploy time would have left them
reading stale pages. This is the same approach the dashboard takes with
`dashboard/shell.html` and its partials (`app/src/dashboard_shell.py`).

```sh
python3 app/tools/site_templates.py           # render www/templates/pages into www/public
python3 app/tools/site_templates.py --check   # list pages that are out of date
```

`build_site_assets.py www` refuses to stage when a rendered page is out of
date, and `www/tests/test_site_templates.py` fails on it too. So edit the
template, not the page in `www/public`: a hand edit to a rendered page fails
both.

A branch cut before the templates landed still edits `www/public` directly,
and merging it keeps that edit while the template keeps the old text. That is
how the 2026-10-06 website deploys of `ac7f6a992d` and `08b6c8c9bf` failed:
Marketing 1.11.0, Analytics 1.2.0, Browser Extension 2.5.0 and the October 6
privacy email paragraph were in `market.html` and `privacy.html` but not in
their templates. After such a merge, port the edit into the template (for the
market, run `app/tools/build_market_catalog.py`) and check that
`site_templates.py` renders `www/public` unchanged.

## Syntax

A small subset of Jinja:

- `{% extends "base.html" %}` on the first line. Text outside blocks in a
  child template is an error.
- `{% block name %}…{% endblock %}` defines or overrides a block. Inside an
  override, `{{ super() }}` renders the parent's version.
- `{% include "partials/x.html" %}` pastes a partial in.
- `{% set name = "value" %}` sets a variable (a double-quoted string with no
  `"` in it); `{{ name }}` prints it as is, with no escaping, so write values
  already HTML-escaped. An undefined variable is an error.
- `{% if name %}…{% else %}…{% endif %}` tests whether a variable is set and
  non-empty.

A line holding only one tag is dropped entirely, together with its indentation
and newline, so tags on their own lines leave no blank lines behind. Names in
`extends` and `include` are relative to `www/templates/` and cannot leave it.

## Blocks in `base.html`

| Block | Default | Used for |
| --- | --- | --- |
| `html_attrs` | empty | Extra `<html>` attributes, with a leading space (` class="dark h-full"`) |
| `title` | `ForkMesh` | The `<title>` text |
| `meta` | empty | Description, canonical, Open Graph and other tags that sit next to the title |
| `head` | empty | Stylesheets, inline styles and scripts, in the page's own order |
| `chrome` | footer then header partials | Override with an empty block when the page places the chrome itself (inside `head`) or has none |
| `genie` | the spider partial | Override with an empty block to leave the spider out |
| `body_attrs` | empty | Extra `<body>` attributes, with a leading space |
| `body` | empty | Everything inside `<body>` |

The blog layout adds `post_chrome` (header then footer; posts without the
footer override it with the header alone), `post_head` (after the blog
assets, for a post's own `<style>`) and `content` (inside `<main>`). A post
whose body is not the standard header, `.shell` and footer overrides `body`
instead of `content`.

## Adding a page

1. Create `www/templates/pages/<path>.html` extending `base.html`, or
   `layouts/blog-post.html` for a blog post.
2. Run `python3 app/tools/site_templates.py`.
3. Commit both the template and the rendered page.

## Share images

Link previews on X, Slack and the like come from the page's `og:image` and
`twitter:image`. Pages without an image of their own use
`/assets/social-card.jpg`, the "Your Code, Your World" art at 1200×630, the
size X's `summary_large_image` card expects. The source is a 1536×1024 image
in a white rounded frame. The card is cut from inside the frame (x 58–1478,
y 80–826), so it fills the card edge to edge and keeps the logo whole. X crops
anything that isn't 1.91:1, so it would otherwise have shown the frame. Until 2026-10-07 the homepage and the
hackathon pages pointed at `/assets/blog/intro-to-forkmesh.png`, a file that
was never committed, so X showed the title over an empty placeholder. The site
build used to swap that path for `/assets/logo.png` on app.forkmesh.com. It no
longer does, because `shared_assets` now stages the card at the root of
`assets/`. `www/tests/test_site_templates.py` fails when a page's share image
names a file that `www/public` does not have.

`market.html` holds a generated catalog between markers.
`app/tools/build_market_catalog.py` now rewrites the template
(`www/templates/pages/market.html`) and renders the site after it.
`docs/git-compatibility/index.html` is written whole by
`tools/git_conformance.py render` and has no template; its head is in that
tool. The Markdown docs library pages are rendered by `app/tools/docs_site.py`
from its own `PAGE_TEMPLATE`.

## The conversion

The 123 hand-written pages were converted by a one-off script. Each page's
body was kept byte for byte. Only the shared head lines moved into the base,
and the stylesheets and scripts that stayed kept their order. The rendered
pages differ from the originals only in:

- the spider's script tag;
- the shared head lines written once, in one style (`/>`), in one order. The
  blog layout keeps the posts' own `>` style for its tags, since
  `app/tests/test_blog_frontend.py` matches them exactly;
- the missing shared lines added to `docs/dependency-audit/index.html`
  (`api-client.js`, the Apple title, the 16 px favicon) and to
  `ios-install.html` (`api-client.js`, identity tags and favicons).

93 of the 94 blog posts use the blog layout: 72 feature posts entirely from
variables, 21 longer posts with their own `meta` (extra Open Graph fields)
and, where needed, `post_head` and `body`. `blog/introducing-forkmesh/`
extends the base directly.

## Edits from branches older than the templates

A branch cut before the templates landed still edits the page in
`www/public`. Git merges it cleanly, and the rendered page then holds text the
template lacks. The deploy's test gate runs only `app/tests/host`, so the
first thing to notice is the website step of the autodeploy:
`build_site_assets.py` refuses with "www/public is out of date with
www/templates", wrangler exits 1, and forkmesh.com keeps serving the previous
build.

A pre-push hook now stops this before the deploy. `tools/www_push_check.py`
archives each commit pushed to `main` and runs `site_templates.py --check` and
`build_market_catalog.py --check` on it; a stale page refuses the push with
"Nothing was pushed". Pushes to other branches are not checked. Install it once
per checkout (its worktrees share it):

```sh
python3 tools/www_push_check.py install       # writes .git/hooks/pre-push
python3 tools/www_push_check.py check [REV]   # the same check by hand
```

`install` leaves a `pre-push` it did not write alone; call
`tools/www_push_check.py hook "$@"` from that hook instead, with the pushed refs
on stdin. A worktree whose branch predates the tool runs the copy in the
checkout that installed it. The hook is a guard, not a gate: `git push
--no-verify` and pushes from other machines skip it, and the deploy's test gate
still does not run `www/tests`. Tests: `app/tests/test_www_push_check.py`. It
would have refused every failed push listed below.

Do not fix it by running `site_templates.py` on its own: that renders the
older template over the newer page and throws the edit away. Port the edit
into the template (or rerun the generator that owns it, such as
`build_market_catalog.py`), then check that rendering rewrites no page.

On 2026-10-06 the deploy of `ac7f6a992d66` failed this way: the Marketing
1.11.0 catalog (`5582c76cd4`) and the account-mail tracking change to
`privacy.html` (`5cde21d68f`) had gone to `www/public` only.

The next three deploys (`ac7f6a992d66` to `f061a2b89c16`) failed the same way
after that fix: the contest prize minimums (`33bd08ab10`) had gone to
`www/public/hackathon.html` and `hackathon/rules.html` only, and Marketing
1.12.0 / browser extension 2.6.0 (`d5bb25019f`) to `www/public/market.html`
only. Their edits are now in the templates too.

On 2026-10-08 the deploys of `d8a2ccffd99f` and `4e37aa18689d` failed the same
way: the per-send email tracking change (`a07c3d0d43`) rewrote the "Email we
send" paragraph in `www/public/privacy.html` only. That paragraph is now in
`www/templates/pages/privacy.html`.

On 2026-10-08 the deploys of `a1efd6c6d66f` and `fa132c7141bd` failed the same
way: Marketing 1.31.0 (`aba196c65b`) set the version in
`www/public/market.html` only. `2530b07d6e` put it in the template, and the
pre-push hook above was added after it.
