# Contribute with git only

You can contribute to a public repository on ForkMesh with plain `git` and a
browser. You do not need the desktop app. The steps match GitHub: fork, push a
branch to your fork, open a pull request.

The examples use `app.forkmesh.com`. On a self-hosted instance, use that
instance's address instead, for example `git.example.org`.

## 1. Fork the repository

Sign in with a verified email address, open the repository and click **Fork**
next to **Star** at the top of the page. ForkMesh creates `<you>/<repo>` in
your account. Your fork shares the original repository's history, so it is
ready at once, whatever the repository's size.

If you already have a fork, **Fork** opens it. The arrow next to **Fork** lists
everyone's forks.

## 2. Clone and add your fork as a remote

```bash
git clone https://app.forkmesh.com/<owner>/<repo>.git
cd <repo>
git remote add fork https://app.forkmesh.com/<you>/<repo>.git
```

`origin` stays the original repository, so `git pull` keeps you up to date.

## 3. Push a branch

```bash
git checkout -b my-change
git commit -am "Describe the change"
git push fork my-change
```

When git asks for a username and password, enter your ForkMesh account name or
email and your account password. A push credential from **Settings → SSH keys
→ Push code over HTTPS** also works as the password.

After the push, git prints the page to open:

```
remote: Open a pull request for 'my-change' by visiting:
remote:      https://app.forkmesh.com/<you>/<repo>/compare/main...my-change
```

## 4. Open the pull request

Open that link, write a title and a description, and click **Create pull
request**. The pull request goes to the original repository. Your browser signs
it, so the maintainer can check it came from your account.

A pull request opened from a branch of the same repository follows that
branch: push more commits (or force-push a rebase) and the pull request shows
them, with "pushed N commits" in its timeline. A pull request from a fork
records your branch as it was when you opened it; to update it, open a new pull
request from the same link.

## Without a fork

Some repositories let contributors push straight to them. There, push a branch
named `pr/<your-account>/<topic>` to the original repository and open the pull
request from the link git prints. See
[Contributing without write access](hosted-repositories.md#contributing-without-write-access).

## Good to know

- You can fork public repositories hosted on ForkMesh's servers. A repository
  published from someone's own node cannot be forked from the website yet.
- You can force-push your own branches after a rebase, amend or squash, which
  updates an open pull request. Use `git push --force-with-lease`: it refuses
  when the branch has commits you have not fetched (say, after **Update
  branch** on the pull request). If that happens, run `git pull --rebase` and
  push again. The default branch and protected branches only move forward,
  and the previous tip of a force-pushed branch is kept, so reviews of older
  commits still resolve.
- A push never deletes a branch. Delete branches on the fork's **Branches**
  page.
- Forks have their own per-account limit, separate from the repositories you
  create.
- If git says `Authentication failed`, check the account name or email and the
  password. [Git errors](git-errors.md) lists every message ForkMesh sends.
