# Silent Speech

`marketplace/packages/silent-speech` (Market id `silent-speech`) recognises a
small vocabulary of words mouthed without a sound, from the head motion that
AirPods or head-tracking earbuds report. It is a phone section on iPhone and
Android (route `silentSpeech`, web path `/dashboard/silent-speech`), off by
default and account-synced like the other mobile tools
([core-plugins.md](core-plugins.md)). User guide: the package
[README](../../marketplace/packages/silent-speech/README.md).

## Where it comes from

It reimplements the idea of Joe Blau's [Subvox](https://github.com/joeblau/subvox)
(MIT), a macOS research prototype: record per-user templates of mouthed words
from `CMHeadphoneMotionManager`, match new takes by dynamic time warping, and
test against a label-permutation null whether the motion carries any signal.
No Subvox code is copied; the recogniser is written from the description.

What this version changes:

| Subvox | Silent Speech |
| --- | --- |
| macOS, Space bar to hold | iPhone and Android, a hold button, plus a VoiceOver/TalkBack tap-to-start action |
| AirPods only | AirPods on iPhone; Android 13+ `Sensor.TYPE_HEAD_TRACKER` earbuds |
| Attitude change as angles | Attitude change as the rotation vector of `q0⁻¹·q`, which has no wrap or gimbal lock |
| Raw sensor rate | Every take resampled to 25 Hz, so earbuds with other rates match |
| DTW distance | Band-limited DTW normalised by `n + m`, with early abandon |
| Leave-one-out re-runs DTW per shuffle | One distance matrix, reused by leave-one-out and all 500 shuffles, so checking runs in about a second on a phone |
| p-value | `(1 + b) / (1 + N)` with a seeded SplitMix64 shuffle, identical on both phones; null mean shown beside 1/K chance |
| Hand-set max distance | Suggested from the correct leave-one-out matches (90th percentile × 1.25) |
| Noise floor | Median + MAD (robust to a stray movement while calibrating), hysteresis on/off thresholds |
| Transcript | Transcript with undo, copy, read aloud, and a phrase per word for one-word messages |

## Recogniser

Shared logic is `FMTools/SilentSpeechModels.swift` and the Kotlin port
`core/.../silentspeech/SilentSpeech.kt`; both are platform-free and tested on
Linux and the host JVM with the same synthetic data and expected numbers
(`SilentSpeechTests.swift`, `SilentSpeechTest.kt`, `testCrossPlatformParity`).

1. **Samples.** iPhone: `CMDeviceMotion` rotation rate (rad/s), attitude
   quaternion and user acceleration (g). Android: the head tracker's rotation
   vector (turned into a quaternion) and angular velocity; it has no
   acceleration, so the Movement channel is hidden there.
2. **Features** (`SilentFeatures.frames`). Resample to 25 Hz (linear,
   quaternion nlerp), trim lead-in and lead-out below 12% of the peak energy
   (`|rotation rate| + |acceleration|`, at least 4 frames kept), then per
   frame three values per chosen channel: acceleration, rotation rate and
   attitude change since the first kept frame. Each channel is centred on the
   take's own mean.
3. **Scaling.** `SilentNormalizer` divides each channel by its standard
   deviation over every training frame. It uses no labels, so it is fair to
   share between leave-one-out folds.
4. **Matching.** `SilentDTW.distance`: Euclidean frame cost, Sakoe–Chiba band
   of 25% of the longer take (at least the length difference), total path cost
   divided by `n + m`. A word's score is its best template, or the mean of
   its best three once every word has at least 10 takes
   (`SilentEvaluator.neighbors`), so one mislabelled take cannot win alone.
   A match is rejected when it is above the max distance or less than 8% ahead
   of the runner-up.
5. **Check** (`SilentEvaluator.evaluate`). Words with at least two takes are
   compared. Leave-one-out accuracy on the distance matrix, then 500 label
   shuffles of the same matrix; `p < 0.05` is reported as better than chance.
6. **Listening without holding** (`SilentSegmenter`). Two seconds of
   stillness set the floor (median) and spread (1.4826 × MAD). A word starts
   after two samples above `floor + max(4·spread, 0.05)`, with 0.2 s of
   pre-roll, and ends after 0.35 s below `floor + max(2·spread, 0.025)`;
   anything under 0.3 s from onset is dropped, and 3 s is the longest word.

## Reading session

One take per button press gives a handful of takes a word, and DTW with a
handful of templates is fragile. **Read sentences for 5 minutes**
(`SilentReading`) collects far more, in the same condition the listener
decodes in: words mouthed one after another with a short stop between them.

- **Script.** `SilentReading.script` deals the vocabulary in shuffled passes
  (seeded SplitMix64, identical on both phones), never the same word twice in
  a row, cut into sentences of 4 to 6 words. Every word appears equally often
  and in varied neighbours.
- **Labelling.** The session first measures stillness (two seconds, as in
  step 6), then feeds `SilentSegmenter` output to `hear`. The highlighted word
  moves on with each segment; when a sentence has as many segments as words,
  they are labelled in order and saved as takes. **Start again** drops a
  half-read sentence when the count slips (a nod, two words run together),
  **Drop last sentence** removes a finished one, **Skip** moves on.
- **Length.** Five minutes of motion-clock time; the session ends after the
  sentence in progress, or on **Finish** or leaving the page. Around 1.5 s a
  word gives roughly 200 takes, about 40 per word for the starter five.

## Storage and privacy

Takes are kept only on the phone: iPhone in Application
Support/`SilentSpeech/dataset.json` (complete file protection), Android in
`filesDir/silent_speech/dataset.json`. Both use one JSON format (version 1;
samples as `[t, ωx, ωy, ωz, qw, qx, qy, qz]` plus `[ax, ay, az]` when there is
acceleration), so a dataset reads on either phone. Settings are the
`silentSpeech.*` keys in `SilentSpeechPrefs`. Motion is read only while the
page is open. iOS asks for Motion & Fitness through `NSMotionUsageDescription`;
Android's head tracker needs no permission.

## Limits

Headphone motion is filtered and slow (about 25 Hz on AirPods); the speech
accelerometer and inward microphones are not open to apps, so this reads jaw
and head movement, not the voice. Templates are per user and per seating of
the earbuds. Android head trackers are rare and vendor-dependent: the page
says so when none is connected.
