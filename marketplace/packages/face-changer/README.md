# Face Changer

Wear a stand-in face and a new outfit, swap your background and change your
voice, live in the browser, from the ForkMesh dashboard, or in the ForkMesh
iPhone and Android apps. Free, from the ForkMesh Market.

The phone apps do not open this page: their Face Changer is native, built on
the same face, background and voice engine bundled in the app, and it is where
calls get their face, background and voice
(`docs/app/chat-direct-calls.md`, Face Changer on the phones).

- `content/web/view.html` and `content/web/page.js`: the dashboard page at
  `/dashboard/face-changer`. Start camera opens the camera and microphone;
  the picture goes through the same face stage as video calls on `/chat`
  (`app/public/chat-call-faces.js`, `createFaceStage`), so the page never shows
  the raw camera in place of a chosen face: the stage blurs whenever the face
  cannot be followed.
- Face: the built-in stand-in faces and "Your photo" (a front-facing picture
  you upload; it stays in this browser). Outfit: your own clothes or one of
  six drawn outfits (suit and tie, hoodie, cable-knit sweater, denim jacket,
  flannel shirt, turtleneck) that follow your top half
  (`app/public/chat-call-outfits.js`). Background: blur, the built-in rooms
  (with portrait renders for a camera held upright) and your own photo. Voice: the call voices through `createVoiceStage`, with
  "Match face", "Your own voice" and "Hear myself" (the call Preview's voice
  controls, `mountVoicePreviewControls`).
- Record makes a clip from the stage's video track and the voice track
  (`MediaRecorder`, up to five minutes) to play back, share, download or
  discard. Share opens the phone's share sheet in a phone browser through
  `navigator.share` (`shareStudioBlob` in `/call-studio.js`).
  Snapshot downloads a PNG of the stage. Pop out opens the live picture in a
  small window you can share into any call or meeting app.
- Choices are kept in `localStorage` under the same keys the calls use, so a
  face picked here is the face used on the next call, and the other way round.
  The outfit is kept under `forkmesh.chat.callOutfit`; calls do not offer it
  yet.
- The picture box takes the camera's shape, so a portrait camera gets a tall
  box instead of a strip inside a 16:9 frame.
- `content/desktop/FaceChangerLink.cpp`: a desktop rail button that opens the
  page on the active ForkMesh host in the browser.

The page imports the engine from the App (`/chat-call-faces.js`,
`/chat-call-voice.js`, `/chat-call-backgrounds.js`, `/chat-call-face-custom.js`,
`/chat-call-outfits.js`)
and the shared page glue from `/call-studio.js`; there is no copy of the
engine in this package. Nothing is uploaded: the package has no Worker module
and no server data.

Tests: `python3 -m pytest marketplace/packages/face-changer/tests`.
