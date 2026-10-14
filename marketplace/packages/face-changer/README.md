# Face Changer

Wear a stand-in face, swap your background and change your voice, live in the
browser, from the ForkMesh dashboard. Free, from the ForkMesh Market.

- `content/web/view.html` and `content/web/page.js`: the dashboard page at
  `/dashboard/face-changer`. Start camera opens the camera and microphone;
  the picture goes through the same face stage as video calls on `/chat`
  (`app/public/chat-call-faces.js`, `createFaceStage`), so the page never shows
  the raw camera in place of a chosen face: the stage blurs whenever the face
  cannot be followed.
- Face: the built-in stand-in faces and "Your photo" (a front-facing picture
  you upload; it stays in this browser). Background: blur, the built-in rooms
  and your own photo. Voice: the call voices through `createVoiceStage`, with
  "Match face", "Your own voice" and "Hear myself" (the call Preview's voice
  controls, `mountVoicePreviewControls`).
- Record makes a clip from the stage's video track and the voice track
  (`MediaRecorder`, up to five minutes) to play back, download or discard.
  Snapshot downloads a PNG of the stage. Pop out opens the live picture in a
  small window you can share into any call or meeting app.
- Choices are kept in `localStorage` under the same keys the calls use, so a
  face picked here is the face used on the next call, and the other way round.
- `content/desktop/FaceChangerLink.cpp`: a desktop rail button that opens the
  page on the active ForkMesh host in the browser.

The page imports the engine from the App (`/chat-call-faces.js`,
`/chat-call-voice.js`, `/chat-call-backgrounds.js`, `/chat-call-face-custom.js`)
and the shared page glue from `/call-studio.js`; there is no copy of the
engine in this package. Nothing is uploaded: the package has no Worker module
and no server data.

Tests: `python3 -m pytest marketplace/packages/face-changer/tests`.
