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
WhatsApp, FaceTime). Since 2026-10-21 ForkMesh reports its own incoming
direct calls through CallKit too (`DirectCallKit`, see
`native/ios/docs/FMChat.md`), so the observer also sees a ForkMesh call while
it rings or runs, and the rules below apply to it: interface sounds, chimes and
the mesh stay quiet during our own call as well, which is what a call needs. `OtherAppCall.inProgress` reads the
call list each time, and listeners registered with `onChange` hear the start
and end.

Since 2026-10-23 every ForkMesh call counts, not only the CallKit ones: our
outgoing calls (and calls in the China region, where CallKit is off) never
appear in `CXCallObserver`, so during them sounds and dictation could switch
or deactivate the shared session and the other person heard silence.
`DirectCallAudio` calls `OtherAppCall.shared.setOwnCall` while the call engine
has a phase or a group call runs; `inProgress` and `onChange` include it.
Because it is our own session the call needs, code that changes the session
without checking `inProgress` (speech, voice messages, view-once voice,
dictation, the DJ studio, the alarms' stop) skips its category change during
our call and deactivates only through `OtherAppCall.deactivateSession()`,
which does nothing then (`OtherAppCall.ownCall`). Details in
`native/ios/docs/FMChat.md`, "Audio session during a call".

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

## Android

`platform/OtherAppCall.kt` reads `AudioManager.mode` (a mode above normal
means a call, cellular or VoIP), listening with `addOnModeChangedListener`
on Android 12+ and polling every 2 s while the app is in the foreground
below that; every check also reads the mode fresh. ForkMesh's own direct call
(`DirectCallFeature.engine.phase`) does not count. While a call is up
(wired in `App.onCreate`): interface sounds are quietened
(`FmSoundCues.quieten`), Agent Watch plays nothing, and Glasses Alert still
notifies and vibrates but plays no ring; a ring already sounding stops and
the alarm volume is restored. ForkMesh on Android never asks for audio focus
for these sounds, so it already mixes instead of ducking. The diff scroll
ratchet in `ui/kit/FmSoundCues.kt` does not check `quieten`; that kit file
is owned by the design-layer work and is left as it is.
