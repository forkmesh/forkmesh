import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieLegMove: String, Sendable, CaseIterable {
    case rest, wave, cheer, dance, drum, swim, splay, tuck, coverEyes, clap, flex, march, reach, shrug, typing, guitar, conduct
    case salute, point, wiggle, hug, flap
}

public enum GenieEyeMove: String, Sendable, CaseIterable {
    case normal, wide, closed, wink, hearts, stars, spiral, squint, roll, cross, sleepy, up, down
}

public enum GenieMouthMove: String, Sendable, CaseIterable {
    case normal, grin, o, frown, tongue, whistle, yawn, chatter, open, flat
}

public enum GenieEffectMotion: String, Sendable {
    case rise, fall, burst, orbit, pop, drift, thread
}

public enum GenieEffect: String, Sendable, CaseIterable {
    case none, hearts, stars, notes, zzz, sparkles, confetti, bubbles, question, exclaim, sweat, tears, flames, snow, lightning
    case petals, rainbow, smoke, coins, ghosts, balloons, idea, muscles, feathers, bugs, food, laptop, peace, phone, web, juggling
    case rocket, camera, crown, clouds, carousel, tornado, hero, coaster, wind, telescope, moon, skateboard, waves, parachute, umbrella
    case queasy

    public var glyphs: [String] {
        switch self {
        case .none, .web: return []
        case .hearts: return ["💖", "💕", "💗"]
        case .stars: return ["⭐️", "🌟"]
        case .notes: return ["🎵", "🎶"]
        case .zzz: return ["💤"]
        case .sparkles: return ["✨"]
        case .confetti: return ["🎉", "🎊"]
        case .bubbles: return ["🫧"]
        case .question: return ["❓"]
        case .exclaim: return ["❗️"]
        case .sweat: return ["💦"]
        case .tears: return ["💧"]
        case .flames: return ["🔥"]
        case .snow: return ["❄️"]
        case .lightning: return ["⚡️"]
        case .petals: return ["🌸", "🌺"]
        case .rainbow: return ["🌈"]
        case .smoke: return ["💨"]
        case .coins: return ["🪙", "💰"]
        case .ghosts: return ["👻"]
        case .balloons: return ["🎈"]
        case .idea: return ["💡"]
        case .muscles: return ["💪"]
        case .feathers: return ["🪶"]
        case .bugs: return ["🪰"]
        case .food: return ["🍕", "🍩", "🍪"]
        case .laptop: return ["💻", "⌨️"]
        case .peace: return ["✌️", "☮️"]
        case .phone: return ["📞"]
        case .juggling: return ["🔴", "🟡", "🔵"]
        case .rocket: return ["🚀"]
        case .camera: return ["📸"]
        case .crown: return ["👑"]
        case .clouds: return ["☁️"]
        case .carousel: return ["🎠"]
        case .tornado: return ["🌪️"]
        case .hero: return ["🦸"]
        case .coaster: return ["🎢"]
        case .wind: return ["🌬️", "🍃"]
        case .telescope: return ["🔭"]
        case .moon: return ["🌙", "🌠"]
        case .skateboard: return ["🛹"]
        case .waves: return ["🌊"]
        case .parachute: return ["🪂"]
        case .umbrella: return ["☂️"]
        case .queasy: return ["🤢"]
        }
    }

    public var motion: GenieEffectMotion {
        switch self {
        case .hearts, .notes, .zzz, .bubbles, .ghosts, .balloons, .muscles, .rocket, .moon: return .rise
        case .confetti, .snow, .petals, .coins, .feathers, .tears, .food: return .fall
        case .stars, .sparkles, .exclaim, .lightning, .flames, .camera: return .burst
        case .question, .bugs, .juggling, .laptop, .tornado: return .orbit
        case .rainbow, .idea, .peace, .phone, .crown, .none, .carousel, .hero, .coaster, .telescope, .skateboard, .parachute, .umbrella, .queasy:
            return .pop
        case .smoke, .sweat, .clouds, .wind, .waves: return .drift
        case .web: return .thread
        }
    }
}

public enum GenieTravel: String, Sendable, CaseIterable {
    case none, toHand, circle, zigzag, hopAround, flee, hide, rope, zoom, figureEight, backAndForth
}

public enum GenieTrickSound: String, Sendable {
    case none, giggle, boing, whoosh, dizzy, spotted, appear, vanish, reply, wakeOn, listen
}

public enum GenieTrickMood: String, Sendable, CaseIterable {
    case happy, silly, calm, scared, sad, proud, sleepy, cool, loving
}

public struct GenieBodyMove: Equatable, Sendable {
    public var spin: Double = 0
    public var hops: Int = 0
    public var height: Double = 0
    public var squash: Double = 0
    public var shake: Double = 0
    public var sway: Double = 0
    public var float: Double = 0
    public var grow: Double = 0
    public var wobble: Double = 0
    public var tilt: Double = 0
    public var fade: Double = 0
    public var colors: Double = 0
    public var nod: Double = 0

    public init(
        spin: Double = 0, hops: Int = 0, height: Double = 0, squash: Double = 0, shake: Double = 0, sway: Double = 0, float: Double = 0,
        grow: Double = 0, wobble: Double = 0, tilt: Double = 0, fade: Double = 0, colors: Double = 0, nod: Double = 0
    ) {
        self.spin = spin
        self.hops = hops
        self.height = height
        self.squash = squash
        self.shake = shake
        self.sway = sway
        self.float = float
        self.grow = grow
        self.wobble = wobble
        self.tilt = tilt
        self.fade = fade
        self.colors = colors
        self.nod = nod
    }

    public static let still = GenieBodyMove()
}

public struct GenieParticle: Equatable, Sendable {
    public var glyph: String
    public var x: Double
    public var y: Double
    public var size: Double
    public var opacity: Double
}

public struct GenieTrickFrame: Equatable, Sendable {
    public var age: Double
    public var progress: Double
    public var envelope: Double
    public var rise: Double
    public var rotation: Double
    public var dx: Double
    public var dy: Double
    public var scaleX: Double
    public var scaleY: Double
    public var opacity: Double
    public var hue: Double
    public var legs: GenieLegMove
    public var eyes: GenieEyeMove
    public var mouth: GenieMouthMove
    public var effect: GenieEffect
    public var magic: GenieMagic?
    public var particles: [GenieParticle]
}

public struct GenieTrick: Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let line: String
    public let words: [String]
    public let legs: GenieLegMove
    public let eyes: GenieEyeMove
    public let mouth: GenieMouthMove
    public let body: GenieBodyMove
    public let effect: GenieEffect
    public let magic: GenieMagic?
    public let travel: GenieTravel
    public let sound: GenieTrickSound
    public let mood: GenieTrickMood
    public let duration: Double
    public let symbol: String

    public init(
        _ id: String, _ title: String, _ line: String, words: [String] = [], legs: GenieLegMove = .rest, eyes: GenieEyeMove = .normal,
        mouth: GenieMouthMove = .grin, body: GenieBodyMove = .still, effect: GenieEffect = .none, magic: GenieMagic? = nil, travel: GenieTravel = .none,
        sound: GenieTrickSound = .none, mood: GenieTrickMood = .happy, duration: Double = 2, symbol: String = "sparkles"
    ) {
        self.id = id
        self.title = title
        self.line = line
        self.words = words
        self.legs = legs
        self.eyes = eyes
        self.mouth = mouth
        self.body = body
        self.effect = effect
        self.magic = magic
        self.travel = travel
        self.sound = sound
        self.mood = mood
        self.duration = max(duration, 0.4)
        self.symbol = symbol
    }

    public func frame(age: Double) -> GenieTrickFrame? {
        guard age >= 0, age < duration else { return nil }
        let u = min(max(age / duration, 0), 1)
        let envelope = sin(Double.pi * u)
        let rise = min(max(age / 0.22, 0), 1) * min(max((duration - age) / 0.3, 0), 1)
        let eased = u * u * (3 - 2 * u)
        let hopWave = body.hops > 0 ? abs(sin(Double.pi * Double(body.hops) * u)) : 0
        let landing = body.hops > 0 ? (1 - hopWave) * rise : envelope
        var rotation = body.spin * 360 * eased + body.tilt * rise
        rotation += body.wobble * sin(age * 9) * 14 * envelope
        rotation += body.sway * sin(age * Double.pi * 2.6) * 10 * envelope
        let dx = body.shake * sin(age * 48) * 2.4 * envelope + body.sway * sin(age * Double.pi * 1.3) * 6 * envelope
        let dy = -body.height * hopWave - body.float * 30 * envelope + body.nod * sin(age * Double.pi * 4.8) * 3 * envelope
        let grow = 1 + body.grow * rise
        let squash = body.squash * 0.24 * landing
        return GenieTrickFrame(
            age: age, progress: u, envelope: envelope, rise: rise, rotation: rotation, dx: dx, dy: dy,
            scaleX: max(grow * (1 + squash), 0.05), scaleY: max(grow * (1 - squash), 0.05), opacity: max(1 - body.fade * envelope, 0.08),
            hue: body.colors * 360 * u, legs: legs, eyes: eyes, mouth: mouth, effect: effect, magic: magic,
            particles: GenieTrick.particles(effect, progress: u, age: age, strength: rise))
    }

    public static func particles(_ effect: GenieEffect, progress u: Double, age: Double, strength: Double) -> [GenieParticle] {
        let glyphs = effect.glyphs
        guard !glyphs.isEmpty, strength > 0.01 else { return [] }
        func seed(_ k: Int) -> Double {
            let s = sin(Double(k) * 12.9898 + 78.233) * 43_758.5453
            return s - s.rounded(.down)
        }
        func glyph(_ k: Int) -> String { glyphs[k % glyphs.count] }
        var out: [GenieParticle] = []
        switch effect.motion {
        case .rise:
            for k in 0..<5 {
                let phase = (u * 1.6 + Double(k) / 5).truncatingRemainder(dividingBy: 1)
                let x = 50 + (seed(k) - 0.5) * 64 + sin(age * 2.2 + Double(k)) * 4
                out.append(GenieParticle(glyph: glyph(k), x: x, y: 62 - phase * 62, size: 8 + seed(k + 9) * 4, opacity: sin(Double.pi * phase) * strength))
            }
        case .fall:
            for k in 0..<7 {
                let phase = (u * 1.4 + seed(k)).truncatingRemainder(dividingBy: 1)
                let x = 50 + (seed(k + 3) - 0.5) * 90 + sin(age * 3 + Double(k)) * 3
                out.append(GenieParticle(glyph: glyph(k), x: x, y: -4 + phase * 100, size: 7 + seed(k + 5) * 3, opacity: min(1, (1 - phase) * 1.6) * strength))
            }
        case .burst:
            for k in 0..<8 {
                let angle = Double(k) / 8 * Double.pi * 2 + seed(k) * 0.3
                let r = 12 + u * 40
                out.append(GenieParticle(
                    glyph: glyph(k), x: 50 + cos(angle) * r, y: 50 + sin(angle) * r, size: 6 + (1 - u) * 5, opacity: (1 - u * 0.8) * strength))
            }
        case .orbit:
            let count = max(glyphs.count, 3)
            for k in 0..<count {
                let angle = age * 3 + Double(k) / Double(count) * Double.pi * 2
                out.append(GenieParticle(
                    glyph: glyph(k), x: 50 + cos(angle) * 36, y: 30 + sin(angle) * 12, size: 8, opacity: strength))
            }
        case .pop:
            out.append(GenieParticle(glyph: glyph(0), x: 50, y: 8 - 6 * sin(Double.pi * u), size: 6 + 10 * strength, opacity: strength))
        case .drift:
            for k in 0..<4 {
                let phase = (u * 1.2 + Double(k) / 4).truncatingRemainder(dividingBy: 1)
                out.append(GenieParticle(
                    glyph: glyph(k), x: 50 + (Double(k) - 1.5) * 16 + phase * 18, y: 84 - phase * 26, size: 7 + phase * 6,
                    opacity: (1 - phase) * strength))
            }
        case .thread:
            break
        }
        return out
    }
}

public enum GenieTricks {
    public nonisolated static let tool = "perform_trick"
    public nonisolated static let page = "Tricks"
    public nonisolated static let surpriseWords: Set<String> = [
        "surprise", "surprise_me", "random", "any", "anything", "something", "trick", "a_trick", "tricks", "whatever", "you_pick",
        "your_choice", "",
    ]

    public nonisolated static let all: [GenieTrick] = [
        GenieTrick("wave", "Wave", "Hi there!", words: ["wave", "say hi", "say hello", "wave hello"], legs: .wave, sound: .appear, symbol: "hand.wave.fill"),
        GenieTrick("high_five", "High five", "Up top!", words: ["high five", "gimme five", "give me five"], legs: .reach, eyes: .wide,
                   body: GenieBodyMove(hops: 1, height: 10), effect: .stars, sound: .boing, symbol: "hand.raised.fill"),
        GenieTrick("fist_bump", "Fist bump", "Boom!", words: ["fist bump", "bump it"], legs: .point, eyes: .squint,
                   body: GenieBodyMove(shake: 0.6), effect: .sparkles, sound: .boing, mood: .cool),
        GenieTrick("backflip", "Backflip", "Wheee!", words: ["backflip", "back flip", "flip", "do a flip"], legs: .tuck, eyes: .closed,
                   body: GenieBodyMove(spin: -1, hops: 1, height: 28), effect: .sparkles, sound: .boing, mood: .silly, duration: 1.4, symbol: "arrow.counterclockwise"),
        GenieTrick("frontflip", "Front flip", "Hup!", words: ["front flip", "frontflip", "forward flip"], legs: .tuck, eyes: .closed,
                   body: GenieBodyMove(spin: 1, hops: 1, height: 28), effect: .stars, sound: .boing, duration: 1.4, symbol: "arrow.clockwise"),
        GenieTrick("cartwheel", "Cartwheel", "Look at me go!", words: ["cartwheel", "cartwheels"], legs: .splay,
                   body: GenieBodyMove(spin: 2, hops: 2, height: 8), travel: .backAndForth, sound: .whoosh, mood: .silly, duration: 2.2),
        GenieTrick("spin", "Spin", "Round and round!", words: ["spin", "twirl", "spin around"], legs: .splay, eyes: .closed,
                   body: GenieBodyMove(spin: 2), sound: .whoosh, duration: 1.4, symbol: "arrow.triangle.2.circlepath"),
        GenieTrick("pirouette", "Pirouette", "Très élégant.", words: ["pirouette", "ballet"], legs: .reach, eyes: .closed,
                   body: GenieBodyMove(spin: 3, float: 0.4), effect: .sparkles, mood: .proud, duration: 2),
        GenieTrick("moonwalk", "Moonwalk", "Hee-hee!", words: ["moonwalk", "moon walk"], legs: .march, eyes: .squint,
                   body: GenieBodyMove(sway: 0.4), effect: .notes, travel: .backAndForth, mood: .cool, duration: 2.6, symbol: "moon.stars.fill"),
        GenieTrick("dance", "Dance", "Let's boogie!", words: ["dance", "boogie", "moves", "dance moves", "move", "groove"],
                   legs: .dance, body: GenieBodyMove(hops: 4, height: 4, sway: 1), effect: .notes, sound: .giggle, duration: 2.8, symbol: "music.note"),
        GenieTrick("disco", "Disco", "Saturday night fever!", words: ["disco", "disco dance"], legs: .point,
                   body: GenieBodyMove(sway: 1, colors: 1.5), effect: .sparkles, mood: .cool, duration: 2.8, symbol: "light.beacon.max.fill"),
        GenieTrick("breakdance", "Breakdance", "Windmill!", words: ["breakdance", "break dance", "windmill"], legs: .splay,
                   body: GenieBodyMove(spin: 3, squash: 0.6), effect: .stars, sound: .whoosh, mood: .cool, duration: 2),
        GenieTrick("headbang", "Headbang", "Rock on!", words: ["headbang", "head bang", "rock on", "rock out"], legs: .cheer, eyes: .closed,
                   mouth: .tongue, body: GenieBodyMove(nod: 2.4), effect: .lightning, mood: .cool, duration: 2.2, symbol: "bolt.fill"),
        GenieTrick("air_guitar", "Air guitar", "Wah-wah-weeee!", words: ["air guitar", "guitar", "play guitar"], legs: .guitar, eyes: .closed,
                   mouth: .open, body: GenieBodyMove(sway: 0.6, nod: 1), effect: .notes, mood: .cool, duration: 2.6, symbol: "guitars.fill"),
        GenieTrick("drum_solo", "Drum solo", "Ba-dum-tss!", words: ["drum", "drums", "drum solo", "drum roll", "drumroll"], legs: .drum,
                   body: GenieBodyMove(nod: 1.4), effect: .notes, sound: .boing, mood: .cool, duration: 2.2, symbol: "music.quarternote.3"),
        GenieTrick("conduct", "Conduct", "And… crescendo!", words: ["conduct", "conduct an orchestra", "orchestra"], legs: .conduct,
                   eyes: .closed, body: GenieBodyMove(sway: 0.4), effect: .notes, mood: .proud, duration: 2.6, symbol: "music.note.list"),
        GenieTrick("sing", "Sing", "La la laaa!", words: ["sing", "sing a song", "sing me a song"], eyes: .closed, mouth: .open,
                   body: GenieBodyMove(sway: 0.5), effect: .notes, sound: .reply, duration: 2.6, symbol: "music.mic"),
        GenieTrick("whistle", "Whistle", "Fweet fweet!", words: ["whistle"], eyes: .up, mouth: .whistle, effect: .notes, mood: .calm, duration: 2),
        GenieTrick("beatbox", "Beatbox", "Boots and cats and boots and cats!", words: ["beatbox", "beat box"], legs: .drum, mouth: .chatter,
                   body: GenieBodyMove(nod: 1.6), effect: .notes, mood: .cool, duration: 2.4),
        GenieTrick("tap_dance", "Tap dance", "Clickety-clack!", words: ["tap dance"], legs: .march,
                   body: GenieBodyMove(hops: 6, height: 3), effect: .stars, duration: 2.2),
        GenieTrick("conga", "Conga", "Conga line!", words: ["conga", "conga line"], legs: .dance, body: GenieBodyMove(sway: 1),
                   effect: .confetti, travel: .zigzag, mood: .silly, duration: 3),
        GenieTrick("limbo", "Limbo", "How low can I go?", words: ["limbo"], legs: .splay, mouth: .o, body: GenieBodyMove(squash: 1.6, tilt: -18),
                   mood: .silly, duration: 2.2),
        GenieTrick("jump", "Jump", "Boing!", words: ["jump", "leap"], legs: .tuck, eyes: .wide, body: GenieBodyMove(hops: 1, height: 34, squash: 1),
                   sound: .boing, duration: 1.1, symbol: "figure.jumprope"),
        GenieTrick("bunny_hops", "Bunny hops", "Hop hop hop!", words: ["hop", "hops", "bunny hop", "bunny hops"], legs: .tuck,
                   body: GenieBodyMove(hops: 4, height: 14, squash: 0.6), travel: .hopAround, sound: .boing, mood: .silly, duration: 2.4, symbol: "hare.fill"),
        GenieTrick("bounce", "Bounce", "Boing boing boing!", words: ["bounce", "boing"], body: GenieBodyMove(hops: 6, height: 10, squash: 1),
                   sound: .boing, mood: .silly, duration: 2),
        GenieTrick("trampoline", "Trampoline", "Higher! Higher!", words: ["trampoline"], legs: .splay, eyes: .wide,
                   body: GenieBodyMove(hops: 3, height: 40, squash: 1.2), sound: .boing, mood: .silly, duration: 2.4),
        GenieTrick("levitate", "Levitate", "I'm a genie, after all.", words: ["levitate", "float", "hover"], legs: .tuck, eyes: .closed,
                   body: GenieBodyMove(float: 1), effect: .sparkles, sound: .appear, mood: .calm, duration: 2.6, symbol: "sparkles"),
        GenieTrick("teleport", "Teleport", "Now you see me…", words: ["teleport", "beam me up"], eyes: .closed, body: GenieBodyMove(grow: -0.4, fade: 1),
                   effect: .smoke, travel: .zoom, sound: .vanish, mood: .cool, duration: 1.8),
        GenieTrick("vanish", "Disappear", "Poof!", words: ["disappear", "vanish", "poof"], eyes: .closed, body: GenieBodyMove(grow: -0.6, fade: 1),
                   effect: .smoke, sound: .vanish, mood: .silly, duration: 1.8, symbol: "eye.slash.fill"),
        GenieTrick("grow", "Grow", "Big spider energy!", words: ["grow", "get big", "get bigger"], legs: .flex, eyes: .wide,
                   body: GenieBodyMove(grow: 0.6), effect: .stars, sound: .boing, mood: .proud, duration: 1.8, symbol: "arrow.up.left.and.arrow.down.right"),
        GenieTrick("shrink", "Shrink", "Tiny me!", words: ["shrink", "get small", "get smaller"], eyes: .wide, mouth: .o, body: GenieBodyMove(grow: -0.5),
                   effect: .sparkles, sound: .giggle, mood: .silly, duration: 1.8, symbol: "arrow.down.right.and.arrow.up.left"),
        GenieTrick("ghost", "Ghost", "Boooo!", words: ["ghost", "boo", "haunt"], legs: .reach, eyes: .wide, mouth: .o,
                   body: GenieBodyMove(float: 0.5, fade: 0.6), effect: .ghosts, sound: .dizzy, mood: .silly, duration: 2.4),
        GenieTrick("rainbow", "Rainbow", "Taste the rainbow!", words: ["rainbow"], legs: .cheer,
                   body: GenieBodyMove(colors: 2), effect: .rainbow, sound: .spotted, duration: 2.6, symbol: "rainbow"),
        GenieTrick("glow_up", "Glow up", "Shine bright!", words: ["glow", "glow up", "shine"], eyes: .stars, body: GenieBodyMove(grow: 0.15),
                   effect: .sparkles, sound: .spotted, mood: .proud, duration: 2),
        GenieTrick("sparkle", "Sparkle", "Bling!", words: ["sparkle", "bling"], eyes: .stars, effect: .sparkles, sound: .spotted, duration: 1.6),
        GenieTrick("fireworks", "Fireworks", "Ooooh, aaaah!", words: ["fireworks", "firework"], legs: .cheer, eyes: .wide, mouth: .o,
                   effect: .stars, sound: .spotted, duration: 2.4),
        GenieTrick("party", "Party", "Woo, party time!", words: ["party", "celebrate", "celebration"], legs: .cheer,
                   body: GenieBodyMove(hops: 2, height: 10), effect: .confetti, sound: .giggle, duration: 2.6, symbol: "party.popper.fill"),
        GenieTrick("balloons", "Balloons", "Happy un-birthday!", words: ["balloons", "balloon", "birthday"], legs: .reach,
                   body: GenieBodyMove(float: 0.3), effect: .balloons, duration: 2.6, symbol: "balloon.2.fill"),
        GenieTrick("bubbles", "Blow bubbles", "Bloop bloop.", words: ["bubbles", "blow bubbles"], mouth: .o, effect: .bubbles, mood: .calm, duration: 2.6),
        GenieTrick("snow_day", "Snow day", "Brrr!", words: ["snow", "snow day", "brr"], legs: .hug, mouth: .chatter,
                   body: GenieBodyMove(shake: 0.5), effect: .snow, mood: .silly, duration: 2.6, symbol: "snowflake"),
        GenieTrick("thunder", "Thunderstorm", "Kaboom!", words: ["thunder", "lightning", "storm"], eyes: .wide, mouth: .o,
                   body: GenieBodyMove(shake: 1), effect: .lightning, sound: .dizzy, mood: .scared, duration: 1.8, symbol: "cloud.bolt.fill"),
        GenieTrick("on_fire", "On fire", "I'm on fire today!", words: ["on fire", "hot streak"], legs: .cheer, eyes: .squint,
                   effect: .flames, sound: .whoosh, mood: .proud, duration: 2.2, symbol: "flame.fill"),
        GenieTrick("flowers", "Flower shower", "These are for you!", words: ["flowers", "flower", "petals"], legs: .reach, eyes: .hearts,
                   effect: .petals, mood: .loving, duration: 2.6, symbol: "camera.macro"),
        GenieTrick("love", "Love", "I love you!", words: ["love", "i love you", "love you", "heart"], legs: .hug, eyes: .hearts,
                   effect: .hearts, sound: .giggle, mood: .loving, duration: 2.4, symbol: "heart.fill"),
        GenieTrick("hug", "Hug", "Come here, you!", words: ["hug", "cuddle", "give me a hug"], legs: .hug, eyes: .closed,
                   body: GenieBodyMove(squash: 0.4), effect: .hearts, mood: .loving, duration: 2.2, symbol: "figure.2.arms.open"),
        GenieTrick("blush", "Blush", "Oh, stop it…", words: ["blush", "you're cute", "cute"], eyes: .down, body: GenieBodyMove(tilt: -10),
                   effect: .hearts, mood: .loving, duration: 2),
        GenieTrick("blow_kiss", "Blow a kiss", "Mwah!", words: ["kiss", "blow a kiss", "blow me a kiss"], legs: .reach, eyes: .wink, mouth: .o,
                   effect: .hearts, mood: .loving, duration: 1.8, symbol: "mouth.fill"),
        GenieTrick("wink", "Wink", "You and me, partner.", words: ["wink"], eyes: .wink, mood: .cool, duration: 1.4, symbol: "eye.fill"),
        GenieTrick("purr", "Purr", "Prrrrr…", words: ["purr", "pet", "pat", "stroke"], eyes: .closed, body: GenieBodyMove(squash: 0.3, shake: 0.15),
                   effect: .hearts, mood: .loving, duration: 2.4, symbol: "pawprint.fill"),
        GenieTrick("giggle", "Giggle", "Hee-hee-hee! That tickles!", words: ["giggle", "tickle", "tickle tickle"], eyes: .squint,
                   body: GenieBodyMove(hops: 3, height: 4, shake: 0.4), sound: .giggle, mood: .silly, duration: 1.6, symbol: "face.smiling.inverse"),
        GenieTrick("belly_laugh", "Belly laugh", "Ha ha ha ha!", words: ["laugh", "belly laugh", "lol"], eyes: .closed, mouth: .open,
                   body: GenieBodyMove(shake: 0.6, tilt: -14), sound: .giggle, mood: .silly, duration: 2),
        GenieTrick("sneeze", "Sneeze", "Ah… ah… ACHOO!", words: ["sneeze", "achoo"], eyes: .closed, mouth: .open,
                   body: GenieBodyMove(squash: 1, shake: 0.8, tilt: 12), effect: .smoke, sound: .boing, mood: .silly, duration: 1.6),
        GenieTrick("hiccups", "Hiccups", "Hic! …hic!", words: ["hiccup", "hiccups"], eyes: .wide, mouth: .o, body: GenieBodyMove(hops: 3, height: 6),
                   mood: .silly, duration: 2.2),
        GenieTrick("yawn", "Yawn", "Hmm, sleepy…", words: ["yawn", "tired"], legs: .reach, eyes: .sleepy, mouth: .yawn,
                   body: GenieBodyMove(grow: 0.08), mood: .sleepy, duration: 2.2),
        GenieTrick("nap", "Nap", "Zzz…", words: ["sleep", "nap", "go to sleep", "take a nap", "bedtime"], legs: .tuck, eyes: .closed, mouth: .flat,
                   body: GenieBodyMove(squash: 0.3), effect: .zzz, mood: .sleepy, duration: 4, symbol: "moon.zzz.fill"),
        GenieTrick("snore", "Snore", "Hrrrnk… mimimi.", words: ["snore"], legs: .tuck, eyes: .closed, mouth: .open, body: GenieBodyMove(grow: 0.1),
                   effect: .zzz, mood: .sleepy, duration: 3),
        GenieTrick("stretch", "Stretch", "Ahh, that's better.", words: ["stretch"], legs: .splay, eyes: .closed, mouth: .o,
                   body: GenieBodyMove(squash: -0.8, grow: 0.15), mood: .calm, duration: 2.2, symbol: "figure.flexibility"),
        GenieTrick("meditate", "Meditate", "Ommm…", words: ["meditate", "om", "zen", "relax", "calm down"], legs: .tuck, eyes: .closed, mouth: .flat,
                   body: GenieBodyMove(float: 0.5), effect: .sparkles, mood: .calm, duration: 3.4, symbol: "figure.mind.and.body"),
        GenieTrick("yoga", "Yoga", "Downward spider.", words: ["yoga"], legs: .reach, eyes: .closed, body: GenieBodyMove(tilt: 24),
                   mood: .calm, duration: 2.6, symbol: "figure.yoga"),
        GenieTrick("pushups", "Push-ups", "One, two, three!", words: ["push ups", "pushups", "push up", "exercise", "work out", "workout"], legs: .splay,
                   body: GenieBodyMove(hops: 3, height: -6, squash: 1), effect: .sweat, mood: .proud, duration: 2.4, symbol: "figure.strengthtraining.traditional"),
        GenieTrick("flex", "Flex", "Feel these eight legs!", words: ["flex", "muscles", "show your muscles"], legs: .flex, eyes: .squint,
                   body: GenieBodyMove(grow: 0.12), effect: .muscles, mood: .proud, duration: 2, symbol: "dumbbell.fill"),
        GenieTrick("karate", "Karate", "Hi-yah!", words: ["karate", "kung fu", "chop"], legs: .point, eyes: .squint, mouth: .open,
                   body: GenieBodyMove(shake: 0.7, tilt: -10), effect: .exclaim, sound: .whoosh, mood: .cool, duration: 1.4, symbol: "figure.martial.arts"),
        GenieTrick("ninja", "Ninja", "You saw nothing.", words: ["ninja", "sneak attack"], legs: .tuck, eyes: .squint, mouth: .flat,
                   body: GenieBodyMove(fade: 0.8), effect: .smoke, travel: .zoom, sound: .whoosh, mood: .cool, duration: 1.8),
        GenieTrick("salute", "Salute", "Aye aye, captain!", words: ["salute"], legs: .salute, mood: .proud, duration: 1.8, symbol: "flag.fill"),
        GenieTrick("bow", "Take a bow", "Thank you, thank you.", words: ["bow", "take a bow"], legs: .shrug, eyes: .closed,
                   body: GenieBodyMove(squash: 0.5, tilt: 14), mood: .proud, duration: 1.8),
        GenieTrick("curtsy", "Curtsy", "Charmed, I'm sure.", words: ["curtsy", "curtsey"], legs: .shrug, eyes: .closed,
                   body: GenieBodyMove(squash: 0.7), mood: .loving, duration: 1.8),
        GenieTrick("applause", "Applause", "Bravo! Bravo!", words: ["clap", "applause", "bravo", "clap your hands"], legs: .clap,
                   body: GenieBodyMove(hops: 2, height: 4), effect: .stars, sound: .reply, duration: 2, symbol: "hands.clap.fill"),
        GenieTrick("nod", "Nod", "Mm-hmm, mm-hmm.", words: ["nod", "nod your head"], body: GenieBodyMove(nod: 2), mood: .calm, duration: 1.4),
        GenieTrick("no_no", "No-no", "Nuh-uh.", words: ["shake your head", "no no"], mouth: .flat, body: GenieBodyMove(wobble: 1), mood: .sad, duration: 1.4),
        GenieTrick("shrug", "Shrug", "Beats me!", words: ["shrug"], legs: .shrug, eyes: .up, mouth: .flat,
                   body: GenieBodyMove(hops: 1, height: 3), mood: .silly, duration: 1.6),
        GenieTrick("ponder", "Ponder", "Hmmm, let me think…", words: ["think", "ponder"], legs: .point, eyes: .up, mouth: .flat,
                   effect: .question, mood: .calm, duration: 2.4, symbol: "brain.head.profile"),
        GenieTrick("eureka", "Eureka", "I've got it!", words: ["eureka", "idea", "i've got an idea"], legs: .cheer, eyes: .wide,
                   body: GenieBodyMove(hops: 1, height: 12), effect: .idea, sound: .spotted, duration: 1.8, symbol: "lightbulb.fill"),
        GenieTrick("confused", "Confused", "Huh?", words: ["confused", "huh"], eyes: .roll, mouth: .flat, body: GenieBodyMove(wobble: 0.6, tilt: 16),
                   effect: .question, mood: .silly, duration: 1.8, symbol: "questionmark"),
        GenieTrick("gasp", "Gasp", "Oh!", words: ["gasp", "surprised"], eyes: .wide, mouth: .o, body: GenieBodyMove(hops: 1, height: 6),
                   effect: .exclaim, sound: .spotted, mood: .scared, duration: 1.3, symbol: "exclamationmark"),
        GenieTrick("spooked", "Spooked", "Eek! Too close!", words: ["scared", "spooked", "eek", "scare"], legs: .coverEyes, eyes: .wide, mouth: .o,
                   body: GenieBodyMove(shake: 1), effect: .sweat, travel: .flee, sound: .dizzy, mood: .scared, duration: 1.8),
        GenieTrick("hide", "Hide", "You can't see me!", words: ["hide", "hide and seek"], legs: .coverEyes, eyes: .closed,
                   body: GenieBodyMove(squash: 0.6), travel: .hide, mood: .silly, duration: 2.2),
        GenieTrick("peekaboo", "Peekaboo", "Peekaboo!", words: ["peekaboo", "peek a boo"], legs: .coverEyes, eyes: .wide,
                   body: GenieBodyMove(hops: 1, height: 10), effect: .sparkles, sound: .giggle, mood: .silly, duration: 1.8, symbol: "eyes"),
        GenieTrick("sulk", "Sulk", "Hmph.", words: ["sulk", "pout"], eyes: .down, mouth: .frown, body: GenieBodyMove(squash: 0.4, tilt: -6),
                   mood: .sad, duration: 2.2, symbol: "cloud.rain.fill"),
        GenieTrick("cry", "Cry", "Waaah!", words: ["cry", "weep", "sob"], eyes: .closed, mouth: .frown, body: GenieBodyMove(shake: 0.3),
                   effect: .tears, mood: .sad, duration: 2.2, symbol: "drop.fill"),
        GenieTrick("tantrum", "Tantrum", "Not fair, not fair!", words: ["tantrum", "throw a tantrum"], legs: .drum, eyes: .squint, mouth: .frown,
                   body: GenieBodyMove(hops: 4, height: 5, shake: 0.8), effect: .smoke, mood: .silly, duration: 2),
        GenieTrick("dizzy", "Dizzy", "Whoa, the room is spinning…", words: ["dizzy", "spin me"], eyes: .spiral, mouth: .o,
                   body: GenieBodyMove(wobble: 1), effect: .stars, sound: .dizzy, mood: .silly, duration: 2, symbol: "tornado"),
        GenieTrick("faint", "Faint", "Oh my… *thud*", words: ["faint", "swoon"], eyes: .cross, mouth: .o, body: GenieBodyMove(tilt: 80),
                   effect: .stars, sound: .boing, mood: .silly, duration: 2),
        GenieTrick("play_dead", "Play dead", "Bang! …ugh.", words: ["play dead", "bang"], legs: .splay, eyes: .cross, mouth: .tongue,
                   body: GenieBodyMove(tilt: 180), sound: .boing, mood: .silly, duration: 2.4, symbol: "xmark.circle.fill"),
        GenieTrick("zombie", "Zombie", "Braaains…", words: ["zombie", "brains"], legs: .reach, eyes: .roll, mouth: .open,
                   body: GenieBodyMove(sway: 0.3, tilt: 8), travel: .toHand, mood: .silly, duration: 2.6),
        GenieTrick("robot", "Robot dance", "Beep. Boop. Beep.", words: ["robot", "robot dance", "beep boop"], legs: .point, mouth: .flat,
                   body: GenieBodyMove(shake: 0.25, nod: 1), effect: .lightning, sound: .spotted, mood: .cool, duration: 2.4, symbol: "cpu.fill"),
        GenieTrick("chicken_dance", "Chicken dance", "Bawk bawk bagawk!", words: ["chicken", "chicken dance"], legs: .flap,
                   body: GenieBodyMove(hops: 3, height: 6), effect: .feathers, sound: .giggle, mood: .silly, duration: 2.4, symbol: "bird.fill"),
        GenieTrick("worm", "The worm", "Wiggle wiggle wiggle.", words: ["worm", "do the worm", "wiggle"], legs: .wiggle,
                   body: GenieBodyMove(squash: 1.2, sway: 0.4), travel: .backAndForth, mood: .silly, duration: 2.4),
        GenieTrick("swim", "Swim", "Just keep swimming.", words: ["swim", "swimming"], legs: .swim, body: GenieBodyMove(sway: 0.4, float: 0.3),
                   effect: .bubbles, mood: .calm, duration: 2.6, symbol: "figure.pool.swim"),
        GenieTrick("fly", "Fly", "To infinity and beyond!", words: ["fly", "take off"], legs: .flap, eyes: .wide, body: GenieBodyMove(float: 1),
                   effect: .sparkles, travel: .figureEight, sound: .whoosh, duration: 3, symbol: "airplane"),
        GenieTrick("blast_off", "Blast off", "Three, two, one… liftoff!", words: ["blast off", "rocket", "liftoff", "lift off"], legs: .tuck,
                   eyes: .squint, body: GenieBodyMove(shake: 1, float: 1.6), effect: .rocket, sound: .whoosh, mood: .proud, duration: 2.4, symbol: "flame.fill"),
        GenieTrick("web_swing", "Web swing", "Thwip!", words: ["web swing", "swing", "thwip"], legs: .reach, body: GenieBodyMove(sway: 1),
                   effect: .web, travel: .rope, sound: .whoosh, mood: .cool, duration: 2.6, symbol: "point.3.connected.trianglepath.dotted"),
        GenieTrick("abseil", "Abseil", "Going down!", words: ["abseil", "rappel", "drop down"], legs: .tuck, eyes: .down,
                   effect: .web, travel: .rope, mood: .calm, duration: 3),
        GenieTrick("spin_web", "Spin a web", "A little something to hang my turban on.", words: ["spin a web", "web", "make a web"], legs: .typing,
                   eyes: .squint, effect: .web, mood: .calm, duration: 3, symbol: "circle.hexagongrid.fill"),
        GenieTrick("catch_fly", "Catch a fly", "Snack time!", words: ["catch a fly", "catch flies"], legs: .reach, eyes: .roll, mouth: .tongue,
                   effect: .bugs, sound: .giggle, mood: .silly, duration: 2.4, symbol: "ladybug.fill"),
        GenieTrick("juggle", "Juggle", "Three balls, eight legs. Easy.", words: ["juggle", "juggling"], legs: .dance, eyes: .up,
                   effect: .juggling, mood: .proud, duration: 3, symbol: "circle.grid.3x3.fill"),
        GenieTrick("magic", "Magic trick", "Abracadabra!", words: ["magic", "magic trick", "abracadabra"], legs: .conduct, eyes: .stars,
                   body: GenieBodyMove(colors: 0.5), effect: .sparkles, sound: .spotted, mood: .proud, duration: 2.2, symbol: "wand.and.stars"),
        GenieTrick("grant_wish", "Grant a wish", "Your wish is my command!", words: ["grant a wish", "wish", "make a wish"], legs: .cheer,
                   eyes: .stars, body: GenieBodyMove(float: 0.4), effect: .stars, sound: .appear, mood: .proud, duration: 2.4, symbol: "star.fill"),
        GenieTrick("jackpot", "Jackpot", "Ka-ching!", words: ["jackpot", "ka ching"], legs: .cheer, eyes: .stars,
                   body: GenieBodyMove(hops: 2, height: 8), effect: .coins, sound: .spotted, duration: 2.4, symbol: "dollarsign.circle.fill"),
        GenieTrick("snack", "Snack", "Nom nom nom.", words: ["snack", "nom nom", "have a snack"], eyes: .closed, mouth: .chatter,
                   effect: .food, mood: .happy, duration: 2.4, symbol: "fork.knife"),
        GenieTrick("hack", "Hack away", "Shipping code!", words: ["hack", "hack away", "ship it"], legs: .typing, eyes: .squint,
                   body: GenieBodyMove(nod: 0.6), effect: .laptop, mood: .proud, duration: 2.6, symbol: "laptopcomputer"),
        GenieTrick("zoomies", "Zoomies", "Nyoooom!", words: ["zoomies", "nyoom"], legs: .march, eyes: .wide,
                   effect: .smoke, travel: .zoom, sound: .whoosh, mood: .silly, duration: 2.2, symbol: "hare.fill"),
        GenieTrick("run_circles", "Run in circles", "Wheeeee!", words: ["run in circles", "circles", "circle"], legs: .march,
                   travel: .circle, sound: .whoosh, mood: .silly, duration: 3, symbol: "circle.dotted"),
        GenieTrick("chase_tail", "Chase my tail", "I'll get it this time!", words: ["chase your tail", "chase tail"], legs: .march, eyes: .roll,
                   body: GenieBodyMove(spin: 3), sound: .giggle, mood: .silly, duration: 2.2),
        GenieTrick("follow", "Follow", "Right behind you!", words: ["follow me", "come here", "come to me"], legs: .march, travel: .toHand,
                   mood: .happy, duration: 2, symbol: "figure.walk"),
        GenieTrick("fetch", "Fetch", "Got it!", words: ["fetch", "go fetch"], legs: .march, eyes: .wide, effect: .sparkles, travel: .toHand,
                   sound: .boing, duration: 2),
        GenieTrick("roll_over", "Roll over", "Belly rub?", words: ["roll over", "roll"], legs: .wiggle, body: GenieBodyMove(spin: 1, squash: 0.4),
                   travel: .backAndForth, mood: .loving, duration: 2.2),
        GenieTrick("freeze", "Freeze", "…", words: ["freeze", "statue"], eyes: .wide, mouth: .flat, mood: .calm, duration: 2, symbol: "pause.fill"),
        GenieTrick("peace", "Peace", "Peace and love, friend.", words: ["peace", "peace out", "peace sign"], legs: .reach, eyes: .closed,
                   effect: .peace, mood: .calm, duration: 2.2, symbol: "hand.peace"),
        GenieTrick("phone_call", "Phone call", "Hello? Yes, this is spider.", words: ["call me", "phone", "phone call"], legs: .salute, mouth: .chatter,
                   effect: .phone, mood: .silly, duration: 2.4, symbol: "phone.fill"),
        GenieTrick("selfie", "Selfie", "Say cheese!", words: ["selfie", "say cheese"], legs: .reach, eyes: .wink,
                   effect: .camera, sound: .spotted, mood: .cool, duration: 1.8, symbol: "camera.fill"),
        GenieTrick("tiptoe", "Tiptoe", "Shh… sneaky spider.", words: ["tiptoe", "sneak", "sneaky", "shh"], legs: .march, eyes: .up,
                   body: GenieBodyMove(squash: 0.3), travel: .zigzag, mood: .silly, duration: 3),
        GenieTrick("crown", "Royal wave", "All hail the spider king!", words: ["crown", "king", "queen", "royal"], legs: .wave, eyes: .closed,
                   effect: .crown, mood: .proud, duration: 2.2, symbol: "crown.fill"),
        GenieTrick("lullaby", "Lullaby", "Rock-a-bye spider…", words: ["lullaby", "rock a bye"], legs: .conduct, eyes: .sleepy, mouth: .whistle,
                   body: GenieBodyMove(sway: 0.6), effect: .notes, sound: .reply, mood: .sleepy, duration: 2.6, symbol: "moon.zzz.fill"),
        GenieTrick("daydream", "Daydream", "That cloud looks like a fly.", words: ["daydream", "day dream"], eyes: .up, mouth: .normal,
                   body: GenieBodyMove(float: 0.2), effect: .clouds, mood: .calm, duration: 2.6, symbol: "cloud.fill"),
        GenieTrick("kick_legs", "Kick my legs", "Dangle dangle dangle!", words: ["kick your legs", "dangle"], legs: .swim, eyes: .wide,
                   body: GenieBodyMove(nod: 0.8), sound: .giggle, mood: .silly, duration: 1.8),
        GenieTrick("loop_de_loop", "Loop-de-loop", "Round and round!", words: ["loop the loop", "loop de loop"], legs: .tuck, eyes: .spiral,
                   mouth: .o, body: GenieBodyMove(spin: 1), effect: .sparkles, sound: .whoosh, mood: .silly, duration: 1.6, symbol: "arrow.triangle.2.circlepath"),
        GenieTrick("merry_go_round", "Merry-go-round", "Wheee, a carousel!", words: ["merry go round", "carousel"], legs: .cheer, eyes: .closed,
                   body: GenieBodyMove(spin: 2, float: 0.2), effect: .carousel, sound: .giggle, duration: 2.4),
        GenieTrick("tornado", "Tornado", "I am the whirlwind!", words: ["tornado", "whirlwind", "twister"], legs: .splay, eyes: .cross, mouth: .open,
                   body: GenieBodyMove(spin: 4, shake: 0.4), effect: .tornado, sound: .dizzy, mood: .silly, duration: 2, symbol: "tornado"),
        GenieTrick("maracas", "Maracas", "Cha-cha-cha!", words: ["maracas", "cha cha"], legs: .drum, eyes: .closed,
                   body: GenieBodyMove(shake: 1.2), effect: .notes, sound: .giggle, mood: .silly, duration: 1.8, symbol: "music.note"),
        GenieTrick("jelly", "Jelly legs", "I'm all wobbly!", words: ["jelly", "wobbly"], legs: .wiggle, eyes: .roll, mouth: .tongue,
                   body: GenieBodyMove(squash: 0.6, wobble: 1.4), sound: .boing, mood: .silly, duration: 1.8),
        GenieTrick("rattled", "Rattled", "Okay, okay, I'm awake!", words: ["rattled", "rattle"], legs: .splay, eyes: .wide, mouth: .chatter,
                   body: GenieBodyMove(shake: 2), effect: .exclaim, sound: .dizzy, mood: .scared, duration: 1.6),
        GenieTrick("superhero", "Superhero", "Up, up and away!", words: ["superhero", "super hero"], legs: .reach, eyes: .squint,
                   body: GenieBodyMove(tilt: 25), effect: .hero, sound: .whoosh, mood: .proud, duration: 2, symbol: "bolt.fill"),
        GenieTrick("rollercoaster", "Rollercoaster", "Hands up, all eight of them!", words: ["rollercoaster", "roller coaster"], legs: .cheer, eyes: .wide,
                   mouth: .open, body: GenieBodyMove(sway: 1.2), effect: .coaster, sound: .whoosh, mood: .silly, duration: 2.2),
        GenieTrick("windswept", "Windswept", "Hold my turban!", words: ["windswept", "windy"], legs: .flap, eyes: .squint, mouth: .o,
                   body: GenieBodyMove(shake: 0.5, tilt: -20), effect: .wind, mood: .silly, duration: 2, symbol: "wind"),
        GenieTrick("vertigo", "Vertigo", "Don't look down. I looked down.", words: ["vertigo", "look down"], legs: .coverEyes, eyes: .down,
                   mouth: .frown, body: GenieBodyMove(wobble: 0.6), effect: .sweat, mood: .scared, duration: 2.2),
        GenieTrick("lookout", "Lookout", "I can see my web from here!", words: ["lookout", "look out"], legs: .salute, eyes: .squint,
                   body: GenieBodyMove(grow: 0.1), effect: .telescope, mood: .proud, duration: 2.2, symbol: "binoculars.fill"),
        GenieTrick("stargaze", "Stargaze", "Make a wish on that one.", words: ["stargaze", "stargazing"], legs: .reach, eyes: .up, mouth: .o,
                   body: GenieBodyMove(float: 0.3), effect: .moon, sound: .appear, mood: .calm, duration: 2.6, symbol: "moon.stars.fill"),
        GenieTrick("skate", "Skateboard", "Kickflip! Sort of.", words: ["skate", "skateboarding", "kickflip"], legs: .splay, eyes: .wink,
                   body: GenieBodyMove(sway: 0.4, tilt: 10), effect: .skateboard, sound: .boing, mood: .cool, duration: 2, symbol: "figure.skateboarding"),
        GenieTrick("surf", "Surf", "Cowabunga!", words: ["surf", "surfing", "cowabunga"], legs: .flap, eyes: .squint, mouth: .tongue,
                   body: GenieBodyMove(sway: 0.9, float: 0.15), effect: .waves, sound: .whoosh, mood: .cool, duration: 2.4, symbol: "figure.surfing"),
        GenieTrick("sled", "Sled", "Weee, down the hill!", words: ["sled", "sledging", "sledding"], legs: .tuck, eyes: .wide,
                   body: GenieBodyMove(tilt: -12), effect: .snow, sound: .giggle, duration: 2, symbol: "snowflake"),
        GenieTrick("cannonball", "Cannonball", "Bombs away!", words: ["cannonball", "cannon ball"], legs: .tuck, eyes: .closed, mouth: .o,
                   body: GenieBodyMove(spin: 2, squash: 0.5), effect: .bubbles, sound: .boing, mood: .silly, duration: 1.6),
        GenieTrick("skydive", "Skydive", "Free fall!", words: ["skydive", "skydiving", "free fall"], legs: .splay, eyes: .wide, mouth: .open,
                   body: GenieBodyMove(float: 0.6, wobble: 0.3), effect: .clouds, sound: .whoosh, mood: .cool, duration: 2.2),
        GenieTrick("starfish", "Starfish", "Ta-da, I'm a starfish!", words: ["starfish", "star fish"], legs: .splay, eyes: .stars,
                   body: GenieBodyMove(spin: 0.5, grow: 0.15), effect: .sparkles, sound: .appear, duration: 1.8, symbol: "star.circle.fill"),
        GenieTrick("dust_off", "Dust off", "Just brushing myself off.", words: ["dust off", "dust yourself off"], legs: .clap, eyes: .squint, mouth: .flat,
                   body: GenieBodyMove(shake: 0.4), effect: .smoke, mood: .calm, duration: 1.8),
        GenieTrick("thanks_lift", "Thanks for the lift", "Thanks for the ride!", words: ["thanks for the lift", "thanks for the ride"], legs: .hug,
                   eyes: .hearts, body: GenieBodyMove(nod: 1), effect: .hearts, sound: .reply, mood: .loving, duration: 2, symbol: "heart.fill"),
        GenieTrick("comfy", "Get comfy", "Ah, this spot is perfect.", words: ["get comfy", "comfy", "cozy"], legs: .tuck, eyes: .sleepy, mouth: .yawn,
                   body: GenieBodyMove(squash: 0.3), mood: .sleepy, duration: 2.2, symbol: "bed.double.fill"),
        GenieTrick("parachute", "Parachute", "Pop the chute!", words: ["parachute", "chute"], legs: .reach, eyes: .up, mouth: .o,
                   body: GenieBodyMove(sway: 0.5, float: 0.5), effect: .parachute, sound: .appear, mood: .cool, duration: 2.4),
        GenieTrick("umbrella", "Umbrella drop", "Practically perfect in every way.", words: ["umbrella", "brolly"], legs: .point, eyes: .closed,
                   mouth: .whistle, body: GenieBodyMove(float: 0.4), effect: .umbrella, mood: .calm, duration: 2.4, symbol: "umbrella.fill"),
        GenieTrick("feather_fall", "Feather fall", "Light as a feather…", words: ["feather fall", "light as a feather"], legs: .flap, eyes: .sleepy,
                   mouth: .normal, body: GenieBodyMove(sway: 0.3, float: 0.7), effect: .feathers, mood: .calm, duration: 2.6, symbol: "leaf.fill"),
        GenieTrick("seasick", "Seasick", "Too many rides…", words: ["seasick", "queasy", "motion sick"], legs: .coverEyes, eyes: .cross, mouth: .frown,
                   body: GenieBodyMove(sway: 0.4, wobble: 0.8), effect: .queasy, sound: .dizzy, mood: .sad, duration: 2.2),
        GenieTrick("giddy", "Giddy", "Again! Again! Again!", words: ["giddy", "again again"], legs: .cheer, eyes: .stars, mouth: .open,
                   body: GenieBodyMove(hops: 3, height: 12), effect: .confetti, sound: .giggle, duration: 1.8),
        GenieTrick("pick_me", "Pick me up", "Up! Up! Carry me!", words: ["pick me up", "carry me"], legs: .reach, eyes: .wide,
                   body: GenieBodyMove(hops: 2, height: 6), effect: .exclaim, sound: .boing, mood: .loving, duration: 1.8, symbol: "arrow.up.circle.fill"),
        GenieTrick("genie_poof", "Genie poof", "Poof! Over here now.", words: ["genie poof", "poof over", "puff of smoke"], legs: .tuck, eyes: .closed,
                   mouth: .o, body: GenieBodyMove(squash: 0.2, grow: -0.3, fade: 1), magic: .poof, sound: .vanish, mood: .silly, duration: 2.4,
                   symbol: "smoke.fill"),
        GenieTrick("genie_levitate", "Smoke levitation", "Rising on a puff of smoke…", words: ["smoke levitation", "levitate on smoke", "float on smoke"],
                   legs: .tuck, eyes: .sleepy, mouth: .whistle, body: GenieBodyMove(float: 1, nod: 0.5), magic: .levitate, sound: .appear, mood: .calm,
                   duration: 3, symbol: "cloud.fill"),
        GenieTrick("genie_lamp", "Magic lamp", "Rub the lamp… eureka!", words: ["magic lamp", "genie lamp", "rub the lamp", "lamp"], legs: .reach,
                   eyes: .stars, mouth: .open, body: GenieBodyMove(hops: 1, height: 6), magic: .lamp, sound: .spotted, mood: .proud, duration: 3.2,
                   symbol: "lamp.table.fill"),
        GenieTrick("genie_wishes", "Three wishes", "Three wishes, coming right up!", words: ["three wishes", "wishes", "make three wishes"],
                   legs: .conduct, eyes: .up, body: GenieBodyMove(sway: 0.3, float: 0.2), magic: .wishes, sound: .appear, mood: .loving, duration: 3.2,
                   symbol: "star.fill"),
        GenieTrick("genie_smoke_rings", "Smoke rings", "Puff… puff… puff.", words: ["smoke rings", "smoke ring", "blow smoke rings"], eyes: .squint,
                   mouth: .whistle, body: GenieBodyMove(nod: 0.8), magic: .smokeRings, mood: .cool, duration: 3, symbol: "circle.dashed"),
        GenieTrick("genie_shimmer", "Golden shimmer", "Ooh, shiny!", words: ["golden shimmer", "shimmer", "make it shimmer"], legs: .point, eyes: .wide,
                   body: GenieBodyMove(tilt: 8), magic: .shimmer, sound: .spotted, duration: 2.6, symbol: "sun.max.fill"),
        GenieTrick("genie_stardust", "Stardust", "A sprinkle of stardust.", words: ["stardust", "star dust", "sprinkle stardust"], legs: .reach,
                   eyes: .up, mouth: .o, body: GenieBodyMove(float: 0.15, tilt: -6), magic: .stardust, sound: .appear, mood: .calm, duration: 3,
                   symbol: "moon.stars.fill"),
        GenieTrick("genie_carpet", "Magic carpet", "All aboard the magic carpet!", words: ["magic carpet", "flying carpet", "carpet ride", "carpet"],
                   legs: .splay, eyes: .wide, mouth: .open, body: GenieBodyMove(sway: 0.2, float: 0.9), magic: .carpet, sound: .whoosh, mood: .proud,
                   duration: 3.2, symbol: "wind"),
        GenieTrick("genie_vortex", "Spark vortex", "Round and round and… flash!", words: ["spark vortex", "vortex", "swirl of sparks"], legs: .conduct,
                   eyes: .spiral, mouth: .o, body: GenieBodyMove(wobble: 0.3), magic: .vortex, sound: .whoosh, mood: .cool, duration: 2.8,
                   symbol: "hurricane"),
        GenieTrick("genie_crystal", "Crystal ball", "I see… something wonderful.", words: ["crystal ball", "tell my fortune", "fortune"], legs: .hug,
                   mouth: .o, body: GenieBodyMove(squash: 0.15, nod: 0.3), magic: .crystal, sound: .appear, mood: .calm, duration: 3.2,
                   symbol: "circle.circle"),
        GenieTrick("genie_runes", "Glowing runes", "Ancient spider magic.", words: ["glowing runes", "runes", "cast a spell"], legs: .conduct,
                   eyes: .squint, mouth: .flat, body: GenieBodyMove(sway: 0.25, nod: 0.4), magic: .runes, sound: .spotted, mood: .cool, duration: 3.2,
                   symbol: "seal.fill"),
        GenieTrick("genie_halo", "Golden halo", "Angelic, if I say so myself.", words: ["golden halo", "halo", "angel"], legs: .tuck, eyes: .closed,
                   body: GenieBodyMove(float: 0.25, tilt: -4), magic: .halo, sound: .appear, mood: .loving, duration: 2.8,
                   symbol: "smallcircle.filled.circle"),
        GenieTrick("genie_fireflies", "Fireflies", "Off you go, little lights.", words: ["fireflies", "firefly", "glow worms"], legs: .wave, eyes: .up,
                   body: GenieBodyMove(sway: 0.15), magic: .fireflies, sound: .appear, mood: .calm, duration: 3.4, symbol: "sparkle"),
        GenieTrick("genie_whirl", "Smoke whirl", "Whirl!", words: ["smoke whirl", "whirl", "whirl around"], legs: .tuck, eyes: .spiral, mouth: .o,
                   body: GenieBodyMove(spin: 2, float: 0.15), magic: .whirl, sound: .whoosh, mood: .silly, duration: 2.2, symbol: "rotate.3d"),
        GenieTrick("genie_grow", "Grow in smoke", "Behold, mighty spider!", words: ["grow in smoke", "grow huge", "genie grow"], legs: .flex,
                   eyes: .wide, mouth: .open, body: GenieBodyMove(shake: 0.15, grow: 0.5), magic: .grow, sound: .boing, mood: .proud, duration: 2.4,
                   symbol: "arrow.up.left.and.arrow.down.right"),
        GenieTrick("genie_bottle", "Into the bottle", "Back in my bottle… and out!", words: ["into the bottle", "back in the bottle", "genie bottle", "bottle"],
                   legs: .tuck, eyes: .closed, mouth: .o, body: GenieBodyMove(spin: 1, grow: -0.85, fade: 0.85), magic: .bottle, sound: .vanish,
                   mood: .silly, duration: 3, symbol: "drop.fill"),
        GenieTrick("genie_rainbow", "Rainbow bridge", "A rainbow, just for you.", words: ["rainbow bridge", "make a rainbow", "rainbow arc"],
                   legs: .point, eyes: .stars, body: GenieBodyMove(tilt: 6, colors: 0.4), magic: .rainbow, sound: .spotted, duration: 3, symbol: "rainbow"),
        GenieTrick("genie_fireworks", "Golden fireworks", "Ooh! Aah!", words: ["golden fireworks", "gold fireworks", "set off fireworks"], legs: .cheer,
                   eyes: .wide, mouth: .open, body: GenieBodyMove(hops: 2, height: 5), magic: .fireworks, sound: .spotted, duration: 3,
                   symbol: "sparkles"),
        GenieTrick("genie_bubbles", "Magic bubbles", "Pop, pop, pop!", words: ["magic bubbles", "iridescent bubbles", "pop bubbles"], legs: .wiggle,
                   eyes: .up, mouth: .o, body: GenieBodyMove(float: 0.1, nod: 0.3), magic: .bubbles, mood: .calm, duration: 3,
                   symbol: "bubbles.and.sparkles"),
        GenieTrick("genie_glitter", "Glitter shower", "Everything is better with glitter.", words: ["glitter", "glitter shower", "sprinkle glitter"],
                   legs: .flap, eyes: .stars, body: GenieBodyMove(shake: 0.1, sway: 0.2), magic: .glitter, sound: .giggle, duration: 3,
                   symbol: "sparkles"),
        GenieTrick("genie_jewel_beam", "Jewel beam", "Zap! A beam of light.", words: ["jewel beam", "beam of light", "light beam"], legs: .salute,
                   eyes: .squint, mouth: .flat, body: GenieBodyMove(tilt: -8, nod: 0.2), magic: .jewelBeam, sound: .spotted, mood: .cool, duration: 2.8,
                   symbol: "rays"),
        GenieTrick("genie_ripple", "Light ripple", "Ripples of light.", words: ["light ripple", "ripple", "ripples"], legs: .point, eyes: .closed,
                   mouth: .flat, body: GenieBodyMove(squash: 0.25), magic: .ripple, mood: .calm, duration: 3, symbol: "dot.radiowaves.left.and.right"),
        GenieTrick("genie_moons", "Little moons", "My little moons, off you go!", words: ["little moons", "moons", "three moons"], legs: .conduct,
                   eyes: .up, mouth: .o, body: GenieBodyMove(float: 0.2), magic: .moons, sound: .appear, mood: .proud, duration: 3.2,
                   symbol: "moon.circle.fill"),
        GenieTrick("genie_constellation", "Constellation", "Join the stars…", words: ["constellation", "join the stars", "connect the stars"],
                   legs: .reach, eyes: .stars, mouth: .o, body: GenieBodyMove(float: 0.1, tilt: 4), magic: .constellation, mood: .calm, duration: 3.4,
                   symbol: "star.circle"),
        GenieTrick("genie_crescent", "Crescent moon", "Moonrise.", words: ["crescent moon", "crescent", "moonrise"], legs: .wave, eyes: .sleepy,
                   mouth: .whistle, body: GenieBodyMove(sway: 0.2, float: 0.1), magic: .crescent, sound: .appear, mood: .sleepy, duration: 3.2,
                   symbol: "moon.fill"),
        GenieTrick("genie_mirage", "Mirage", "Is it really there?", words: ["mirage", "illusion", "ghost image"], legs: .shrug, eyes: .roll, mouth: .o,
                   body: GenieBodyMove(wobble: 0.25, fade: 0.3), magic: .mirage, mood: .silly, duration: 3, symbol: "water.waves"),
        GenieTrick("genie_coins", "Gold coins", "Treasure!", words: ["gold coins", "coins", "treasure"], legs: .clap, eyes: .stars, mouth: .open,
                   body: GenieBodyMove(hops: 3, height: 4), magic: .coins, sound: .spotted, duration: 2.8, symbol: "dollarsign.circle"),
        GenieTrick("genie_flourish", "Flourish", "Ta-da!", words: ["flourish", "with a flourish", "ta da"], legs: .conduct, eyes: .wink,
                   body: GenieBodyMove(sway: 0.3, tilt: 10), magic: .flourish, mood: .proud, duration: 2.6, symbol: "scribble.variable"),
        GenieTrick("genie_incense", "Incense", "Breathe in… breathe out.", words: ["incense", "burn incense", "breathe"], legs: .tuck, eyes: .closed,
                   mouth: .flat, body: GenieBodyMove(sway: 0.25, nod: 0.25), magic: .incense, mood: .calm, duration: 3.4, symbol: "wind"),
        GenieTrick("genie_whirlwind", "Smoke whirlwind", "Whoooosh!", words: ["smoke whirlwind", "whirlwind of smoke", "dust devil"], legs: .flap,
                   eyes: .squint, mouth: .o, body: GenieBodyMove(spin: 1, float: 0.35), magic: .whirlwind, sound: .whoosh, mood: .silly, duration: 2.8,
                   symbol: "tornado"),
    ]

    public nonisolated static let byId: [String: GenieTrick] = Dictionary(all.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })

    public nonisolated static func named(_ id: String) -> GenieTrick? {
        byId[id]
    }

    public nonisolated static func words(_ raw: String) -> [String] {
        raw.lowercased().replacingOccurrences(of: "’", with: "'")
            .split(whereSeparator: { !($0.isLetter || $0.isNumber || $0 == "'") }).map(String.init)
    }

    public nonisolated static func isSurprise(_ raw: String) -> Bool {
        let joined = words(raw).filter { !["a", "an", "the", "me", "please", "some", "one", "do"].contains($0) }.joined(separator: "_")
        return surpriseWords.contains(joined) || joined.hasPrefix("surprise")
    }

    public nonisolated static func find(_ raw: String) -> GenieTrick? {
        let said = words(raw)
        guard !said.isEmpty else { return nil }
        let slug = said.joined(separator: "_")
        if let exact = byId[slug] { return exact }
        let phrase = said.joined(separator: " ")
        if let titled = all.first(where: { $0.title.lowercased() == phrase }) { return titled }
        if let worded = all.first(where: { $0.words.contains(phrase) }) { return worded }
        var best: (trick: GenieTrick, size: Int)?
        for trick in all {
            for trigger in trick.words + [trick.title.lowercased(), trick.id.replacingOccurrences(of: "_", with: " ")] {
                let parts = words(trigger)
                guard !parts.isEmpty, parts.count > (best?.size ?? 0), contains(said, parts) else { continue }
                best = (trick, parts.count)
            }
        }
        return best?.trick
    }

    public nonisolated static func match(_ said: [String], at start: Int) -> (trick: GenieTrick, count: Int)? {
        guard start < said.count else { return nil }
        var best: (trick: GenieTrick, count: Int)?
        for trick in all {
            for trigger in trick.words + [trick.title.lowercased()] {
                let parts = words(trigger)
                guard !parts.isEmpty, start + parts.count <= said.count, parts.count > (best?.count ?? 0) else { continue }
                if Array(said[start..<(start + parts.count)]) == parts { best = (trick, parts.count) }
            }
        }
        return best
    }

    nonisolated static func contains(_ words: [String], _ phrase: [String]) -> Bool {
        guard phrase.count <= words.count else { return false }
        return (0...(words.count - phrase.count)).contains { at in Array(words[at..<(at + phrase.count)]) == phrase }
    }

    public nonisolated static func pick(
        recent: [String] = [], taste: GenieTrickTaste = GenieTrickTaste(), mood: GenieTrickMood? = nil, random: () -> Double
    ) -> GenieTrick {
        let fresh = all.filter { !recent.suffix(8).contains($0.id) }
        let pool = fresh.isEmpty ? all : fresh
        let weights = pool.map { trick -> Double in
            var weight = exp(min(max(taste.score(trick.id), -4), 4) * 0.45)
            if let mood, trick.mood == mood { weight *= 3 }
            return weight
        }
        let total = weights.reduce(0, +)
        var roll = min(max(random(), 0), 0.999_999) * total
        for (index, weight) in weights.enumerated() {
            roll -= weight
            if roll < 0 { return pool[index] }
        }
        return pool[pool.count - 1]
    }
}

public struct GenieTrickTaste: Equatable, Sendable {
    public nonisolated static let key = "genie/trick_taste"
    public private(set) var scores: [String: Double]

    public init(scores: [String: Double] = [:]) {
        self.scores = scores
    }

    public func score(_ id: String) -> Double {
        scores[id] ?? 0
    }

    public mutating func rate(_ id: String, _ delta: Double) {
        guard GenieTricks.byId[id] != nil else { return }
        scores[id] = min(max(score(id) + delta, -6), 6)
    }

    public var favourites: [String] {
        scores.filter { $0.value > 0.5 }.sorted { $0.value == $1.value ? $0.key < $1.key : $0.value > $1.value }.prefix(5).map(\.key)
    }

    public var dislikes: [String] {
        scores.filter { $0.value < -0.5 }.sorted { $0.value == $1.value ? $0.key < $1.key : $0.value < $1.value }.prefix(5).map(\.key)
    }

    public var encoded: String {
        scores.keys.sorted().map { "\($0):\(String(format: "%.2f", scores[$0] ?? 0))" }.joined(separator: ",")
    }

    public init(encoded: String) {
        var out: [String: Double] = [:]
        for pair in encoded.split(separator: ",") {
            let parts = pair.split(separator: ":")
            guard parts.count == 2, let value = Double(parts[1]), GenieTricks.byId[String(parts[0])] != nil else { continue }
            out[String(parts[0])] = min(max(value, -6), 6)
        }
        scores = out
    }
}
