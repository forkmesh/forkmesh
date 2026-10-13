# Speech Playground

Speech Playground is a free ForkMesh package for iPhone. You type or paste
text into a box and the phone reads it aloud in the most natural voice it
has. It runs on the device: no text or audio goes to a server, and the
package has no Worker, no account data and no web or desktop page yet.

Open it from **You → Personal → Speech**, from the Market's **Open Speech
Playground** button, or with the `forkmesh://dashboard/speech` link.

## What it does

- **Natural voice by default.** On first open the app picks the best voice
  installed for the phone's language, in this order: your Personal Voice
  (once you allow it), then Premium, then Enhanced, then Standard. Novelty
  voices (Bells, Zarvox and the like) and the older robotic voices (Eddy,
  Flo, Grandma and the other Eloquence voices) never win the automatic pick.
  A voice you choose yourself is remembered.
- **Voices list.** Every voice on the phone, grouped by language with the
  phone's language first, best quality first, each with a quality badge and
  a play button that previews it with a short line.
- **Download hint.** When the phone has no Premium or Enhanced voice for its
  language, the page says where to get one: Settings › Accessibility ›
  Read & Speak › Voices. The list refreshes when you come back to the app.
- **Personal Voice.** On iOS 17 and later, **Use my Personal Voice** asks for
  permission to speak in the voice you recorded in Settings › Accessibility ›
  Personal Voice, and picks it once you allow it.
- **Follow along.** While it speaks, the box shows the text with the word
  being spoken highlighted. Tap the text to stop.
- **Speed and pitch.** Speed from 0.5× to 2× and pitch from lower to higher,
  remembered between visits; **Reset** puts both back.
- **Samples.** Four sample texts (Hello, Story, Update, Poem) to try voices
  quickly.
- **Save audio.** The share button renders the whole text to an AAC `.m4a`
  file in the app's temporary folder and opens the share sheet, to save to
  Files, send in Messages, or AirDrop.

Speech pauses other audio's volume (ducks it) while it talks and hands the
audio session back when it finishes. It never records anything.

## Where the code lives

| Part | File |
| --- | --- |
| Model, voice ranking, tuning, highlight | `native/ios/ForkMeshCore/Sources/FMTools/SpeechModels.swift` |
| Tests (run on Linux) | `native/ios/ForkMeshCore/Tests/FMToolsTests/SpeechTests.swift` |
| AVSpeechSynthesizer engine, audio export | `native/ios/ForkMesh/Platform/SpeechSynthesis.swift` |
| Page | `native/ios/ForkMesh/Features/Tools/SpeechScreen.swift` |
| Route `.speech`, Market mapping | `FMApi/Navigation`, `FMPackages/PackageRoutes.swift`, `FMPackages/MarketModels.swift` |

The manifest's `ios: builtin:speech` entrypoint marks the package as built
into the iPhone app. The desktop Market lists it as "Catalog only" because it
has no `desktop` entrypoint yet.

## Notes

- Speed maps onto `AVSpeechUtterance.rate` in two straight lines: 0.5× is
  0.25, 1× is Apple's default 0.5, and 2× is 0.75. Rates above 0.75 sound
  rushed even in Premium voices.
- The highlight uses the UTF-16 ranges the synthesizer reports; a range that
  splits an emoji is ignored rather than drawn wrong.
- A finished or cancelled utterance only ends the playback it belongs to, so
  starting a new line while another is stopping does not reset the page.
- Saving audio uses a second synthesizer, so it can run while the page is
  speaking. The file is written as the buffers arrive; the zero-length final
  buffer, or the synthesizer's finish callback 0.4 s later, closes it.
