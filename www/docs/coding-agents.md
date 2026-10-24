# Working with coding agents

ForkMesh publishes one guide written for coding agents such as Claude Code:
[app.forkmesh.com/forkmesh-agent.md](https://app.forkmesh.com/forkmesh-agent.md).
Hand it to your agent and it can start your own ForkMesh instance for you,
work on ForkMesh repositories, and open pull requests the way the ForkMesh
team does.

## What the guide covers

- **Starting an instance.** What your server needs, writing the settings
  file for the [start page](https://forkmesh.com/start), or running
  `install-instance.sh` over SSH yourself, and what to do once it is up.
- **Working on a repository.** Cloning, branching, pushing to a hosted
  repository, and small commits with one scope each.
- **Opening pull requests.** From the desktop app, the MCP server's
  `open_pr_from_branch`, or the signed HTTP API, and the rules that keep a
  pull request easy to accept: small, independent of the others, built on
  the current `main`, and confirmed on the server before it is called live.
- **Ground rules.** Ask before anything hard to undo, never print secrets,
  never delete ForkMesh data folders.

## Use it in one message

Copy this into Claude Code (the start page has a Copy button for it):

```text
Read https://app.forkmesh.com/forkmesh-agent.md and help me start my own ForkMesh instance. Ask me for anything you need first.
```

The agent asks for your server address, domain, admin account and repository
owner, writes the settings file, and tells you when to upload it on the start
page. Your SSH password or key and your admin password stay out of the file:
you type them into the page yourself, so the agent never sees them. It asks
before it runs anything on your server.

## Keep it in your repository without committing it

To have every session read the guide, save it next to your code and tell Git
to ignore it only in your clone:

```sh
curl -fsSL https://app.forkmesh.com/forkmesh-agent.md -o FORKMESH.md
echo FORKMESH.md >> .git/info/exclude
```

`.git/info/exclude` is a per-clone `.gitignore` that is never committed, so
the file stays yours. Add `FORKMESH.md` to `.gitignore` instead when the
whole team should ignore it. Then point your agent at it: add `@FORKMESH.md`
to an ignored `CLAUDE.md`, or start a session with "read FORKMESH.md".

Re-download it now and then; the guide changes as ForkMesh does.

## Connect the ForkMesh MCP server

The ForkMesh source ships an MCP server, `tools/forkmesh_mcp_server.py`,
and lists it in `.mcp.json`, so Claude Code opened in a ForkMesh checkout
finds it. Reading code, issues and notes needs no token. Writing (issues,
notes, pull requests) needs a device-local token from **Desktop → Settings →
MCP**. No MCP tool gives a shell, arbitrary file access, pushes or
administration.

## Related

- [Start your own instance](start-your-own-instance.md)
- [Start from a settings file](start-config.md)
- [Contributing](../../CONTRIBUTING.md), including the pull request workflow
- [Contribute with git only](contributing-with-git.md)
