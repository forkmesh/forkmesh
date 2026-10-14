# Host-mode agent boundaries

What a desktop coding agent can read, execute and reach for each provider and
mode, which secrets it can see, what limits its network, how injected
instructions are handled, and what limits spend. Each claim is proven by a
named check.

## Run the checks

```sh
cmake -S desktop -B /tmp/fm-build -G Ninja
cmake --build /tmp/fm-build --target forkmesh-agent-boundary-tests \
  forkmesh-codex-tests forkmesh-vm-runtime-tests forkmesh-agent-access-tests \
  forkmesh-agent-transport-tests
/tmp/fm-build/forkmesh-agent-boundary-tests
```

`desktop/tests/test_agent_host_boundaries.cpp` prints one `PASS`/`FAIL` line
per check. It runs in a temporary `HOME` and XDG tree, drops `FORKMESH_HOME`
and `FORKMESH_PROJECTS_DIR`, and plants canary secrets. Stand-in `claude` and
`codex` executables go through the real launch paths (login shell, jail,
Landlock wrapper, KVM wrapper) and try to read outside the worktree, run a
subprocess, connect over loopback TCP and connect to a Unix socket. Checks
that pin a known weakness fail when the boundary changes in either direction.

## Provider by mode

| Mode (host, no KVM) | Launch | Read | Execute | Reach |
| --- | --- | --- | --- | --- |
| Claude Auto | skips permission prompts | anything the user can read | any command | any destination and socket |
| Claude Edit | edits run, other tools ask in the CLI | same | after the CLI's prompt | same |
| Claude Plan | edits and commands held until the plan is accepted | same | held by the CLI | same |
| Claude Ask | asks before edits and commands | same | after the prompt | same |
| Codex (all modes) | approval `never`, full-access sandbox | same | any command | same |
| Project lock (default rules, Linux) | Landlock helper (`tools/forkmesh_agent_guard.py`) | worktree, project folders, Git metadata, provider login, read-only Git config | as above | as above |
| KVM (Linux, opt-in) | a QEMU/KVM guest via Lima | launch directory and Git metadata only, not the host home | any command in the guest | guest network |

Off Linux there is no folder lock. The external-call gate (a Claude
PreToolUse hook) applies on top of host modes whenever rules need it; it is a
tool-layer classifier, not a kernel policy.

## Secrets

- **Environment.** A host agent inherits the desktop's environment except its
  own provider's credential variables (`desktop/src/AgentCredentialEnvironment.h`).
  Other providers' keys, forge tokens, cloud credentials and `SSH_AUTH_SOCK`
  are inherited.
- **Files.** Unrestricted, the agent reads whatever the user can. Under the
  project lock, `~/.ssh`, `~/.aws` and the rest of home are refused unless
  granted; the provider login directory stays readable.
- **Sockets.** Landlock does not mediate Unix-socket connects, so a loaded SSH
  agent remains usable.
- **KVM.** The environment bridge is deny-by-default; SSH agent forwarding is
  off and host login files are not mounted.

Recommendation: run high-risk work with only staging credentials and no SSH
agent loaded, or in KVM. `.gitignore` is not a secret boundary.

## Egress

No host-mode configuration limits the network at the OS level. The
external-call gate (`AgentNetworkGate.cpp`, `AgentRunRules.cpp`) classifies
shell commands that name a non-local destination and web fetch/search tools,
and asks, blocks or allows per the project rule (default Ask). Commands that
hide their destination (encoded payloads, self-written programs, binaries with
their own HTTP client) are not seen. Server runners have their own egress
rules ([../RUNNERS.md](../RUNNERS.md)).

## Prompt injection

Teammate prompts instruct models to treat repository, issue, pull request and
external content as untrusted data; that is an instruction, not isolation.
`conformance/agent-adversarial/v1/fixtures.json` holds malicious text per
vector with the tool call it requests. For each fixture the test runs the
rules decision, sends the call through the real hook to a gate that declines,
and runs secret reads with and without the lock. Outcomes are `ask`, `deny`,
`folder-lock` or `known-gap`; to add a fixture, append it with its `expect`.

Known gaps share one cause: anything that looks local to the classifier runs
without a prompt, such as printing an inherited token or decoding and running
a hidden network command. Defences for high-risk work: a narrower environment,
Ask or Plan mode, KVM, and human review of the branch.

## Spend

- Per-session token and cost estimates (display only).
- A concurrency limit (`agents/maxRunning`, default 5).
- Continual teammates wait for a person's decision between cycles.
- A CLI that exits without a result is relaunched at most twice.

There is no hard dollar cap; the only hard ceilings are the provider
account's own limits. Set them at the provider for unattended use.
