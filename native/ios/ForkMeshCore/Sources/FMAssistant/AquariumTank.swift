import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public struct AquariumTank: Sendable {
    public nonisolated static let most: Int = 12
    public nonisolated static let guests: [AquariumSwimmer] = [
        AquariumSwimmer(id: "guest-bubbles", name: "Bubbles", model: "guest"),
        AquariumSwimmer(id: "guest-finn", name: "Finn", model: "guest"),
        AquariumSwimmer(id: "guest-coral", name: "Coral", model: "guest"),
    ]
    public nonisolated static let spiderOdds: Double = 0.45
    public nonisolated static let wander: ClosedRange<Double> = 2...5
    public nonisolated static let spiderRest: ClosedRange<Double> = 2.5...5
    public nonisolated static let reach: Double = 6
    public nonisolated static let crossing: Double = 0.2
    public nonisolated static let calmPace: Double = 0.4
    public nonisolated static let mostParticles: Int = 160
    public nonisolated static let callEvery: ClosedRange<Double> = 9...15
    public nonisolated static let margin: CGFloat = 6

    public private(set) var screen: CGRect
    public private(set) var water: CGRect
    public private(set) var fish: [AquariumFish] = []
    public private(set) var particles: [AquariumParticle] = []
    public private(set) var marks: [AquariumMark] = []
    public private(set) var time: Double = 0
    public private(set) var landmarks: [GenieLandmark] = []
    public private(set) var blockers: [CGRect] = []
    public private(set) var elementBag: GenieBag<AquariumTrick> = GenieBag(AquariumTrick.element)
    public private(set) var spiderBag: GenieBag<AquariumTrick> = GenieBag(AquariumTrick.spider)
    public private(set) var spiderRestUntil: Double = 0
    public var spider: AquariumSpider?
    public var calm: Bool = false
    private var nextBubble: Double = 0

    public init(screen: CGRect, top: CGFloat, floor: CGFloat) {
        self.screen = screen
        self.water = AquariumTank.water(screen: screen, top: top, floor: floor)
    }

    public static func water(screen: CGRect, top: CGFloat, floor: CGFloat) -> CGRect {
        let upper: CGFloat = max(top, screen.minY) + margin
        let lower: CGFloat = max(min(floor, screen.maxY) - margin, upper + 60)
        return CGRect(x: screen.minX + margin, y: upper, width: max(screen.width - margin * 2, 60), height: lower - upper)
    }

    public var spiderPlaying: Bool {
        fish.contains { ($0.phase == .going || $0.phase == .acting) && $0.act?.trick.withSpider == true }
    }

    public mutating func resize(screen next: CGRect, top: CGFloat, floor: CGFloat) {
        screen = next
        water = AquariumTank.water(screen: next, top: top, floor: floor)
        for index in fish.indices where fish[index].phase != .leaving && fish[index].phase != .entering {
            fish[index].point = clamp(fish[index].point)
            fish[index].goal = clamp(fish[index].goal)
        }
    }

    public mutating func see(_ marks: [GenieLandmark]) {
        landmarks = marks
        blockers = GenieLandmarks.blockers(marks)
    }

    @discardableResult
    public mutating func roster(_ swimmers: [AquariumSwimmer], random: () -> Double) -> [AquariumEvent] {
        let wanted: [AquariumSwimmer] = swimmers.isEmpty ? AquariumTank.guests : Array(swimmers.prefix(AquariumTank.most))
        let ids: Set<String> = Set(wanted.map(\.id))
        var events: [AquariumEvent] = []
        for index in fish.indices where !ids.contains(fish[index].id) && fish[index].phase != .leaving {
            var one: AquariumFish = fish[index]
            one.phase = .leaving
            one.act = nil
            one.nudge = .zero
            one.goal = CGPoint(x: one.point.x, y: screen.minY - 80)
            say(&one, "Bye!", 1.6)
            fish[index] = one
            events.append(.log("\(one.name) swims off"))
        }
        for swimmer in wanted {
            if let index = fish.firstIndex(where: { $0.id == swimmer.id && $0.phase != .leaving }) {
                var one: AquariumFish = fish[index]
                one.name = swimmer.name
                if one.waiting != swimmer.waiting {
                    one.waiting = swimmer.waiting
                    if swimmer.waiting {
                        drop(&one, random: random)
                        one.nextCall = time + 0.8
                        one.restUntil = time
                    }
                }
                fish[index] = one
                continue
            }
            let x: CGFloat = water.minX + water.width * CGFloat(0.1 + 0.8 * random())
            var made: AquariumFish = AquariumFish(
                id: swimmer.id, name: swimmer.name, model: swimmer.model, waiting: swimmer.waiting,
                at: CGPoint(x: x, y: screen.minY - 30))
            let upper: CGRect = CGRect(x: water.minX, y: water.minY, width: water.width, height: water.height * 0.5)
            made.goal = AquariumClear.waypoint(in: upper, body: { [made] in made.body(at: $0) }, blockers: blockers, water: water, random: random)
            made.nextCall = time + 1.5
            fish.append(made)
            splash(at: CGPoint(x: x, y: water.minY))
            events.append(.log("\(swimmer.name) drops in"))
        }
        return events
    }

    @discardableResult
    public mutating func step(_ seconds: Double, random: () -> Double) -> [AquariumEvent] {
        let dt: Double = min(max(seconds, 0), 0.1)
        time += dt
        var events: [AquariumEvent] = []
        if calm {
            particles.removeAll()
        } else {
            ambient(random)
        }
        for index in fish.indices {
            var one: AquariumFish = fish[index]
            move(&one, dt: dt, random: random, events: &events)
            fish[index] = one
        }
        fish.removeAll { $0.phase == .leaving && $0.point.y < screen.minY - 40 }
        advanceParticles(dt)
        for index in marks.indices { marks[index].age += dt }
        marks.removeAll { $0.age >= $0.life }
        return events
    }

    private mutating func move(_ one: inout AquariumFish, dt: Double, random: () -> Double, events: inout [AquariumEvent]) {
        one.appear = min(1, one.appear + dt / 0.6)
        switch one.phase {
        case .entering:
            if steer(&one, to: one.goal, speed: 95, dt: dt) {
                one.phase = .wandering
                one.restUntil = time + range(AquariumTank.wander, random)
            }
        case .wandering:
            wander(&one, dt: dt, random: random)
        case .going:
            go(&one, dt: dt, random: random, events: &events)
        case .acting:
            perform(&one, dt: dt, random: random, events: &events)
        case .leaving:
            _ = steer(&one, to: one.goal, speed: 150, dt: dt)
        }
        if let speech = one.speech, time < one.speechUntil {
            one.speechBox = AquariumClear.speech(speech, body: one.box, blockers: blockers, screen: screen)
        } else {
            one.speech = nil
            one.speechBox = nil
        }
        let target: Double = AquariumClear.opacity(one.box, blockers: blockers)
        one.opacity += (target - one.opacity) * min(1, dt * 6)
    }

    private mutating func wander(_ one: inout AquariumFish, dt: Double, random: () -> Double) {
        let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
        if one.waiting {
            let band: CGRect = CGRect(x: water.minX, y: water.minY, width: water.width, height: min(150, water.height * 0.3))
            let arrived: Bool = steer(&one, to: one.goal, speed: 50, dt: dt)
            if arrived, time >= one.restUntil || !band.insetBy(dx: -1, dy: -1).contains(one.goal) {
                one.goal = AquariumClear.waypoint(in: band, body: body, blockers: blockers, water: water, random: random)
                one.restUntil = time + range(3...6, random)
            }
            if time >= one.nextCall {
                say(&one, "I need you!", 2.2)
                one.nextCall = time + range(AquariumTank.callEvery, random)
            }
            return
        }
        let arrived: Bool = steer(&one, to: one.goal, speed: 55, dt: dt)
        guard arrived, time >= one.restUntil else { return }
        if start(&one, random: random) { return }
        one.goal = AquariumClear.waypoint(in: water, body: body, blockers: blockers, water: water, random: random)
        one.restUntil = time + range(AquariumTank.wander, random)
    }

    private mutating func start(_ one: inout AquariumFish, random: () -> Double) -> Bool {
        if let spider = spider, spider.free, !spiderPlaying, time >= spiderRestUntil, random() < AquariumTank.spiderOdds,
            let trick = spiderBag.next(random: random), let spot = spiderSpot(for: one, spider: spider)
        {
            begin(&one, AquariumAct(trick: trick, target: spider.rect, text: "", spot: spot))
            return true
        }
        let taken: [CGRect] = fish.compactMap { $0.id == one.id ? nil : $0.act?.target }
        let marks: [GenieLandmark] = landmarks.filter { (mark: GenieLandmark) -> Bool in
            !taken.contains(mark.rect) && AquariumClear.fits(mark.rect.insetBy(dx: 1, dy: 1), in: screen)
        }
        guard let trick = elementBag.next(random: random, fits: { (trick: AquariumTrick) -> Bool in
            marks.contains { trick.fits($0) }
        }) else { return false }
        let here: CGPoint = one.point
        let scored: [(GenieLandmark, Double)] = marks.filter { trick.fits($0) }.map { (mark: GenieLandmark) -> (GenieLandmark, Double) in
            (mark, Double(hypot(mark.rect.midX - here.x, mark.rect.midY - here.y)) * (0.4 + random()))
        }
        let others: [CGRect] = fish.filter { $0.id != one.id && $0.act != nil }.map(\.box)
        let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
        for (mark, _) in scored.sorted(by: { $0.1 < $1.1 }).prefix(4) {
            if let spot = AquariumClear.spot(beside: mark.rect, body: body, blockers: blockers + others, water: water, prefer: here) {
                begin(&one, AquariumAct(trick: trick, target: mark.rect, text: mark.text, spot: spot))
                return true
            }
        }
        return false
    }

    private mutating func begin(_ one: inout AquariumFish, _ act: AquariumAct) {
        one.act = act
        one.phase = .going
        one.deadline = time + AquariumTank.reach
        one.nudge = .zero
    }

    private func spiderSpot(for one: AquariumFish, spider: AquariumSpider) -> CGPoint? {
        let body: (CGPoint) -> CGRect = { one.body(at: $0) }
        let sample: CGRect = body(.zero)
        let rect: CGRect = spider.rect
        let gap: CGFloat = 6
        let options: [CGPoint] = [
            CGPoint(x: rect.minX - gap - sample.maxX, y: rect.midY - rect.height * 0.1),
            CGPoint(x: rect.maxX + gap - sample.minX, y: rect.midY - rect.height * 0.1),
            CGPoint(x: rect.midX, y: rect.minY - gap - sample.maxY),
            CGPoint(x: rect.midX, y: rect.maxY + gap - sample.minY),
        ]
        return options.filter { AquariumClear.clear(body($0), blockers: blockers, water: water, room: AquariumClear.room) }
            .min { hypot($0.x - one.point.x, $0.y - one.point.y) < hypot($1.x - one.point.x, $1.y - one.point.y) }
    }

    private mutating func go(_ one: inout AquariumFish, dt: Double, random: () -> Double, events: inout [AquariumEvent]) {
        guard var act = one.act else {
            one.phase = .wandering
            return
        }
        if act.trick.withSpider {
            guard let spider = spider, spider.free, let spot = spiderSpot(for: one, spider: spider) else {
                drop(&one, random: random)
                return
            }
            act.spot = spot
            act.target = spider.rect
        }
        let arrived: Bool = steer(&one, to: act.spot, speed: 110, dt: dt)
        if arrived {
            act.start = time
            one.act = act
            one.phase = .acting
            events.append(.log("\(one.name) \(act.trick.title)" + (act.text.isEmpty ? "" : " “\(GenieScreenPlay.short(act.text))”")))
        } else if time > one.deadline {
            drop(&one, random: random)
        } else {
            one.act = act
        }
    }

    private mutating func drop(_ one: inout AquariumFish, random: () -> Double) {
        if one.act?.trick.withSpider == true, one.phase == .acting {
            spiderRestUntil = time + 2.5
        }
        one.act = nil
        one.nudge = .zero
        one.phase = .wandering
        let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
        one.goal = AquariumClear.clear(one.box, blockers: blockers, water: water)
            ? one.point : AquariumClear.waypoint(in: water, body: body, blockers: blockers, water: water, random: random)
        one.restUntil = time + 1
    }

    private mutating func finish(_ one: inout AquariumFish, random: () -> Double) {
        let away: CGPoint? = one.act?.away
        if one.act?.trick.withSpider == true {
            spiderRestUntil = time + range(AquariumTank.spiderRest, random)
        }
        one.act = nil
        one.nudge = .zero
        one.phase = .wandering
        let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
        if let away = away, AquariumClear.clear(body(away), blockers: blockers, water: water) {
            one.goal = away
        } else if AquariumClear.clear(one.box, blockers: blockers, water: water) {
            one.goal = one.point
        } else {
            one.goal = AquariumClear.waypoint(in: water, body: body, blockers: blockers, water: water, random: random)
        }
        one.restUntil = time + range(AquariumTank.wander, random)
    }

    private mutating func perform(_ one: inout AquariumFish, dt: Double, random: () -> Double, events: inout [AquariumEvent]) {
        guard var act = one.act else {
            one.phase = .wandering
            return
        }
        let age: Double = time - act.start
        if act.trick.withSpider {
            guard let spider = spider, spider.free else {
                one.act = act
                finish(&one, random: random)
                return
            }
            act.target = spider.rect
            spiderAct(&one, &act, spider: spider, age: age, dt: dt, random: random, events: &events)
        } else {
            elementAct(&one, &act, age: age, dt: dt, random: random)
        }
        one.act = act
        if age >= act.duration { finish(&one, random: random) }
    }

    private mutating func elementAct(_ one: inout AquariumFish, _ act: inout AquariumAct, age: Double, dt: Double, random: () -> Double) {
        let rect: CGRect = act.target
        let toward: CGSize = unit(from: act.spot, to: CGPoint(x: rect.midX, y: rect.midY))
        one.facingLeft = rect.midX < act.spot.x
        if act.trick != .startle || age < 1 { _ = steer(&one, to: act.spot, speed: 60, dt: dt) }
        let tint: AquariumColor = one.look.body
        switch act.trick {
        case .peek:
            one.nudge = CGSize(width: toward.width * 3 * sin(age * 2), height: 1.5 * sin(age * 5))
            if act.beat(0, at: 1.4, age: age) { say(&one, "!?", 1) }
        case .boop:
            let push: Double = max(sin(age * Double.pi / 0.6), 0)
            one.nudge = CGSize(width: toward.width * 5 * push, height: toward.height * 5 * push)
            for (index, at) in [0.3, 0.9, 1.5].enumerated() where act.beat(index, at: at, age: age) {
                marks.append(AquariumMark(.ring, rect: rect, life: 0.5, tint: tint))
                emit(.sparkle, at: edge(of: rect, toward: one.point), life: 0.5, size: 5)
            }
        case .read:
            let letters: [Character] = Array(act.text.prefix(24))
            if calm {
                if act.beat(0, at: 0, age: age) { say(&one, "“" + GenieScreenPlay.short(act.text) + "”", act.duration) }
            } else {
                for (index, letter) in letters.enumerated() where !letter.isWhitespace && act.beat(index, at: 0.3 + Double(index) * 0.14, age: age) {
                    let x: CGFloat = rect.minX + rect.width * (CGFloat(index) + 0.5) / CGFloat(max(letters.count, 1))
                    emit(.glyph(String(letter)), at: CGPoint(x: x, y: rect.minY - 2), velocity: CGSize(width: 0, height: -26), life: 1.6, size: 11)
                }
            }
        case .underline:
            if act.beat(0, at: 0, age: age) { marks.append(AquariumMark(.underline, rect: rect, life: 2.4, tint: tint)) }
            for index in 0..<5 where act.beat(index + 1, at: 0.2 + Double(index) * 0.3, age: age) {
                let x: CGFloat = rect.minX + rect.width * CGFloat(index) / 4
                emit(.bubble, at: CGPoint(x: x, y: rect.maxY + 3), velocity: CGSize(width: 0, height: -10), life: 1, size: 3)
            }
        case .polish:
            one.nudge = CGSize(width: -toward.height * 3 * sin(age * 14), height: toward.width * 3 * sin(age * 14))
            for index in 0..<9 where act.beat(index, at: Double(index) * 0.2, age: age) {
                emit(.sparkle, at: perimeter(rect, random()), life: 0.7, size: 5)
            }
            if act.beat(20, at: 2, age: age) { marks.append(AquariumMark(.ring, rect: rect, life: 0.8, tint: .gold)) }
        case .bubbleRing:
            let radius: Double = Double(max(rect.width, rect.height)) / 2 + 10
            if act.beat(0, at: 0, age: age) {
                for index in 0..<10 {
                    var bit: AquariumParticle = AquariumParticle(.bubble, at: CGPoint(x: rect.midX, y: rect.midY), life: 2, size: 3.5)
                    bit.center = CGPoint(x: rect.midX, y: rect.midY)
                    bit.radius = radius
                    bit.angle = Double(index) / 10 * Double.pi * 2
                    bit.spin = 1.8
                    add(bit)
                }
            }
            if act.beat(1, at: 2, age: age) {
                for index in 0..<6 {
                    let angle: Double = Double(index) / 6 * Double.pi * 2
                    emit(.sparkle, at: CGPoint(x: rect.midX + CGFloat(cos(angle) * radius), y: rect.midY + CGFloat(sin(angle) * radius)), life: 0.5, size: 5)
                }
            }
        case .measure:
            let width: Int = Int(rect.width.rounded())
            if act.beat(0, at: 0, age: age) { marks.append(AquariumMark(.tape("\(width) pt"), rect: rect, life: 2.8, tint: tint)) }
            if act.beat(1, at: 1.8, age: age) { say(&one, "\(width) points!", 1.4) }
        case .highlight:
            if act.beat(0, at: 0, age: age) { marks.append(AquariumMark(.highlight, rect: rect, life: 2.6, tint: tint)) }
        case .quote:
            if act.beat(0, at: 0.1, age: age) { say(&one, "“" + GenieScreenPlay.short(act.text) + "”", 2.4) }
        case .count:
            let letters: [Character] = AquariumTrick.letters(act.text)
            for index in letters.indices where act.beat(index, at: 0.3 + Double(index) * 0.18, age: age) {
                if !calm {
                    let x: CGFloat = rect.minX + rect.width * (CGFloat(index) + 0.5) / CGFloat(max(letters.count, 1))
                    emit(.glyph("\(index + 1)"), at: CGPoint(x: x, y: rect.minY - 4), velocity: CGSize(width: 0, height: -12), life: 0.9, size: 9)
                }
            }
            if act.beat(100, at: max(act.duration - 0.6, 0.4), age: age) {
                say(&one, letters.count == 1 ? "1 letter!" : "\(letters.count) letters!", 1.4)
            }
        case .startle:
            if age < 1 {
                one.nudge = CGSize(width: toward.width * 4 * age, height: toward.height * 4 * age)
            }
            if act.beat(0, at: 1, age: age) {
                say(&one, "!", 0.8)
                one.nudge = .zero
                let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
                act.away = AquariumClear.waypoint(in: water, body: body, blockers: blockers, water: water, random: random)
            }
            if age >= 1, let away = act.away {
                _ = steer(&one, to: away, speed: 230, dt: dt)
                if !calm, act.beat(Int(age * 10) + 10, at: 0, age: age) {
                    emit(.speed, at: one.point, velocity: CGSize(width: -one.velocity.width * 0.2, height: -one.velocity.height * 0.2), life: 0.4, size: 8)
                }
            }
        case .perch:
            one.nudge = CGSize(width: 0, height: 1.5 * sin(age * 1.6))
            for index in 0..<4 where act.beat(index, at: 0.4 + Double(index) * 0.7, age: age) {
                emit(.zzz, at: CGPoint(x: one.point.x, y: one.point.y - one.height / 2), velocity: CGSize(width: 6, height: -16), life: 1.4, size: 10)
            }
        case .trace:
            if act.beat(0, at: 0, age: age) { marks.append(AquariumMark(.trace, rect: rect, life: 2.8, tint: tint)) }
            for index in 0..<12 where act.beat(index + 1, at: Double(index) * 0.2, age: age) {
                emit(.bubble, at: perimeter(rect, Double(index) / 12), velocity: CGSize(width: 0, height: -6), life: 0.8, size: 3)
            }
        default:
            break
        }
    }

    private mutating func spiderAct(
        _ one: inout AquariumFish, _ act: inout AquariumAct, spider: AquariumSpider, age: Double, dt: Double, random: () -> Double,
        events: inout [AquariumEvent]
    ) {
        let rect: CGRect = spider.rect
        let center: CGPoint = spider.point
        let toward: CGSize = unit(from: one.point, to: center)
        let reach: CGFloat = max(hypot(center.x - one.point.x, center.y - one.point.y) - rect.width / 2 - one.length * 0.5, 0)
        one.facingLeft = center.x < one.point.x
        func ask(_ index: Int, _ at: Double, _ what: AquariumSpiderAsk) {
            if act.beat(index, at: at, age: age) { events.append(.spider(what)) }
        }
        switch act.trick {
        case .orbit, .tag, .race:
            break
        default:
            _ = steer(&one, to: act.spot, speed: 70, dt: dt)
        }
        switch act.trick {
        case .hello:
            if act.beat(0, at: 0, age: age) { say(&one, "Hi, spider!", 1.6) }
            ask(1, 0, .look(one.point))
            ask(2, 0.3, .wave)
            if act.beat(3, at: 0.4, age: age) { fly(.heart, from: one.point, to: center, life: 1.2, size: 12) }
        case .noseBoop:
            let push: Double = age < 0.55 ? age / 0.55 : max(0, 1 - (age - 0.55) / 0.5)
            one.nudge = CGSize(width: toward.width * reach * CGFloat(push), height: toward.height * reach * CGFloat(push))
            ask(0, 0, .look(one.point))
            if act.beat(1, at: 0.55, age: age) {
                emit(.sparkle, at: edge(of: rect, toward: one.point), life: 0.5, size: 6)
                events.append(.spider(.giggle))
            }
        case .tag:
            if act.beat(0, at: 0, age: age) {
                say(&one, "Tag! You're it!", 1.4)
                let body: (CGPoint) -> CGRect = { [one] in one.body(at: $0) }
                act.away = AquariumClear.waypoint(in: water, body: body, blockers: blockers, water: water, random: random)
            }
            ask(1, 0.35, .surprise)
            if let away = act.away {
                if age < 0.35 {
                    _ = steer(&one, to: act.spot, speed: 70, dt: dt)
                } else {
                    _ = steer(&one, to: away, speed: 200, dt: dt)
                }
                ask(2, 0.9, .walk(CGPoint(x: away.x, y: center.y)))
            }
            ask(3, 2.3, .giggle)
        case .orbit:
            let radius: Double = Double(spider.side) * 0.6 + Double(one.length) * 0.6
            if act.beat(0, at: 0, age: age) {
                act.angle = atan2(Double(one.point.y - center.y), Double(one.point.x - center.x))
                events.append(.spider(.look(one.point)))
            }
            let turn: Double = act.angle + Double.pi * 4 * min(age / act.duration, 1)
            let ring: CGPoint = CGPoint(x: center.x + CGFloat(cos(turn) * radius), y: center.y + CGFloat(sin(turn) * radius * 0.85))
            _ = steer(&one, to: clamp(ring), speed: 240, dt: dt)
            one.facingLeft = sin(turn) > 0
            ask(1, act.duration - 0.25, .dizzy)
        case .bubbleCrown:
            if act.beat(0, at: 0.1, age: age) { say(&one, "A crown for you!", 1.6) }
            if act.beat(1, at: 0.2, age: age) {
                for index in 0..<5 {
                    let offset: CGFloat = CGFloat(index - 2) * spider.side * 0.12
                    let lift: CGFloat = spider.side * (0.5 + 0.05 * CGFloat(2 - abs(index - 2)))
                    emit(.bubble, at: CGPoint(x: center.x + offset, y: center.y - lift), velocity: CGSize(width: 0, height: -3), life: 2.2, size: 5)
                }
            }
            ask(2, 0.9, .cheer)
        case .highFive:
            let push: Double = age < 0.5 ? age / 0.5 : max(0, 1 - (age - 0.5) / 0.5)
            one.nudge = CGSize(width: toward.width * reach * CGFloat(push), height: toward.height * reach * CGFloat(push))
            if act.beat(0, at: 0.5, age: age) {
                say(&one, "High five!", 1.2)
                for _ in 0..<5 {
                    emit(.star, at: edge(of: rect, toward: one.point), velocity: CGSize(width: (random() - 0.5) * 60, height: -20 - random() * 30), life: 0.8, size: 9)
                }
                events.append(.spider(.cheer))
            }
        case .race:
            if act.beat(0, at: 0, age: age) {
                say(&one, "Ready, set, go!", 1.2)
                let left: Bool = center.x > water.midX
                act.away = clamp(CGPoint(x: left ? water.minX + one.length : water.maxX - one.length, y: one.point.y))
            }
            if age < 1.2 {
                _ = steer(&one, to: act.spot, speed: 70, dt: dt)
            } else if let away = act.away {
                _ = steer(&one, to: away, speed: 170, dt: dt)
                ask(1, 1.2, .walk(CGPoint(x: away.x, y: center.y)))
                if !calm, act.beat(Int(age * 8) + 10, at: 0, age: age) {
                    emit(.speed, at: one.point, velocity: CGSize(width: -one.velocity.width * 0.2, height: 0), life: 0.4, size: 8)
                }
            }
            ask(2, 3.3, .cheer)
        case .serenade:
            if act.beat(0, at: 0, age: age) { say(&one, "♪ La la la ♪", 1.8) }
            for index in 0..<7 where act.beat(index + 1, at: 0.1 + Double(index) * 0.3, age: age) {
                fly(.note, from: one.point, to: center, life: 1.1, size: 12)
            }
            ask(20, 0.8, .nod)
            ask(21, 1.8, .nod)
        case .legCount:
            for count in 1...8 where act.beat(count, at: 0.2 + Double(count - 1) * 0.3, age: age) {
                say(&one, "\(count)", 0.3)
            }
            if act.beat(20, at: 2.7, age: age) {
                say(&one, "Eight legs!", 0.9)
                events.append(.spider(.nod))
            }
        case .photo:
            if act.beat(0, at: 0, age: age) { say(&one, "Say cheese!", 1.1) }
            if act.beat(1, at: 1.2, age: age) {
                emit(.flash, at: center, life: 0.4, size: Double(spider.side))
                emit(.photo, at: CGPoint(x: center.x, y: center.y - spider.side * 0.4), velocity: CGSize(width: 0, height: -30), life: 1.4, size: 22)
                events.append(.spider(.wave))
            }
        case .kiss:
            let push: Double = age < 0.5 ? age / 0.5 : max(0, 1 - (age - 0.5) / 0.4)
            one.nudge = CGSize(width: toward.width * reach * CGFloat(push), height: toward.height * reach * CGFloat(push))
            if act.beat(0, at: 0.5, age: age) {
                say(&one, "Mwah!", 1)
                for _ in 0..<3 {
                    emit(.heart, at: edge(of: rect, toward: one.point), velocity: CGSize(width: (random() - 0.5) * 30, height: -24), life: 1.1, size: 11)
                }
                events.append(.spider(.cheer))
            }
        default:
            break
        }
    }

    private mutating func steer(_ one: inout AquariumFish, to target: CGPoint, speed: Double, dt: Double) -> Bool {
        let dx: Double = Double(target.x - one.point.x)
        let dy: Double = Double(target.y - one.point.y)
        let distance: Double = hypot(dx, dy)
        let pace: Double = calm ? AquariumTank.calmPace : 1
        let want: Double = min(speed * pace, distance * 2.4)
        let wantX: Double = distance > 0.01 ? dx / distance * want : 0
        let wantY: Double = distance > 0.01 ? dy / distance * want : 0
        let blend: Double = min(1, dt * 4)
        let vx: Double = Double(one.velocity.width) + (wantX - Double(one.velocity.width)) * blend
        let vy: Double = Double(one.velocity.height) + (wantY - Double(one.velocity.height)) * blend
        one.velocity = CGSize(width: vx, height: vy)
        one.point = CGPoint(x: one.point.x + CGFloat(vx * dt), y: one.point.y + CGFloat(vy * dt))
        if abs(vx) > 4 { one.facingLeft = vx < 0 }
        one.swim += dt * (2 + hypot(vx, vy) / 18) * pace
        return distance < 4
    }

    private mutating func say(_ one: inout AquariumFish, _ text: String, _ seconds: Double) {
        one.speech = text
        one.speechUntil = time + seconds
    }

    private func clamp(_ point: CGPoint) -> CGPoint {
        CGPoint(x: min(max(point.x, water.minX + 20), water.maxX - 20), y: min(max(point.y, water.minY + 20), water.maxY - 24))
    }

    private func range(_ span: ClosedRange<Double>, _ random: () -> Double) -> Double {
        span.lowerBound + (span.upperBound - span.lowerBound) * random()
    }

    private func unit(from a: CGPoint, to b: CGPoint) -> CGSize {
        let dx: CGFloat = b.x - a.x
        let dy: CGFloat = b.y - a.y
        let length: CGFloat = max(hypot(dx, dy), 0.001)
        return CGSize(width: dx / length, height: dy / length)
    }

    private func edge(of rect: CGRect, toward point: CGPoint) -> CGPoint {
        CGPoint(x: min(max(point.x, rect.minX), rect.maxX), y: min(max(point.y, rect.minY), rect.maxY))
    }

    private func perimeter(_ rect: CGRect, _ share: Double) -> CGPoint {
        let total: Double = Double(rect.width + rect.height) * 2
        var along: Double = (share - share.rounded(.down)) * total
        let w: Double = Double(rect.width)
        let h: Double = Double(rect.height)
        if along < w { return CGPoint(x: rect.minX + CGFloat(along), y: rect.minY) }
        along -= w
        if along < h { return CGPoint(x: rect.maxX, y: rect.minY + CGFloat(along)) }
        along -= h
        if along < w { return CGPoint(x: rect.maxX - CGFloat(along), y: rect.maxY) }
        along -= w
        return CGPoint(x: rect.minX, y: rect.maxY - CGFloat(min(along, h)))
    }

    private mutating func emit(_ bit: AquariumBit, at point: CGPoint, velocity: CGSize = .zero, life: Double, size: Double) {
        guard !calm else { return }
        add(AquariumParticle(bit, at: point, velocity: velocity, life: life, size: size))
    }

    private mutating func fly(_ bit: AquariumBit, from: CGPoint, to: CGPoint, life: Double, size: Double) {
        emit(bit, at: from, velocity: CGSize(width: (to.x - from.x) / CGFloat(life), height: (to.y - from.y) / CGFloat(life)), life: life, size: size)
    }

    private mutating func add(_ particle: AquariumParticle) {
        guard !calm, particles.count < AquariumTank.mostParticles else { return }
        particles.append(particle)
    }

    private mutating func splash(at point: CGPoint) {
        for index in 0..<6 {
            let angle: Double = Double.pi * (1.1 + 0.8 * Double(index) / 5)
            emit(.splash, at: point, velocity: CGSize(width: cos(angle) * 50, height: sin(angle) * 50), life: 0.6, size: 3)
        }
    }

    private mutating func ambient(_ random: () -> Double) {
        guard time >= nextBubble else { return }
        nextBubble = time + 0.6 + random() * 0.9
        let rise: Double = 16 + random() * 14
        var bubble: AquariumParticle = AquariumParticle(
            .bubble, at: CGPoint(x: water.minX + water.width * CGFloat(random()), y: water.maxY),
            velocity: CGSize(width: 0, height: -rise), life: Double(water.height) / rise, size: 2 + random() * 3)
        bubble.angle = random() * Double.pi * 2
        add(bubble)
    }

    private mutating func advanceParticles(_ dt: Double) {
        for index in particles.indices {
            var bit: AquariumParticle = particles[index]
            bit.age += dt
            if let center = bit.center {
                bit.angle += bit.spin * dt
                bit.point = CGPoint(x: center.x + CGFloat(cos(bit.angle) * bit.radius), y: center.y + CGFloat(sin(bit.angle) * bit.radius * 0.9))
            } else {
                let sway: CGFloat = bit.bit == .bubble ? CGFloat(sin(bit.age * 3 + bit.angle) * 6 * dt) : 0
                bit.point = CGPoint(x: bit.point.x + bit.velocity.width * CGFloat(dt) + sway, y: bit.point.y + bit.velocity.height * CGFloat(dt))
            }
            particles[index] = bit
        }
        particles.removeAll { $0.age >= $0.life }
    }
}
