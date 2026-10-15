# Using Siri with ForkMesh on iPhone

The ForkMesh iPhone app adds its commands to Siri and the Shortcuts app. You
can start an agent, ask what your agents are doing, answer their requests, and
stop, continue, message, merge or delete them without opening the app.

## Before you start

- Install the app and sign in. For commands that control an agent, sign in to
  the ForkMesh desktop app with the same account; the phone reaches it through
  the relay.
- Open the app once after installing or updating it so Siri learns your
  agents' names.
- In **Settings → Siri**, turn Siri on and allow ForkMesh to work with it.

Apple requires the app's name in every built-in phrase: say "in ForkMesh" (or
"for", "on", "with ForkMesh"). For shorter phrases, see
[Your own phrases](#your-own-phrases).

## Naming an agent

Siri knows each desktop agent by its session number ("Agent 754"), spoken as a
number or digit by digit, or by part of its title or repository. Leave the
agent out and Siri lists your agents and asks which one.

## What you can say

| Goal | Example phrases |
| --- | --- |
| Start an agent | "Start a new agent in ForkMesh", "Start a ForkMesh agent on *repository*" |
| Check status | "What are my ForkMesh agents doing?", "What did agent 754 change in ForkMesh?" |
| Answer a request | "Approve ForkMesh request", "Deny the ForkMesh request" |
| Message | "Tell agent 754 in ForkMesh" |
| Stop or continue | "Stop agent 754 in ForkMesh", "Continue agent 754 in ForkMesh" |
| Merge | "Merge and clean up agent 754 in ForkMesh" |
| Delete | "Delete agent 754 in ForkMesh", "Delete all agents in ForkMesh" |
| Open a screen | "Open inbox in ForkMesh", "Open *owner/repository* in ForkMesh" |
| Listening | "Start listening in ForkMesh", "Turn the ForkMesh Genie on", "Stop listening in ForkMesh" |

- Starting an agent opens the prompt composer, even if the app was closed.
- Approving, merging and deleting read a summary from the desktop, ask for
  confirmation, and need the iPhone unlocked. Merge and continue work only on
  desktop agents. "All agents" means the agents ForkMesh started on your
  desktop; deleting removes each agent with its worktree and branch.
- Listening turns on the in-app voice assistant while ForkMesh is on screen.
  Speech is recognised on the device and nothing is kept.

## Apple Watch

Say "Start a new agent in ForkMesh" on the watch, dictate the prompt and
confirm. The iPhone starts the agent on the last repository and model you used.
The iPhone must be nearby and signed in.

## Commands without a built-in phrase

Apple allows ten built-in commands per app, so **Stop all agents**, **Start a
timer**, **Open agent run**, **Open inbox**, **Open agent approvals**, **Open
issue** and **Open note** are available in the Shortcuts app only.

## Your own phrases

In the Shortcuts app, add a ForkMesh action (for example **Start an agent**),
fill in fixed values, and name the shortcut with the phrase you want. "Hey
Siri, *shortcut name*" then runs it without saying "ForkMesh".

## Troubleshooting

| Siri says | Fix |
| --- | --- |
| Agent number not recognised | Open the app to refresh the agent list, or pick from the list |
| "Sign in to ForkMesh first." | Sign in on the phone |
| "Pair this phone with your desktop first." | Run the desktop app, signed in to the same account |
| "Siri can only do that for agents on your desktop." | Manage cloud agents in the app |
| "Update ForkMesh on the desktop to delete agents from here." | Update the desktop app |
| "Allow the microphone and speech recognition…" | Enable both in **Settings → ForkMesh** |
| A web search opens instead | Include "ForkMesh" in the phrase |
