# Legal pages on self-hosted instances (Terms, Privacy, Security)

forkmesh.com links Terms, Privacy and Security from the dashboard sidebar,
the shared site header's menu, the site footer, the profile page footer and
the sign-up agreement box. Those pages are forkmesh.com's own policies. On a
self-hosted instance (for example `code.example.org`) they do not apply to
the operator, so each one is an instance setting that starts **off**.

Code: `app/src/instance_legal.py` (setting, validation, links, page and
`security.txt`), `app/src/app_runtime.py` (`_instance_legal_setting`,
`_instance_legal_route`, `/api/instance`), `app/src/server_update_admin.py`
(admin page), `app/public/instance-brand.js` (links in the browser),
`app/host/forkmesh_host/instance_bootstrap.py` and
`www/public/install-instance.sh` (install settings).

## Where the links are

| Place | File | Links |
|---|---|---|
| Dashboard sidebar, "Legal and security" | `app/public/dashboard/partials/sidebar.html` | `/security-report`, `/privacy`, `/terms` |
| Shared header menu, "Legal & security" | `www/public/site-header.js` (`LEGAL_HTML`) | `/security-report`, `/privacy`, `/terms` |
| Site footer columns | `www/public/site-footer.js` | `/terms`, `/privacy`, `/security-report` |
| Profile page footer | `app/public/dashboard/partials/views/profile-overview.html` | `/terms`, `/privacy`, `/security` |
| Sign-up agreement | `app/public/signup.html` | `/terms`, `/privacy` |

Server-rendered hackathon pages (`hackathon_runtime.py`) and the
acquisition transparency page (`attribution.py`) also link `/privacy`; both
are switched off on an instance (the `hackathon` plugin and the transparency
flag), so they are left alone.

## The setting

One `instance_settings` row, `k = 'legal_pages'`:

```json
{"terms":    {"mode": "markdown", "markdown": "# Terms\n...", "url": ""},
 "privacy":  {"mode": "url", "markdown": "", "url": "https://example.org/privacy"},
 "security": {"mode": "off", "markdown": "", "url": "", "contact": ""}}
```

- `mode` is `off`, `markdown` (a page on the instance, at `/terms`,
  `/privacy` or `/security`) or `url` (the link goes to that address).
- `markdown` is at most 32 KB. `url` must be a full `http`/`https` address
  without spaces, quotes or angle brackets. `contact` (Security only) is an
  email address (stored as `mailto:`) or an `https` address.
- Turning a page off keeps its text and link, so turning it back on needs no
  retyping.
- No row, a damaged row, or a page entry that fails validation reads as off.
- The app remembers the row for 30 seconds per process; a save forgets it in
  the process that saved, and other processes see it within 30 seconds.

forkmesh.com and the core hosts (not branded: no `INSTANCE_NAME`) never read
the row: the router only consults it inside the `instance_branded` branch,
`/api/instance` reports today's links, and the admin page shows no form and
refuses a write. Their routes, links and sign-up box are unchanged.

## What the instance serves

The router handles these before the plugin pages and before the www site
routes, only on a branded instance, only for `GET` and `HEAD`:

| Path | Off | Markdown | Link |
|---|---|---|---|
| `/terms`, `/privacy` | the instance's 404 page | the page, rendered in the instance page shell (`instance_pages.page`) | `302` to the address |
| `/security` | not handled here (normal routes, normally 404) | the page | `302` to the address |
| `/security-report` | 404 page | `302` to `/security` | `302` to the address |
| `/terms/` etc. | 404 page | `302` to the path without the slash | `302` to the address |
| `/.well-known/security.txt` | not handled (404, as today) | served | served |

`/security` falls through when off because it was never a reserved path, so
a short repository address named `security` keeps working until the
operator turns the Security page on.

Redirects are `302` and `no-store`: the operator can change the setting at
any time.

### Markdown and sanitizing

The page text goes through `markdown_html` from
`app/host/forkmesh_host/repo_ssr.py`, the renderer the host already uses for
server-rendered READMEs. It drops raw HTML tags, escapes all text, keeps
only `http`, `https`, `mailto`, `#` and `/` links (a `javascript:` link
becomes plain text) and escapes code blocks. When the page text has no
top-level heading, the page title (`Terms`, `Privacy`, `Security`) is added
as `<h1>`. Outside the host (the module cannot be imported) the text is shown
as escaped paragraphs.

### security.txt

With Security on, `/.well-known/security.txt` (RFC 9116) answers:

```
Contact: <contact, or the security page address>
Expires: <now + 180 days>
Policy: <https://<instance>/security, or the external address>
Canonical: https://<instance>/.well-known/security.txt
```

With Security off there is no `security.txt`: a file that pointed reporters
at forkmesh.com, or at a page the operator has not written, would send
vulnerability reports to the wrong people. forkmesh.com has no
`security.txt` today and keeps none.

## Links in the browser

`GET /api/instance` carries `legal`:

```json
{"terms":    {"enabled": true,  "href": "/terms", "external": false},
 "privacy":  {"enabled": true,  "href": "https://example.org/privacy", "external": true},
 "security": {"enabled": false, "href": "", "external": false}}
```

On forkmesh.com it is always the three current links (`/terms`, `/privacy`,
`/security-report`), all enabled.

`instance-brand.js`, which `apply_instance_brand.py` writes only into an
instance's pages:

- adds a stylesheet at start that hides every `/terms`, `/privacy`,
  `/security` and `/security-report` link inside `header`, `nav`, `aside`
  and `footer`, the sign-up agreement (`[data-instance-legal-consent]`) and
  legal groups (`[data-instance-legal-group]`) until the script has marked
  them, so an off link never flashes;
- after `/api/instance` answers, hides off links (as it hides switched-off
  plugins), points on links at their `href` (external ones open in a new tab
  with `rel="noopener"`) and marks them `data-instance-legal`, and hides a
  legal group with no link left (the header menu's "Legal & security"
  title);
- rewrites the sign-up agreement: Terms and Privacy on, "I agree to the
  Terms and have read the Privacy policy."; Terms only, "I agree to the
  Terms."; Privacy only, "I have read the Privacy policy."; both off, the box
  is hidden. `signup.js` treats a hidden box as accepted (`termsAccepted()`),
  so sign-up never asks for terms that do not exist. The server never
  required the box;
- if `/api/instance` fails, treats every page as off.

Links inside user content (READMEs, chat, notes) are left as written.

## Admin page (Platform → Legal pages, `?view=legal-pages`)

The admin console (admins only, `_check_admin_page_auth`) shows one card per
page with its state, an Open link when on, and a form: Show (off, a page on
this site, a link), the Markdown text, the link and, for Security, the
security contact. Each form saves one page (`save_legal_page`), is
CSRF-checked, and is audited as `admin.console_save_legal_page` with the
page and mode. There is no API write.

## Install settings

`install-instance.sh` takes an optional `legalPages` object; leave it out and
every page stays off:

```json
"legalPages": {
  "terms": {"markdown": "# Terms of use\n\n..."},
  "privacy": "https://example.org/privacy",
  "security": {"url": "https://example.org/security", "contact": "security@example.org"}
}
```

Each page is `{"markdown": ...}`, `{"url": ...}` (a bare string is a URL),
or `null`/`false`/absent for off; `contact` only on `security`. The
installer checks the shape and passes the object to `instance-bootstrap` on
stdin (it can be larger than a command-line argument), which validates it
with `instance_legal.from_install` and writes the row with `ON CONFLICT DO
NOTHING`, so a re-run never replaces what an admin saved. `legalPages` is
not kept in `instance.json`, and `--update` ignores it and never runs the
bootstrap, so an update never turns a page on.

## Applying to an existing instance

Update the instance (Platform → Update ForkMesh, or
`install-instance.sh --update`). The update rebuilds the pages (so they get
the new `instance-brand.js`, `signup.js` and header script) and restarts the
app. No database migration is needed: `instance_settings` already exists.
After the update every page is off; turn pages on from Platform → Legal
pages.

## Tests

`app/tests/test_instance_legal_pages.py` (defaults, validation, rendering
and sanitizing, `security.txt`, routes on and off, forkmesh.com unchanged,
admin-only writes, `/api/instance`, brand script and sign-up structure),
`app/host/tests_unit/test_instance_bootstrap.py` (install settings) and
`app/tests/test_install_instance_script.py` (`legalPages` parsing, update
mode, bootstrap stdin).
