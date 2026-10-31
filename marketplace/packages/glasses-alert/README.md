# Glasses Alert

Glasses Alert is a free ForkMesh package for Android and iPhone. It tells you
when someone near you wears Meta smart glasses (Ray-Ban Meta, Oakley Meta) or
carries a camera with Bluetooth on (action, 360, mirrorless and compact
cameras, body cameras, dash cams, security cameras and camera drones), which
can film and record without the people around them noticing. The phone
listens to Bluetooth adverts and rings a loud alarm with strong vibration
when a new one comes into range.

It only listens. It never connects to, pairs with or sends anything to the
glasses or cameras, and no scan result leaves the phone. The package has no Worker, no
account data and no web or desktop page.

Open it from **You → Personal → Glasses Alert**, from the Market's **Open**
button, or with the `forkmesh://dashboard/glasses-alert` link.

## How it finds glasses

Every Bluetooth LE advert can carry manufacturer data, which starts with the
maker's company ID assigned by the Bluetooth SIG. Phones and glasses cannot
randomise this ID the way they randomise their addresses. Glasses Alert
matches these IDs:

| Company ID | Maker | Label shown |
|---|---|---|
| `0x01AB` | Meta Platforms, Inc. | Meta glasses or headset |
| `0x058E` | Meta Platforms Technologies, LLC | Meta glasses or headset |
| `0x0D53` | Luxottica Group S.p.A (makes Ray-Ban Meta and Oakley Meta) | Ray-Ban or Oakley Meta glasses |

A device whose advertised name contains `ray-ban`, `rayban`, `ray ban`,
`oakley meta`, `meta ray` or `meta glasses` also matches, and the name is
shown.

The IDs come from two open-source detectors that do the same thing:
[Nearby Glasses](https://github.com/yjeanrenaud/yj_nearbyglasses) (AGPL-3.0)
and [NoPeek](https://github.com/getnopeek/nopeek-android) (MIT). ForkMesh
reuses their published findings (the IDs, the -75 dBm default) but none of
their code.

Meta Quest headsets advertise the same Meta IDs, so a Quest nearby can set
off the alarm too. The label says "glasses or headset" when only a Meta ID
matched.

## How it finds cameras

Cameras with Bluetooth advertise all the time so their phone app can wake
them, and most keep doing it while they record. Glasses Alert matches the
company IDs of makers whose Bluetooth products are cameras, taken from the
Bluetooth SIG's
[assigned numbers](https://bitbucket.org/bluetooth-SIG/public/src/main/assigned_numbers/):

| Company ID | Maker | Label shown |
|---|---|---|
| `0x02F2` | GoPro, Inc. | GoPro camera |
| `0x10D7` | Arashi Vision Inc. (Insta360) | Insta360 camera |
| `0x08AA` | SZ DJI Technology (Osmo, Action, drones) | DJI camera or drone |
| `0x01A9` | Canon Inc. | Canon camera |
| `0x0399` | Nikon Corporation | Nikon camera |
| `0x04D8` | FUJIFILM Corporation | Fujifilm camera |
| `0x04D0` | Olympus Corporation | Olympus camera |
| `0x09F1` | OM Digital Solutions | OM System camera |
| `0x065F` | Ricoh Company (Ricoh GR, Pentax, Theta) | Ricoh, Pentax or Theta camera |
| `0x051A` | Leica Camera AG | Leica camera |
| `0x10D4` | Victor Hasselblad AB | Hasselblad camera |
| `0x02A0` | Impossible Camera GmbH (Polaroid) | Polaroid camera |
| `0x04D7` | Blincam, Inc. (camera clipped to glasses) | Blincam wearable camera |
| `0x0F30` | Opal Camera, Inc. | Opal webcam |
| `0x0909` | 70mai Co., Ltd. | 70mai dash cam |
| `0x0C19` | Arlo Technologies | Arlo security camera |
| `0x0E25` | Hangzhou Hikvision | Hikvision security camera |
| `0x060C` | Vuzix Corporation (camera glasses) | Vuzix camera glasses |

Some cameras advertise a 16-bit service UUID their maker registered instead:

| Service | Maker | Label shown |
|---|---|---|
| `0xFEA5`, `0xFEA6` | GoPro | GoPro camera |
| `0xFC2D`–`0xFC30` | Arashi Vision (Insta360) | Insta360 camera |
| `0xFC81` | Axon Enterprise (police body cameras) | Axon body camera |
| `0xFD4D`, `0xFD4E` | 70mai | 70mai dash cam |
| `0xFE45` | Snap (Spectacles camera glasses) | Snap Spectacles |

Sony, Panasonic, Samsung, Xiaomi, Garmin, Amazon (Ring, Blink) and Wyze also
make headphones, phones, watches and speakers under the same company ID, so
their IDs alone would ring for every pair of earbuds. Their cameras match by
name instead: a device whose name contains a camera word or model, such as
`camera`, `webcam`, `dashcam`, `bodycam`, `gopro`, `insta360`, `osmo action`,
`canon`, `eos r`, `nikon`, `coolpix`, `fujifilm`, `lumix`, `olympus`, `ricoh`,
`pentax`, `theta`, `leica`, `ilce-` and `zv-e` (Sony Alpha and ZV), `fdr-x` and
`hdr-as` (Sony action cams), `kodak`, `polaroid`, `akaso`, `sjcam`,
`nextbase`, `viofo`, `blackvue`, `thinkware`, `vantrue`, `garmin dash`,
`eufycam`, `wyze cam`, `spypoint`, `mavic`, `dji mini`, `avata`, `skydio`,
`spectacles`, `vuzix` or `rokid`, matches and the name is shown. The full list
is `CAMERA_KEYWORDS` in `GlassesWatch.kt` and `cameraKeywords` in
`GlassesWatchModels.swift`.

Canon, Fujifilm and Ricoh also make Bluetooth printers. A match whose name
says `printer`, `pixma`, `maxify`, `selphy`, `imageclass` or `instax ... link`
stays quiet and shows in the device grid instead.

A camera only shows up while its Bluetooth is on. Many switch it off in
airplane mode or when their app connection is turned off, and cheap hidden
cameras often have no Bluetooth at all, so a quiet scan does not prove that
nothing is filming.

## Range

| Setting | Signal | Roughly |
|---|---|---|
| Close by | -65 dBm or stronger | a few metres |
| Same room (default) | -75 dBm or stronger | 3–10 m indoors, 10–15 m outside |
| Anywhere in range | -90 dBm or stronger | as far as Bluetooth reaches |

## The alarm

When glasses or a camera it has not seen in the last two minutes come into
range, the alarm rings for 30 seconds or until you press **Stop alarm**. After an alarm,
new ones stay quiet for 30 seconds so a group does not ring over and over.
The **Nearby now** list shows every match with its distance, signal and maker.
**Test alarm** rings it once so you know what it sounds and feels like.

## Finding the glasses

While glasses or a camera are in range, a card under the status shows:

- **An arrow** pointing where the glasses probably are. Bluetooth alone cannot
  tell direction, so the phone uses its compass: hold it in front of you and
  turn slowly. Your body blocks some of the signal from behind you, so the
  signal is strongest when you face the glasses, and the arrow points to the
  direction where it was strongest over the last 20 seconds. Until you have
  turned through enough of the circle, it shows a pulsing ring and asks you to
  turn. Without a compass reading only closeness is shown.
- **A live closeness bar** from the smoothed signal (-95 dBm empty, -45 dBm
  full), with *Very close*, *Close* or *In the area*, and whether the signal is
  *Getting closer*, *Moving away* or *Holding steady* over the last few
  seconds. *Signal lost* means nothing was heard for 8 seconds.

It follows the strongest match and sticks with it while it is still heard.

## Other Bluetooth around you

While watching, the screen also lists every other Bluetooth device the scan
hears in a compact three-column glass grid. Each tile puts a tiny device-kind
icon beside the kind, then shows the name, address or identifier, highlighted
distance band and colour-coded signal strength. The strongest devices stay at
the top. RSSI
is smoothed so small fluctuations do not constantly reorder the grid, and a
device stays visible until it has gone unheard for 45 seconds. These devices
are not smart glasses or cameras; they show that the scan is working.

## The watching pill

While watching, a small pill floats over every ForkMesh page so you can see
it running elsewhere in the app. It shows a pulsing dot and how many other
devices it hears, or the arrow and closeness bar when glasses are in range,
and turns red while the alarm rings. Drag it anywhere; tap it to open Glasses
Alert. It hides on the Glasses Alert screen itself.

**Android**

- Scans in a foreground service, so it keeps watching with the screen off and
  the app closed. A "Glasses Alert is watching" notification stays up with a
  **Stop watching** button.
- Plays the phone's alarm sound on the alarm stream. Silent and vibrate mode
  do not mute it. While it rings, the alarm volume goes to maximum and then
  back to where it was.
- Vibrates with a strong repeating pattern marked as an alarm, so it also
  vibrates in silent mode.
- Do Not Disturb set to alarms only or priority lets the alarm through. For
  total silence, tap **Ring through Do Not Disturb** and allow access. While
  it rings, Do Not Disturb switches to alarms only and then back.
- Watching resumes when the app starts if it was on before.
- Needs Nearby devices (Android 12+) or Location (Android 11 and older,
  Bluetooth scanning only) and notifications.
- The list of other devices only fills while ForkMesh is open, because
  Android pauses scans for every device when the screen is off. The glasses
  scan itself keeps running.

**iPhone**

- iOS only lets an app scan for unknown Bluetooth devices while it is open,
  so watching runs while ForkMesh is open, on any page. **Keep the screen on
  while watching** (on by default) stops the phone locking itself.
- Plays a loud alarm that rings even with the ring switch on silent, shows a
  full-screen red alert with **Stop alarm**, and plays a strong looping
  Core Haptics pattern (or the system vibration on older phones).
- Also posts a time-sensitive notification, which breaks through Focus when
  you allow time-sensitive notifications for ForkMesh.
- Apple's critical alerts, which ring through everything when the app is
  closed, need an entitlement Apple grants case by case. The app does not
  have it.
