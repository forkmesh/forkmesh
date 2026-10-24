# Mirror goals

An instance admin turns this on with the **Mirror goal** plugin (admin
console, Plugins). It is off until then.

A project that could be blocked or taken down survives if copies of it are
kept in many places. A repository page can show how many copies are running
and how healthy they are, in the **Mirrors** card at the bottom of the right
rail: "3 / 10 healthy" with a progress bar, then one row per copy with a
green, yellow or red dot.

- **This instance** and, for a repository synced from GitHub, Gitea or
  Forgejo, **the original** are always listed.
- **Mirrors people run** are added with **Add a mirror**. Anyone signed in can
  list a copy they run; it is checked about every 15 minutes.
- **Green** means the copy answered and has a commit this repository had in
  the last day. **Yellow** means it answered but is behind or different, or
  has not answered for a day. **Red** means it could not be reached twice in a
  row, or never. Grey means not checked yet, or an address that is listed but
  not checked (git://, ssh, .onion).

The goal is set by the repository's maintainers (the owner, an organization
maintainer or admin, or an instance administrator) with **Set target**. A goal
of 0 turns the count off.

Every repository has a page that explains all of this step by step, with the
live count: `https://<instance>/<owner>/<repository>/mirror` (for the
example forum, `https://forum.example.org/example/forum/mirror`).
While a repository is below its goal, its home page, issue list, lite pages
and feeds ask readers to help and link there.

## Run a mirror

```sh
git clone --mirror https://<instance>/<owner>/<repository>.git
cd <repository>.git && git update-server-info
```

Refresh it every 15 minutes, for example with cron:

```
*/15 * * * * cd /srv/<repository>.git && git remote update --prune && git update-server-info
```

Serve the folder over HTTPS with any web server (Git's "dumb HTTP" needs only
static files), or push it to another forge, then list its `https://` address
with **Add a mirror**. A repository synced with [forge sync](forge-sync.md)
keeps its issues in `.forkmesh/issues`, so the clone carries the whole forum,
not only the code. To serve it with issues, pull requests and a web page,
[start your own ForkMesh instance](https://forkmesh.com/start) and import the
clone.

Checking a mirror reads only its list of branches (`info/refs`); nothing is
loaded from it in visitors' browsers.
