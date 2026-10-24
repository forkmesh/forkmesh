# Translating posts

Everything on this page is behind the `post-translate` instance plugin, and
community translations also need `community-translations`. Both are `OPT_IN`
plugins: off on instances until an admin turns them on under Plugins, and off
on forkmesh.com unless `FORKMESH_CORE_PLUGINS` names them. While
`post-translate` is off, the dashboard does not start the translation code at
all.

Repository threads on a forum-style instance are written in many languages
(a forum mirrored from GitHub can have English, Chinese, Persian, Arabic,
Russian, Turkmen and more). Such forums want translations attached to posts,
kept client-side, with the original post as the source of truth and no
third-party service in the loop. The dashboard
answers with three layers:

0. **The site's own translations** (below, `tools/forge_translate.py`): the
   instance translates each post that is not in English once, on its own
   server with open models, and English readers see it automatically in any
   browser. Only English is made this way; other directions use layer 1.
1. **On-device translation** of any post into the reader's language, by the
   browser's own built-in model. Nothing leaves the device.
2. **Community translations** (below): a person can attach a signed
   translation to a post; readers see it beside the original, never instead
   of it. Stored in the App database, behind the `community-translations`
   instance plugin.

## Privacy

On-device translation makes **no network request**. It uses the browser's
built-in `Translator` and `LanguageDetector` APIs; the model runs locally.
When a language pair is not on the device yet, the browser downloads its model
once from the browser vendor (Chrome: from Google's component updater, the
same channel that updates Chrome itself); the post text is never part of that
download. ForkMesh never sends post text to a translation service, and the
page loads no translation script.

The reader's preferred target language and the "translate automatically"
choice are stored only in that browser's `localStorage`
(`forkmesh.postTranslate.v1`).

## On-device translation (`09-post-translate.js`)

The fragment decorates every post body on a record page:

- `[data-repo-record-detail] [data-repo-record-body]`: the opening post;
- `[data-repo-record-detail] [data-repo-issue-comment-text]`: each comment.

It does not touch the issue renderers (`renderRepoIssueDetail`,
`renderIssueTimelineComment`, `repo_ssr_issue.py`): a `MutationObserver`
finds bodies when the dashboard or the server-rendered first view puts them
on the page and inserts a small bar (`[data-post-translate]`) right after each
body. Re-renders (relations arriving, a new comment) replace the bodies; a
post the reader had translated is translated again from the in-memory cache
without another click.

For each body it:

1. sets `dir="auto"` (unless set) and `translate="yes"`, and
   `translate="no"` on `pre`, `code`, `kbd`, `samp` and `var` inside it, so
   the browser's own Translate page feature also leaves code alone;
2. detects the language: `LanguageDetector` when its model is already on the
   device (top result with confidence at least 0.5). Otherwise, which is the
   usual case on a first visit, the page guesses without it, so a reader never
   waits for a model to learn a post is in English:
   - the script, with links, mentions, paths and anything containing a digit
     left out: Han (also when technical terms in Latin letters are mixed in:
     12 or more Han characters and over 12% of the letters), kana, Hangul,
     Thai, Hebrew, Greek, Myanmar, Armenian, Georgian; Arabic script becomes
     `fa` or `ar` by their own letters; Cyrillic becomes `kk` (Kazakh
     letters), `uk` (`і ї є ґ` outnumber `ы э ъ ё`) or `ru`; Vietnamese by its
     tone marks;
   - common words for Latin-script text (en, es, fr, de, pt, it, tr, id, nl,
     pl, tk, plus a few letters only one of them uses): the leader needs at
     least two hits, 6% of the words and 1.6 times the runner-up.
   A detected language is written to the body's `lang`, which the browser's
   own Translate page feature reads;
3. asks `Translator.availability({sourceLanguage, targetLanguage})` once per
   language pair for the page and draws the bar.

A post that also contains the reader's language by the word count (example
threads are often written twice, Chinese then English) counts as `same`.

The target is the reader's choice from the bar's settings menu, else the
first usable entry of `navigator.languages`, else English. Codes are reduced
to what the Translator API expects (`zh-TW`/`zh-HK`/`zh-Hant` to `zh-Hant`,
other `zh-*` to `zh`, everything else to its primary subtag).

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
"Translated from Persian · on your device". The first post the reader
translates puts the page in thread mode: every other post (and comments that
stream in later) follows, with no further clicks, until "Show all originals"
in the settings menu. The menu also holds the target language, "Show this
site's translations automatically" (on by default), "Translate automatically
when the model is on this device" (only pairs already `available`; nothing
is downloaded without a click) and "Translate every post on this page".

**Hover to translate.** On a device with a mouse (`(hover: hover) and
(pointer: fine)`), a post whose translation is ready (the site's own, or an
on-device pair that is already `available`, so no download needs a click)
shows its original and gets a faint highlight; resting the pointer on it for
350 ms turns it into the reader's language with a burst of small sparkles at
the pointer and a short blur-to-sharp fade. It stays translated; "Show
original" puts it back and that post no longer reacts to hover until it is
translated again. Hover never starts a model download and never turns on
thread mode. On touch screens, which cannot hover, the site's translations
show right away; "Translate right away instead of on hover" in the menu does
the same on desktop. The styles are injected by the fragment (one `<style
data-post-translate-style>`) and use the theme's `primary` and `accent`
colours; with `prefers-reduced-motion: reduce` there are no sparkles and no
fade.

A translator that never starts (no download progress for 20 s, or 8 s when
the availability check never answered, as in some embedded browsers) gives
up: that pair is marked `unavailable` for the page and its posts show the
Translate page hint instead of a button that cannot work.

### How text is replaced

The rendered DOM is translated in place, so links, mentions, images, task
boxes and code keep their markup:

- The body is split into runs of text. A run continues through inline
  formatting (`strong`, `em`, `span`, `del`, ...) so a sentence with a bold
  word is translated as one sentence; it breaks at links, inline code, line
  breaks and block elements. A link's text is its own run.
- `pre`, `code`, `kbd`, `samp`, `var`, form controls, media and anything with
  `translate="no"` are skipped, as are runs without letters, bare URLs,
  `@mentions` and `#123` references.
- A run's translation goes into its first text node (leading and trailing
  whitespace kept); its other text nodes are emptied. The original values are
  kept in memory and "Show original" puts them back. Formatting inside one
  run (a bold word mid-sentence) is not reproduced in the translation.
- A failure halfway restores everything already replaced.
- Translations are cached per language pair and text for the page's
  lifetime; one translator per pair is created and reused.

## Community translations

A signed-in member can attach a translation to any post (the opening post or
a comment of an issue, pull request or discussion). It is shown beside the
original, marked as community-provided with the translator's name, and never
replaces the post: the original stays the source of truth.

### Why not an issue event

Comments live in the signed issue ledger (`.forkmesh/issues`), replayed by
every client (desktop `IssueStore`, the Go mirror's `writeIssueItem`, the
Worker's `verify_issue_event`, the web view). A new `translation` event type
would be refused or dropped by older clients, would wait in the owner's inbox
until their node drains it, and on a mirror-mode instance (example/forum is
mirrored from GitHub, which wins every sync) would be overwritten. Like web
reactions (`comment-reactions.md`), translations are an annotation, not part
of the record, so they live in the App database next to `comment_reactions`
and leave the ledger untouched. Each one is still signed by the author's
browser key, which binds the text to its post, language and time. The key
is whatever the browser sent and is not checked against the account's
registered keys, so a signature shows that the text was not changed after
it was written, not who wrote it; the account session says who.

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
  `source` is the SHA-256 of the original Markdown when the translation was
  written; the view says "the original was edited after this translation"
  when it no longer matches.
- Deleting the repository (`_REPO_SCOPED_TABLES`) or the account deletes its
  rows; `tools/next_rekey.py` and `docs/REKEY.md` list the table.
- Migration `0293_post_translations.sql` creates it and stamps the schema
  fingerprint (written as 0285 and renumbered when it merged after main's
  0285-0290 and the 0291 and 0292 of the same series).

### Signature

The browser signs with its web issue key (`getWebIssueKey`, the same Ed25519
key that signs web comments):

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

The body is normalized before hashing on both sides: CRLF to LF, C0 control
characters except tab and newline removed, trailing whitespace trimmed.
Invisible format characters such as ZWNJ are kept: Persian needs them. The
account session, not the key, says who the translator is, as for web
comments.

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

Checks: a session (401), the target and language code
(`[a-z]{2,3}(-[A-Za-z0-9]{2,8}){0,2}`) and a body of 1 to 20000 characters
(400), read access to the repository (404 for a private one), the record
type's feature switch (403), a moderation block on the account (403
`account_blocked`, `moderation_blocked_response`; added 2026-10-24, before
which a blocked account could still publish translations), the signature
(400 `bad_signature`), then 10
writes a minute and 100 a day per account (429), 60 translations per account
on one record (409 `too_many_translations`) and 500 per record (409
`record_full`).

### Instance switch

`community-translations` is an instance plugin (`instance_features.py`),
**off by default** on instances: the operator turns it on in the console's
Plugins view. Off, the API answers like any switched-off plugin route and the
chips' slot (`[data-post-translations]`) is hidden by the plugin stylesheet.
On forkmesh.com (an unbranded deployment) every plugin is on. The on-device
Translate button does not depend on it.

### UI

The translation bar under each post carries a chip per language
("Community translations: English · 中文", with a count when several people
translated into one language) and, for signed-in readers, "Add a
translation". A chip opens the translations below the bar, each with its
author, age, the source-of-truth note, Edit (own) and Remove (own, or any for
an issue manager). The form takes a language and Markdown; it opens with the
reader's target language (English when the post is already in it).

The server-rendered first view (`repo_ssr_issue.py`) is unchanged: chips are
added by the same observer once the page's script runs, so there is no SSR
markup to keep in parity. Readers without JavaScript get the `format=html`
page.

### Moving to Git later

A translation row is self-contained, so a later exporter can
write `.forkmesh/issues/translations/<n>/<comment>.<lang>.<account>.json`
with `{lang, body, author, pub, sig, ts, source}` without a format change. It
is not done now because the ledger's mirror-mode sync and older clients would
not keep or understand the folder.

### Cost on a long thread

Bodies are decorated in idle slices (`requestIdleCallback`, at most about
8 ms per slice), so a 99-post thread draws before its bars do. Icons are
created at most once per frame. Word lists and language names are cached for
the page's lifetime.

## The site's own translations (`tools/forge_translate.py`)

Most readers have no on-device translator: Firefox, Safari and phones have
none, and Chrome's model does not always download. So the instance makes an
English translation of every post that is not in English, once, on its own
server:

- **Engine:** Argos Translate's open models, run directly with CTranslate2
  (int8, one thread) and SentencePiece, without the `argostranslate` package
  and its PyTorch dependency, so it fits a 1 GB server. Models live outside
  the repository (`forge_translate.py models --dir DIR --languages ru,zh,fa`
  downloads them from the Argos package index; only the `model/` folder and
  the SentencePiece file are unpacked). One model is in memory at a time.
- **What is translated:** each post's current text (after edits, without
  deleted comments) and the title, when the same language rules as the page
  name a language other than English, either for the whole post or for at
  least 20 characters of its lines. A post written twice (Russian, then the
  same in English) is translated line by line: lines in another script, or
  for Latin-script sources lines whose words are not English, are
  translated, and English lines are kept as written. Posts over 20,000
  characters are left out.
- **Markdown:** fenced and indented code, tables, inline code, images, link
  targets, bare links, HTML tags, mentions, references and emphasis marks are
  kept byte for byte; the prose between them is split into sentences and
  translated in one batch per post.
- **Storage:** `.forkmesh/translations/issues/<n>.json`
  (`forkmesh-machine-translations-issue-v1`): `target` and, per post key
  (`title`, `body` for the opening post, the comment's event id), the source
  language, the SHA-256 of the original text, the model and the translated
  markdown. Kept in the repository like mirrored attachments, so every mirror
  and the static export carry it. Signed records are never touched.
- **Passes:** `forge_translate.py sync --checkout WORK --models DIR --seconds
  240` works newest threads first within its time budget, keeps entries whose
  hash still matches, redoes edited posts, drops deleted ones, and reports
  what is pending; the next pass continues.
- **On the page:** the dashboard reads the issue's file first, before any
  language detection, checks the hash against the post it is showing
  (`crypto.subtle`, only for posts the file has an entry for), and for
  readers whose language is the file's target renders the translated
  markdown in place of the post, labelled "Machine translation from …" with "Show original". It
  works with no `Translator` API at all. The hash ties an entry to the text
  it translates, not the translation to that text: anyone who can push to
  the repository (or, for a mirrored repository, the sync that writes it)
  decides what the translation says. Readers turn it off with "Use this
  site's translations".
- **Downloads:** the model index and packages come only over https (a
  redirect off https is refused), each download is capped at 512 MiB and
  an archive that unpacks to more than 1 GiB is skipped. The index is read
  from the Argos project's moving `main` branch and the packages carry no
  pinned hashes, so this trusts that project and GitHub.

example runs it from `forum-sync` after each GitHub sync, at low
priority.

## Browser support

| Browser | On-device button | Fallback |
| --- | --- | --- |
| Chrome / Chromium 138+ desktop (Windows, macOS, Linux, ChromeOS) | yes: `Translator` and `LanguageDetector` | |
| Edge (Chromium) | when the build exposes `Translator`; otherwise the disabled state | Edge's Translate page (a Microsoft service) |
| Chrome on Android / iOS | not yet: disabled state | Chrome's Translate page |
| Firefox | disabled state | Firefox Translations, itself on-device; reads `lang` and `translate="no"` |
| Safari | disabled state | Safari's Translate (Apple, on-device for some pairs) |

Every fallback benefits from the attributes the fragment sets: a correct
`lang` per post, `dir="auto"`, and `translate="no"` on code.

## Tests

`app/tests/test_post_translations.py` covers the API: signatures (and what
they cover), listing without bodies, replacing, removal by the author and by
a manager, private repositories, limits and the escaped no-script page.

`app/tests/test_forge_translate.py` covers the server tool with a fake
engine: language rules matching the page, markdown kept, a pass that
translates new posts and keeps unchanged ones, edits and deletions, missing
models and the time budget, and model packages unpacked safely.

`app/tests/test_post_translate_frontend.py` runs the controller in Node with a
small fake DOM and fake `Translator` / `LanguageDetector` objects: the four
availability states and the missing API, model download progress, translator
reuse, detection (detector, weak results, script heuristics), code blocks,
links and mentions left alone, "Show original", a failed translation that
restores the post, and the stored preference; and for community translations
the signing text and body normalization against `post_translations.py`,
escaping of chips, entries and the form.
