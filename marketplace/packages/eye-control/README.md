# Eye Control

An experimental **Eye Control** section in the iPhone app: a cursor follows
where you look, and a firm blink taps whatever is under it. Nothing is sent
anywhere; camera frames never leave the phone.

## How it works

- **Tracking.** ARKit face tracking runs on the front TrueDepth camera (the
  colour camera, the infrared camera and the dot projector Face ID uses). Each
  frame gives the position and direction of both eyes; the cursor sits where
  the two gaze rays meet the screen, smoothed so it does not shake.
- **Clicking.** Closing both eyes holds the cursor still. Opening them again
  taps the spot you were looking at just before the blink. **Firm blink**
  (the default) needs the eyes shut for about a quarter of a second, so
  ordinary blinks do nothing; **Any blink** and **Long blink** change that.
- **Snapping.** The cursor locks onto the button you are looking at and
  outlines it, so a blink presses that button even when your gaze is a
  little off. Turn **Snap to buttons** off to click exactly under the cursor.
- **Calibration.** Look at nine dots in turn (about 20 seconds). The phone
  fits your eyes' readings to the dots and keeps the result on this device.
  Without it the cursor uses a rough estimate from the screen's size. With
  **Keep learning** on, every blink-tap on a button and every steady look at
  one fine-tunes the calibration as you go.
- **Stopping.** The **Stop eye control** button floats above every screen;
  tap it or blink on it. Leaving the app pauses the camera.

## Limits

- Needs an iPhone or iPad that supports ARKit face tracking (Face ID, or an
  A12 chip or later).
- It works inside ForkMesh only: iOS does not let one app tap another.
- Taps reach buttons, switches, segmented controls, sliders, text fields,
  list rows and anything VoiceOver can activate. Swipes, scrolling and long
  presses are not supported yet.

Design notes: [`docs/marketplace/eye-control.md`](../../../docs/marketplace/eye-control.md).
