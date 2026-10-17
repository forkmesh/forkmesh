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

`app/browser_tests/tests/dashboard-notes-share.spec.js` drives the built page
with mocked APIs: suggestions for users and organizations, keyboard picking,
already-shared principals left out, the access link row and copy, and opening
`?note=<id>`.
