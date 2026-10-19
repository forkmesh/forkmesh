# Staying out of other apps' calls

Reported 2026-10-19: with ForkMesh installed, a Signal call on the same phone
went silent in bursts. Nothing in ForkMesh is meant to sound or listen during
someone else's call, so both phone apps now detect one and step back until it
ends. Android's half (Bluetooth search and scans) is in
[ANDROID.md](ANDROID.md#6-network-events-and-push), under "Bluetooth stays off
the air during calls".

## iOS

`ForkMesh/Platform/OtherAppCall.swift` watches `CXCallObserver`, which lists
cellular calls and every VoIP call an app reports through CallKit (Signal,
WhatsApp, FaceTime). ForkMesh's own direct calls do not use CallKit, so the
observer only ever sees other apps' calls. `OtherAppCall.inProgress` reads the
call list each time, and listeners registered with `onChange` hear the start
and end.

While a call is up:

- **Interface sounds** (`FmCuePlayer.activateSession`, `sessionReady`), Genie
  chimes and the voice-launch chimes do not play and do not touch the audio
  session. They used to switch the shared session to `.playback` on every app
  activation and leave it on, which can make iOS reconfigure the audio
  hardware under the call.
- **Genie always listening** stops when the call starts and does not restart
  until it ends (`PromptDictation.mayOverhear`, `GenieHub.mayOverhear`).
  Otherwise its 55 s restart switches the session off and back on as
  `.playAndRecord` in `.measurement` mode, which turns off the voice
  processing the call relies on. It also would have failed and turned itself
  off.
- **Ducking becomes mixing.** Speech (`AppleSpeechEngine`), Stretch Break's
  voice and button dictation ask for `OtherAppCall.duckingOptions`:
  `.duckOthers` normally, `.mixWithOthers` during a call. Ducking a call lowers
  the other person's voice to near silence.
- **Alarms** (desktop power alarm, Glasses Alert) still show, notify and
  vibrate but play no sound. A ring already sounding stops its sound when a
  call starts (`hush()`) and releases the session.
- **Agent Watch** sounds (`PlatformAgentWatchOutput`, `WatchMotionSound`) are
  skipped.
- **Bluetooth mesh** (`CoreBluetoothMeshTransport.setCallInProgress`, wired in
  `ChatFeature.register`): scanning stops and every subscribed central's
  connection latency goes from `.low` to `.high`, so AirPods and other
  headsets get the radio time their call audio needs. Links stay open.
  Scanning and `.low` latency return when the call ends.

Things a person starts on purpose (playing a voice message, a DJ deck,
dictating into the composer) still work during a call. Only their ducking
changes.
