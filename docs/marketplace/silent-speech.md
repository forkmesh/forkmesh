# Silent Speech

`marketplace/packages/silent-speech` (Market id `silent-speech`) recognises
words mouthed without a sound, from the head motion that
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
5. **Next-word guess** (`SilentLanguage`, `SilentScores.ranked`). With a
   large vocabulary many words sit close together, so the decoder adds a
   bigram model of the reading sentences (built-in and the user's own):
   `P(w | previous) = (c(previous, w) + 2·P(w)) / (c(previous) + 2)`, with
   add-one `P(w)`. Each word's score becomes
   `d + 0.5·τ·(−ln P − min −ln P)`, where `τ` is half the gap between the
   best and the median word's distance, so the guess only breaks near-ties
   and never outweighs a clearly closer word. The previous word is the
   transcript's last; an empty transcript uses the sentence-start context.
   The 8% margin is measured on these scores.
6. **Check** (`SilentEvaluator.evaluate`). Words with at least two takes are
   compared. Leave-one-out accuracy on the distance matrix, then 500 label
   shuffles of the same matrix; `p < 0.05` is reported as better than chance.
   To stay within seconds on a phone with a big vocabulary, it uses the
   newest 12 takes of each word and at most 600 takes in all
   (`SilentEvaluator.sample`), and runs fewer shuffles when there are many
   takes: `min(500, max(100, 4·10⁷ / n²))` (`SilentEvaluator.rounds`), so
   600 takes get 111. With at least 100 shuffles the smallest p is 0.0099,
   still well under 0.05. An 840-take, 70-word set took 7 s on a desktop JVM
   with 800 takes and 500 shuffles before these limits, about 1 s after.
7. **Listening without holding** (`SilentSegmenter`). Two seconds of
   stillness set the floor (median) and spread (1.4826 × MAD). A word starts
   after two samples above `floor + max(4·spread, 0.05)`, with 0.2 s of
   pre-roll, and ends after 0.35 s below `floor + max(2·spread, 0.025)`;
   anything under 0.3 s from onset is dropped, and 3 s is the longest word.
   On the page this is the **Hands-free** chip.

## Reading session

One take per button press gives a handful of takes a word, and DTW with a
handful of templates is fragile. **Read sentences · 5 min** (`SilentReading`)
collects far more, in the same condition the listener decodes in: words
mouthed one after another with a short stop between them.

- **Vocabulary.** There is no fixed word list. `SilentText.sentences` holds
  30 everyday sentences (about 110 different words, including the starter
  five); the user can add their own sentences, or single words, in the one
  "Add a word or your own sentence" field (`SilentDataset.addEntry`;
  `word: phrase` sets what a one-word message says aloud). Every word read
  becomes a word in the dataset (`SilentDataset.learn`), so the vocabulary
  grows with each session. `SilentText.words` lowercases and splits on
  anything but letters, digits and apostrophes.
- **Plan.** A session reads 8 sentences (`SilentReading.plan`): the user's
  own first, then the built-in ones, ranked by how few takes their words
  already have (each word counted up to 10), plus any added word that is in
  no sentence, dealt into sentences of 5. Repeating 8 sentences for five
  minutes gives each of their words several takes; the next session picks
  the next least-trained 8, so a few sessions cover the whole vocabulary.
- **Script.** `SilentReading.script` deals those sentences in shuffled
  passes (seeded SplitMix64, identical on both phones), never the same
  sentence twice in a row.
- **Labelling.** The session first measures stillness (two seconds, as in
  step 7), then feeds `SilentSegmenter` output to `hear`. The highlighted
  word moves on with each segment; when a sentence has as many segments as
  words, they are labelled in order and saved as takes. **Again** drops a
  half-read sentence when the count slips (a nod, two words run together),
  **Drop last** removes a finished one, **Skip** moves on.
- **Length.** Five minutes of motion-clock time; the session ends after the
  sentence in progress, or on **Finish** or leaving the page.

## Motion view

The top card draws the last six seconds of head motion (`SilentScope`,
resampled to 25 Hz like the recogniser) so the user can see what the earbuds
report and whether a word stands out:

- **Turning** (rotation rate, °/s), **Head angle** (rotation vector of
  `q_ref⁻¹·q`, °) and, on iPhone, **Movement** (user acceleration, g), each
  as X (nod), Y (tilt) and Z (turn) lines on a symmetric scale that grows
  with the signal (at least ±30 °/s, ±5°, ±0.05 g), with the current values
  in the legend. The angle reference is reset after each stillness
  measurement and by the ↻ button.
- **Energy** (`|ω| + |a|`, what the segmenter thresholds) with the on and
  off thresholds dashed; every heard word is shaded across all strips and
  labelled with the word read or recognised.
- **Frequency**: the power spectrum of the three rotation axes summed (last
  64 frames, mean removed, Hann window, zero-padded, so bins are 0.39 Hz up
  to 12.5 Hz), with the strongest peak above 0.5 Hz named.

Axis colours are the first three slots of the shared chart palette (blue,
orange, aqua), which stay apart for colour-blind readers in light and dark;
the legend names each axis, so colour is never the only cue.

## Page layout

One phone screen: the motion card, then the reading button (or the sentence
being read), a collapsed **Words** row (taught and total words, takes;
opens to chips with take counts, hold-to-teach for the selected word and
the add field), the channel chips with **Check** (a mix-up map, the verdict
and the three worst mix-ups), and **Speak silently** (hold button,
**Hands-free**, **Speak** and **Skip far-off** chips, last match, the
transcript with icon actions). The long explanations and per-channel notes
went; the README is the user guide.

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

Headphone motion is filtered and slow (about 25 Hz on AirPods), so words
that move the jaw alike (`would` and `could`) stay hard to tell apart even
with the next-word guess; the speech
accelerometer and inward microphones are not open to apps, so this reads jaw
and head movement, not the voice. Templates are per user and per seating of
the earbuds. Android head trackers are rare and vendor-dependent: the page
says so when none is connected.
