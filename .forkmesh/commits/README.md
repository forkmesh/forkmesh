# ForkMesh commit conversations

This folder holds **per-commit comment threads**, stored inside the repository
so they travel with every clone and mirror — exactly like `.forkmesh/issues/`
and `.forkmesh/pulls/`. The desktop client's commit view reads and writes these files; the
relay only relays signed comments from people who don't have write access (the
owner drains the inbox and commits them).

## Layout

One folder per commented commit, named by the commit's **full 40-char hash**.
Each comment is its own append-only, numbered markdown file with frontmatter:

```
.forkmesh/commits/
  <full-sha>/
    0001-comment.md
    0002-comment.md
    ...
```

## `.forkmesh/commits/<sha>/NNNN-comment.md`

```md
---
type: comment
id: <uuid>
commit: <full-sha>
author: <pubkey-b64url>   # raw 32-byte Ed25519 key, base64url
authorName: node1
ts: 1781600000000         # epoch ms
sig: <base64url>
---

The comment body in **markdown**.
```

## Signatures

Raw **Ed25519** over an explicit canonical string (not canonicalized JSON, to
avoid C++/Python serialization drift), encoded base64url without padding —
matching the client's `ForkMeshIdentity` and the worker's
`verify_commit_comment_event`.

```
canonical =
  "forkmesh-commit-comment-v1" \n
  <full-sha> \n
  <author pubkey> \n
  <ts> \n
  sha256_hex(<body>)
```

## Cross-user

A node without write access POSTs a signed comment to the relay inbox at
`/api/repo/{owner}/{repo}/commits`. The owner's client drains the inbox, verifies
each signature, writes the comment into `.forkmesh/commits/<sha>/` and commits it, so it
syncs out to every mirror with the next fetch.
