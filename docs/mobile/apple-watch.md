# Apple Watch

Follow, steer and hear your coding agents from your wrist. The watch app is
new and still growing. Back to the [phone apps](README.md).

## How it works

The watch has no ForkMesh session of its own. Your iPhone holds the session
and the keys in its device-only Keychain, unseals your agents' transcripts and
sends the watch a compact summary; the watch's commands go back through the
phone.

```text
   desktop · cloud runners           iPhone                     Apple Watch
   agents run here          ───►     unseals transcripts  ───►  agent list,
                                     with its own key           live view, sound
                            ◄───     sends your command   ◄───  Stop, Continue,
                                                                prompt, Retry
```

So the watch needs the iPhone app installed, signed in and nearby. It runs
watchOS 10 or later.

## What it does

| | Feature | What you get |
| --- | --- | --- |
| 📋 | Agent list | Every active and recent agent with its model, repository, status, turns and tokens; running agents first |
| 👀 | Live view | Watch the agent work, with its last reply on screen |
| ⏯️ | Controls | **Stop**, **Continue** and **Retry**; send a prompt by dictation or Scribble |
| 🔍 | Review changes | Read an agent's diff file by file and mark files as reviewed |
| 🎧 | Listen | Each agent plays as sound and haptics, so you can tell what it is doing without looking; the optional Announcer says short lines such as "run done" or "is waiting for you" |
| 🗣️ | Siri | "Start a new ForkMesh agent": name a repository, or it uses the repository and model you last used on the phone |
| ⌚ | Complications and Smart Stack | The current run at a glance |
| 🔔 | Notifications | The phone's ForkMesh notifications appear on the watch |

There is no Pause: an agent is stopped and continued.

## Next

A standalone mode that signs in on the watch itself, so it works without the
phone nearby. How the watch app is built:
[phone app parity, watchOS](../native/PARITY.md#4-watchos-app-control-agents-see-and-hear-them).

Related: [Siri](siri.md) · [agent access](mobile-agent-access.md) ·
[phone apps](README.md)
