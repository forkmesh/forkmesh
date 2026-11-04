# Silent Speech

An experimental **Silent Speech** section in the iPhone and Android apps:
read everyday sentences to it for five minutes, then mouth any of their
words without a sound. Earbuds that track
your head feel the small movements your jaw and face make, and the phone
writes the word down, or says it aloud. Nothing is sent anywhere.

It is ForkMesh's own take on Joe Blau's
[Subvox](https://github.com/joeblau/subvox) (MIT), a macOS research app that
asked whether AirPods head motion can tell mouthed words apart.

## How it works

- **Earbuds.** On iPhone: AirPods Pro, AirPods Max, AirPods (3rd generation
  and later) or Beats Fit Pro. On Android 13 or later: earbuds that expose a
  head tracker, such as Pixel Buds Pro or Galaxy Buds with 360 Audio.
- **Watch.** The top of the page draws the last six seconds of head motion:
  turning speed, head angle and (iPhone) movement on all three axes, the
  motion energy with each heard word shaded and named, and the frequency
  spectrum with its peak. Tap ↻ to zero the head angle. Sit still for ten
  seconds and it may show your heartbeat, felt as the tiny nod each beat
  gives your head (experimental).
- **Teach.** Tap **Read sentences · 5 min**: it shows short everyday
  sentences and highlights the next word to mouth. Mouth each word with a
  short stop between words, and pause at the end of the sentence. When it
  heard as many words as the sentence has, every word becomes a take; when
  the count is off (a nod, two words run together) it says so and asks for
  the sentence again, so no word is saved under the wrong name. Each session picks
  the 8 sentences whose words have the fewest takes, so a few sessions cover
  more than a hundred words. Add your own sentence or word in **Words**
  (type `thirsty: Could I have some water, please?` to have a word say a
  phrase), or pick a word there, hold the button, mouth it and let go.
  Takes are kept on the phone.
- **Diagnose.** Tests every take against all the others, then against 500
  shuffles of the labels, and says whether your head motion really tells the
  words apart better than chance, with a mix-up map. Below it, a list of
  what to fix, most serious first, each with a button that fixes it: takes
  that barely moved, ran too long or look like another word (dropped in one
  tap), words with too few takes, a better choice of channels, a better
  setting for close calls, a stale **Skip far-off** limit, too much movement
  between words, and slow earbuds.
- **Speak silently.** Hold the button and mouth a word or several, or turn on
  **Hands-free**: it measures how still you are for two seconds, then picks
  out words by themselves. It also guesses which word is likely to come next
  from the sentences, to settle close calls. Recognised words build a
  transcript you can copy or have read aloud.

## Limits

- Trained by you for you, and words that move the jaw alike stay hard to
  tell apart. Reseating the earbuds can
  change the signal; record a few fresh takes if it starts missing.
- Earbuds report head motion, not the voice: the speech accelerometer and the
  inward microphones are not open to apps.
- Android head trackers report turning but not movement, so the **Movement**
  channel is iPhone only.

Design notes: [`docs/marketplace/silent-speech.md`](../../../docs/marketplace/silent-speech.md).
