# Genie spider welcomes in #welcome

Part of the [app docs](../README.md). Code: `app/public/chat-genie-welcome.js`
(the 10 scenes, `pickGenieWelcome`, `drawGenieWelcome`, `mountGenieWelcome`,
`loadGeniePainter`), `app/public/chat.js` (`isWelcomeChannel`,
`sendGenieWelcome`, `renderGenieWelcome`), the `#chat-genie-welcome` button in
`app/public/chat.html`. Tests: `app/tests/test_chat_genie_welcome.py`.

## The button

When the open channel is the everyone channel `welcome`
([chat-everyone-channels.md](chat-everyone-channels.md)) and the reader is
signed in, the channel header shows a **Welcome** button. Pressing it posts an
ordinary `chat` frame to the channel with two extra parts:

- `genieWelcome`: an integer 0–9, the scene, picked at random by
  `pickGenieWelcome`, never the same as this tab's previous press;
- `text`: that scene's sentence ("🪔 A genie spider pops out of a magic lamp
  to say welcome!"), so the desktop, the phones and anything else that does not
  know the field still show a readable message.

The button rests for 5 s after a press so one person cannot flood the channel.

## The scenes

`silk` (drops in on a thread), `lamp` (pops out of a magic lamp in purple
smoke), `confetti`, `carpet` (flies in on a magic carpet), `twirl` (spins in
with sparkles), `bounce` (three hops with dust puffs), `balloons` (floats down
on three balloons, then lets them go), `web` (spins a web and writes
WELCOME!), `fireworks` and `peekaboo` (peeks out of a hole, then hops up).

Each scene is a function of time over 4 s on a 280 × 150 stage. It returns the
spider's place, scale, turn and pose (reactions such as wave, cheer, giggle,
nod and eureka) plus effects drawn behind and in front of it. Every reaction
ends before 4 s, so the last frame is a calm spider and is what a message shows
when it is not playing.

## Drawing

The spider is the web Genie painter (`ForkMeshGenie.paint` in
`www/public/genie-spider.js`, [../www/genie-spider.md](../www/genie-spider.md)),
loaded once on demand. The chat page leaves the wandering spider out, so the
loader sets `data-genie="off"` on `<html>` before the script runs: only the
painter is used. Without the painter (the script failed to load) the stage and
its effects still draw.

A message with a valid `genieWelcome` gets a stage (a button) under its text.
It plays once when it first scrolls into view if it is your own or less than
10 minutes old, and not again when the channel is redrawn
(`playedGenieWelcomes`). Clicking it plays it again. With
`prefers-reduced-motion` it only shows the last frame. The stage and the
button carry `data-instance-plugin="genie"`, so a white-label instance with the
`genie` plugin off hides both.

Only the web chat has the button and the animation so far; other clients show
the sentence.
