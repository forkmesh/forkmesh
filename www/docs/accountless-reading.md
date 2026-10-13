# Reading and cloning without an account

You do not need a ForkMesh account to read or clone a public project. The
website never redirects you to sign-up or log-in just to read. It asks you to
sign in only when you try to change something.

## What works without an account

| Surface | Without an account |
| --- | --- |
| Repository overview, code tree, files, branches, commits | Readable |
| Issues and issue detail | Readable |
| Pull requests, their files, and review threads | Readable |
| Discussions, projects, insights | Readable |
| Releases and signed release manifests | Readable and downloadable |
| Mirrors and availability (`/owner/repo/mirrors`) | Readable |
| Public profiles (`/@name`) | Readable |
| `git clone https://forkmesh.com/owner/repo` | Works; no credentials are sent to a mirror |
| Remote ActivityPub replies on public records | Readable |

Clone URLs from other forges, such as `/owner/repo/pull/7` or GitLab's
`/owner/repo/-/merge_requests/7`, redirect to the readable ForkMesh page, not
to an auth page. See [Git compatibility](git-compatibility.md) for clone
transports and their limits.

## Where you are asked to sign in

These actions send you to `/login?next=<the page you were on>`, so you come
back to the same page afterward:

- starring or forking a repository;
- opening an issue, sub-issue, or pull request, or commenting on or reviewing one;
- changing issue state, labels, or assignees;
- copying an issue's agent prompt, which contains a connector token;
- finishing a desktop node link (`?link_node=`), which must attach to an account;
- opening account-only pages: settings and your own profile.

A write request the relay rejects with `401` also sends you to sign in. No
other response does.

## Browser identity and handoff to a node: not yet available

The following are **not yet available**. Until they ship, a first
contribution needs a regular account:

- a passkey-backed **browser identity** that signs comments, issues, reviews,
  and patches, with a consent prompt at signing time;
- exporting that identity, or handing it to a full node, without losing
  authorship, history, follows, or recovery options.

The recovery kit that later handoff builds on is described in
[Portable identities and recovery](portable-identities.md).

## Evidence

The "never redirect to registration" rule is covered by automated tests:

- `app/browser_tests/tests/anonymous-public-reading.spec.js` loads the
  repository overview, issues, pull requests, a pull request detail, releases,
  mirrors, branches, and commits in a browser with no session. It fails if the
  page moves or any navigation reaches `/login` or `/signup`.
- `app/tests/test_anonymous_public_reading.py` checks five things:
  - every public repository view is served as a repository page;
  - the Worker only redirects to `/login` from admin pages;
  - every client-side sign-in redirect follows a write or account check;
  - the guest start-up path treats only settings and your own profile as
    account pages;
  - this page exists and is linked from the docs index.
