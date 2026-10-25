# The Genie spider in the World

Part of [the World docs](README.md). The same Genie spider that guides web
pages ([../www/genie-spider.md](../www/genie-spider.md)), the desktop and the
phone lives in the 3D World too. It follows you around town and, every 4.5 to
11 seconds, plays one of 100 acts with whatever is near: you, other people,
Forkbot, the fountain, the campfire, bikes, cars, drones, the swings,
repository portals, landmarks, boards, or anything else you could click.
Added 2026-10-25.

## Where the code is

| Piece | Code |
| --- | --- |
| Model, leg poses, the 100 acts, motions, particle paths, speech bubble painter, on/off rule | `world/public/world/scene/genie-spider.js` |
| Runtime: following, finding things nearby, approaching, playing an act, effects, scene API | `world/public/world/scene/runtime/genie-spider.js`, registered in `scene/core.js`, stepped by `animate.js` (`updateGenieSpider`) |
| Settings switch, reactions | `world.js` (`data-world-genie-spider`, `popWorldReaction` calls `reactGenieSpider`) |
| Tests | `world/tests/test_world_genie_spider.py` |

## The model

Dark shell, purple turban with a gold-ringed teal gem and six glowing teal
eyes, the colours of the web painter. It is three draw calls: one instanced
mesh of lit spheres (abdomen, head, turban, pupils), one of unlit glowing
spheres (eyes, gem) and one of 16 leg segments. Each leg has a hip on the
body, a knee lifted above the line to its foot, and a foot on the ground.
Walking moves the legs in two alternating groups of four; other leg modes
are dance, tuck, spread, wave (front leg up), drum (front pair tapping) and
punch. It blinks every 2.5 to 6 seconds. It is drawn 1.6 times its model
size so it reads at the town's camera distance, and is lifted out of the
dark with a faint violet glow.

## Behaviour

- **Following.** More than 8 units from you it walks over to stand beside
  you (twice as fast beyond 20); more than 70 away (a teleport, a ride) it
  pops in beside you with a puff. It turns to face you when idle.
- **Finding things.** Every 1.5 s it lists what is within 34 units of you:
  you, other avatars, Forkbot, the World landmarks (fountain and campfire
  have their own acts), bikes, cars, drones, the swing set, repository
  portals, the Start here, bulletin, QA and Mastodon boards, and 30 random
  clickable objects. Sizes come from presets or, for small things, from a
  bounding box measured once per object.
- **Choosing.** Acts are drawn from a shuffled bag of all 100, skipping ones
  that need something not nearby, so every act comes up before any repeats.
- **Playing.** It walks up to the thing (giving up after 9 s), then plays the
  act: a motion (38 different ones), a speech bubble, an emoji particle effect (burst,
  rise, rain, orbit, fountain, spiral, twinkle or trail; 18 particles, one
  draw call) and, for some, a web strand. Motions are relative to where it
  started, to the thing's centre, to its top (climbing onto benches, cars,
  bikes, drones, Forkbot, landmarks or your head), or to the spot behind you
  (the conga line), so it follows moving people.
- **Reactions.** When a reaction pops on screen, an idle spider answers it:
  🔥 starts a fire dance, 👏 a confetti cannon, ❤️ a heart dance, 👋 a high
  five, and so on (`GENIE_REACTION_ACTS`).
- **Where.** Only in the town: it hides inside the Office, on the beach and
  while you are not in the world, and starts 4 s after the scene is up.
- **Reduced motion.** It stays beside you without walking (it moves there
  directly) and plays nothing on its own; asked acts and reactions still play.
- **Local.** Every visitor has their own spider, near themselves. Nothing
  about it goes over the network.

## Turning it off

"Show the Genie spider" in World settings (`forkmesh.world.genie` = `on` or
`off` in `localStorage`). Without that choice the site-wide "Hide for good"
(the `forkmesh_genie` cookie or `forkmesh.genie.off`) also keeps it away;
an explicit World "on" wins over it.

## Scene API

`playGenieSpiderAct(id?)` (no id: the next one from the bag),
`reactGenieSpider(emoji)`, `setGenieSpiderEnabled(on)`, `genieSpiderState()`
(mode, act, target kind, what is nearby, position) and `genieSpiderActs()`.

## The 100 acts

**On its own** (30): ✨ hops for joy (`happy-hops`), 🌪️ spins like a tornado (`tornado-spin`), ⭐ does a backflip (`backflip`), 🎵 moonwalks (`moonwalk`), 🕸️ yo-yos on a web (`web-yo-yo`), 💤 takes a cat nap (`cat-nap`), 🙆 has a big stretch (`big-stretch`), 🌈 turns cartwheels (`cartwheels`), ✨ floats on genie magic (`genie-float`), 🌙 juggles three moons (`moon-juggling`), 💨 gets the zoomies (`zoomies`), 🪩 throws a disco (`disco`), 🌧️ does a rain dance (`rain-dance`), ❄️ makes a snow angel (`snow-angel`), 🥊 shadowboxes (`shadowbox`), 🥁 plays a drum solo (`drum-solo`), 🎉 shrinks away and pops back (`now-you-see-me`), 💫 spirals into the sky (`sky-spiral`), ⚡ dashes in zigzags (`zigzag-dash`), 🫧 blows bubbles (`bubble-blowing`), ⭐ gazes at the stars (`stargazing`), 🌠 wishes on a shooting star (`shooting-star`), 🎊 fires a confetti cannon (`confetti-cannon`), 💜 dances with hearts (`heart-dance`), 🎺 leads a marching band (`marching-band`), 🫢 gets the hiccups (`hiccups`), ❄️ shivers in the cold (`brrr`), 😎 sunbathes (`sunbathing`), 🩰 does a pirouette (`pirouette`), 🪙 makes it rain coins (`make-it-rain`).

**With you** (22): 🙌 high-fives you (`high-five`), 🤗 gives you a hug (`group-hug`), ✨ orbits you like a moon (`orbit-you`), 👑 sits on your head (`head-throne`), 🕺 challenges you to a dance-off (`dance-off`), 🎶 starts a conga line behind you (`conga-line`), 🤭 tickles you (`tickle-attack`), 🙇 bows to you (`royal-bow`), 💐 brings you flowers (`flowers-for-you`), 📣 cheers you on (`cheerleader`), 📸 takes your photo (`photo-op`), 👀 plays peekaboo around you (`peekaboo`), 🎩 does a magic trick for you (`magic-trick`), 💖 showers you with hearts (`heart-shower`), 🎁 gives you a present (`surprise-gift`), 🎵 serenades you (`serenade`), 🐾 pounces on your feet (`pounce-play`), 🦜 rides on your shoulder (`shoulder-ride`), 🕶️ guards you (`bodyguard`), 🕸️ hangs a hammock next to you (`web-hammock`), 🔮 tells your fortune (`fortune-teller`), 🍪 shares cookies (`cookie-break`).

**With other people** (10): 👋 waves hello to someone (`wave-hello`), 💫 circles a new friend (`orbit-friend`), 🎩 sits on someone's head (`hat-of-honour`), 🤝 introduces itself (`introductions`), 💃 dances with someone (`friend-dance`), 👃 sniffs hello (`sniff-hello`), 🙈 says boo to someone (`boo`), 💕 melts at someone (`aww`), 🏃 plays tag (`tag`), 🙌 high-fives someone (`up-top`).

**Forkbot** (4): 🤖 does the robot with forkbot (`robot-dance`), 💬 gossips with forkbot (`bot-gossip`), ⚙️ climbs on top of forkbot (`king-of-the-bots`), 🏁 races around forkbot (`bot-race`).

**The fountain** (5): 💦 splashes in the fountain (`splash`), 🪙 throws a coin in the fountain (`fountain-wish`), 🏊 swims laps round the fountain (`fountain-laps`), 🌊 balances on the fountain rim (`rim-balance`), 🦆 floats a rubber duck (`rubber-duck`).

**The campfire** (5): 🍡 toasts marshmallows (`marshmallows`), 🔥 tells a campfire story (`campfire-story`), 🔥 dances by the fire (`fire-dance`), 👻 tells a ghost story (`ghost-story`), 🎸 leads a sing-along (`sing-along`).

**Bikes** (3): 🚲 rides a bike (`bike-ride`), 💨 pumps up the tyres (`pump-tyres`), 🤸 pulls a wheelie (`wheelie`).

**Cars** (3): 🚗 parties on a car roof (`roof-party`), 📯 honks the horn (`honk`), 🧽 washes the car (`car-wash`).

**Drones** (2): 🛸 chases a drone (`drone-chase`), 🚁 rides a drone (`drone-ride`).

**The swings** (2): 🎠 swings on the swings (`swing-high`), 🌀 gives the swing a push (`swing-push`).

**Repository portals** (4): 🌀 peeks into a portal (`portal-peek`), ✨ hops through a portal (`portal-hop`), ⭐ stars a repository (`star-the-repo`), 📜 reads the commits (`commit-reader`).

**Landmarks** (4): 🏔️ climbs a landmark (`summit`), 🤩 admires a landmark (`admire`), 🗺️ gives a guided tour (`tour-guide`), 🕸️ signs a landmark with a web (`web-signature`).

**Boards** (3): 📰 reads a board (`read-the-board`), 🗒️ sticks a note on a board (`sticky-note`), 👉 points at a board (`look-at-this`).

**Anything else nearby** (3): 🔍 investigates something (`investigate`), 🕸️ wraps something in web (`web-wrap`), 🏆 claims something as its own (`king-of-the-hill`).

## Verified

- `world/tests/test_world_genie_spider.py`: exactly 100 acts with distinct ids
  and titles, every motion, target kind, pose value and particle finite
  across each act, at least two acts for every kind of thing, climbing to the
  top, dangling, all eight legs in every mode, the bag drawing all 100, the
  on/off rule, scene registration and the three draw calls.
- 2026-10-25, headless Chromium (SwiftShader) against the production backend:
  the spider stood by the campfire with its legs, eyes and turban drawn and
  its speech bubbles over it while acts played.
