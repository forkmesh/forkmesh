# Forks on other instances and cross-instance pull requests

Part of [the App docs](README.md). Added 2026-10-10: the user asked to send
pull requests across ForkMesh instances by entering the other copy's address
and saving it as a "fork".

A repository can list copies of itself on other ForkMesh instances (for
example a white-label instance and forkmesh.com, [../INSTANCES.md](../INSTANCES.md)).
From the new pull request form, a branch here can be proposed to one of those
copies. Nothing is cloned or fetched across instances: the pull request is
the same signed, self-contained patch a fork already sends to its parent.

## Code map

| Part | File |
| --- | --- |
| Address parsing, list, save, remove | `app/src/remote_forks.py` (`parse_url`, `handler`, `list_forks`) |
| Route | `REPO_REMOTE_FORKS_RE` in `app/src/urls.py`, dispatched in `app/src/app_runtime.py` next to `/forks` |
| Table | `repo_remote_forks` (`app/migrations/0252_repo_remote_forks.sql`, `app/src/schema.py`) |
| Cross-origin delivery to the target | `_open_pull_cors_headers` in `app/src/entry.py` |
| Fork menu section | `data-repo-remote-forks` in `08-repo-detail-network.js`; `loadRepoRemoteForks`, `renderRepoRemoteForks`, `saveRepoRemoteFork` in `06-repo-content.js` |
| "Send to" in the new pull request form | `fillPullComposeTargets`, `pullComposeRemoteTarget`, `handleRemotePullSubmit` in `07-repo-compose-branch.js` |
| Signing and delivery | `signWebPull`, `submitRemotePullOpen` in `03-issues-profile-io.js` |
| Tests | `app/tests/test_remote_forks.py` |

## The saved list

`GET|POST|DELETE /api/repo/<owner>/<repo>/remote-forks`.

- **GET** is public for a public repository: `{forks, canManage, max}`. Each
  fork is `{url, origin, host, owner, name, label, addedBy, createdAt}`.
  `canManage` is true for a caller who may change the list. A private
  repository answers 404, as `/forks` does.
- **POST** `{url, label?}` saves an address; saving it again updates the
  label and keeps its place. **DELETE** `{url}` (or `?url=`) removes it. Both
  need a session (bearer or cookie) of the account that owns the backing node
  (`_account_owns_node`) or an owner of the organization the repository is
  linked to (`_org_owner_of_linked_repo`), the same people who may edit the
  repository's About.
- `parse_url` takes `https://host[:port]/<owner>/<repo>[.git][/anything]`
  (a missing scheme means `https://`) and keeps the first two path segments.
  It refuses other schemes, credentials in the URL, IP literals, names
  without a dot (`localhost`), owner or repository names outside
  `[A-Za-z0-9._-]` or starting with `.`, whitespace and addresses over 300
  characters. An address on this instance (the request's origin,
  `APP_ORIGIN` or `PUBLIC_BASE_URL`) is refused with `same_instance`: the
  Fork button makes a copy here.
- Up to 20 per repository (`MAX_FORKS`, `too_many_forks`).
- Rows are keyed by `blind_index("remote-forks:<public owner>/<repo>")` and
  `blind_index("remote-fork-url:<url>")`. `data` is plain JSON: the addresses
  are shown to everyone. The owner is the public one
  (`_public_repo_owner`), so an organization alias and its backing node share
  one list, as stars and fork edges do.

## Sending a pull request

1. The form loads the saved list and, if there is one, shows **Send to**:
   this repository, or each saved fork.
2. With a fork chosen, `submitRemotePullOpen` runs the same comparison as
   any web pull (`signWebPull`: the live mirror's `compare` for base and
   head, here), so the base branch must exist here too. Both copies need to
   share history for the patch to apply on the other side.
3. It signs with the target's name: `webEventSigningPrefix("pull-event",
   {owner, name})` uses the other instance's owner and repository, which is
   what `verify_pull_event` there checks (`_event_signing_repo_keys`).
4. `head` is `<this host>/<owner>/<repo>:<branch>`, so the target shows
   where the change came from. The pull view reads everything before the
   last `:` as the source; a value with a `/` is shown as it is instead of
   `<owner>/<this repo's name>`.
5. The browser posts `{owner, repo, pull}` straight to
   `<origin>/api/repo/<owner>/<repo>/pulls` with `credentials: "omit"`. On
   success the form links to `<url>/pulls/<number>` there.

### Why the browser posts directly

- **CORS on the target.** `_cors_headers` only allows configured origins.
  For `POST /api/repo/<o>/<r>/pulls` alone (exactly `REPO_PULLS_RE`, not
  merge, update or rules), `_open_pull_cors_headers` adds
  `Access-Control-Allow-Origin: *` without `Allow-Credentials`, and its
  preflight allows only `POST` with `accept` and `content-type`. This is safe
  because a new pull submission does not use cookies or sessions: it is
  authorized by its Ed25519 signature, and any node could already send the
  same request with `curl`. `*` also means a browser refuses a credentialed
  request, so no session is ever attached.
- **No server relay.** The app could relay the submission, but the egress
  guard refuses `forkmesh.com` from an instance
  (`app/host/forkmesh_host/egress.py`, production rule), so instance to
  forkmesh.com would not work.

An instance that runs an older version answers the preflight with 403, which
the browser reports as a network error; the form then says the other
instance could not be reached or does not take pull requests from other
instances yet (`remote_unreachable`).

## Limits

- Only outgoing: the list names where this repository's pull requests can
  go. The other instance does not learn about it, and its own list is kept
  there.
- The other side's base branch is assumed to match this side's branch of the
  same name. If it moved, the patch is reviewed against an older base, as
  with any fork pull request.
- Comments and reviews on the pull request happen on the target instance.
