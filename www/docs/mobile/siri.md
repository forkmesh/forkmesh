# Using Siri with ForkMesh on iPhone

The ForkMesh iPhone app adds its commands to Siri and the Shortcuts app
automatically. You can start an agent, ask what your agents are doing, answer
their requests, and stop, continue, message, merge or delete them without
opening the app. You can also turn ForkMesh's own listening on or off.

## Before you start

- Install the ForkMesh app from the App Store and sign in.
- For any command that controls an agent, sign in to the ForkMesh desktop app
  with the same account. The phone finds that desktop through the relay, so it
  does not need to be on the same network. Siri answers "Pair this phone with
  your desktop first." when it cannot reach one.
- Open the app once after installing or updating it. Siri learns your agents'
  names from the app, so new agents become sayable after the app has seen them.
- Make sure Siri is on. In **Settings**, open **Siri** (called **Siri &
  Search** or **Apple Intelligence & Siri** on some iOS versions), turn on
  listening for "Siri" or "Hey Siri" or pressing the side button, then open
  **ForkMesh** in the list of apps there and let it work with Siri.

## Every command names ForkMesh

Apple requires the app's name in every built-in phrase, so you say "in
ForkMesh" (or "for", "on" or "with ForkMesh"). "Hey Siri, start a new agent"
on its own does not reach ForkMesh; "Hey Siri, start a new agent in ForkMesh"
does. To use a shorter phrase, see
[Your own phrases](#your-own-phrases).

## Naming an agent

Siri knows each desktop agent by its session number: "Agent 754". Say the
number as a number or digit by digit ("seven five four"); "number",
"session" and "run" are ignored, so "agent number 754" works too. You can
also say part of the agent's title or its repository.

If you leave the agent out ("Stop a ForkMesh agent"), Siri lists your agents,
running ones first and then the most recently active, and asks which one.

## What you can say

### Start an agent

- "Start a new agent in ForkMesh"
- "New agent in ForkMesh"
- "Start a ForkMesh agent on *repository*"

ForkMesh opens on the prompt composer with the keyboard up, ready for you to
type or dictate the prompt. This also works when the app was closed.

### Ask how your agents are doing

- "What are my ForkMesh agents doing?"
- "How is agent 754 doing in ForkMesh?"
- "What did agent 754 change in ForkMesh?"

For all agents, Siri says how many are running and the latest one's state, and
shows up to four of them. For one agent, Siri says its title, state and
repository; for a finished desktop agent it adds how many files and lines it
changed, or that it is already merged.

### Answer an agent's request

- "Approve ForkMesh request"
- "Deny the ForkMesh request"

Siri reads the oldest request an agent is waiting on ("Agent 754 asks: …
Approve it?") and answers it after you confirm. It then tells you how many
requests are still waiting. Your iPhone must be unlocked.

### Message, stop and continue an agent

- "Tell agent 754 in ForkMesh" — Siri asks "What should I tell it?" and sends
  your answer to the agent.
- "Stop agent 754 in ForkMesh" — stops a running agent, on the desktop or in
  the cloud.
- "Continue agent 754 in ForkMesh" — tells a finished desktop agent to keep
  going.

Agents started outside ForkMesh, such as a `claude` session you opened in a
terminal yourself, can be messaged only when the desktop can type into their
terminal, and stopped only when the desktop can interrupt them. Otherwise Siri
tells you to use the terminal that started them.

### Merge an agent

- "Merge agent 754 in ForkMesh"
- "Merge and clean up agent 754 in ForkMesh"

Siri reads the desktop's summary of the merge and asks you to confirm. On yes,
the desktop merges the agent's branch and cleans up its worktree, the same as
**Merge & clean up** in the app, and Siri says which branch it merged and how
many lines changed. Your iPhone must be unlocked. Merging works only for
agents on your desktop.

### Delete agents

- "Delete agent 754 in ForkMesh", "Remove agent 754 from ForkMesh"
- "Delete all agents in ForkMesh", "Delete everything in ForkMesh",
  "Delete all my agents from ForkMesh"

For one agent, Siri reads the desktop's summary of what goes and asks you to
confirm. For all agents, Siri says how many agents are on your desktop, how
many are still running and how many have branches that were never merged, and
asks you to confirm once. On yes, the desktop stops any running ones and
deletes each agent with its worktree and branch, the same as **Delete agent**
in the app; work that was not merged is lost. Siri then says how many it
deleted and how many it could not. Your iPhone must be unlocked.

"All agents" means the agents ForkMesh started on your desktop. Sessions you
started yourself in a terminal and cloud agents are left alone; delete those
in the app.

### Open a screen

- "Open inbox in ForkMesh", "Show my ForkMesh approvals",
  "Go to notes in ForkMesh"
- "Open *owner/repository* in ForkMesh"

The screens you can name are Agents, Running agents, Inbox, Approvals,
Mentions, Reviews, Repositories, Chat, Notes, Tasks, Calendar, Timers,
Desktops, Activity, Stats, Search and Settings.

### Turn listening on or off

- "Start listening in ForkMesh", "Turn on ForkMesh listening",
  "Turn on the ForkMesh microphone"
- "Stop listening in ForkMesh", "Turn off ForkMesh listening",
  "Pause listening in ForkMesh"
- "Toggle listening in ForkMesh" — turns it on when it is off, and off when
  it is on.

This is the Genie's **Always listen**, the same switch as in the Genie's
long-press menu. While it is on, ForkMesh hears you the whole time the app is
open and acts on what you say to the Genie ("Genie, open my inbox", "read my
inbox", "what was the last commit"). ForkMesh opens when you say the command,
and Siri answers "ForkMesh is listening" or "ForkMesh stopped listening".

Speech is recognised on the iPhone itself and nothing it hears is kept. It
listens only while ForkMesh is on screen, and pauses while the prompt
composer is open, while a voice message records and while anything speaks.
The first time, allow the
microphone and speech recognition when ForkMesh asks.

## On Apple Watch

You can start an agent from the ForkMesh watch app by voice too. Raise your
wrist, or hold the Digital Crown, and say:

- "Start a new agent in ForkMesh"
- "New agent in ForkMesh"
- "Create an agent in ForkMesh"

Siri asks "What should the agent do?". Say the prompt, then check it when
Siri reads it back and confirm. The watch app opens and your iPhone starts the
agent the same way the iPhone's Send button would: on the repository you used
last, with the place and model you picked there. The new agent appears on the
watch's agent list.

To aim at another repository, make a shortcut in the Shortcuts app: add the
watch's **Start an agent** action and type the repository's name (for example
`mobile` or `alice/mobile`) in its **Repository** field.

The watch needs your iPhone. It must be nearby, signed in to ForkMesh, with
the app opened at least once since it was installed or updated. If Siri says
"Your iPhone is out of reach", bring the iPhone closer and try again. Agents
set to run on the iPhone itself (Gemma or a model on this iPhone) start only
from the iPhone; pick a desktop or node there first.

## Commands without a built-in phrase

Apple allows ten built-in commands per app, so these are in the Shortcuts app
only: **Stop all agents**, **Start a timer**, **Open agent run**, **Open
inbox**, **Open agent approvals**, **Open issue** and **Open note**. Stop all
agents asks you to confirm first. Start a timer counts down 25 minutes unless
you set the minutes and a label in the action.

## Your own phrases

1. Open the **Shortcuts** app and tap **+**.
2. Tap **Add Action**, search for **ForkMesh** and pick an action, for example
   **Start an agent** or **Stop all agents**.
3. Fill in any fixed values (the repository, the agent, the minutes).
4. Rename the shortcut to the phrase you want to say, for example "New
   agent", and tap **Done**.

Saying "Hey Siri, *shortcut name*" now runs it, without "ForkMesh" in the
phrase. The **App Shortcuts** section of the Shortcuts app also lists every
built-in ForkMesh command, with its phrases.

## If Siri does not understand

- **Siri does not recognise an agent number.** Open the ForkMesh app so it
  refreshes the agent list, then try again. You can also say the command
  without the agent and pick it from the list.
- **"Sign in to ForkMesh first."** The app on this phone is signed out.
- **"Pair this phone with your desktop first."** No desktop signed in to your
  account could be reached. Check that the desktop app is running and signed
  in.
- **"Siri can only do that for agents on your desktop."** Merge and continue
  work on desktop agents only; manage cloud agents in the app.
- **"Update ForkMesh on the desktop to delete agents from here."** The
  desktop app is too old to delete agents remotely.
- **"Allow the microphone and speech recognition for ForkMesh in
  Settings, then try again."** Listening was refused earlier. In
  **Settings → ForkMesh**, turn on **Microphone** and **Speech
  Recognition**.
- **Siri opens a web search instead.** Include "ForkMesh" in the phrase, and
  check in Settings that ForkMesh is allowed to work with Siri.
