# Username autocomplete

Every web field that takes a ForkMesh username suggests matching accounts as
you type. The user asked for it after inviting people to channels by typing
exact names from memory: "make sure usernames are always auto-complete".

## The shared picker

`app/public/username-autocomplete.js` defines `window.ForkMeshUsernames` and
binds one document-level listener set, so any `<input>` that carries
`data-username-autocomplete` gets a suggestion list, including inputs rendered
later by the dashboard bundle:

- `data-username-autocomplete="single"`: the whole value is one username
  (a leading `@` is ignored). Picking a name replaces the value.
- `data-username-autocomplete="list"`: comma-separated usernames. The name
  under the caret is completed and `, ` is appended for the next one. Names
  already in the list are not suggested again.
- `data-username-exclude="a,b"`: names never suggested (current members).
  The chat invite field and the repository collaborator field refresh it
  each time their member list renders.

ArrowUp/ArrowDown move, Enter or Tab pick, Escape closes without closing a
surrounding `<dialog>`. The list is appended inside the input's `<dialog>`
when there is one, because a modal dialog sits in the top layer and makes the
rest of the page inert. It is `position: fixed` and corrects its own offset
if an ancestor turns out to be its containing block. The list is hidden when
its only suggestion is exactly what is already typed, so Enter submits the
form. Styles are injected once (`#fm-username-styles`), reading the chat
page's variables and the dashboard's `--dashboard-*-rgb` roles under
`[data-dashboard-theme]`.

## Where the names come from

`ForkMeshUsernames.search(query)` calls the signed-in account search,
`GET /api/chat/direct-messages/users?query=` (prefix matches first, 20 at
most, the caller left out), the same endpoint the direct-message and
organization member pickers already used. Each query is fetched once per page
load. Results, and any names a page hands to `remember()` (chat passes the
public account directory), go into one known-names set that
`lookup(query, onUpdate, exclude)` ranks synchronously: prefix matches
shortest first, then names that only contain the query, eight at most, never
the signed-in account. `onUpdate` fires once when the search for a new query
answers. An empty query returns nothing.

## Fields and pickers using it

| Place | Field |
| --- | --- |
| Chat, channel settings, Members | `#chat-channel-username` (single, current members excluded) |
| Chat, New channel | `#chat-channel-initial-members` (list); `channelInviteeNames` splits on commas and spaces |
| Repository settings, Collaborators | `[data-repo-collaborator-name]` (single, current collaborators excluded) |
| Security advisory, credit and collaborator | `name="user"` (single) |

The existing @mention pickers keep their own lists and add account search
results after their local people:

- chat composer (`chat.js` `mentionCandidates`) and the dashboard chat panel
  (`dashboard-chat.js`): roster and directory first, then any account;
- issue, pull request, discussion and release editors
  (`06-repo-content.js` `buildMentionCandidates`): people on the record (tier
  0), the repository roster (tier 1), then any account (tier 2).

Not changed: the organization member field already searched the same endpoint
through a `<datalist>`; team members, calendar attendees, Notes sharing and
task @mentions pick from the organization, the invitees or the task board on
purpose (task Pings reach members only, `task-pings.md`). The World page's
media moderator field runs on its own origin and session and does not load
the helper. The desktop app's invite dialogs already search people
(`pickChatPeople`); the phone apps have no channel invite field.

The script is a cache-busted asset (`dashboard_shell.CACHE_BUSTED_ASSETS`),
loaded by `chat.html` and the dashboard shell, and proxied by the www worker
for the profile page like `user-status.js`. Tests:
`app/tests/test_username_autocomplete.py`.
