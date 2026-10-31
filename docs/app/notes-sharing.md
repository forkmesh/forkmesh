# Notes: sharing with people and the access link

The web Notes page (`app/public/dashboard/js/09-notes.js`, markup in
`app/public/dashboard/partials/views/notes.html`, mirrored byte for byte in
`marketplace/packages/notes/content/web/view.html`) lets the owner share a note
with a user or an organization from the **Share** panel.

## Picking who to share with

The name field suggests matches as you type, so nobody has to know the exact
account name:

- **Users** come from `GET /api/chat/direct-messages/users?query=…`, the same
  bounded search of active accounts the organization member picker uses. An
  empty query never asks the server, so the account list is never enumerated.
- **Organizations** come from `GET /api/orgs`, the organizations you belong to,
  fetched once per page and filtered locally.

Requests are debounced (150 ms) and a stale answer is dropped when a newer
query has started. Your own account and every principal the note is already
shared with are left out, and names that start with the query sort first.

Arrow keys move the highlight, Enter or Tab or a click picks it, Escape closes
the list without closing the panel. Picking a suggestion fills the name and
switches the User/Org selector to match; Enter again (or **Invite**) shares
it. When the highlighted suggestion is exactly what is typed, the first Enter
shares straight away. A typed name that matches nothing still works: the
server resolves it and answers `principal_not_found` if it does not exist.

### The notes-scoped suggestion endpoint

The desktop, iOS and Android apps ask one endpoint instead:
`GET /api/notes/<id>/shares?query=…` (owner only, `app/src/notes.py`
`_suggestions`). It answers the usual `shares` plus `suggestions`, a list of
`{type, name}` built by the same rules as above: active accounts from the
shared `search_accounts` (on `_WorldCommunityRuntime` in `entry.py`), the
caller's own organizations containing the text, the caller and principals
already shared left out, prefix matches first, at most 8. An empty query
answers no suggestions without searching; a query over 63 characters is
`invalid_query`.

It lives under `/api/notes` because the desktop without a password session
signs each request with the account key, and that signature is scoped to
`/api/notes` (`docs/desktop/notes.md`); the chat user search would refuse it.
Filtering on the server also means every client leaves out the same people.

## Who an organization share reaches

A share with an organization reaches its insiders only: owners, admins,
maintainers, members and auditors (`ORG_INSIDER_ROLE_SQL` in
`app/src/organization_rbac.py`). Guests are outside collaborators and get
nothing from it, neither in the list nor by id. An auditor is read-only, so an
`editor` share opens the note for an auditor as `viewer`. `_role` and `_list`
in `app/src/notes.py` apply both rules, so every note route (read, edit,
versions, activity, the live socket) agrees.

## The access link

A shared note is private: the published `/notes/<id>` page only reads public
notes and sends no session, so it cannot open a note shared with you. Once a
note is shared with at least one person, the Share panel shows **Link for
people with access**, `/dashboard/notes?note=<id>`, with **Copy link**. The
**⋯** menu has **Copy link** too, so anyone the note is shared with (not only
the owner) can pass the same link on.

Opening that link selects the named note after the list loads. A note you
cannot open shows the error above the editor and the page falls back to your
first note. Selecting a note rewrites the
address bar's `?note=` (with `history.replaceState`, so Back is not flooded),
so the address bar is always a link to the open note.

Publishing to the web is unchanged and separate: its own link and Copy link
appear under **Publish to the web**.

## Tests

`app/tests/test_notes.py` covers the suggestion endpoint: matching users and
organizations, the owner and already-shared people left out, prefix order,
empty and over-long queries, and non-owners refused.

`app/browser_tests/tests/dashboard-notes-share.spec.js` drives the built page
with mocked APIs: suggestions for users and organizations, keyboard picking,
already-shared principals left out, the access link row and copy, and opening
`?note=<id>`.

## The same features on every platform

The web page, the desktop (`docs/desktop/notes.md`), iOS and Android
(`docs/native/notes.md`) offer the same Notes features: filters with counts
and title-and-body search (`GET /api/notes?query=`, `docs/app/notes-convert.md`),
autosave, write and preview with a format bar, Keep mine / Take theirs on a
save conflict, the share panel with suggestions and role changes, the link for
people with access, opening `/dashboard/notes?note=<id>`, publishing with a
copyable public link, version history, linked issues and pull requests,
Markdown export, delete, and turning a note into a task or an issue.

Known differences:

- Live presence and inline comments run everywhere
  (`docs/app/notes-comments.md`). The web and phones use the note's socket;
  the desktop does not link QtWebSockets, so it polls
  `/api/notes/<id>/presence` every 5 seconds instead.
- The desktop editor has Write and Preview tabs but no split view; phones
  have no room for one.
- The desktop autosaves Cloud-only notes. Local and Local + cloud notes save
  with the Save button, because each local save adds an on-disk snapshot.
- On iOS the format bar adds text at the end of the note: the text editor
  cannot report the selection before iOS 18.
- Local (on-disk) notes are a desktop feature.
