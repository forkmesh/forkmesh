# Translating posts

Part of [the App docs](README.md). Everything here is behind the
`post-translate` instance plugin; community translations also need
`community-translations`. Both are `OPT_IN` plugins (`instance_features.py`):
off on instances until an admin turns them on under Plugins, and off on
forkmesh.com unless `FORKMESH_CORE_PLUGINS` names them. While `post-translate`
is off, the dashboard does not start the translation code.

Forum-style repository threads (a forum mirrored from GitHub) are written in
many languages. Translations are attached to posts, the original stays the
source of truth, and no third-party service is in the loop. Three layers:

0. **The site's own translations** (`tools/forge_translate.py`): the instance
   translates each non-English post into English once, on its own server with
   open models; English readers see it in any browser. Other directions use
   layer 1.
1. **On-device translation** into the reader's language by the browser's
   built-in model. Nothing leaves the device.
2. **Community translations**: a person attaches a signed translation to a
   post; readers see it beside the original, never instead of it. Stored in
   the App database.

## Privacy

On-device translation makes **no network request**. It uses the browser's
built-in `Translator` and `LanguageDetector` APIs. A missing language pair's
model is downloaded once from the browser vendor (Chrome: Google's component
updater); post text is never part of that download. ForkMesh never sends post
text to a translation service and loads no translation script. The target
language and the "translate automatically" choice live only in the browser's
`localStorage` (`forkmesh.postTranslate.v1`).

## On-device translation (`09-post-translate.js`)

The fragment decorates every post body on a record page:

- `[data-repo-record-detail] [data-repo-record-body]`: the opening post;
- `[data-repo-record-detail] [data-repo-issue-comment-text]`: each comment.

It does not touch the issue renderers (`renderRepoIssueDetail`,
`renderIssueTimelineComment`, `repo_ssr_issue.py`): a `MutationObserver` finds
bodies as the dashboard or the server-rendered first view adds them and
inserts a bar (`[data-post-translate]`) after each. When a re-render replaces a
body the reader had translated, it is translated again from the in-memory
cache.

For each body it:

1. sets `dir="auto"` (unless set) and `translate="yes"`, and `translate="no"`
   on `pre`, `code`, `kbd`, `samp` and `var` inside it, so the browser's own
   Translate page feature also leaves code alone;
2. detects the language: `LanguageDetector` when its model is already on the
   device (top result, confidence at least 0.5). Otherwise (the usual first
   visit) it guesses without waiting for a model:
   - by script, ignoring links, mentions, paths and anything with a digit:
     Han (also with Latin technical terms mixed in: 12 or more Han characters
     and over 12% of the letters), kana, Hangul, Thai, Hebrew, Greek, Myanmar,
     Armenian, Georgian; Arabic script becomes `fa` or `ar` by their own
     letters; Cyrillic becomes `kk` (Kazakh letters), `uk` (`і ї є ґ`
     outnumber `ы э ъ ё`) or `ru`; Vietnamese by its tone marks;
   - by common words for Latin script (en, es, fr, de, pt, it, tr, id, nl, pl,
     tk, plus letters only one of them uses): the leader needs at least two
     hits, 6% of the words and 1.6 times the runner-up.
   The detected language is written to the body's `lang`, which browser
   Translate features read;
3. asks `Translator.availability({sourceLanguage, targetLanguage})` once per
   pair for the page and draws the bar.

A post that also contains the reader's language by word count (threads
written twice, Chinese then English) counts as `same`.

The target is the reader's choice from the bar's settings menu, else the first
usable `navigator.languages` entry, else English. Codes are reduced to what the
Translator API expects (`zh-TW`/`zh-HK`/`zh-Hant` to `zh-Hant`, other `zh-*`
to `zh`, everything else to its primary subtag).

### The bar

| Availability | Bar |
| --- | --- |
| `same` (post already in the reader's language, or written twice) | nothing (community chips only) |
| `machine` (the site has a translation for this post) | shown automatically: "Machine translation from Russian", "Show original" |
| `available` | "Translate to English" |
| `downloadable` / `downloading` | the same button; the first click downloads the model (a user gesture is required) and shows its progress |
| `unavailable` | the button marked `aria-disabled`; a click explains that this browser can't translate the pair, names the post's language and suggests the browser's Translate page option |
| no API | the same disabled state: "Your browser can't translate on-device; try your browser's Translate page feature." Posts whose language the script heuristic cannot name show only the settings icon, so a page is not covered in disabled buttons |
| language unknown, detector not downloaded yet (`unknown`) | an enabled button: the click creates the detector (the model download needs the click), detects, then translates |
| availability check did not answer within 4 s (`unknown`) | an enabled button that tries to create the translator on click |
| language unknown, fewer than 12 words (`unknown-source`) | nothing |
| still checking (`pending`) | nothing |

After translating, the button reads "Show original" and the status
"Translated from Persian · on your device". The first translated post puts the
page in thread mode: every other post, including comments that stream in
later, follows until "Show all originals" in the settings menu. The menu also
holds the target language, "Show this site's translations automatically" (on
by default), "Translate automatically when the model is on this device" (only
pairs already `available`; nothing downloads without a click) and "Translate
every post on this page".

**Hover to translate.** On a mouse device (`(hover: hover) and (pointer:
fine)`), a post whose translation is ready (the site's own, or an `available`
on-device pair) shows its original with a faint highlight; resting the pointer
on it for 350 ms translates it with a burst of sparkles and a short
blur-to-sharp fade. It stays translated; "Show original" puts it back and stops
that post reacting to hover until translated again. Hover never starts a model
download or thread mode. Touch screens show the site's translations right
away; "Translate right away instead of on hover" does the same on desktop.
Styles are one injected `<style data-post-translate-style>` using the theme's
`primary` and `accent`; `prefers-reduced-motion: reduce` drops sparkles and
fade.

A translator that never starts (no download progress for 20 s, or 8 s when the
availability check never answered, as in some embedded browsers) gives up: the
pair is marked `unavailable` for the page and its posts show the Translate
page hint.

### How text is replaced

The rendered DOM is translated in place, so links, mentions, images, task
boxes and code keep their markup:

- The body is split into runs of text. A run continues through inline
  formatting (`strong`, `em`, `span`, `del`, ...) so a sentence with a bold
  word is translated whole; it breaks at links, inline code, line breaks and
  block elements. A link's text is its own run.
- `pre`, `code`, `kbd`, `samp`, `var`, form controls, media and anything with
  `translate="no"` are skipped, as are runs without letters, bare URLs,
  `@mentions` and `#123` references.
- A run's translation goes into its first text node (surrounding whitespace
  kept); its other text nodes are emptied. Originals stay in memory for "Show
  original". Formatting inside one run is not reproduced.
- A failure halfway restores everything already replaced.
- Translations are cached per pair and text for the page's lifetime; one
  translator per pair is reused.

### Cost on a long thread

Bodies are decorated in idle slices (`requestIdleCallback`, about 8 ms each),
so a 99-post thread draws before its bars do. A slice started by the 500 ms
timeout (`didTimeout`) still decorates one body, so a page that is never idle
(an animation) still gets its bars. Icons are created at most once per frame;
word lists and language names are cached for the page's lifetime.

## Community translations

A signed-in member can attach a translation to any post (opening post or
comment of an issue, pull request or discussion). It is shown beside the
original, marked as community-provided with the translator's name.

### Why not an issue event

Comments live in the signed issue ledger (`.forkmesh/issues`), replayed by
every client (desktop `IssueStore`, the Go mirror's `writeIssueItem`, the
relay's `verify_issue_event`, the web view). A new `translation` event type
would be refused by older clients, would wait in the owner's inbox until their
node drains it, and on a mirror-mode instance (mirrored from GitHub, which
wins every sync) would be overwritten. Like web reactions
([comment-reactions.md](comment-reactions.md)), translations are an annotation,
so they live in the App database next to `comment_reactions`. Each is signed
by the author's browser key, binding the text to its post, language and time.
The key is not checked against the account's registered keys, so the
signature shows the text was not changed, not who wrote it; the account
session says who.

### Storage

```
post_translations (repo_bi, subject, comment, lang, account_bi,
                   created_at, updated_at, data)
  PRIMARY KEY (repo_bi, subject, comment, lang, account_bi)
  idx_post_translations_account (account_bi, updated_at)
```

- `subject` is `issue:<n>`, `pull:<n>` or `discussion:<n>`; `comment` the
  post's event id or `body` for the opening post (the ids the reaction bar
  carries).
- One translation per account, post and language: publishing again replaces
  it (`created_at` kept, `updated_at` moved).
- `data` is the encrypted `{name, agent, body, pub, sig, ts, source}`.
  `source` is the SHA-256 of the original Markdown at writing time; the view
  says "the original was edited after this translation" when it no longer
  matches.
- Deleting the repository (`_REPO_SCOPED_TABLES`) or the account deletes its
  rows; `tools/next_rekey.py` and [rekey](../operations/rekey.md) list the table.
- Migration `0293_post_translations.sql` creates it and stamps the schema
  fingerprint.

### Signature

The browser signs with its web issue key (`getWebIssueKey`, the Ed25519 key
that signs web comments; see [encryption](../encryption.md)):

```
forkmesh-translation-v1
<owner>/<repo>            (lowercase, as in the API path)
<kind>:<number>
<comment>
<lang>
<pub>
<ts>                      (epoch milliseconds, within 10 minutes of the server clock)
<sha256 hex of the body>
```

Both sides normalize the body before hashing: CRLF to LF, C0 control
characters except tab and newline removed, trailing whitespace trimmed.
Invisible format characters such as ZWNJ are kept: Persian needs them.

### API

`GET /api/repo/<owner>/<repo>/translations?kind=issue&number=<n>` lists every
post's translations without bodies (`lang`, `name`, `agent`, `mine`,
`createdAt`, `updatedAt`, `source`). Adding `&comment=<id>` (and optionally
`&lang=<code>`) returns bodies with `pub`, `sig` and `ts`. `&format=html`
returns a plain, script-free HTML page of the record's translations (escaped
text, `lang` and `dir="auto"` per translation, CSP `default-src 'none'`) for
readers without JavaScript. Public, anonymous reads are cached
`public, max-age=5`; others `private, no-store`.

`POST` with `{kind, number, comment, lang, body, ts, pub, sig, source?}`
publishes or replaces the caller's translation; `{kind, number, comment,
lang, remove: true}` removes it, and with `account: <name>` an issue manager
(`_authorize_issue_manager`: the repository owner or an organization role with
`organization.issues.manage`) or a moderator of the repository
(`moderation_can_moderate_repo`) removes someone else's.

Checks, in order: a session (401), the target and language code
(`[a-z]{2,3}(-[A-Za-z0-9]{2,8}){0,2}`) and a body of 1 to 20000 characters
(400), read access (404 for a private repository), the record type's feature
switch (403), a moderation block (403 `account_blocked`,
`moderation_blocked_response`), the signature (400 `bad_signature`), then 10
writes a minute and 100 a day per account (429), 60 translations per account
on one record (409 `too_many_translations`) and 500 per record (409
`record_full`).

### Instance switch

With `community-translations` off, the API answers like any switched-off
plugin route and the chips' slot (`[data-post-translations]`) is hidden by the
plugin stylesheet. The on-device Translate button does not depend on it.

### UI

The bar under each post carries a chip per language ("Community translations:
English · 中文", with a count when several people translated into one
language) and, for signed-in readers, "Add a translation". A chip opens the
translations below the bar, each with author, age, the source-of-truth note,
Edit (own) and Remove (own, or any for an issue manager). The form takes a
language and Markdown and opens with the reader's target language (English
when the post is already in it).

The server-rendered first view (`repo_ssr_issue.py`) has no chip markup: the
same observer adds chips once the script runs. Readers without JavaScript get
the `format=html` page.

### Moving to Git later

A row is self-contained, so an exporter could write
`.forkmesh/issues/translations/<n>/<comment>.<lang>.<account>.json` with
`{lang, body, author, pub, sig, ts, source}` without a format change. It is
not done because mirror-mode sync and older clients would not keep the folder.

## The site's own translations (`tools/forge_translate.py`)

Firefox, Safari and phones have no on-device translator and Chrome's model
does not always download, so the instance makes one English translation of
every non-English post on its own server:

- **Engine:** Argos Translate's open models run directly with CTranslate2
  (int8, one thread) and SentencePiece, without the `argostranslate` package
  and PyTorch, so it fits a 1 GB server. Models live outside the repository
  (`forge_translate.py models --dir DIR --languages ru,zh,fa` downloads them
  from the Argos package index, unpacking only `model/` and the SentencePiece
  file). One model is in memory at a time.
- **What is translated:** each post's current text (after edits, without
  deleted comments) and the title, when the page's language rules name a
  language other than English for the whole post or for at least 20
  characters of its lines. A post written twice is translated line by line:
  lines in another script, or Latin-script lines whose words are not English,
  are translated; English lines are kept. Posts over 20,000 characters are
  skipped.
- **Markdown:** fenced and indented code, tables, inline code, images, link
  targets, bare links, HTML tags, mentions, references and emphasis marks are
  kept byte for byte; the prose between them is split into sentences and
  translated in one batch per post.
- **Storage:** `.forkmesh/translations/issues/<n>.json`
  (`forkmesh-machine-translations-issue-v1`): `target` and, per post key
  (`title`, `body` for the opening post, the comment's event id), the source
  language, the SHA-256 of the original text, the model and the translated
  Markdown. It is kept in the repository like mirrored attachments, so every
  mirror and the static export carry it. Signed records are never touched.
- **Passes:** `forge_translate.py sync --checkout WORK --models DIR --seconds
  240` works newest threads first within its time budget, keeps entries whose
  hash still matches, redoes edited posts, drops deleted ones and reports what
  is pending; the next pass continues.
- **On the page:** the dashboard reads the issue's file before any language
  detection, checks the hash against the shown post (`crypto.subtle`, only
  for posts with an entry), and for readers whose language is the file's
  target renders the translation in place of the post, labelled "Machine
  translation from …" with "Show original". It needs no `Translator` API. The
  hash ties an entry to the source text, not the translation: anyone who can
  push to the repository (or, for a mirrored repository, the sync) decides
  what the translation says. Readers turn it off with "Use this site's
  translations".
- **Downloads:** the model index and packages come only over https (a
  redirect off https is refused), each download is capped at 512 MiB, and an
  archive that unpacks to more than 1 GiB is skipped. The index is read from
  the Argos project's moving `main` branch and packages carry no pinned
  hashes, so this trusts that project and GitHub.

The example forum runs it from `forum-sync` after each GitHub sync, at low
priority.

## Browser support

| Browser | On-device button | Fallback |
| --- | --- | --- |
| Chrome / Chromium 138+ desktop (Windows, macOS, Linux, ChromeOS) | yes: `Translator` and `LanguageDetector` | |
| Edge (Chromium) | when the build exposes `Translator`; otherwise the disabled state | Edge's Translate page (a Microsoft service) |
| Chrome on Android / iOS | not yet: disabled state | Chrome's Translate page |
| Firefox | disabled state | Firefox Translations, itself on-device; reads `lang` and `translate="no"` |
| Safari | disabled state | Safari's Translate (Apple, on-device for some pairs) |

Every fallback benefits from the attributes the fragment sets: `lang` per
post, `dir="auto"` and `translate="no"` on code.

## Tests

- `app/tests/test_post_translations.py`: the API (signatures and what they
  cover, listing without bodies, replacing, removal by author and manager,
  private repositories, limits, the escaped no-script page).
- `app/tests/test_forge_translate.py`: the server tool with a fake engine
  (language rules matching the page, Markdown kept, passes, edits and
  deletions, missing models, the time budget, safe unpacking).
- `app/tests/test_post_translate_frontend.py`: the controller in Node with a
  fake DOM and fake `Translator` / `LanguageDetector` (availability states,
  missing API, download progress, translator reuse, detection, code, links
  and mentions left alone, "Show original", failure restore, the stored
  preference), plus community signing text and body normalization against
  `post_translations.py` and escaping of chips, entries and the form.
