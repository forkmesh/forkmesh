# Stretch Break

Stretch Break is an optional ForkMesh Health package that any account installs
for itself. It runs a guided stretch session: a voice tells you what to do, an
animated example person does each move with you, and a countdown shows how long
is left of each step. It is for your back and posture after a long day at the
computer, and it includes a lymph tapping routine. Finished sessions are logged
on your account, and on iPhone each one is saved to Apple Health as a workout.

The same package runs on the web dashboard (`/dashboard/stretch`), the desktop
rail page (**Stretch Break**) and the iPhone app (**Stretch Break**).

## Routines

| Id | Title | Steps | Apple Health workout |
| --- | --- | --- | --- |
| `desk` | Desk stretch | Stand tall, hip circles, arm swings that rotate the torso, side stretch left and right, twist left and right, neck rolls, neck stretch left and right, look left and right, shoulder rolls, chest opener, forward fold, reach up (about 5 minutes) | Flexibility |
| `lymph` | Lymph tapping | Belly breaths, collarbones, down and up the left arm, the right arm, under the arms, down the sides, top of the head, around the eyes, face and neck, chest, shake out (about 4.5 minutes) | Mind and Body |
| `full` | Both routines | `desk` then `lymph` | Flexibility |

The routines, their spoken cues and the example person's moves are data in the
Worker (`content/worker/stretch.py`, `library()`). Every client reads them from
the API, so a new step or a better cue ships to web, desktop and iPhone at once.
`GET /api/stretch/library` answers without a session; `GET /api/stretch` carries
the same object under `library`.

## Guided session

Every client runs the same session:

1. Pick a routine (the profile's `routine` is preselected) and press **Start**.
2. Each step starts by speaking `"<title>. <say>"`. When a step is at least 20
   seconds long and has a `half` cue, the cue is spoken at the halfway mark.
   When a step is at least 15 seconds long and another step follows, `"Next:
   <next title>"` is spoken 3 seconds before the end.
3. The screen shows the example person, the step title and `say` text, a
   countdown for the step, the overall progress, and the next step.
4. Controls: pause and resume (pausing also pauses speech), back, skip, stop,
   and a voice on/off toggle saved as the profile's `voice`.
5. At the end, `"Well done. That is your <routine title> finished."` is spoken
   and the session is logged with `POST /api/stretch/sessions`. Stopping early
   logs the session as not completed when at least 30 seconds were done.
   `seconds` counts time actually spent (not paused).

The voice is the platform's own speech: `speechSynthesis` on the web, the
system voice on desktop, `AVSpeechSynthesizer` on iPhone. Nothing is sent to a
speech service.

## The example person

The person is a front-facing figure that mirrors you, like an instructor facing
a class: "lean to your left" makes it lean toward the viewer's left. A motion is
`{"period": seconds, "tap": "" | "left" | "right" | "both", "frames": [...]}`;
frames have `t` from 0 to 1 (the first is 0, the last is 1 and repeats the
first) and every field in `poseFields`, in degrees or fractions. Arm angles are
0 hanging down, 90 straight out to that side, 180 straight up, negative across
the body; the elbow angle is added to the arm angle to get the forearm. Tapping
arm targets are solved with two-bone inverse kinematics in the Worker, so the
clients only interpolate.

Every renderer does the same arithmetic in the 200 by 264 `figure` box
(`F` below), scaled to fit:

```text
phase = (seconds into the step mod period) / period
pick frames a, b with a.t <= phase <= b.t; u = (phase - a.t) / (b.t - a.t);
u = u*u*(3 - 2u); p = a + (b - a) * u for every pose field
R(deg, x, y) = (x cos - y sin, x sin + y cos)
hip = (F.hip.x + p.hipX * F.hipShift, F.hip.y + p.knees * F.kneeDip)
torso length L = F.torso * (1 - 0.5 p.fold); C = hip + R(p.lean, 0, -1) * L
half width W = F.shoulderHalf * (1 - 0.4 |p.twist|); side = R(p.lean, 1, 0)
shoulders = C -/+ side * W, both moved up by p.shrug * F.shrug
head angle h = p.lean + p.headTilt
head centre = C + R(h, 0, -1) * F.neck + (p.twist * 3, p.headNod * 6), radius F.head
eyes = head centre + R(h, +-5 + p.headTurn * 5, -2 + p.headNod * 3), radius 2
arm (sign -1 left, +1 right): elbow = shoulder + R(p.lean, sign sin a, cos a) * F.upperArm
  hand = elbow + R(p.lean, sign sin(a + e), cos(a + e)) * F.forearm
tapping hand(s): hand.y -= F.tapLift * |sin(2 pi F.tapHz * seconds)|
legs: hip -/+ (F.hipHalf, 0) to F.feet; knee at the midpoint moved out by p.knees * 10
draw: torso hip to C (14 wide), shoulder line (10), upper arms (9), forearms (8),
  legs (10), all with round caps; filled head; eyes in the background colour
```

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/stretch.py`, `/api/stretch` | Account-scoped, sealed per account; also serves the routine library. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/stretch`; SVG figure, `speechSynthesis`. |
| Desktop | `content/desktop/StretchFeature.{h,cpp}` | Rail page **Stretch Break**, signed with `forkmesh-stretch-request-v1`; `QPainter` figure, system voice. |
| iOS | `native/ios/ForkMesh/Features/Tools/StretchScreen.swift`, `ForkMeshCore/Sources/FMTools/StretchModels.swift` | SwiftUI `Canvas` figure, `AVSpeechSynthesizer`; writes each session to Apple Health through `ForkMesh/Platform/HealthKitBridge.swift`. |

## API

Every request may pass `?day=YYYY-MM-DD`, the client's local today; without it
the Worker uses the profile's `tzOffsetMinutes`.

| Method | Path | Body | Answer |
| --- | --- | --- | --- |
| GET | `/api/stretch` | | `installed`, `library`, and when installed `profile`, `today` (`sessions`, `seconds`), `summary` (`streak`, `bestStreak`, `sessions`, `weekSeconds`, `weekSessions`), `days` (the last 14), `recent` (the last 20 sessions) |
| GET | `/api/stretch/library` | | the routine library, no session needed |
| POST | `/api/stretch/install` | `voice`, `routine`, `tzOffsetMinutes` | |
| POST | `/api/stretch/uninstall` | | the export, then everything is deleted |
| GET, PUT | `/api/stretch/profile` | `voice` (true), `routine` (`desk`, `lymph`, `full`), `tzOffsetMinutes` | |
| POST | `/api/stretch/sessions` | `routine`, `seconds` (1 to 14400), `steps`, `stepsTotal`, `completed`, `at`, `day`, `source` (`web`, `desktop`, `ios`, `android`), `health` | 201 with the session |
| DELETE | `/api/stretch/sessions/<id>` | | |
| GET | `/api/stretch/export` | | `forkmesh-stretch-export-v1` |

A day holds at most 50 sessions. A day with at least one session counts toward
the streak; today with nothing yet does not break it.

## Apple Health

On iPhone, with **Save to Apple Health** turned on, every logged session is
written to Health as an `HKWorkout` (Flexibility for `desk` and `full`, Mind and
Body for `lymph`) from the session's start to its end, and the session is sent
with `health: true` so other devices can show that it is in Health. The screen
also reads this week's Flexibility and Mind and Body workout minutes from Health
(including ones other apps saved) and shows them beside the ForkMesh total.

## Data and privacy

Each account gets its own random key at install (`stretch_keys`). A session row
keeps the account's blind index, the day and the logging time in the clear; the
routine, length, steps, source and the Health flag are sealed. Uninstalling
returns the export and deletes the rows and the key; account deletion does the
same through `purge_account_data`.

This is general wellness guidance, not medical advice: every cue says to move
gently, and the routines avoid rolling the head backwards.
