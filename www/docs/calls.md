# Voice and video calls

You can call anyone you have a direct message with, or everyone in a private
channel at once. Calls work on the web at `/chat`, on iPhone and on Android.
The desktop app has no call engine of its own: its call buttons open the call
in your browser and ask before anything rings.

## Start a call

1. Open the direct message and press the phone button (voice) or the camera
   button (video) in the header.
2. A pre-call check opens first. You see your camera, can pick a face,
   background or voice filter, and can press **Record a 10-second test clip**
   to hear exactly what the other person will get.
3. Press **Start voice call** or **Start video call**. Only now does it ring.

The other person's phone rings on any screen. An iPhone shows the system call
screen, even when it is locked or ForkMesh is closed, and even with
notifications turned off. The push notification never names the caller; the
app shows the name once it has opened the encrypted invite. A call nobody
answers leaves a "Missed audio call" or "Missed video call" line in the
conversation. A declined call leaves nothing, and the caller sees "Call
declined" or "Busy" with the reason.

## During a call

- **Back to chat** shrinks the call into a small window with Mute, Hang up
  and Open. Drag it anywhere; tap it or press Open to make it large again.
  The web remembers where you left it.
- Until the other person's video arrives, or while their camera is off, you
  see your own camera and their name instead of a black screen.
- On iPhone, video calls play through the loudspeaker, like FaceTime. Voice
  calls use the earpiece. The ring/silent switch does not mute a call.
- On iPhone, app sounds, speech and dictation leave your microphone alone
  while a call runs. During someone else's call (a phone call, Signal,
  FaceTime) ForkMesh stays quiet too.

## When the connection drops

A call reconnects by itself after a network drop, or when the other person's
audio goes silent while the connection still looks fine. The call says
"Reconnecting…" meanwhile. If it is not back within 30 seconds it ends with
"Call lost". A call that never connects ends after 30 seconds with "Could not
connect". If your microphone is unplugged or interrupted, the call opens it
again and says "Microphone reconnected".

One-to-one media goes straight between the two devices, with no relay server
in between. Two people who are both behind strict networks (some mobile
carriers and office firewalls) may not be able to connect at all.

## Call quality and Diagnose

Every call shows a small pill with four signal bars, a voice waveform (you up,
the other side down) and the round trip time. Four bars is a good connection,
one a poor one. Click the pill to open charts of your voice, the network and,
on video calls, the video in and out.

Next to it, **Diagnose** is grey while it measures, green while the call is
fine, and red when
it finds a lasting problem: high latency, lost packets, jitter, video held
back by bandwidth or by your device, a low frame rate, or a slow face filter.
Press it to see each problem, why it matters and the fixes in order. **Copy
prompt** gives you a ready question for an AI assistant; paste its `LESSON:`
line back and the next prompt starts from it. Fixes that worked on your
device move up the list. Nothing is sent anywhere: the report stays on your
device.

Voice is sent at up to 128 kbps with protection against lost packets, and when
the network is tight the call keeps your voice and lowers the picture first.

## Group calls

In a private channel, the same buttons start a group call for up to 16 people.
Members who are not in it see "A call is in progress here" with a Join button.
Public channels have no group calls, because their messages are readable by
the server. On the web, stay on the channel during the call: if you switch to
another one, people who join meanwhile see a blank tile for you until you
come back.

## Verification codes

Group calls on ForkMesh's newer call engine (on iPhone, and on the web in
browsers that support it) show four emoji with their names and "Or compare
numbers: 123 456". Read them aloud with everyone and press **They match** or
**They don't match**. The code changes when someone joins, and the panel says
who is not verified yet. If the codes differ, someone may be listening in:
leave the call and tell the others another way. A tile warns you when a
person's video cannot be confirmed as theirs or fails a check. Scanning a QR
code instead, and remembering verified people between calls, are not built
yet.

## What is encrypted

- **One-to-one calls.** The messages that set up the call are sealed with
  chat encryption, so the relay sees only ciphertext and stores none of it.
  Audio and video are encrypted and go directly between the two devices. The
  relay learns that a call is ringing in a conversation and when, not whether
  it is voice or video.
- **Group calls.** Each sender encrypts every audio and video frame on their
  device. ForkMesh's call server forwards the frames without being able to
  decrypt them. On the newer engine the keys use post-quantum key exchange and
  each sender signs their media, so nobody in the call can fake someone
  else's picture.
- Face, background and voice filters run on your device.

See [Chat encryption and integrations](security/sealed-chat.md) for how chat
itself is sealed, and [Safety numbers and trusted devices](chat-safety-numbers.md)
for checking who you are talking to.

## On Android

Android has one-to-one calls with the pre-call check and the quality pill.
Group calls, automatic reconnecting, the small call window, the Diagnose
button and the loudspeaker change arrive on iPhone first and are still being
ported. See [ForkMesh apps](apps.md).
