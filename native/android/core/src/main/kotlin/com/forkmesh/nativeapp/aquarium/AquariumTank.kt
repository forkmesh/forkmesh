package com.forkmesh.nativeapp.aquarium

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

class AquariumTank(screen: AquariumRect, top: Double, floor: Double) {
    var screen: AquariumRect = screen
        private set
    var water: AquariumRect = water(screen, top, floor)
        private set
    private val fishList: MutableList<AquariumFish> = ArrayList()
    private val particleList: MutableList<AquariumParticle> = ArrayList()
    private val markList: MutableList<AquariumMark> = ArrayList()
    val fish: List<AquariumFish> get() = fishList
    val particles: List<AquariumParticle> get() = particleList
    val marks: List<AquariumMark> get() = markList
    var time: Double = 0.0
        private set
    var landmarks: List<GenieLandmark> = emptyList()
        private set
    var blockers: List<AquariumRect> = emptyList()
        private set
    private val elementBag = GenieBag(AquariumTrick.element)
    private val spiderBag = GenieBag(AquariumTrick.spider)
    private var spiderRestUntil: Double = 0.0
    var spider: AquariumSpider? = null
    var calm: Boolean = false
    private var nextBubble: Double = 0.0

    val spiderPlaying: Boolean
        get() = fishList.any { (it.phase == AquariumPhase.GOING || it.phase == AquariumPhase.ACTING) && it.act?.trick?.withSpider == true }

    fun resize(next: AquariumRect, top: Double, floor: Double) {
        screen = next
        water = water(next, top, floor)
        for (one in fishList) {
            if (one.phase != AquariumPhase.LEAVING && one.phase != AquariumPhase.ENTERING) {
                one.point = clamp(one.point)
                one.goal = clamp(one.goal)
            }
        }
    }

    fun see(marks: List<GenieLandmark>) {
        landmarks = marks
        blockers = GenieLandmarks.blockers(marks)
    }

    fun roster(swimmers: List<AquariumSwimmer>, random: () -> Double): List<AquariumEvent> {
        val wanted = if (swimmers.isEmpty()) GUESTS else swimmers.take(MOST)
        val ids = wanted.map { it.id }.toSet()
        val events = ArrayList<AquariumEvent>()
        for (one in fishList) {
            if (one.id !in ids && one.phase != AquariumPhase.LEAVING) {
                one.phase = AquariumPhase.LEAVING
                one.act = null
                one.nudge = AquariumSize.ZERO
                one.goal = AquariumPoint(one.point.x, screen.minY - 80)
                say(one, "Bye!", 1.6)
                events.add(AquariumEvent.Log("${one.name} swims off"))
            }
        }
        for (swimmer in wanted) {
            val known = fishList.firstOrNull { it.id == swimmer.id && it.phase != AquariumPhase.LEAVING }
            if (known != null) {
                known.name = swimmer.name
                if (known.waiting != swimmer.waiting) {
                    known.waiting = swimmer.waiting
                    if (swimmer.waiting) {
                        drop(known, random)
                        known.nextCall = time + 0.8
                        known.restUntil = time
                    }
                }
                continue
            }
            val x = water.minX + water.width * (0.1 + 0.8 * random())
            val made = AquariumFish(swimmer.id, swimmer.name, swimmer.model, swimmer.waiting, AquariumPoint(x, screen.minY - 30))
            val upper = AquariumRect(water.minX, water.minY, water.width, water.height * 0.5)
            made.goal = AquariumClear.waypoint(upper, { made.body(it) }, blockers, water, random)
            made.nextCall = time + 1.5
            fishList.add(made)
            splash(AquariumPoint(x, water.minY))
            events.add(AquariumEvent.Log("${swimmer.name} drops in"))
        }
        return events
    }

    fun step(seconds: Double, random: () -> Double): List<AquariumEvent> {
        val dt = min(max(seconds, 0.0), 0.1)
        time += dt
        val events = ArrayList<AquariumEvent>()
        if (calm) {
            particleList.clear()
        } else {
            ambient(random)
        }
        for (one in fishList) move(one, dt, random, events)
        fishList.removeAll { it.phase == AquariumPhase.LEAVING && it.point.y < screen.minY - 40 }
        advanceParticles(dt)
        for (mark in markList) mark.age += dt
        markList.removeAll { it.age >= it.life }
        return events
    }

    private fun move(one: AquariumFish, dt: Double, random: () -> Double, events: MutableList<AquariumEvent>) {
        one.appear = min(1.0, one.appear + dt / 0.6)
        when (one.phase) {
            AquariumPhase.ENTERING -> if (steer(one, one.goal, 95.0, dt)) {
                one.phase = AquariumPhase.WANDERING
                one.restUntil = time + range(WANDER, random)
            }
            AquariumPhase.WANDERING -> wander(one, dt, random)
            AquariumPhase.GOING -> go(one, dt, random, events)
            AquariumPhase.ACTING -> perform(one, dt, random, events)
            AquariumPhase.LEAVING -> steer(one, one.goal, 150.0, dt)
        }
        val speech = one.speech
        if (speech != null && time < one.speechUntil) {
            one.speechBox = AquariumClear.speech(speech, one.box, blockers, screen)
        } else {
            one.speech = null
            one.speechBox = null
        }
        val target = AquariumClear.opacity(one.box, blockers)
        one.opacity += (target - one.opacity) * min(1.0, dt * 6)
    }

    private fun wander(one: AquariumFish, dt: Double, random: () -> Double) {
        val body: (AquariumPoint) -> AquariumRect = { one.body(it) }
        if (one.waiting) {
            val band = AquariumRect(water.minX, water.minY, water.width, min(150.0, water.height * 0.3))
            val arrived = steer(one, one.goal, 50.0, dt)
            if (arrived && (time >= one.restUntil || !band.insetBy(-1.0, -1.0).contains(one.goal))) {
                one.goal = AquariumClear.waypoint(band, body, blockers, water, random)
                one.restUntil = time + range(3.0..6.0, random)
            }
            if (time >= one.nextCall) {
                say(one, "I need you!", 2.2)
                one.nextCall = time + range(CALL_EVERY, random)
            }
            return
        }
        val arrived = steer(one, one.goal, 55.0, dt)
        if (!arrived || time < one.restUntil) return
        if (start(one, random)) return
        one.goal = AquariumClear.waypoint(water, body, blockers, water, random)
        one.restUntil = time + range(WANDER, random)
    }

    private fun start(one: AquariumFish, random: () -> Double): Boolean {
        val spider = spider
        if (spider != null && spider.free && !spiderPlaying && time >= spiderRestUntil && random() < SPIDER_ODDS) {
            val trick = spiderBag.next(random)
            if (trick != null) {
                val spot = spiderSpot(one, spider)
                if (spot != null) {
                    begin(one, AquariumAct(trick, spider.rect, "", spot))
                    return true
                }
            }
        }
        val taken = fishList.mapNotNull { if (it.id == one.id) null else it.act?.target }
        val usable = landmarks.filter { mark ->
            !taken.contains(mark.rect) && AquariumClear.fits(mark.rect.insetBy(1.0, 1.0), screen)
        }
        val trick = elementBag.next(random) { candidate -> usable.any { candidate.fits(it) } } ?: return false
        val here = one.point
        val scored = usable.filter { trick.fits(it) }.map { mark ->
            mark to hypot(mark.rect.midX - here.x, mark.rect.midY - here.y) * (0.4 + random())
        }
        val others = fishList.filter { it.id != one.id && it.act != null }.map { it.box }
        val body: (AquariumPoint) -> AquariumRect = { one.body(it) }
        for ((mark, _) in scored.sortedBy { it.second }.take(4)) {
            val spot = AquariumClear.spot(mark.rect, body, blockers + others, water, here)
            if (spot != null) {
                begin(one, AquariumAct(trick, mark.rect, mark.text, spot))
                return true
            }
        }
        return false
    }

    private fun begin(one: AquariumFish, act: AquariumAct) {
        one.act = act
        one.phase = AquariumPhase.GOING
        one.deadline = time + REACH
        one.nudge = AquariumSize.ZERO
    }

    private fun spiderSpot(one: AquariumFish, spider: AquariumSpider): AquariumPoint? {
        val sample = one.body(AquariumPoint.ZERO)
        val rect = spider.rect
        val gap = 6.0
        val options = listOf(
            AquariumPoint(rect.minX - gap - sample.maxX, rect.midY - rect.height * 0.1),
            AquariumPoint(rect.maxX + gap - sample.minX, rect.midY - rect.height * 0.1),
            AquariumPoint(rect.midX, rect.minY - gap - sample.maxY),
            AquariumPoint(rect.midX, rect.maxY + gap - sample.minY),
        )
        return options.filter { AquariumClear.clear(one.body(it), blockers, water, AquariumClear.ROOM) }
            .minByOrNull { hypot(it.x - one.point.x, it.y - one.point.y) }
    }

    private fun go(one: AquariumFish, dt: Double, random: () -> Double, events: MutableList<AquariumEvent>) {
        val act = one.act
        if (act == null) {
            one.phase = AquariumPhase.WANDERING
            return
        }
        if (act.trick.withSpider) {
            val spider = spider
            val spot = if (spider != null && spider.free) spiderSpot(one, spider) else null
            if (spider == null || spot == null) {
                drop(one, random)
                return
            }
            act.spot = spot
            act.target = spider.rect
        }
        val arrived = steer(one, act.spot, 110.0, dt)
        if (arrived) {
            act.start = time
            one.phase = AquariumPhase.ACTING
            events.add(AquariumEvent.Log("${one.name} ${act.trick.title}" + (if (act.text.isEmpty()) "" else " “${GenieLandmarks.short(act.text)}”")))
        } else if (time > one.deadline) {
            drop(one, random)
        }
    }

    private fun drop(one: AquariumFish, random: () -> Double) {
        if (one.act?.trick?.withSpider == true && one.phase == AquariumPhase.ACTING) {
            spiderRestUntil = time + 2.5
        }
        one.act = null
        one.nudge = AquariumSize.ZERO
        one.phase = AquariumPhase.WANDERING
        one.goal = if (AquariumClear.clear(one.box, blockers, water)) one.point
        else AquariumClear.waypoint(water, { one.body(it) }, blockers, water, random)
        one.restUntil = time + 1
    }

    private fun finish(one: AquariumFish, random: () -> Double) {
        val away = one.act?.away
        if (one.act?.trick?.withSpider == true) {
            spiderRestUntil = time + range(SPIDER_REST, random)
        }
        one.act = null
        one.nudge = AquariumSize.ZERO
        one.phase = AquariumPhase.WANDERING
        val body: (AquariumPoint) -> AquariumRect = { one.body(it) }
        one.goal = if (away != null && AquariumClear.clear(body(away), blockers, water)) away
        else if (AquariumClear.clear(one.box, blockers, water)) one.point
        else AquariumClear.waypoint(water, body, blockers, water, random)
        one.restUntil = time + range(WANDER, random)
    }

    private fun perform(one: AquariumFish, dt: Double, random: () -> Double, events: MutableList<AquariumEvent>) {
        val act = one.act
        if (act == null) {
            one.phase = AquariumPhase.WANDERING
            return
        }
        val age = time - act.start
        if (act.trick.withSpider) {
            val spider = spider
            if (spider == null || !spider.free) {
                finish(one, random)
                return
            }
            act.target = spider.rect
            spiderAct(one, act, spider, age, dt, random, events)
        } else {
            elementAct(one, act, age, dt, random)
        }
        if (age >= act.duration) finish(one, random)
    }

    private fun elementAct(one: AquariumFish, act: AquariumAct, age: Double, dt: Double, random: () -> Double) {
        val rect = act.target
        val toward = unit(act.spot, AquariumPoint(rect.midX, rect.midY))
        one.facingLeft = rect.midX < act.spot.x
        if (act.trick != AquariumTrick.STARTLE || age < 1) steer(one, act.spot, 60.0, dt)
        val tint = one.look.body
        when (act.trick) {
            AquariumTrick.PEEK -> {
                one.nudge = AquariumSize(toward.width * 3 * sin(age * 2), 1.5 * sin(age * 5))
                if (act.beat(0, 1.4, age)) say(one, "!?", 1.0)
            }
            AquariumTrick.BOOP -> {
                val push = max(sin(age * PI / 0.6), 0.0)
                one.nudge = AquariumSize(toward.width * 5 * push, toward.height * 5 * push)
                for ((index, at) in listOf(0.3, 0.9, 1.5).withIndex()) {
                    if (act.beat(index, at, age)) {
                        markList.add(AquariumMark(AquariumMarkKind.Ring, rect, 0.5, tint))
                        emit(AquariumBit.Sparkle, edge(rect, one.point), life = 0.5, size = 5.0)
                    }
                }
            }
            AquariumTrick.READ -> {
                val letters = act.text.take(24)
                if (calm) {
                    if (act.beat(0, 0.0, age)) say(one, "“" + GenieLandmarks.short(act.text) + "”", act.duration)
                } else {
                    for ((index, letter) in letters.withIndex()) {
                        if (!letter.isWhitespace() && act.beat(index, 0.3 + index * 0.14, age)) {
                            val x = rect.minX + rect.width * (index + 0.5) / max(letters.length, 1)
                            emit(AquariumBit.Glyph(letter.toString()), AquariumPoint(x, rect.minY - 2), AquariumSize(0.0, -26.0), 1.6, 11.0)
                        }
                    }
                }
            }
            AquariumTrick.UNDERLINE -> {
                if (act.beat(0, 0.0, age)) markList.add(AquariumMark(AquariumMarkKind.Underline, rect, 2.4, tint))
                for (index in 0 until 5) {
                    if (act.beat(index + 1, 0.2 + index * 0.3, age)) {
                        val x = rect.minX + rect.width * index / 4
                        emit(AquariumBit.Bubble, AquariumPoint(x, rect.maxY + 3), AquariumSize(0.0, -10.0), 1.0, 3.0)
                    }
                }
            }
            AquariumTrick.POLISH -> {
                one.nudge = AquariumSize(-toward.height * 3 * sin(age * 14), toward.width * 3 * sin(age * 14))
                for (index in 0 until 9) {
                    if (act.beat(index, index * 0.2, age)) emit(AquariumBit.Sparkle, perimeter(rect, random()), life = 0.7, size = 5.0)
                }
                if (act.beat(20, 2.0, age)) markList.add(AquariumMark(AquariumMarkKind.Ring, rect, 0.8, AquariumColor.GOLD))
            }
            AquariumTrick.BUBBLE_RING -> {
                val radius = max(rect.width, rect.height) / 2 + 10
                if (act.beat(0, 0.0, age)) {
                    for (index in 0 until 10) {
                        val bit = AquariumParticle(AquariumBit.Bubble, AquariumPoint(rect.midX, rect.midY), life = 2.0, size = 3.5)
                        bit.center = AquariumPoint(rect.midX, rect.midY)
                        bit.radius = radius
                        bit.angle = index / 10.0 * PI * 2
                        bit.spin = 1.8
                        add(bit)
                    }
                }
                if (act.beat(1, 2.0, age)) {
                    for (index in 0 until 6) {
                        val angle = index / 6.0 * PI * 2
                        emit(AquariumBit.Sparkle, AquariumPoint(rect.midX + cos(angle) * radius, rect.midY + sin(angle) * radius), life = 0.5, size = 5.0)
                    }
                }
            }
            AquariumTrick.MEASURE -> {
                val width = rect.width.roundToInt()
                if (act.beat(0, 0.0, age)) markList.add(AquariumMark(AquariumMarkKind.Tape("$width pt"), rect, 2.8, tint))
                if (act.beat(1, 1.8, age)) say(one, "$width points!", 1.4)
            }
            AquariumTrick.HIGHLIGHT -> {
                if (act.beat(0, 0.0, age)) markList.add(AquariumMark(AquariumMarkKind.Highlight, rect, 2.6, tint))
            }
            AquariumTrick.QUOTE -> {
                if (act.beat(0, 0.1, age)) say(one, "“" + GenieLandmarks.short(act.text) + "”", 2.4)
            }
            AquariumTrick.COUNT -> {
                val letters = AquariumTrick.letters(act.text)
                for (index in letters.indices) {
                    if (act.beat(index, 0.3 + index * 0.18, age) && !calm) {
                        val x = rect.minX + rect.width * (index + 0.5) / max(letters.size, 1)
                        emit(AquariumBit.Glyph("${index + 1}"), AquariumPoint(x, rect.minY - 4), AquariumSize(0.0, -12.0), 0.9, 9.0)
                    }
                }
                if (act.beat(100, max(act.duration - 0.6, 0.4), age)) {
                    say(one, if (letters.size == 1) "1 letter!" else "${letters.size} letters!", 1.4)
                }
            }
            AquariumTrick.STARTLE -> {
                if (age < 1) {
                    one.nudge = AquariumSize(toward.width * 4 * age, toward.height * 4 * age)
                }
                if (act.beat(0, 1.0, age)) {
                    say(one, "!", 0.8)
                    one.nudge = AquariumSize.ZERO
                    act.away = AquariumClear.waypoint(water, { one.body(it) }, blockers, water, random)
                }
                val away = act.away
                if (age >= 1 && away != null) {
                    steer(one, away, 230.0, dt)
                    if (!calm && act.beat((age * 10).toInt() + 10, 0.0, age)) {
                        emit(AquariumBit.Speed, one.point, AquariumSize(-one.velocity.width * 0.2, -one.velocity.height * 0.2), 0.4, 8.0)
                    }
                }
            }
            AquariumTrick.PERCH -> {
                one.nudge = AquariumSize(0.0, 1.5 * sin(age * 1.6))
                for (index in 0 until 4) {
                    if (act.beat(index, 0.4 + index * 0.7, age)) {
                        emit(AquariumBit.Zzz, AquariumPoint(one.point.x, one.point.y - one.height / 2), AquariumSize(6.0, -16.0), 1.4, 10.0)
                    }
                }
            }
            AquariumTrick.TRACE -> {
                if (act.beat(0, 0.0, age)) markList.add(AquariumMark(AquariumMarkKind.Trace, rect, 2.8, tint))
                for (index in 0 until 12) {
                    if (act.beat(index + 1, index * 0.2, age)) {
                        emit(AquariumBit.Bubble, perimeter(rect, index / 12.0), AquariumSize(0.0, -6.0), 0.8, 3.0)
                    }
                }
            }
            else -> Unit
        }
    }

    private fun spiderAct(
        one: AquariumFish,
        act: AquariumAct,
        spider: AquariumSpider,
        age: Double,
        dt: Double,
        random: () -> Double,
        events: MutableList<AquariumEvent>,
    ) {
        val rect = spider.rect
        val center = spider.point
        val toward = unit(one.point, center)
        val reach = max(hypot(center.x - one.point.x, center.y - one.point.y) - rect.width / 2 - one.length * 0.5, 0.0)
        one.facingLeft = center.x < one.point.x
        fun ask(index: Int, at: Double, what: AquariumSpiderAsk) {
            if (act.beat(index, at, age)) events.add(AquariumEvent.Spider(what))
        }
        when (act.trick) {
            AquariumTrick.ORBIT, AquariumTrick.TAG, AquariumTrick.RACE -> Unit
            else -> steer(one, act.spot, 70.0, dt)
        }
        when (act.trick) {
            AquariumTrick.HELLO -> {
                if (act.beat(0, 0.0, age)) say(one, "Hi, spider!", 1.6)
                ask(1, 0.0, AquariumSpiderAsk.Look(one.point))
                ask(2, 0.3, AquariumSpiderAsk.Wave)
                if (act.beat(3, 0.4, age)) fly(AquariumBit.Heart, one.point, center, 1.2, 12.0)
            }
            AquariumTrick.NOSE_BOOP -> {
                val push = if (age < 0.55) age / 0.55 else max(0.0, 1 - (age - 0.55) / 0.5)
                one.nudge = AquariumSize(toward.width * reach * push, toward.height * reach * push)
                ask(0, 0.0, AquariumSpiderAsk.Look(one.point))
                if (act.beat(1, 0.55, age)) {
                    emit(AquariumBit.Sparkle, edge(rect, one.point), life = 0.5, size = 6.0)
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Giggle))
                }
            }
            AquariumTrick.TAG -> {
                if (act.beat(0, 0.0, age)) {
                    say(one, "Tag! You're it!", 1.4)
                    act.away = AquariumClear.waypoint(water, { one.body(it) }, blockers, water, random)
                }
                ask(1, 0.35, AquariumSpiderAsk.Surprise)
                val away = act.away
                if (away != null) {
                    if (age < 0.35) {
                        steer(one, act.spot, 70.0, dt)
                    } else {
                        steer(one, away, 200.0, dt)
                    }
                    ask(2, 0.9, AquariumSpiderAsk.Walk(AquariumPoint(away.x, center.y)))
                }
                ask(3, 2.3, AquariumSpiderAsk.Giggle)
            }
            AquariumTrick.ORBIT -> {
                val radius = spider.side * 0.6 + one.length * 0.6
                if (act.beat(0, 0.0, age)) {
                    act.angle = atan2(one.point.y - center.y, one.point.x - center.x)
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Look(one.point)))
                }
                val turn = act.angle + PI * 4 * min(age / act.duration, 1.0)
                val ring = AquariumPoint(center.x + cos(turn) * radius, center.y + sin(turn) * radius * 0.85)
                steer(one, clamp(ring), 240.0, dt)
                one.facingLeft = sin(turn) > 0
                ask(1, act.duration - 0.25, AquariumSpiderAsk.Dizzy)
            }
            AquariumTrick.BUBBLE_CROWN -> {
                if (act.beat(0, 0.1, age)) say(one, "A crown for you!", 1.6)
                if (act.beat(1, 0.2, age)) {
                    for (index in 0 until 5) {
                        val offset = (index - 2) * spider.side * 0.12
                        val lift = spider.side * (0.5 + 0.05 * (2 - abs(index - 2)))
                        emit(AquariumBit.Bubble, AquariumPoint(center.x + offset, center.y - lift), AquariumSize(0.0, -3.0), 2.2, 5.0)
                    }
                }
                ask(2, 0.9, AquariumSpiderAsk.Cheer)
            }
            AquariumTrick.HIGH_FIVE -> {
                val push = if (age < 0.5) age / 0.5 else max(0.0, 1 - (age - 0.5) / 0.5)
                one.nudge = AquariumSize(toward.width * reach * push, toward.height * reach * push)
                if (act.beat(0, 0.5, age)) {
                    say(one, "High five!", 1.2)
                    repeat(5) {
                        emit(AquariumBit.Star, edge(rect, one.point), AquariumSize((random() - 0.5) * 60, -20 - random() * 30), 0.8, 9.0)
                    }
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Cheer))
                }
            }
            AquariumTrick.RACE -> {
                if (act.beat(0, 0.0, age)) {
                    say(one, "Ready, set, go!", 1.2)
                    val left = center.x > water.midX
                    act.away = clamp(AquariumPoint(if (left) water.minX + one.length else water.maxX - one.length, one.point.y))
                }
                val away = act.away
                if (age < 1.2) {
                    steer(one, act.spot, 70.0, dt)
                } else if (away != null) {
                    steer(one, away, 170.0, dt)
                    ask(1, 1.2, AquariumSpiderAsk.Walk(AquariumPoint(away.x, center.y)))
                    if (!calm && act.beat((age * 8).toInt() + 10, 0.0, age)) {
                        emit(AquariumBit.Speed, one.point, AquariumSize(-one.velocity.width * 0.2, 0.0), 0.4, 8.0)
                    }
                }
                ask(2, 3.3, AquariumSpiderAsk.Cheer)
            }
            AquariumTrick.SERENADE -> {
                if (act.beat(0, 0.0, age)) say(one, "♪ La la la ♪", 1.8)
                for (index in 0 until 7) {
                    if (act.beat(index + 1, 0.1 + index * 0.3, age)) fly(AquariumBit.Note, one.point, center, 1.1, 12.0)
                }
                ask(20, 0.8, AquariumSpiderAsk.Nod)
                ask(21, 1.8, AquariumSpiderAsk.Nod)
            }
            AquariumTrick.LEG_COUNT -> {
                for (count in 1..8) {
                    if (act.beat(count, 0.2 + (count - 1) * 0.3, age)) say(one, "$count", 0.3)
                }
                if (act.beat(20, 2.7, age)) {
                    say(one, "Eight legs!", 0.9)
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Nod))
                }
            }
            AquariumTrick.PHOTO -> {
                if (act.beat(0, 0.0, age)) say(one, "Say cheese!", 1.1)
                if (act.beat(1, 1.2, age)) {
                    emit(AquariumBit.Flash, center, life = 0.4, size = spider.side)
                    emit(AquariumBit.Photo, AquariumPoint(center.x, center.y - spider.side * 0.4), AquariumSize(0.0, -30.0), 1.4, 22.0)
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Wave))
                }
            }
            AquariumTrick.KISS -> {
                val push = if (age < 0.5) age / 0.5 else max(0.0, 1 - (age - 0.5) / 0.4)
                one.nudge = AquariumSize(toward.width * reach * push, toward.height * reach * push)
                if (act.beat(0, 0.5, age)) {
                    say(one, "Mwah!", 1.0)
                    repeat(3) {
                        emit(AquariumBit.Heart, edge(rect, one.point), AquariumSize((random() - 0.5) * 30, -24.0), 1.1, 11.0)
                    }
                    events.add(AquariumEvent.Spider(AquariumSpiderAsk.Cheer))
                }
            }
            else -> Unit
        }
    }

    private fun steer(one: AquariumFish, target: AquariumPoint, speed: Double, dt: Double): Boolean {
        val dx = target.x - one.point.x
        val dy = target.y - one.point.y
        val distance = hypot(dx, dy)
        val pace = if (calm) CALM_PACE else 1.0
        val want = min(speed * pace, distance * 2.4)
        val wantX = if (distance > 0.01) dx / distance * want else 0.0
        val wantY = if (distance > 0.01) dy / distance * want else 0.0
        val blend = min(1.0, dt * 4)
        val vx = one.velocity.width + (wantX - one.velocity.width) * blend
        val vy = one.velocity.height + (wantY - one.velocity.height) * blend
        one.velocity = AquariumSize(vx, vy)
        one.point = AquariumPoint(one.point.x + vx * dt, one.point.y + vy * dt)
        if (abs(vx) > 4) one.facingLeft = vx < 0
        one.swim += dt * (2 + hypot(vx, vy) / 18) * pace
        return distance < 4
    }

    private fun say(one: AquariumFish, text: String, seconds: Double) {
        one.speech = text
        one.speechUntil = time + seconds
    }

    private fun clamp(point: AquariumPoint): AquariumPoint = AquariumPoint(
        min(max(point.x, water.minX + 20), water.maxX - 20),
        min(max(point.y, water.minY + 20), water.maxY - 24),
    )

    private fun range(span: ClosedFloatingPointRange<Double>, random: () -> Double): Double =
        span.start + (span.endInclusive - span.start) * random()

    private fun unit(a: AquariumPoint, b: AquariumPoint): AquariumSize {
        val dx = b.x - a.x
        val dy = b.y - a.y
        val length = max(hypot(dx, dy), 0.001)
        return AquariumSize(dx / length, dy / length)
    }

    private fun edge(rect: AquariumRect, toward: AquariumPoint): AquariumPoint =
        AquariumPoint(min(max(toward.x, rect.minX), rect.maxX), min(max(toward.y, rect.minY), rect.maxY))

    private fun perimeter(rect: AquariumRect, share: Double): AquariumPoint {
        val total = (rect.width + rect.height) * 2
        var along = (share - Math.floor(share)) * total
        val w = rect.width
        val h = rect.height
        if (along < w) return AquariumPoint(rect.minX + along, rect.minY)
        along -= w
        if (along < h) return AquariumPoint(rect.maxX, rect.minY + along)
        along -= h
        if (along < w) return AquariumPoint(rect.maxX - along, rect.maxY)
        along -= w
        return AquariumPoint(rect.minX, rect.maxY - min(along, h))
    }

    private fun emit(bit: AquariumBit, at: AquariumPoint, velocity: AquariumSize = AquariumSize.ZERO, life: Double, size: Double) {
        if (calm) return
        add(AquariumParticle(bit, at, velocity, life, size))
    }

    private fun fly(bit: AquariumBit, from: AquariumPoint, to: AquariumPoint, life: Double, size: Double) {
        emit(bit, from, AquariumSize((to.x - from.x) / life, (to.y - from.y) / life), life, size)
    }

    private fun add(particle: AquariumParticle) {
        if (calm || particleList.size >= MOST_PARTICLES) return
        particleList.add(particle)
    }

    private fun splash(at: AquariumPoint) {
        for (index in 0 until 6) {
            val angle = PI * (1.1 + 0.8 * index / 5)
            emit(AquariumBit.Splash, at, AquariumSize(cos(angle) * 50, sin(angle) * 50), 0.6, 3.0)
        }
    }

    private fun ambient(random: () -> Double) {
        if (time < nextBubble) return
        nextBubble = time + 0.6 + random() * 0.9
        val rise = 16 + random() * 14
        val bubble = AquariumParticle(
            AquariumBit.Bubble,
            AquariumPoint(water.minX + water.width * random(), water.maxY),
            AquariumSize(0.0, -rise),
            water.height / rise,
            2 + random() * 3,
        )
        bubble.angle = random() * PI * 2
        add(bubble)
    }

    private fun advanceParticles(dt: Double) {
        for (bit in particleList) {
            bit.age += dt
            val center = bit.center
            if (center != null) {
                bit.angle += bit.spin * dt
                bit.point = AquariumPoint(center.x + cos(bit.angle) * bit.radius, center.y + sin(bit.angle) * bit.radius * 0.9)
            } else {
                val sway = if (bit.bit == AquariumBit.Bubble) sin(bit.age * 3 + bit.angle) * 6 * dt else 0.0
                bit.point = AquariumPoint(bit.point.x + bit.velocity.width * dt + sway, bit.point.y + bit.velocity.height * dt)
            }
        }
        particleList.removeAll { it.age >= it.life }
    }

    companion object {
        const val MOST = 12
        val GUESTS = listOf(
            AquariumSwimmer("guest-bubbles", "Bubbles", "guest"),
            AquariumSwimmer("guest-finn", "Finn", "guest"),
            AquariumSwimmer("guest-coral", "Coral", "guest"),
        )
        const val SPIDER_ODDS = 0.45
        val WANDER = 2.0..5.0
        val SPIDER_REST = 2.5..5.0
        const val REACH = 6.0
        const val CROSSING = 0.2
        const val CALM_PACE = 0.4
        const val MOST_PARTICLES = 160
        val CALL_EVERY = 9.0..15.0
        const val MARGIN = 6.0

        fun water(screen: AquariumRect, top: Double, floor: Double): AquariumRect {
            val upper = max(top, screen.minY) + MARGIN
            val lower = max(min(floor, screen.maxY) - MARGIN, upper + 60)
            return AquariumRect(screen.minX + MARGIN, upper, max(screen.width - MARGIN * 2, 60.0), lower - upper)
        }
    }
}
