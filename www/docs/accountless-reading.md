# Reading and cloning without an account

Public projects are readable without an account. The website never redirects
to sign-up or log-in to read; it asks you to sign in only when you change
something.

## What works without an account

Repository overview, code, branches, commits, issues, pull requests and
reviews, discussions, releases and their manifests, mirrors
(`/owner/repo/mirrors`), public profiles (`/@name`), and
`git clone https://forkmesh.com/owner/repo`. Other forges' URL shapes redirect
to the readable page. See [Git compatibility](git-compatibility.md).

## Where you are asked to sign in

Starring, forking, writing issues, pulls, comments or reviews, changing issue
state, copying an agent prompt, linking a desktop node, and account pages send
you to `/login?next=<page>`. So does a write rejected with `401`, and nothing
else.

## Not yet available

A passkey-backed **browser identity** for signing contributions, and handing it
to a full node, are not yet available. See
[Portable identities and recovery](portable-identities.md).

## Evidence

`app/browser_tests/tests/anonymous-public-reading.spec.js` and
`app/tests/test_anonymous_public_reading.py` check the rule.
