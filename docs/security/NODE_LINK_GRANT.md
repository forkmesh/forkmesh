# Node link grants: both parties consent

`POST /api/accounts/link-grant` moves a node account to a new owner. Until
2026-09-24 the new owner was the request body's `user`, so anyone holding a
node's private key could hand that node to any account without the account
agreeing. The handler is `_account_link_grant` in `app/src/entry.py`.

## Rule

A node changes owner only when both sides prove themselves in one request:

1. **The node** signs a grant with its account key.
2. **The new owner** accepts it with its own credentials: either an
   authenticated session (cookie, `sessionToken` or `Authorization: Bearer`)
   or a signature from one of its own signing keys (the account `pubkey` or an
   enabled `owner_sign` device key).

The owner is always the accepting account. The body's `user` is only a claim
that is checked against it, never the source of the owner.

App tokens (`fmat_…`) are not accepted as a session here.

## Grant forms

| Form | Canonical string (`\n`-joined) | Names the owner | Who may accept |
| --- | --- | --- | --- |
| v1 | `forkmesh-link-grant-v1`, node, ts | no | the session account; a body `user` must equal it |
| v2 | `forkmesh-link-grant-v2`, node, user, ts, nonce | yes | only `user`, by session or signed accept |

Signed accept:

```
forkmesh-link-accept-v1
<node>
<user>
<grant sig>
<acceptTs>
```

It goes in the body as `user`, `acceptTs` and `acceptSig`. Because it
includes the grant signature, an accept cannot be moved to a different grant.

`nonce` is 16–64 characters of `[A-Za-z0-9_-]`. When a v2 grant is sent, `user`
is required.

## Freshness and replay

- A grant is valid from 1 minute before its `ts` until 5 minutes after it
  (`LINK_GRANT_MAX_FUTURE_MS`, `LINK_GRANT_MAX_AGE_MS`).
- A signed accept uses the login window (`_ts_ok`, ±5 minutes).
- Each redeemed grant is burned in `link_codes`:
  - v1 is burned under `grant:<node>:<ts>:<sig>`;
  - v2 is burned under `grant2:<node>:<nonce>`.
- A second redemption answers `409 grant_used`. The exception is when the
  node is already owned by the acceptor: then the answer is
  `alreadyLinked`, and nothing is written.

## Errors

| Error | Status | Meaning |
| --- | --- | --- |
| `bad_signature` | 401 | The node signature does not verify. For v2 this includes a `user` other than the signed one. |
| `grant_expired` | 401 | `ts` is outside the grant window. |
| `session_required` | 401 | No session and no signed accept. |
| `accept_expired` | 401 | `acceptTs` is outside `_ts_ok`. |
| `bad_accept_signature` | 401 | The accept does not verify with the named account's keys. |
| `grant_user_mismatch` | 403 | The grant (or the body `user`) names a different account from the acceptor. |
| `no_such_user`, `not_a_user` | 404, 403 | The acceptor is missing, inactive, or a node account. |
| `grant_used` | 409 | The grant was already redeemed. |

## Audit

Every decision made after a valid node signature adds a `sensitive_audit_log`
row. The action is `node.link_grant` and the target is the node.

- **Accept:** `outcome=success`, with details `{grant, via: session|signature, from, to}`.
- **Refusal:** `outcome=denied`, with details `{reason, grant, named, from}`.

A request whose node signature fails is not audited, so unauthenticated
callers cannot fill the log.

## Clients

| Client | Flow | Change |
| --- | --- | --- |
| Desktop (`desktop/src/MainWindowNodes.cpp`, `openLinkNodeInBrowser`) | Signs a v1 grant and opens `/dashboard?link_node=…` in the browser | None needed. The browser session is the acceptance. |
| Web dashboard (`app/public/dashboard/js/04-account.js`, `redeemLinkGrant`) | Redeems with the logged-in session | Sends the session (`sessionToken` / Bearer, or the cookie) and shows the new errors. |
| `tools/next_node_owner.py` (production repo) | Operator transfer of fleet nodes | Signs a v2 grant on the node that names the target. The target then accepts with its own key (`--accept-key`) or session (`--session-env`). |
| Go daemon, native iOS/Android | none | Do not call link-grant. |
