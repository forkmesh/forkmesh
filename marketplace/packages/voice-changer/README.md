# Voice Changer

Talk in another voice on any call or meeting app, from the ForkMesh desktop
app. Free, from the ForkMesh Market.

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
  stall back behind a short fade. Without it the live voice crackled with
  static (fixed 2026-09-22).
- Echo when hearing yourself (fixed 2026-09-24): the engine itself adds no
  echo (a burst in leaves nothing behind it in any preset); the echo was the
  whole trip from the microphone to the ForkMesh Voice microphone or the
  "hear myself" output, measured at 161-188 ms on a PipeWire desktop and
  heard as a second voice after your own. Two cuts brought it to 138 ms:
  the STFT keeps its 1024-sample analysis window (the formant and noise
  estimates need it) but analyses with a long-rise, short-fall square-root
  Hann window and synthesises with a 512-sample window at the frame's end,
  so that stage delays 11 ms instead of 21 (unchanged voices still come out
  bit for bit; formant warps match the old spectrograms); and the player
  queue target dropped from 30 to 10 ms. Each queue refill adds its target
  as permanent delay, because `pw-cat` pulls the whole pipe into its own
  4 KB buffer and `FIONREAD` reads empty at the start. A smaller target was
  just as clean: no gaps idle on a 440 Hz tone, and fewer than the 30 ms
  target under full CPU load (7 clicks against 32 in 19 s). The rest is the
  helper processes: a bare `pw-cat --record | pw-cat --playback` chain
  costs 64 ms at 40 ms streams, and 20 ms streams crackle again (16 clicks).
  Going lower needs the engine inside the PipeWire graph (libpipewire), not
  behind `pw-cat` pipes.
- `content/desktop/VoiceChangerFeature.{h,cpp}`: the rail page. Start/Stop,
  microphone and cable pickers, "hear myself", presets and sliders, saved
  voices (package data in QSettings, removed with the package), AI voice
  design, and voice matching from a recording or WAV (consent required).
- `content/worker/voice_changer.py`: `/api/voice-changer`, signed by the
  desktop with `REQUEST_PROOF`. `GET` says whether AI voice design is
  available, and `POST /design` asks the host AI (`runtime.ai_json`) for
  clamped settings. Everything except AI design works with no account at all.

Tests: `python3 -m pytest marketplace/packages/voice-changer/tests` builds
and runs `content/desktop/tests/test_voice_engine.cpp` (g++) and
`test_voice_audio.cpp` (Qt Core), the second against stand-in `pw-cat`,
`pw-loopback` and `pw-dump` scripts, so it needs no sound card.
