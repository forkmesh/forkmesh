# Website page templates

Part of [the website docs](README.md). Website pages are rendered from
templates with inheritance and shared partials, so a change to the shared
head or chrome is one edit.

## Layout

| Path | What it holds |
| --- | --- |
| `www/templates/base.html` | The document every page extends: doctype, `<html>`, the shared `<head>` (charset, `api-client.js`, `analytics.js`, viewport, title, identity and favicons), the default header and footer chrome |
| `www/templates/partials/identity.html` | `application-name`, `theme-color`, `color-scheme`, the favicons and the web manifest |
| `www/templates/partials/site-header.html`, `site-footer.html` | The stylesheet and script pair of each piece of shared chrome |
| `www/templates/partials/genie-spider.html` | The spider's script tag ([genie-spider.md](genie-spider.md)) |
| `www/templates/layouts/blog-post.html` | Blog posts: Open Graph and Twitter tags built from `heading`, `slug`, `image` and `description`, the feature-post and social assets, and the `.shell` / `<main>` wrapper |
| `www/templates/pages/**` | One child template per page, at the same path as its output in `www/public/` |
| `app/tools/site_templates.py` | The renderer (standard library only) |

The rendered pages stay checked in under `www/public/`, because tests, the
App (`BACKEND_ASSETS`, `SHARED_FILES`), the Worker and other generators read
`www/public` directly. The dashboard uses the same approach
(`dashboard/shell.html`, `app/src/dashboard_shell.py`).

```sh
python3 app/tools/site_templates.py           # render www/templates/pages into www/public
python3 app/tools/site_templates.py --check   # list pages that are out of date
```

`build_site_assets.py www` refuses to stage when a rendered page is out of
date, and `www/tests/test_site_templates.py` fails on it too. Edit the
template, not the page in `www/public`.

## Syntax

A small subset of Jinja:

- `{% extends "base.html" %}` on the first line. Text outside blocks in a
  child template is an error.
- `{% block name %}…{% endblock %}` defines or overrides a block;
  `{{ super() }}` renders the parent's version.
- `{% include "partials/x.html" %}` pastes a partial in.
- `{% set name = "value" %}` sets a variable (a double-quoted string with no
  `"` in it); `{{ name }}` prints it with no escaping, so write values
  already HTML-escaped. An undefined variable is an error.
- `{% if name %}…{% else %}…{% endif %}` tests whether a variable is set and
  non-empty.

A line holding only one tag is dropped entirely. Names in `extends` and
`include` are relative to `www/templates/` and cannot leave it.

## Blocks in `base.html`

| Block | Default | Used for |
| --- | --- | --- |
| `html_attrs` | empty | Extra `<html>` attributes, with a leading space (` class="dark h-full"`) |
| `title` | `ForkMesh` | The `<title>` text |
| `meta` | empty | Description, canonical, Open Graph and other tags next to the title |
| `head` | empty | Stylesheets, inline styles and scripts, in the page's own order |
| `chrome` | footer then header partials | Override with an empty block when the page places the chrome itself or has none |
| `genie` | the spider partial | Override with an empty block to leave the spider out |
| `body_attrs` | empty | Extra `<body>` attributes, with a leading space |
| `body` | empty | Everything inside `<body>` |

The blog layout adds `post_chrome`, `post_head` (after the blog assets) and
`content` (inside `<main>`). A post with a non-standard body overrides `body`.

## Adding a page

1. Create `www/templates/pages/<path>.html` extending `base.html`, or
   `layouts/blog-post.html` for a blog post.
2. Run `python3 app/tools/site_templates.py`.
3. Commit both the template and the rendered page.

## Share images

Pages without an image of their own use `/assets/social-card.jpg`
(1200×630, the `summary_large_image` size). `www/tests/test_site_templates.py`
fails when a page's share image names a file `www/public` does not have.

Generated content: `market.html` holds a generated catalog between markers,
rewritten in the template by `app/tools/build_market_catalog.py`, which then
renders the site. `docs/git-compatibility/index.html` is written whole by
`tools/git_conformance.py render`. The Markdown docs library pages are
rendered by `app/tools/docs_site.py`.

## Edits from branches older than the templates

A branch that edits a page in `www/public` directly merges cleanly but leaves
the rendered page holding text the template lacks; the website deploy then
fails with "www/public is out of date with www/templates".

A pre-push hook stops this: `tools/www_push_check.py` archives each commit
pushed to `main` and runs `site_templates.py --check` and
`build_market_catalog.py --check` on it; a stale page refuses the push with
"Nothing was pushed". Pushes to other branches are not checked.

```sh
python3 tools/www_push_check.py install       # writes .git/hooks/pre-push
python3 tools/www_push_check.py check [REV]   # the same check by hand
```

`install` leaves a `pre-push` it did not write alone; call
`tools/www_push_check.py hook "$@"` from that hook instead. The hook is a
guard, not a gate: `git push --no-verify` and pushes from other machines skip
it. Tests: `app/tests/test_www_push_check.py`.

To fix a stale page, do not run `site_templates.py` on its own: that renders
the older template over the newer page and throws the edit away. Port the
edit into the template (or rerun the generator that owns it), then check
that rendering rewrites no page.
