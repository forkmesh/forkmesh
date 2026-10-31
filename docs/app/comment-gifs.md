# GIFs and stickers in comments

Every record composer on a repository page (issue, pull request and
discussion comments and replies, reviews, edits: any textarea marked
`data-mention-input="record"`) has a **Sticker** button and, when the instance
has a GIPHY key, a **GIF** button. Both open one picker (a popover, a bottom
sheet under 640 px) that inserts Markdown at the caret:

- a GIF as `![title](https://media.giphy.com/media/<id>/200.gif)`;
- a sticker as `![label](<same-origin image>#w=96)`: the built-in Thank you
  sticker and the custom reactions usable on the repository
  (`docs/app/comment-reactions.md`).

The reaction picker also has **Reply with a GIF**: picking one posts a reply
to the record (mentioning the comment's author) in one click. A draft already
in the composer is never replaced or sent; the GIF is added to it instead.

## The GIPHY proxy

The key is an administrator setting (Settings → Reactions, GIFs & tips →
GIFs), stored in `instance_settings` under `giphy` and sealed with
`encrypt_row`. No browser ever receives it:

| Endpoint | Who | What |
| --- | --- | --- |
| `GET /api/gifs/config` | anyone | `{enabled, rating, attribution}`, cached a minute |
| `GET /api/gifs/trending?offset=` | signed in | 24 trending GIFs |
| `GET /api/gifs/search?q=&offset=` | signed in | 24 results for `q` (50 characters) |
| `GET/POST /api/gifs/settings` | administrators | `{configured, rating}`; set, replace or clear the key, rating `g`/`pg`/`pg-13`/`r` (default `pg`) |

Results keep only each GIF's id, title and preview size; every address is
rebuilt from the id on `media.giphy.com`, so a picked GIF can only point at
GIPHY's media host whatever the upstream answer says. Answers are remembered
per isolate (trending 5 minutes, searches 10) and a search costs the account
one of 30 a minute (`comment_extras_rate`). With no key, `enabled` is false,
the GIF buttons stay hidden and searches answer 404. The picker shows
"Powered by GIPHY", as GIPHY's terms ask.

## Rendering

Markdown images whose address is on GIPHY's media hosts
(`https://media[0-4].giphy.com/media/[v1.../]<id>/(giphy|200|200w|giphy-downsized).gif`,
no query) render as

```html
<span class="fm-gif" data-fm-gif role="button" tabindex="0" aria-label="GIF: …">
  <picture><source srcset="…_s.gif" media="(prefers-reduced-motion: reduce)" data-fm-gif-still>
  <img src="….gif" alt="…" loading="lazy" decoding="async" referrerpolicy="no-referrer"></picture>
  <span class="fm-gif-badge">GIF</span></span>
```

in the dashboard (`mdGifHtml`), the server-rendered issue page
(`repo_ssr_issue.gif_html`, byte-identical) and the pull request and
discussion first views. Under reduced motion the still frame shows until the
GIF is hovered; anyone can click, tap or press Enter/Space to pause or play
it. Every other image renders as before. The page CSP already allows
`img-src https:`, which covers GIPHY's media hosts on every page that renders
comments (`_BROWSER_CSP`, the static-page fallback headers).

Code: `app/src/comment_gifs.py`, `app/public/dashboard/js/09-comment-gifs.js`.
