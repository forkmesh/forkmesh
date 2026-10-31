# Back up a GitHub repository

This guide is for keeping a private, safe copy of a repository that lives on
GitHub, for example a website you share with a friend, without changing
anything on GitHub. You need two commands in a terminal; everything else
happens in the ForkMesh desktop app.

## What you need first

- **Permission to view the repository on GitHub.** If you can open its files
  on github.com, that is enough. You do not need permission to push, and you
  do not need a new branch: a backup only reads.
- **A ForkMesh account** and the desktop app, signed in. See
  [Getting started](getting-started.md); the desktop uses the same email and
  password as the website.
- **Git** on your computer. On Debian or Ubuntu: `sudo apt install git`.

## The words, in plain terms

| Word | What it means | Does it change GitHub? |
| --- | --- | --- |
| View | Look at the files on github.com | No |
| Clone | Make a copy of the repository on your computer | No |
| Pull | Bring the newest changes from GitHub into that copy | No |
| Commit | Save a change you made, on your computer only | No |
| Push | Send your saved changes to GitHub | **Yes**, and it can change the live website |
| Mirror | ForkMesh's copy of a repository, kept on the ForkMesh network | No |

A backup uses only **clone** and **pull**. ForkMesh never pushes anything to
GitHub for you.

On the ForkMesh website, **Import from GitHub, GitLab, Codeberg** copies a
project's issues and pull requests once. It does not copy the code and is
not kept up to date, so it is not a backup. **Clone URL** on the website
copies a public repository once. A private repository is backed up from the
desktop app, as below.

## 1. Copy the repository to your computer

Open a terminal and run (put in your friend's GitHub name and the repository
name from its github.com address):

```sh
git clone https://github.com/OWNER/REPOSITORY.git ~/Backups/REPOSITORY
```

For a private repository Git asks for a username and a password. Type your
GitHub username, and for the password paste a **GitHub access token**, not
your GitHub password (GitHub no longer accepts passwords here). To make one,
on github.com open **Settings → Developer settings → Personal access tokens**:

- If the repository belongs to you or to an organization you are in, choose a
  **fine-grained token**, limit it to that one repository, and set
  **Contents** to **Read-only**. It cannot change anything.
- If the repository belongs to a friend's personal account, GitHub does not
  let fine-grained tokens reach it; use a **classic token** with the `repo`
  scope and an expiry date. That token can also write, so keep it private.
  Git uses it only on your computer; ForkMesh never sees it.

## 2. Add it to ForkMesh as a private repository

1. In the desktop app open **Repos** and choose **Import folder…**.
2. Pick the folder you just cloned (`~/Backups/REPOSITORY`).
3. ForkMesh asks **Import REPOSITORY?**. Choose **Private**.

**Private** means the copy is encrypted before it leaves your computer, is
hidden from public browsing and cloning, and only you (and people you share it
with) can read it. Do not choose **Public**: that lists it for anyone.

## 3. Keep it up to date

While the desktop app is open, ForkMesh checks the folder about once a minute
and copies anything new to the ForkMesh network (change how often under
**Settings → Mirror sync**). ForkMesh does not fetch from GitHub by itself yet, so bring in your
friend's newest changes by pulling them into the folder:

```sh
git -C ~/Backups/REPOSITORY pull
```

ForkMesh then copies them within the next sync. Pull as often as you want a
fresh backup, for example once a week.

## 4. Check the backup

Open the repository in **Repos** and look at its settings: it shows
**Managed mirror enabled** and **Last sync** with a time. A recent time means
the ForkMesh copy matches the folder on your computer.

## Things that will not happen

- Nothing is changed on GitHub or on your live website: clone and pull only
  read. Avoid the **Push** button and `git push` until you mean to change the
  website.
- Nothing becomes public unless you choose **Public**.
- Removing the repository from ForkMesh does not delete it from GitHub.

Back to [Getting started](getting-started.md) or the
[documentation index](README.md).
