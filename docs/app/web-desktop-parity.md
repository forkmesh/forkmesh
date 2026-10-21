# Web and desktop parity

What a signed-in person can do in the browser, with nothing installed, and
what still depends on the desktop app (or another node the person runs). Links
to download the apps are not dependencies and are not listed. This page is the
inventory; each row says why the dependency exists and how it could go.

## Done on the web

| Feature | Notes |
|---|---|
| Discussions: start, reply, mark answer, close/reopen, archive, delete a reply | [web-discussions.md](web-discussions.md) |
| Add file (create a file, or propose one as a pull request) | [repository-file-edits.md](repository-file-edits.md#add-file) |
| Issues and pull requests (browser-signed events), file edits and proposals, forks, repository deletion, branch operations on core-hosted repositories, webhooks, Actions secrets, security advisories, notes, tasks, calendar, docs, organizations, accounts and SSH keys | already had web paths |

## Still desktop-dependent

Effort: S = copy or wiring over an existing route, M = a new web flow over an
existing route, L = a new route, event type or storage, XL = architectural.

| Feature | Where | Why | Proposed design | Effort |
|---|---|---|---|---|
| Merging pull requests on personal (non-organization) repositories | `loadRepoPullMergeAuthorization` (06-repo-content.js); `_organization_owner_can_merge_repo` (entry.py) | Deliberately closed: the web merge executor is only enabled for organization owners until a merge policy for personal repositories is agreed | Accept the personal owner when `_account_owns_node(actor, owner)` and the branch rules gate passes; keep the audit entry; the dashboard check adds `isRepoOwner(repo)` | S-M (policy decision first) |
| Deleting a whole discussion | DiscussionStore::deleteDiscussion | Destructive, owner-only, needs the working tree | An owner-only signed `delete` with target `discussion`, removed by the mirror intake and the desktop | M |
| Volunteering a node to mirror an external listing | `volunteerForExternalMirror` (05-repo-list-explorer.js) | The point is to lend a node; the browser is not one | Offer "Import and host here" beside it for supported forges, through the hosted-repository import | M |
| New repository from a local folder | new-repo view | Reads the local file system | Inherent; the web offers empty repositories and imports instead | n/a |
| Private repositories on a forge without core hosting | `hosted_git.py` (`private_hosted_unsupported`) | Private content is sealed with the owner's key, which the relay never holds | Core-hosted private repositories already work; elsewhere this needs key escrow or a browser-held key | XL |
| Issue, pull and discussion submissions on private repositories | `app_runtime.py` routes return 404 for every POST | The relay's side effects (federation, provider mirrors, mentions) are public-facing | Accept POSTs from readers (`_hosted_read_allowed`) and skip every public side effect for private repositories | L |
| Visibility change, rename and transfer | repository settings | The desktop republishes a signed catalog record | Session-authorized catalog update route for core-hosted repositories, mirrored by the owner node on next sync | L |
| Collaborators on private repositories | `shares_handler` (owner signature) | Shares are sealed to the recipient's key by the owner's desktop | Session path for core-hosted repositories (no sealing needed there) | L / XL |
| Projects (create and edit boards) | Projects tab | Events are signed with the node identity and committed locally (ProjectStore) | Browser-signed project events through `fetchRepoMutation`, materialized by the mirror intake like issues | L |
| Releases (create, publish, assets) | releases list | Owner-signed `release.json` and asset uploads | Browser-signed release manifest for core-hosted repositories, assets through the asset store | L / XL |
| Tags (create, delete) | branch operations | No tag operation exists | Add tag operations to the branch-operation queue | L |
| Deleting and renaming files, binary uploads | file editor | `/file-edit` creates and edits text only | Add `delete` and `rename` modes (one tree rewrite) and base64 content | M |
| Repository settings only stored on the desktop (peer approval, secret-scanning push protection, Actions auto-approve, agent guardrails) | repository settings | Kept in the desktop's local record | Store them in `.forkmesh/settings.json` through `/file-edit` and read them from there on every client | L |
| Branch operations and issue/pull state on repositories held only by a personal node | branch queue, inbox drains | The owner's node applies them when it next syncs | Already immediate on core-hosted and mirrored repositories; otherwise inherent to node-held repositories | XL |
| Agent transcripts and prompts | Agents tab | Encrypted to a key in the owner's desktop vault | Use the live desktop link when it is online; offline reading needs a web-held key | S (online) / XL |
| Actions runs that ran on the owner's machine | Actions tab | The runs stay on that machine | Read them through the live desktop link when it is online | M |
| Microjob reward payouts | microjobs view | Uses the owner's local wallet | Out of scope for the web (financial) | n/a |

When one of these moves to the web, move its row to "Done on the web" and
link the page that describes it.
