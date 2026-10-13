# Editing files and opening pull requests from the phone

Status: `IMPLEMENTED` for Tier 1 (2026-09-20). A person browsing a repository
in the iOS or Android app can edit one text file and propose it as a pull request,
with no clone, no fork and no push access. Tiers 2 and 3 are not built; see
[Delivery](#9-delivery) for what was deliberately deferred.

This document describes that flow and why it is shaped differently here than
on GitHub.

Companion documents: [`mobile-node-contract.md`](mobile-node-contract.md),
[`owned-git-engine-spec.md`](owned-git-engine-spec.md),
[`feature-parity-audit.md`](feature-parity-audit.md).

---

## 1. The finding that shapes this design

The obvious design is GitHub's: if you can write to the repository, edit it
directly; if you cannot, fork it, edit the fork, and open a pull request from
the fork. That shape exists because a GitHub pull request is **a pointer to a
branch in a repository you must be able to push to**. Without a fork there is
nowhere to put the commits, so the fork has to come first.

A ForkMesh pull request is not a pointer. It is **a signed patch**, and it
carries the change inside the submission:

- `app/src/events.py:277` — `verify_pull_event` checks only the author's own
  Ed25519 signature over `title`, `base`, `head`, `patch` and (in the current
  five-field form) `commits`. There is no write-access check, no membership
  check, and no check that the head branch exists anywhere.
- `app/src/entry.py:36239` — the `POST /api/repo/<owner>/<repo>/pulls` handler
  gates on signature, byte size (`MAX_PULL_BYTES`, 100 MB), inbox depth
  (`MAX_PENDING_PULLS`, 200) and a per-author quota. Nothing else.
- `desktop/src/MainWindowPulls.cpp:6975` — when the desktop submits to a
  repository it does not own, it labels the head `<node>:<branch>` and records
  the reason in a comment: *the owner merges from the carried patch/commits,
  never by resolving head.* The head name is informational on the receiving
  side.
- `desktop/src/PullStore.cpp:3067` — a pull request that is not branch-backed
  is merged with `git apply --index --3way`, or `git am --3way` when the
  submission carried a `commits` mbox.

**Forking is therefore not a precondition for contributing.** The half of the
GitHub flow that says "no access, so fork first" is a habit imported from a
different protocol, and it happens to be the half a phone is worst at: it
demands a full clone, a hosted copy, and a second published repository before
anyone can fix a typo.

Dropping it removes the hardest piece of the feature and leaves a better one.

### What forking is actually for here

A fork on ForkMesh is a **durable, addressable copy you keep**: somewhere to
land many changes over time, a link other people can clone, a place to publish
work that is not ready to propose. That is a real thing to want, and §6
describes how the phone does it. It is simply not a gate that belongs in front
of an edit button.

---

## 2. What the phone already has

| Piece | Where |
| --- | --- |
| Read-only remote browse | `RepoFileScreen` — `native/ios/ForkMesh/Features/Code/RepoFileScreen.swift`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/code/RepoFileScreen.kt` |
| Clone to device, capped at 96 MB | `native/ios/ForkMesh/Features/LocalRepos/CloneToDeviceButton.swift`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/localrepos/CloneToDevice.kt`; `GitWorkspace.maxCloneBytes` |
| A real file editor over a local workspace | `LocalFileEditorScreen` — `native/ios/ForkMesh/Features/LocalRepos/LocalFileEditorScreen.swift`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/localrepos/LocalFileEditorScreen.kt` |
| Stage, commit, branch, push, fetch, publish | `GitWorkspace`, `PublishService` — `native/ios/ForkMeshCore/Sources/FMCode/Local/`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/` |
| Unified-diff builder with `index` lines for three-way apply | `PatchBuilder` — `native/ios/ForkMeshCore/Sources/FMDiff/PatchBuilder.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/PatchBuilder.kt` |
| Pull request prepare and submit | `PullRequestService` — `native/ios/ForkMeshCore/Sources/FMCode/Local/PullRequestService.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/PullRequestService.kt` |
| Signed submission over the wire | `InboxService.submitNewPull` — `native/ios/ForkMeshCore/Sources/FMApi/Services/InboxService.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/api/InboxService.kt` |
| A dialog that asks the human to paste a unified diff | `showNewPullDialog` — `native/ios/ForkMesh/Features/Code/Collab/RepoPullsTab.swift`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/code/collab/Composers.kt` |

When this design was written, most of the machinery existed, but
`PullRequestService` was reachable only by the coding agent and by nothing a
human could tap.

### Three gaps (closed by §9)

1. **`PullRequestService.submit` pushes the branch before submitting and hard
   fails when the push is rejected**. On
   a repository the person cannot push to, that fails before the pull request
   is attempted at all. The push has to become conditional.
2. **`submitNewPull` signs the four-field legacy canonical**
   and never sends `commits`, `description` or
   `authorName`. Every pull request opened from the phone today therefore
   arrives with an empty body and loses the contributor's commit authorship and
   message, because the owner merges it with `git apply` rather than `git am`.
   This is worth fixing on its own merits, independently of file editing.
3. Nothing tells the app whether the signed-in account can write to
   `owner/repo`, and there is no human-reachable "create pull request" action.

---

## 3. Three tiers, and what ships first

| Tier | What it is | Needs a clone |
| --- | --- | --- |
| 1 | Quick edit: change one file from the repository browser and propose it | No |
| 2 | Clone and work: the existing local workspace, with a pull request action | Yes |
| 3 | Fork: keep a published copy of your own | Yes |

**Round one is Tier 1, one file per pull request.** Tier 2 is mostly plumbing
on top of what exists. Tier 3 reduces to a single catalog field (§6).

Two decisions constrain the rest of this document:

- **Every edit opens a pull request, including when the person has write
  access.** Write access does not change the flow; it only means the branch can
  also be pushed so the pull request is fetchable. One code path, and nothing
  merges unreviewed from a phone.
- **One file per pull request to start.** The common case is a typo, a README
  line, a one-line patch. Multi-file work goes through a clone, where a real
  index and a real status view already exist.

---

## 4. Building a patch without a clone

A git blob id is `sha1("blob " + <byte length> + "\0" + <bytes>)`. Nothing
about that needs a repository, so the phone can compute both sides of an
`index` line from bytes it already has. That is what makes Tier 1 possible:

1. Fetch the file's **raw bytes** for the ref being browsed.
2. `oldId` = sha1 of those bytes in blob form. `newId` = sha1 of the edited
   text encoded as UTF-8.
3. Emit one `diff --git a/<path> b/<path>` section with an `index
   <oldId>..<newId>` line and hunks from the Myers differ already in
   `PatchBuilder`.
4. Sign and submit it as a pull request.

No clone, no 96 MB ceiling, works against a repository of any size, and the
round trip is seconds rather than minutes.

### Four rules that keep the patch valid

**Diff the raw bytes, not the decoded string.** `git apply --3way` resolves the
`index` ids to reconstruct the pre-image; if `oldId` is not the *real* blob id,
the three-way fallback has no anchor and the patch either applies by luck or
fails confusingly. `RepoBlob.content` is a `String` with a separate `encoding`
field, so it may have been decoded or
normalized in transit. The base side must come from the raw endpoint —
`ApiService.rawBytes`.

**Cross-check against the served blob id when there is one.**
`RepoTreeEntry.sha` carries the blob id when the serving
node supplies it. When it is present and disagrees with the computed id, the
bytes are not the bytes git has. Refuse the quick edit and offer the clone path
instead of submitting a patch with a wrong pre-image. A clean refusal beats a
silent corruption.

**Omit the mode suffix on the `index` line.** `PatchBuilder.between` always
writes one, which is correct when it read the mode out of a
real tree. The tree API does not expose file mode, so the clone-free path does
not know it. `index <old>..<new>` with no mode is valid git for a modification
and asserts nothing — which is exactly right. Writing `100644` on a guess would
silently strip the executable bit from any script edited on a phone.

**Refuse bytes that are not valid UTF-8 rather than decoding them lossily.**
This rule was not in the first draft of this design; it surfaced in
implementation and is the sharpest of the four. `PatchBuilder.between` decodes
lossily, which is safe there because the decoded text is
only ever *compared* and git applies the result against blobs it holds. In the
clone-free path the decoded text **becomes the new file content**, so a byte
that U+FFFD stood in for would be written back as U+FFFD and silently corrupt
the file. Refusing sends the person to the clone path, where the bytes survive
round-tripping.

### Shape of the change

`PatchBuilder` gains a sibling to `between()`: `forFile`, in
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/PatchBuilder.kt`
and, on iOS, `LocalPatch.forFile` in
`native/ios/ForkMeshCore/Sources/FMCode/Local/PullRequestService.swift`:

```swift
public static func forFile(
    path: String,
    beforeBytes: [UInt8],
    afterText: String,
    oldId: GitObjectId,
    newId: GitObjectId,
    context: Int = PatchBuilder.defaultContext
) -> PatchBuildResult
```

The per-file half of the diff is already parameterized on a blob id and
mode, which is trivially constructible from a computed id, so this is an
extraction rather than a rewrite. Binary detection, the trailing-newline
handling and the edit-distance ceiling all come along unchanged.

---

## 5. Write access, and why it does not gate round one

The relay authorizes a push when the presented key is the owner account's
primary key, **or** an enabled `owner_sign` device carrying `publish_repo`,
**or** the key of an account holding org write-or-better permission on a linked
repository (`app/src/entry.py:46715`, `_org_write_allowed` at
`app/src/entry.py:25545`). The third case cannot be computed on the phone.

Because every edit opens a pull request and Tier 1 has no branch to push,
**round one does not need to know the answer at all.** The question returns in
Tier 2, where pushing the branch makes the pull request fetchable.

When it does return, the client should not model the rule — it should ask. The
signed advertisement request is already the authorization oracle:

```
GET /{owner}/{repo}/info/refs?service=git-receive-pack
X-ForkMesh-Pubkey / X-ForkMesh-Ts / X-ForkMesh-Sig
```

It runs through the same `_git_push_authorize` with the empty-body digest
(`app/src/entry.py:46952`). `200` means the push would be accepted, `401` means
it would not, and `503 push_unavailable` means the mesh has no push target
right now — which is **not** a permission answer and must not be shown as one.
No new server code, and the result is the truth rather than an inference that
can drift from the rule it copies.

---

## 6. Forking, as the phone can do it

A mobile node cannot host: `mobile_node` capabilities are
`browse,comment,submit_issue,submit_pr,publish_repo,owner_sign` — no
`host_repo`, no `mirror_repo` (`mobile-node-contract.md:37`). It can publish,
and a first push lands on the configured hosted write endpoint. So a fork from
the phone is not a new mechanism at all:

> clone the upstream → publish as `<me>/<name>` with `forkedFrom` set to
> `<upstream owner>/<name>` in the signed catalog record.

The relay already normalizes that field, refuses a self-referential edge, and
indexes it into the parent's fork list (`app/src/catalog.py:388-394`). The
record it hashes must match byte for byte, so the field has to be **absent, not
empty**, when the repository is not a fork — an empty string would change the
canonical JSON and break catalog-v2 signatures from older clients.

That is one optional parameter threaded through `buildCatalogRecord`
(`PublishService`). It is worth landing on
its own, ahead of any fork button, so an already-published clone can be
correctly marked.

The relay's own fork endpoint (`POST /api/repo/<owner>/<repo>/forks`) requires
a linked headless managed mirror and returns `409 headless_mirror_required`
otherwise. It is the right path for the website and the wrong one for a phone;
publish directly instead.

---

## 7. Wire shape of a Tier 1 submission

```json
{
  "pull": {
    "title":       "Fix the install command in the README",
    "base":        "main",
    "head":        "kaif:mobile-edit/readme-md-0919-1430",
    "patch":       "diff --git a/README.md b/README.md\nindex a1b2…..c3d4…\n…",
    "commits":     "",
    "description": "Typed on a phone.",
    "author":      "<node pubkey, base64url>",
    "authorName":  "kaif",
    "ts":          1758290000000,
    "sig":         "<Ed25519 over the canonical>"
  }
}
```

- **`base`** is the ref being browsed. `_CodeTab` only ever switches among
  branch names from `_BranchSelector`, so this is always a branch name and
  never a bare commit id — which matters, because the owner's node merges into
  it.
- **`head`** follows the desktop's cross-node convention: the account name, a
  colon, then a generated label. There is no branch behind it and none is
  needed; the receiving node never resolves it.
- **`commits`** is empty in Tier 1 — there are no local commits to format. It
  is populated in Tier 2, where the work happened on a real branch.
- The canonical is the five-field form,
  `"\x00".join([title, base, head, patch, commits])`, hashed and signed under
  `forkmesh-pull-event-v1` (`app/src/events.py:292-306`).

The relay assigns the number after verifying the signature, deliberately
outside the signed bytes, so the owner's node can renumber on a collision
without invalidating anything (`app/src/entry.py:36332-36340`).

---

## 8. Limits and failure modes

| Condition | Behaviour |
| --- | --- |
| Binary file, or content that sniffs as binary | Named as such in the editor, with the clone path offered |
| Bytes that are not valid UTF-8 | Same: refused rather than decoded lossily (§4) |
| File over 512 KB | Refused, with the clone path offered |
| Served blob id disagrees with the computed one | Refuse, explain, offer the clone path (§4) |
| Signed out | `requireAccount`, as every other write action already does |
| Private repository | Already a 404 on the pulls endpoint for non-owners; browse does not reach it either |
| Upstream moved while the person was typing | Submitted anyway. `--3way` absorbs ordinary drift; a genuine conflict surfaces to the owner at merge, the same exposure the GitHub web editor carries |
| Repository inbox at `MAX_PENDING_PULLS` (200) | `429 inbox_full` — surface it as "this repository is not accepting pull requests right now", not as a failure of the edit |
| Author over per-repo quota | `429 author_quota` — same treatment |

Two things this design makes newly relevant. Patch size is a non-issue
(`MAX_PULL_BYTES` is 100 MB), but the **inbox depth and per-author quota** were
sized for desktop nodes; making contribution cheap on a phone will reach them
sooner. And a repository whose owner node never drains its inbox will swallow
submissions silently — the pending state deserves to be visible in the UI
before this feature is widely used.

---

## 9. Delivery

What shipped, in dependency order:

1. **The full pull request submission.** `submitNewPull` gained `commits`,
   `description` and `authorName`, and signs the five-field canonical. Stands
   alone; fixes gap 2 for the agent flow and the paste-a-diff dialog as well.
2. **Blob ids from bytes** — `BlobId` (`native/ios/ForkMeshCore/Sources/FMCode/Local/BlobId.swift`,
   `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/BlobId.kt`), with
   `servedBlobIdAgrees` for the cross-check. Vectors are `git hash-object`
   output.
3. **Single-file diffing** — `PatchBuilder.forFile`, with the no-mode `index`
   line. Held to the suite's existing standard: every generated patch is fed
   to real `git apply`, including a test that builds a `100755` file and
   asserts the bit survives the round trip.
4. **`ApiService.rawBytes`** — the file's exact bytes, uncached, since a blob
   id computed from a round-tripped `String` is an id git disagrees with.
5. **The quick editor** — `QuickEditScreen` (`native/ios/ForkMesh/Features/LocalRepos/QuickEditScreen.swift`,
   `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/code/QuickEditScreen.kt`), reached
   from an Edit action in `RepoFileScreen`. The Edit action is offered on path
   alone so it can sit in the app bar without waiting on the blob; the editor
   itself refuses binary, non-UTF-8, oversized and disagreeing content once it
   has the bytes.
6. **`forkedFrom` through `buildCatalogRecord`** (§6). This went one step
   past plumbing: the information was already on hand, since a cloned record's
   `publishedAs` names the upstream until the publish rewrites it, so
   publishing a clone under your own account now records the parent without
   any new UI.

Deferred, deliberately: the push-access probe (§5), the "create pull request"
action in `LocalRepoScreen`, multi-file quick edits, file creation and
deletion, and a fork button.

### Known limits of what shipped

- **A Tier 1 pull request carries no `commits` mbox**, because a change
  composed without local commits has none to format. The owner therefore
  merges it with `git apply --index --3way` under their own name, and the
  contributor is recorded by the relay rather than by git. Change 1 fixes this
  for the flows that *do* have commits; Tier 1 cannot, and an mbox writer in
  `PatchBuilder` is the work that would close it.
- **`InboxService` calls `http.post` directly** rather than through an
  injectable client, so the submission leg has no test double. The patch it
  submits is covered against real git, and the screen either side of it is
  covered, but the wire call itself is not. Making the client injectable —
  as `ApiService` already is — would close that gap.

`showNewPullDialog`, which asks a person to
paste a unified diff into a phone, is now redundant for the single-file case.
Retiring it belongs in its own change, not folded into a feature.

---

## 10. Evidence

| Claim | Where to check |
| --- | --- |
| A pull request is verified only against its author's signature | `app/src/events.py:277` |
| Submission gates are signature, size, inbox depth, author quota | `app/src/entry.py:36239`, `:557`, `:577`, `:578` |
| Cross-node pull requests carry the patch; head is informational | `desktop/src/MainWindowPulls.cpp:6975` |
| A non-branch-backed pull request merges by patch | `desktop/src/PullStore.cpp:3056-3067` |
| Branch-backing is decided by the receiving node, not the sender | `desktop/src/PullStore.cpp:2705-2745` |
| Push authorization rule | `app/src/entry.py:46715`, `:25545` |
| The advertisement GET runs the same authorization | `app/src/entry.py:46952` |
| `forkedFrom` is normalized and indexed by the relay | `app/src/catalog.py:382-394` |
| A mobile node may publish but never host | `www/docs/mobile/mobile-node-contract.md:37` |
| Mobile signs the five-field canonical | `native/ios/ForkMeshCore/Sources/FMApi/Services/InboxService.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/api/InboxService.kt` |
| Clone ceiling on mobile | `GitWorkspace.maxCloneBytes` in `native/ios/ForkMeshCore/Sources/FMCode/Local/GitWorkspace.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/localrepos/GitWorkspace.kt` |
