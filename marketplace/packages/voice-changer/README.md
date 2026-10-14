# Voice Changer

Talk in another voice on any call or meeting app, from the ForkMesh desktop
app, or try the voices on the web dashboard. Free, from the ForkMesh Market.

- `content/desktop/VoiceEngine.{h,cpp}`: the voice engine, plain C++ with no
  Qt, about 35 ms of latency. Each 21 ms STFT frame is split into source
  (pitch) and filter (vocal tract) by cepstral smoothing and only the filter
  is warped, so harmonics never move there. The pitch then moves in the time
  domain by pitch-synchronous overlap-add: single periods of the waveform are
  laid down closer together or further apart, each keeping its own shape, so
  the new voice stays one clean voice instead of a phasey, watery one.
  Optional noise reduction (minimum statistics + Wiener gain, floor -18 dB)
  learns the room from the gaps between words, never from the voice. A
  pitch tracker learns the speaker's own pitch, so a voice naming a target
  in hertz lands there from any starting voice. `learnVoice()` fits a voice from two
  recordings (the target's pitch plus the envelope warp and tilt that best
  map one speaker onto the other).
- `content/desktop/VoiceAudio.{h,cpp}`: the audio path, on its own thread.
  On Linux it creates a **ForkMesh Voice** microphone while running
  (PipeWire: one `pw-loopback`; PulseAudio: `module-null-sink` +
  `module-remap-source` through `pactl`) and streams raw float audio through
  `pw-cat` or `parec`/`pacat`. On Windows and macOS there is no user-space
  virtual microphone, so the changed voice is played through Qt Multimedia
  into a virtual audio cable the user installs once; that needs a ForkMesh
  build with Qt Multimedia (`FORKMESH_HAVE_MULTIMEDIA`).
  Microphone and output clocks drift apart, so a `RateMatcher`
  (`VoiceEngine.h`) keeps the output queue near 10 ms: it bends the
  resampling ratio by a few cents to follow the drift, refills a queue that
  ran dry once with faded-in silence, and cuts a queue grown long after a
  stall back behind a short fade, so the live voice does not crackle.
- Latency: the engine adds no echo of its own; the round trip from the
  microphone to the ForkMesh Voice microphone or the "hear myself" output is
  kept low by synthesising with a 512-sample window at the frame's end (the
  1024-sample analysis window is kept for the formant and noise estimates,
  with a long-rise, short-fall square-root Hann window), and a 10 ms player
  queue target (each refill adds its target as permanent delay, because
  `pw-cat` buffers the pipe). The remaining delay is the `pw-cat` helper
  processes; going lower needs the engine inside the PipeWire graph
  (libpipewire).
- `content/desktop/VoiceChangerFeature.{h,cpp}`: the rail page. Start/Stop,
  microphone and cable pickers, "hear myself", presets and sliders, saved
  voices (package data in QSettings, removed with the package), AI voice
  design, and voice matching from a recording or WAV (consent required).
- `content/worker/voice_changer.py`: `/api/voice-changer`, signed by the
  desktop with `REQUEST_PROOF`. `GET` says whether AI voice design is
  available, and `POST /design` asks the host AI (`runtime.ai_json`) for
  clamped settings. Everything except AI design works with no account at all.

- `content/web/view.html` and `content/web/page.js`: the dashboard page at
  `/dashboard/voice-changer`. It runs the call voice changer
  (`app/public/chat-call-voice.js`, `createVoiceStage`, through
  `createVoicePreview` and `mountVoicePreviewControls`) on the microphone:
  pick a voice, "Hear myself" with headphones, read your usual pitch, the
  pitch you speak at and the pitch the voice aims for, and record a clip to
  download. Shared page glue is `/call-studio.js`. It does not create a
  microphone other apps can pick; that stays the desktop page's job.

Tests: `python3 -m pytest marketplace/packages/voice-changer/tests` builds
and runs `content/desktop/tests/test_voice_engine.cpp` (g++) and
`test_voice_audio.cpp` (Qt Core), the second against stand-in `pw-cat`,
`pw-loopback` and `pw-dump` scripts, so it needs no sound card.
