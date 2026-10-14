# Git errors when cloning, pulling, or pushing

ForkMesh serves Git over HTTPS at `https://app.forkmesh.com/<owner>/<repo>`.
Clones and pulls are often redirected to a mirror, so a
`warning: redirecting to ...` line is normal. Errors ForkMesh explains are
printed on `remote:` lines with a link to this page; connection failures show
only Git's own message, usually with a `curl` error.

## Messages from ForkMesh

| `remote:` message | What it means | What to do |
| --- | --- | --- |
| ForkMesh could not reach a mirror of this repository just now. | No mirror that holds the repository answered in time. | Try again in a minute. The repository's **Mirrors** page (`/<owner>/<repo>/mirrors`) shows which mirrors are up. |
| This ForkMesh mirror could not read the repository just now. | The mirror you were sent to failed while reading the repository. | Try again; the next attempt may use another mirror. |
| ForkMesh has no public repository at this address. | The owner or repository name is wrong, or the repository is private. | Check the spelling in `git remote -v`. Private repositories need their owner's credential. |
| This ForkMesh mirror has no public repository at this address. | Same as above, answered by the mirror itself. | Same as above. |
| Pushing over HTTPS needs your ForkMesh account name and the repository's push credential. | The push had no credential, or a wrong one. | Use your account name as the username and the push credential as the password. See [Hosted repositories](hosted-repositories.md). |
| This repository is private. Sign in with your ForkMesh account name and the repository's credential. | The repository is private and the credential was missing or wrong. | Use the credential its owner gave you. |
| ForkMesh could not reach the node that accepts pushes for this repository. | The node that writes this repository did not answer. | Try again in a minute. Your commits are still local and safe. |
| Use Git protocol version 0, 1, or 2. | Your Git sent a protocol version ForkMesh does not speak. | Remove `protocol.version` from your Git configuration, or set it to `2`. |

## Errors from your own Git client

These come from Git and curl on your computer; ForkMesh never saw the request.

| Git prints | What happened | What to do |
| --- | --- | --- |
| `Error in the HTTP2 framing layer`, `HTTP/2 stream … was not closed cleanly`, `curl 16`, `curl 92` | The HTTP/2 connection broke partway through. VPNs, proxies, antivirus that inspects HTTPS, and unstable mobile or Wi-Fi links cause most of these. | Run the command again. If it keeps failing, use HTTP/1.1 for this repository: `git config http.version HTTP/1.1`. Add `--global` to use it everywhere. |
| `RPC failed; curl 18 transfer closed with outstanding read data remaining`, `curl 56`, `early EOF`, `unexpected disconnect while reading sideband packet` | The connection dropped while Git was downloading. | Run the command again. If it fails at the same point every time, include a trace in your report (below). |
| `Could not resolve host`, `Failed to connect`, `Connection timed out`, `Connection refused` | Your computer could not reach ForkMesh at all. | Check your network, DNS, and any VPN or proxy. |
| `SSL certificate problem` | Something between you and ForkMesh is presenting another certificate, or your clock is wrong. | Check the system clock, and any proxy or antivirus that intercepts HTTPS. |

## Reporting a problem

Open an issue on
[forkmesh/forkmesh](https://app.forkmesh.com/forkmesh/forkmesh/issues) with
`git --version`, `curl --version`, the time of failure with time zone, and a
trace (check it for secrets before posting):

```bash
GIT_TRACE_CURL=1 GIT_TRACE_CURL_NO_DATA=1 git pull origin main 2>&1 | tail -60
```
