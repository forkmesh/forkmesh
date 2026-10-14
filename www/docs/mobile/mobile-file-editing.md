# Editing files and opening pull requests from the phone

From the repository browser in the iOS or Android app, a person can edit one
text file and propose it as a pull request with no clone, no fork and no push
access. Related: [mobile node contract](mobile-node-contract.md).

## Why no fork is needed

A ForkMesh pull request is a signed patch, not a pointer to a branch. The
relay verifies only the author's Ed25519 signature over title, base, head,
patch and commits, plus size, inbox depth and per-author quota. The receiving
node merges from the carried patch (`git apply --index --3way`, or `git am
--3way` when commits are included) and never resolves the head branch. So a
contributor needs nowhere to push. A fork on ForkMesh is a durable copy you
keep, not a gate in front of an edit button.

## Every edit opens a pull request

Even with write access, the phone opens a pull request, so nothing merges
unreviewed from a phone. Round one is one file per pull request; multi-file
work goes through a clone.

## Building a patch without a clone

A Git blob id is `sha1("blob " + <byte length> + "\0" + <bytes>)`, so the phone
can compute both sides of an `index` line:

1. Fetch the file's raw bytes for the ref being browsed.
2. `oldId` from those bytes, `newId` from the edited text as UTF-8.
3. Emit one `diff --git` section with `index <oldId>..<newId>` and Myers hunks.
4. Sign and submit.

Rules that keep the patch valid:

- Diff the raw bytes, never a decoded string, so `oldId` is the real blob id
  for the three-way fallback.
- If the server supplies a blob id and it disagrees with the computed one,
  refuse and offer the clone path.
- Omit the mode on the `index` line; the tree API does not expose modes, and
  guessing `100644` would strip an executable bit.
- Refuse bytes that are not valid UTF-8 rather than decoding them lossily,
  because the decoded text becomes the new file content.

## Submission

```json
{"pull": {"title": "…", "base": "main", "head": "<account>:mobile-edit/<label>",
  "patch": "diff --git …", "commits": "", "description": "…",
  "author": "<node pubkey>", "authorName": "<account>", "ts": 0, "sig": "…"}}
```

`base` is the branch being browsed. `head` is informational. `commits` is
empty because there are no local commits. The signature covers
`"\x00".join([title, base, head, patch, commits])` under
`forkmesh-pull-event-v1`. The relay assigns the number outside the signed
bytes.

## Limits

| Condition | Behaviour |
| --- | --- |
| Binary or non-UTF-8 content | Refused, clone path offered |
| File over 512 KB | Refused, clone path offered |
| Served blob id disagrees | Refused, clone path offered |
| Inbox full (`429 inbox_full`) or author quota (`429 author_quota`) | Shown as "not accepting pull requests right now" |
| Upstream moved while editing | Submitted; `--3way` absorbs drift, real conflicts surface at merge |

Because a single-file edit carries no commit mbox, the owner merges it under
their own name and the relay records the contributor.

## Forks from the phone

A phone cannot host, but it can publish. A fork is a clone published as
`<me>/<name>` with `forkedFrom` set in the signed catalog record. The field is
omitted, not empty, when a repository is not a fork, so older catalog
signatures stay valid.

Not built yet: a push-access probe (the signed `info/refs?service=git-receive-pack`
request answers it: `200` yes, `401` no, `503 push_unavailable` unknown),
multi-file edits, file creation and deletion, and a fork button.
