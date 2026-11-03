# Silent Speech

An experimental **Silent Speech** section in the iPhone and Android apps:
teach it a few words, then mouth them without a sound. Earbuds that track
your head feel the small movements your jaw and face make, and the phone
writes the word down, or says it aloud. Nothing is sent anywhere.

It is ForkMesh's own take on Joe Blau's
[Subvox](https://github.com/joeblau/subvox) (MIT), a macOS research app that
asked whether AirPods head motion can tell mouthed words apart.

## How it works

- **Earbuds.** On iPhone: AirPods Pro, AirPods Max, AirPods (3rd generation
  and later) or Beats Fit Pro. On Android 13 or later: earbuds that expose a
  head tracker, such as Pixel Buds Pro or Galaxy Buds with 360 Audio.
- **Teach.** Pick a word, hold the button, mouth it, let go. Each take is
  kept on the phone. Add your own words, and optionally a phrase to say
  instead ("thirsty" can say "Could I have some water, please?").
- **Check.** Tests every take against all the others, then against 500
  shuffles of the labels, and says whether your head motion really tells the
  words apart better than chance, with a table of what was taken for what.
- **Speak silently.** Hold the button and mouth a word, or turn on **Listen
  without holding**: it measures how still you are for two seconds, then
  picks out words by themselves. Recognised words build a transcript you can
  copy or have read aloud.

## Limits

- A small vocabulary, trained by you for you. Reseating the earbuds can
  change the signal; record a few fresh takes if it starts missing.
- Earbuds report head motion, not the voice: the speech accelerometer and the
  inward microphones are not open to apps.
- Android head trackers report turning but not movement, so the **Movement**
  channel is iPhone only.

Design notes: [`docs/marketplace/silent-speech.md`](../../../docs/marketplace/silent-speech.md).
