# Cloud Dragon

Part of [the website docs](README.md). A second mascot for web pages, chosen
instead of the [Genie spider](genie-spider.md). It reuses the spider's behaviour
(walking, throwing, keep-clear, tips, menu, hiding) and replaces its drawing
and its plays. The spider stays the default. iOS, Android and the desktop come
next; they can reuse the same atlas.

## Where the code is

| Part | File |
| --- | --- |
| Reference art | `tools/art/cloud-dragon/` (`master.webp`: the transparent master, cut into the puppet; `poses.webp` and `poses-clean.webp`: the 20 activities, source of the props, the second with real transparency) |
| Art pipeline | `tools/cloud_dragon_art.py`, writes the atlas and stamps `DRAGON_VERSION` |
| Atlas | `www/public/assets/cloud-dragon/atlas.webp`, `atlas.json` |
| Skeleton, springs, the 20 activities, props and particles | `www/public/assets/cloud-dragon/dragon.js` |
| Choosing it, loading it, its plays and naps | `www/public/genie-spider.js` (`mascot`, `setMascot`, `loadDragon`, `dragonAct`, `doze`) |
| Settings | Appearance, under "Show the Genie spider"; `data-genie-mascot` in `dashboard/js/02-helpers.js` |
| Tests | `www/tests/cloud_dragon.test.mjs` |

## One puppet, not a set of pictures

The first version swapped a cut picture for each activity. It read as a slide
show: the size jumped, the feet never moved, and nothing joined one moment to
the next. Now one skeleton performs everything, so the dragon is always the same
size and every change of pose is movement.

- **Layers.** `master.webp` is cut into wings, body, far and near thigh and foot,
  far and near arm and paw, tail strip, ear, head and two eyes, with feathered
  masks. Nothing moving may leave a hole, so whatever the art hid is rebuilt:
  the body is a closed shape filled where limbs and tail lay. Joints live in
  `atlas.json`. No art shows the lower legs; when the body lifts off its feet the
  arm layer draws as a shin, so a foot is never left floating.
- **Springs.** Each activity gives a target `pose` (lean, crouch, head, paw and
  foot positions, wings, tail, mouth, eyelids), and every value chases it on an
  underdamped spring, so moves overshoot slightly and settle. Eyes are quick,
  head and ears are slow: a look starts with the eyes and the head follows.
  Going up or down stretches the body, drags the head behind, and lifts wings and
  ears. Each activity opens with a small crouch as anticipation.
- **Tail.** `straight-tail.webp` shows the tail stretched out with nothing over
  it, so the tail, near thigh and near foot all come from there, aligned to the
  master and graded to its paler colours. The master's tail was curled over the
  thigh, and unrolling it left patches and a gap. The tail is one straight strip
  redrawn each frame on a 28-segment mesh along a chain: `tail` 0 follows the
  sitting curl, 1 trails straight as in the art. Each segment chases the one
  before it, more slowly toward the tip, so it lags behind the body. The first
  42% draws behind the body and the rest in front, so the root stays tucked in
  while the tip wraps past the foot. The strip stretches about 1.3 times to reach
  round the curl. Its ridges stay on the outer edge, so the sitting curl has them
  along the floor, where the master has none.
- **Limbs** are stretchy one-bone IK: a paw or foot goes to a target and the limb
  rotates and stretches to reach it, up to a limit. The far arm is too thin in the
  art to read when raised, so it fades to a mirrored copy of the near arm.
- **Face.** The mouth opens in code, eyes close, squint happily or wink, and gaze
  follows the pointer or the page element.
- **Props** come from the sheets, held at a paw, the chest, the head or the
  floor, and pop in and out. `poses-clean.webp` has real transparency, so white
  props are cut from it: clouds, plane, flame, Z's and hearts. The magnifier, binoculars, globe and shield come from their own 1536 px art; the globe's covered side is filled by turning the sphere's far side round.
  The dragon stood on them in the art, so a cloud keeps its outline, drops tan and
  green pixels and seals the notches, and the window's top band is rebuilt.
  Confetti, sparkles, notes, steam and dust are drawn in code.

**Walking** follows [the spider's](../native/GENIE.md) gait: legs in opposite
phase. Stride phase comes from pixels travelled, not time, and a planted foot
moves back at a constant rate exactly matching the body, so feet do not skate.
A lifted foot swings forward (the spider's swing backward). It starts and stops
with a ramp. It walks upright, like the Travel pose on the sheet; a trot on four
legs pulled the sitting torso out of shape. While it moves, position is
sub-pixel and frames run at the display rate; the spider's 30 fps and whole
pixels read as stutter on a body this detailed.

**Turning round** runs head first, then body, wings and tail, 0.09 to 0.2 s
apart, with a small hop and a blink. A part never narrows past 0.56 of its width,
so the midpoint reads as facing the viewer and not as a card flipping.

**Idle.** Between activities it plays a quirk every 5.5 s or so, picked by seed:
yawn, sneeze with a puff of smoke, look around, head tilt, wing stretch, tail
flick, paw lick, weight shift, little hop. Eyes jump between fixation points
instead of drifting. A near pointer perks the ears and quickens the tail;
resting on it makes the chest purr.

## Behaviour

- **Size.** 96 px (76 px under 520 px wide). The canvas is three times that, so
  flame, rocket and confetti can leave the spider's box.
- **Thrown** it lifts its wings, flaps and trails its tail; it lands in a crouch.
  **Held** it dangles, kicking its feet.
- **Activities.** The sheet has 30; 20 were kept after review. On a play turn it draws from the bag, walks to
  a page element that fits and plays beside it. With no element it plays where it
  stands. Stay put plays in place. With reduced motion it plays nothing.
- **Naps.** After 45 s with no pointer near, no press and no bubble, it sleeps on
  a cloud at its next rest. The pointer coming within 260 px wakes it with a
  stretch.
- **Reactions** are short poses: wave, cheer, nod, eureka, giggle, surprise, dizzy.

## Playing with the page

Thirty interactions (`TRICKS` in `dragon.js`) join the 20 activities in the shuffled
bag, so it fills 50 plays before repeating. Each says where it stands (`at`:
beside the element, on top, on an edge, under it, over it, around it, on its wall
or behind it), and it flies there instead of walking, so any element in view can
be used, not only those near the floor.

- **Fire:** burns it to a crisp and it grows back, warms it until it glows, lights
  a candle, spits fireballs, fireworks, smoke rings.
- **Weather and light:** freezes it and it thaws, rain cloud, static, rainbow, gust
  of leaves.
- **Sitting and hanging:** perches on top, naps on top, chin-up on the top edge with
  feet kicking, hangs upside down like a bat, climbs the side, peeks from behind.
- **Flying:** cloud-trail fly-by, a lap around it, dive-bomb and bounce.
- **Marks:** takes a bite that grows back, sneezes sparks, polishes it, claw marks,
  paw prints, fogs it and draws a heart.
- **Character:** roars, piles treasure on it, lays an egg that hatches, stands guard.

**The page is never touched.** No style, attribute, DOM change, click or focus: the
burn, the frost and the bite are drawn on a canvas over the page with
`pointer-events: none`, so a "burned" button still works and comes back because
nothing happened to it. A test fails if `dragon.js` touches the DOM. The bite paints
the colour of the page behind the element, so it can look off over a picture.

Burning a big element blacks out a lot of the page, so the bag only picks elements
under 20% of the viewport. Headings and links use the text's own width, not their
full row. It faces whichever side has room. The element may scroll, and the dragon
follows it; if it leaves the screen or disappears, the dragon flies home. Any press
ends a play at once, and the dragon falls from wherever it was. Stay put plays only
the solo activities, and with reduced motion it plays nothing.

**Peek** clips the dragon with `clip-path` at the element's top edge, which is how it
appears to be behind something it is really in front of.

**Hang** is a chin-up on the top edge. Hanging below an edge was tried, but cream
paws over the cream head vanish and the arms cannot reach past that head.

These exist only on the web. The native ports do not have them yet.

## Choosing it

`ForkMeshGenie.setMascot("dragon")`, the mascot's menu or Settings. Stored as
`forkmesh.genie.mascot` and a `forkmesh_mascot` cookie, shared across
forkmesh.com hosts like `forkmesh_genie`. The cookie is read first. The choice
survives sign-out: the website header (`site-header.js`) and the dashboard
(`02-helpers.js`) both keep `forkmesh.genie.mascot` when they clear the
browser's ForkMesh keys.

Only visitors who chose it pay for it: `dragon.js` (92 KB) and the 170 KB atlas load on
demand and spider visitors fetch nothing. If they fail, the spider appears.

## Changing the art or painter

```bash
pip install pillow numpy scipy
python3 tools/cloud_dragon_art.py
python3 tools/cloud_dragon_art.py --stamp
```

The first rebuilds the atlas and stamps the version. After editing only
`dragon.js`, run the second. The version joins `?v=` on all three files, and the
test fails if it is stale.
