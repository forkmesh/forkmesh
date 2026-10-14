# Campaign brief: "Is your repo mirrored?"

**Campaign tag:** `repo_mirrored`
**Landing page:** `https://forkmesh.com/mirror` (`www/templates/pages/mirror.html`)
**Plan:** [../plan.md](../plan.md)

A campaign brief is a file under `docs/marketing/campaigns/` that pins what
the copy may claim, the tagged links and the drafts. This one promotes
copying a public repository into ForkMesh from the browser.

## Claims and their evidence

| Copy may say | Evidence | Copy must not say |
| --- | --- | --- |
| Paste the `https://` URL of a public GitHub, GitLab or Codeberg repository into **Remote clone URL** | `HOSTED_IMPORT_HOSTS` in `app/wrangler.toml` | That private repositories can be copied |
| ForkMesh copies every branch and tag | `server/hosted.go` fetches `+refs/heads/*` and `+refs/tags/*` | That issues, pull requests, releases, wikis or LFS files come along |
| The copy is kept on several servers | [HOSTED_REPOS.md](../../HOSTED_REPOS.md) | "An independent network" |
| Anyone can clone a public copy without an account | [HOSTED_REPOS.md](../../HOSTED_REPOS.md) | Uptime, speed or "never down" |
| You push new commits to it with plain Git over HTTPS | The credential shown after **Create & mirror** | That it follows the original: the browser path copies once |
| Up to 200 MiB per repository and 10 per account | `app/wrangler.toml`, `server/hosted.go` | Any pricing promise |
| Set it up before an outage | Copying from a host that is down fails | That you can copy a repository while its host is down |

Every answer says "copy" and "push to keep it current", never "sync",
"automatically" or "stays up to date". `www/tests/test_mirror_page.py` checks
the page keeps these limits and never promises a live mirror.

## Tagged links

```
https://forkmesh.com/mirror?utm_source=x&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
https://forkmesh.com/mirror?utm_source=mastodon&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
```

`repo_mirrored` is on `ATTRIBUTION_ALLOWED_CAMPAIGNS` in `app/wrangler.toml`
and `DEFAULT_ALLOWED_CAMPAIGNS` in `app/src/attribution.py`. The page's own
links are untagged so they keep the post's first touch
([../../attribution.md](../../attribution.md)).

## Gates before posting

1. The page is deployed and answers 200, with the allowlist in the same
   deploy.
2. A fresh test account copies a throwaway public repository through
   **Remote clone URL**, and the copy clones from a machine with no
   credential.
3. A person presses Post.

## X draft

`www/tests/test_mirror_page.py` reads the block below and checks it fits 280
characters with the link counted as 23:

<!-- repo-mirrored-x:start -->
```text
Is your repo mirrored?

If its only public copy lives on one host, an outage or a locked account takes it from everyone who clones it.

Paste a public GitHub, GitLab or Codeberg URL into ForkMesh and anyone can clone the copy, no account needed.

https://forkmesh.com/mirror?utm_source=x&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
```
<!-- repo-mirrored-x:end -->
