package com.forkmesh.nativeapp.genie

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

data class GeniePoint(val x: Double, val y: Double) {
    fun distance(to: GeniePoint): Double = hypot(to.x - x, to.y - y)

    companion object {
        val ZERO = GeniePoint(0.0, 0.0)
    }
}

data class GenieVector(val dx: Double, val dy: Double) {
    val length: Double get() = hypot(dx, dy)

    companion object {
        val ZERO = GenieVector(0.0, 0.0)
        val DOWN = GenieVector(0.0, 1.0)
    }
}

data class GenieBox(val minX: Double, val minY: Double, val maxX: Double, val maxY: Double) {
    val width: Double get() = maxX - minX
    val height: Double get() = maxY - minY

    fun clamp(point: GeniePoint): GeniePoint {
        if (width < 0 || height < 0) return point
        return GeniePoint(point.x.coerceIn(minX, maxX), point.y.coerceIn(minY, maxY))
    }
}

enum class GenieEdge(val degrees: Double) {
    Left(90.0), Right(-90.0), Top(180.0), Bottom(0.0);

    val normal: GenieVector
        get() = when (this) {
            Left -> GenieVector(-1.0, 0.0)
            Right -> GenieVector(1.0, 0.0)
            Top -> GenieVector(0.0, -1.0)
            Bottom -> GenieVector(0.0, 1.0)
        }
}

data class GenieArena(val box: GenieBox) {
    companion object {
        fun of(width: Double, height: Double, ceiling: Double, floor: Double, side: Double): GenieArena {
            val stand = side * GenieGravity.STAND
            val top = max(ceiling, 0.0) + stand
            val bottom = min(floor, height) - stand
            val left = stand
            val right = width - stand
            val midX = (left + right) / 2
            val midY = (top + bottom) / 2
            val x0 = if (right > left) left else midX - 0.5
            val y0 = if (bottom > top) top else midY - 0.5
            return GenieArena(GenieBox(x0, y0, x0 + max(right - left, 1.0), y0 + max(bottom - top, 1.0)))
        }
    }

    fun on(surface: GenieEdge, near: GeniePoint): GeniePoint {
        val x = near.x.coerceIn(box.minX, max(box.minX, box.maxX))
        val y = near.y.coerceIn(box.minY, max(box.minY, box.maxY))
        return when (surface) {
            GenieEdge.Left -> GeniePoint(box.minX, y)
            GenieEdge.Right -> GeniePoint(box.maxX, y)
            GenieEdge.Top -> GeniePoint(x, box.minY)
            GenieEdge.Bottom -> GeniePoint(x, box.maxY)
        }
    }

    fun nearest(point: GeniePoint): GenieEdge = listOf(
        GenieEdge.Bottom to abs(box.maxY - point.y), GenieEdge.Left to abs(point.x - box.minX),
        GenieEdge.Right to abs(box.maxX - point.x), GenieEdge.Top to abs(point.y - box.minY),
    ).minByOrNull { it.second }?.first ?: GenieEdge.Bottom

    fun touches(point: GeniePoint, surface: GenieEdge, slack: Double = 1.5): Boolean {
        val snapped = on(surface, point)
        return abs(snapped.x - point.x) <= slack && abs(snapped.y - point.y) <= slack
    }

    fun corners(clockwise: Boolean, from: GenieEdge, to: GenieEdge): List<Pair<GeniePoint, GenieEdge>> {
        val ring = listOf(GenieEdge.Top, GenieEdge.Right, GenieEdge.Bottom, GenieEdge.Left)
        val ends = mapOf(
            GenieEdge.Top to (GeniePoint(box.minX, box.minY) to GeniePoint(box.maxX, box.minY)),
            GenieEdge.Right to (GeniePoint(box.maxX, box.minY) to GeniePoint(box.maxX, box.maxY)),
            GenieEdge.Bottom to (GeniePoint(box.maxX, box.maxY) to GeniePoint(box.minX, box.maxY)),
            GenieEdge.Left to (GeniePoint(box.minX, box.maxY) to GeniePoint(box.minX, box.minY)),
        )
        var index = ring.indexOf(from)
        val out = mutableListOf<Pair<GeniePoint, GenieEdge>>()
        var current = from
        repeat(4) {
            if (current == to) return out
            val end = ends.getValue(current)
            out.add((if (clockwise) end.second else end.first) to current)
            index = (index + if (clockwise) 1 else 3) % 4
            current = ring[index]
        }
        return out
    }
}

data class GenieGait(
    val phase: Double = 0.0,
    val walking: Double = 0.0,
    val facing: Double = 0.0,
    val air: Double = 0.0,
    val land: Double = 0.0,
) {
    companion object {
        val STILL = GenieGait()
    }
}

data class GenieMove(
    val kind: Kind,
    val from: GeniePoint,
    val to: GeniePoint,
    val surface: GenieEdge?,
    val start: Double,
    val rawDuration: Double,
    val turnFrom: Double,
    val turnTo: Double,
    val path: List<GeniePoint> = emptyList(),
    val before: Double = 0.0,
    val anchor: GeniePoint? = null,
) {
    enum class Kind { Crawl, Fly, Hop, Land, Silk, Slide }

    val duration: Double get() = max(rawDuration, 0.01)
    val end: Double get() = start + duration
    val length: Double get() = from.distance(to)
    val hanging: Boolean get() = kind == Kind.Silk && from == to

    fun progress(now: Double): Double = ((now - start) / duration).coerceIn(0.0, 1.0)

    private fun lerp(a: GeniePoint, b: GeniePoint, k: Double) = GeniePoint(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k)

    fun point(now: Double): GeniePoint {
        val u = progress(now)
        return when (kind) {
            Kind.Fly, Kind.Slide -> {
                if (path.size < 2) return to
                val spot = u * (path.size - 1)
                val index = min(spot.toInt(), path.size - 2)
                lerp(path[index], path[index + 1], spot - index)
            }
            Kind.Land -> to
            Kind.Crawl, Kind.Hop -> lerp(from, to, GenieCrawl.ease(u))
            Kind.Silk -> {
                val p = lerp(from, to, GenieCrawl.ease(u))
                val pivot = anchor ?: return p
                val angle = swing(now) * PI / 180
                val dx = p.x - pivot.x
                val dy = p.y - pivot.y
                GeniePoint(pivot.x + dx * cos(angle) - dy * sin(angle), pivot.y + dx * sin(angle) + dy * cos(angle))
            }
        }
    }

    fun swing(now: Double): Double {
        if (!hanging) return 0.0
        return sin((now - start) * 2 * PI / GenieGravity.SWING_PERIOD) * GenieGravity.SWING_DEGREES * sin(PI * progress(now))
    }

    fun rotation(now: Double): Double {
        val u = progress(now)
        val delta = GenieGravity.turn(turnFrom, turnTo)
        return when (kind) {
            Kind.Fly -> turnFrom + delta * GenieCrawl.ease(u) + sin(u * PI * 3) * 14 * (1 - u)
            Kind.Crawl -> {
                val blend = min(0.3, duration * 0.45)
                turnFrom + delta * GenieCrawl.ease(((now - start) / blend).coerceIn(0.0, 1.0))
            }
            Kind.Hop, Kind.Land, Kind.Slide -> turnFrom + delta * u
            Kind.Silk -> {
                val blend = min(0.6, duration * 0.5)
                turnFrom + delta * GenieCrawl.ease(((now - start) / blend).coerceIn(0.0, 1.0)) + swing(now)
            }
        }
    }

    fun gait(now: Double): GenieGait {
        val u = progress(now)
        val elapsed = now - start
        return when (kind) {
            Kind.Fly -> GenieGait(phase = elapsed * 14, air = min(u / 0.1, 1.0))
            Kind.Land -> GenieGait(land = sin(PI * u))
            Kind.Slide -> {
                val land = if (elapsed < GenieGravity.LANDING) sin(PI * elapsed / GenieGravity.LANDING) else 0.0
                val walking = if (u < 1) min((1 - u) / 0.2, 1.0) else 0.0
                GenieGait(phase = elapsed * 16, walking = walking, land = land)
            }
            Kind.Crawl, Kind.Hop -> {
                val travelled = before + length * GenieCrawl.ease(u)
                val walking = if (u > 0 && u < 1) min(min(u / 0.12, (1 - u) / 0.12), 1.0) else 0.0
                val angle = -rotation(now) * PI / 180
                val local = (to.x - from.x) * cos(angle) - (to.y - from.y) * sin(angle)
                val facing = if (abs(local) < 4) 0.0 else if (local > 0) 1.0 else -1.0
                GenieGait(phase = GenieCrawl.phase(travelled), walking = walking, facing = facing)
            }
            Kind.Silk -> {
                val pivot = anchor
                if (pivot == null || hanging) return GenieGait(phase = elapsed * 2, air = GenieGravity.DANGLE)
                val climbing = to.distance(pivot) < from.distance(pivot)
                if (!climbing) return GenieGait(phase = elapsed * 3, air = GenieGravity.DANGLE)
                val walking = if (u > 0 && u < 1) min(min(u / 0.12, (1 - u) / 0.12), 1.0) else 0.0
                GenieGait(phase = GenieCrawl.phase(before + length * GenieCrawl.ease(u)), walking = walking)
            }
        }
    }
}

data class GenieTrack(val moves: List<GenieMove>) {
    val start: Double get() = moves.firstOrNull()?.start ?: 0.0
    val end: Double get() = moves.lastOrNull()?.end ?: 0.0
    val final: GeniePoint? get() = moves.lastOrNull()?.to
    val surface: GenieEdge? get() = moves.lastOrNull()?.surface
    val turn: Double get() = moves.lastOrNull()?.turnTo ?: 0.0

    fun move(now: Double): GenieMove? = moves.firstOrNull { now < it.end } ?: moves.lastOrNull()

    fun airborne(now: Double): Boolean = move(now)?.kind == GenieMove.Kind.Fly && now < end

    fun silk(now: Double): GeniePoint? {
        val move = move(now) ?: return null
        return if (now < end && move.kind == GenieMove.Kind.Silk) move.anchor else null
    }

    fun point(now: Double): GeniePoint? = move(now)?.point(now)

    fun rotation(now: Double): Double? = move(now)?.rotation(now)

    fun gait(now: Double): GenieGait = move(now)?.gait(now) ?: GenieGait.STILL
}

object GenieCrawl {
    const val SPEED = 80.0
    const val STRIDE_LENGTH = 11.0
    const val SHORT_HOP = 170.0

    fun speed(vitality: Double): Double = SPEED * (0.5 + 0.75 * vitality.coerceIn(0.0, 1.0))

    fun phase(travelled: Double): Double = travelled / STRIDE_LENGTH * PI

    fun ease(u: Double): Double = u * u * (3 - 2 * u)
}

object GenieGravity {
    const val STAND = 0.42
    const val PULL = 2200.0
    const val DRAG = 0.00055
    const val SETTLE = 240.0
    const val GRIP = 1500.0
    const val FLOOR_BOUNCE = 0.3
    const val WALL_BOUNCE = 0.42
    const val GLANCE = 0.78
    const val FRICTION = 2400.0
    const val REST = 20.0
    const val MAX_SPEED = 4200.0
    const val MAX_FLIGHT = 6.0
    const val STEP = 1.0 / 120.0
    const val LANDING = 0.24
    const val CEILING = -0.6
    const val SHORTEST_SILK = 60.0
    const val SWING_PERIOD = 1.7
    const val SWING_DEGREES = 7.0
    const val DANGLE = 0.3
    const val TUCK_SPEED = 260.0
    const val WANDER_MIN = 5.0
    const val WANDER_MAX = 13.0

    fun unit(gravity: GenieVector): GenieVector {
        val length = gravity.length
        if (length <= 0.05) return GenieVector.DOWN
        return GenieVector(gravity.dx / length, gravity.dy / length)
    }

    fun upright(gravity: GenieVector): Double {
        val g = unit(gravity)
        return atan2(-g.dx, g.dy) * 180 / PI
    }

    fun turn(from: Double, to: Double): Double {
        var delta = (to - from) % 360
        if (delta > 180) delta -= 360
        if (delta < -180) delta += 360
        return delta
    }

    fun lowness(surface: GenieEdge, gravity: GenieVector): Double {
        val g = unit(gravity)
        val n = surface.normal
        return n.dx * g.dx + n.dy * g.dy
    }

    fun floor(gravity: GenieVector): GenieEdge = GenieEdge.entries.maxByOrNull { lowness(it, gravity) } ?: GenieEdge.Bottom

    fun pace(direction: GenieVector, surface: GenieEdge, gravity: GenieVector): Double {
        val length = direction.length
        if (length <= 0.01) return 1.0
        val g = unit(gravity)
        val climb = -(direction.dx * g.dx + direction.dy * g.dy) / length
        var pace = 1 - 0.45 * max(climb, 0.0) + 0.2 * max(-climb, 0.0)
        if (lowness(surface, gravity) < -0.5) pace *= 0.82
        return pace
    }

    fun flightStep(point: GeniePoint, velocity: GenieVector, gravity: GenieVector, seconds: Double = STEP): Pair<GeniePoint, GenieVector> {
        val dt = seconds.coerceIn(0.0, 0.05)
        val g = unit(gravity)
        var beforeX = velocity.dx
        var beforeY = velocity.dy
        val launch = hypot(beforeX, beforeY)
        if (launch > MAX_SPEED) {
            beforeX *= MAX_SPEED / launch
            beforeY *= MAX_SPEED / launch
        }
        fun slow(v: Double) = v / (1 + DRAG * abs(v) * dt)
        var vx = slow(beforeX + g.dx * PULL * dt)
        var vy = slow(beforeY + g.dy * PULL * dt)
        val speed = hypot(vx, vy)
        if (speed > MAX_SPEED) {
            vx *= MAX_SPEED / speed
            vy *= MAX_SPEED / speed
        }
        return GeniePoint(point.x + (beforeX + vx) * dt / 2, point.y + (beforeY + vy) * dt / 2) to GenieVector(vx, vy)
    }

    fun fly(start: GeniePoint, velocity: GenieVector, gravity: GenieVector, arena: GenieArena, time: Double, turn: Double): GenieTrack {
        val box = arena.box
        val g = unit(gravity)
        var p = box.clamp(start)
        val speed = velocity.length
        val cap = if (speed > MAX_SPEED) MAX_SPEED / speed else 1.0
        var vx = velocity.dx * cap
        var vy = velocity.dy * cap
        val samples = mutableListOf(p)
        var landed: GenieEdge? = null
        var t = 0.0
        while (t < MAX_FLIGHT && landed == null) {
            val next = flightStep(p, GenieVector(vx, vy), g)
            p = next.first
            vx = next.second.dx
            vy = next.second.dy
            t += STEP
            val hits = listOf(
                GenieEdge.Left to (p.x < box.minX), GenieEdge.Right to (p.x > box.maxX),
                GenieEdge.Top to (p.y < box.minY), GenieEdge.Bottom to (p.y > box.maxY),
            )
            for ((surface, hit) in hits) {
                if (!hit || landed != null) continue
                val n = surface.normal
                val into = vx * n.dx + vy * n.dy
                p = box.clamp(p)
                val floorLike = lowness(surface, g) > 0.6
                if (into < (if (floorLike) SETTLE else GRIP)) {
                    landed = surface
                    vx -= into * n.dx
                    vy -= into * n.dy
                    if (!floorLike) {
                        vx = 0.0
                        vy = 0.0
                    }
                    break
                }
                val bounce = if (floorLike) FLOOR_BOUNCE else WALL_BOUNCE
                val tx = vx - into * n.dx
                val ty = vy - into * n.dy
                vx = tx * GLANCE - into * bounce * n.dx
                vy = ty * GLANCE - into * bounce * n.dy
            }
            samples.add(p)
        }
        val surface = landed ?: arena.nearest(p)
        val touch = arena.on(surface, p)
        if (samples.last() != touch) samples.add(touch)
        val flight = GenieMove(
            GenieMove.Kind.Fly, samples.first(), touch, surface, time, (samples.size - 1) * STEP, turn, surface.degrees, samples,
        )
        val trail = if (landed == null) listOf(touch) else skid(touch, GenieVector(vx, vy), arena)
        if (trail.size > 2) {
            val slide = GenieMove(
                GenieMove.Kind.Slide, touch, trail.last(), surface, flight.end, (trail.size - 1) * STEP, surface.degrees, surface.degrees, trail,
            )
            return GenieTrack(listOf(flight, slide))
        }
        val bump = GenieMove(GenieMove.Kind.Land, touch, touch, surface, flight.end, LANDING, surface.degrees, surface.degrees)
        return GenieTrack(listOf(flight, bump))
    }

    fun skid(start: GeniePoint, velocity: GenieVector, arena: GenieArena): List<GeniePoint> {
        val box = arena.box
        var p = start
        var vx = velocity.dx
        var vy = velocity.dy
        val path = mutableListOf(p)
        var t = 0.0
        while (t < MAX_FLIGHT) {
            val speed = hypot(vx, vy)
            val slower = speed - FRICTION * STEP
            if (slower < REST) break
            vx *= slower / speed
            vy *= slower / speed
            p = GeniePoint(p.x + vx * STEP, p.y + vy * STEP)
            t += STEP
            if (p.x < box.minX || p.x > box.maxX) vx = -vx * WALL_BOUNCE
            if (p.y < box.minY || p.y > box.maxY) vy = -vy * WALL_BOUNCE
            p = box.clamp(p)
            path.add(p)
        }
        return path
    }

    fun fall(point: GeniePoint, surface: GenieEdge?, gravity: GenieVector, arena: GenieArena, time: Double, turn: Double): GenieTrack {
        val push = surface?.normal?.let { GenieVector(-it.dx * 140, -it.dy * 140) } ?: GenieVector.ZERO
        return fly(point, push, gravity, arena, time, turn)
    }

    fun crawl(
        point: GeniePoint,
        surface: GenieEdge,
        target: GeniePoint,
        goal: GenieEdge,
        arena: GenieArena,
        gravity: GenieVector,
        speed: Double,
        time: Double,
        turn: Double,
    ): GenieTrack {
        val a = arena.on(surface, point)
        val b = arena.on(goal, target)
        val options = listOf(true, false).map { clockwise -> arena.corners(clockwise, surface, goal) + (b to goal) }
        fun cost(path: List<Pair<GeniePoint, GenieEdge>>): Double {
            var from = a
            var total = 0.0
            for ((to, on) in path) {
                total += from.distance(to) / pace(GenieVector(to.x - from.x, to.y - from.y), on, gravity)
                from = to
            }
            return total
        }
        val chosen = if (surface == goal) listOf(b to goal) else options.minByOrNull(::cost) ?: listOf(b to goal)
        val moves = mutableListOf<GenieMove>()
        var from = a
        var clock = time
        var facing = turn
        var walked = 0.0
        for ((to, on) in chosen) {
            val length = from.distance(to)
            if (length < 1 && on.degrees == facing) continue
            val direction = GenieVector(to.x - from.x, to.y - from.y)
            val duration = max(length / (speed * pace(direction, on, gravity)), 0.3)
            val move = GenieMove(GenieMove.Kind.Crawl, from, to, on, clock, duration, facing, on.degrees, before = walked)
            moves.add(move)
            walked += length
            clock = move.end
            facing = on.degrees
            from = to
        }
        val last = moves.lastOrNull()
        if (last != null && last.surface != goal) {
            moves.add(GenieMove(GenieMove.Kind.Crawl, b, b, goal, clock, 0.3, facing, goal.degrees, before = walked))
        }
        return GenieTrack(moves)
    }

    fun silk(
        point: GeniePoint,
        surface: GenieEdge,
        arena: GenieArena,
        gravity: GenieVector,
        depth: Double,
        hang: Double,
        down: Boolean,
        speed: Double,
        time: Double,
        turn: Double,
    ): GenieTrack? {
        if (lowness(surface, gravity) >= CEILING || speed <= 0) return null
        val g = unit(gravity)
        val anchor = arena.on(surface, point)
        val hit = ray(anchor, g, arena.box) ?: return null
        if (hit.third < SHORTEST_SILK) return null
        val (ground, edge, reach) = hit
        val length = reach * depth.coerceIn(0.15, 0.85)
        val low = GeniePoint(anchor.x + g.dx * length, anchor.y + g.dy * length)
        val hanging = upright(g)
        val lower = GenieMove(GenieMove.Kind.Silk, anchor, low, null, time, max(length / (speed * 0.9), 0.6), turn, hanging, anchor = anchor)
        val wait = GenieMove(GenieMove.Kind.Silk, low, low, null, lower.end, max(hang, 0.5), hanging, hanging, anchor = anchor)
        if (down) {
            val rest = GenieMove(
                GenieMove.Kind.Silk, low, ground, null, wait.end, max((reach - length) / (speed * 1.1), 0.4), hanging, edge.degrees, anchor = anchor,
            )
            val bump = GenieMove(GenieMove.Kind.Land, ground, ground, edge, rest.end, LANDING, edge.degrees, edge.degrees)
            return GenieTrack(listOf(lower, wait, rest, bump))
        }
        val climb = GenieMove(GenieMove.Kind.Silk, low, anchor, null, wait.end, max(length / (speed * 0.6), 0.6), hanging, hanging, anchor = anchor)
        val grip = GenieMove(GenieMove.Kind.Crawl, anchor, anchor, surface, climb.end, 0.3, hanging, surface.degrees)
        return GenieTrack(listOf(lower, wait, climb, grip))
    }

    fun ray(point: GeniePoint, g: GenieVector, box: GenieBox): Triple<GeniePoint, GenieEdge, Double>? {
        val hits = mutableListOf<Pair<Double, GenieEdge>>()
        if (g.dx > 0.001) hits.add((box.maxX - point.x) / g.dx to GenieEdge.Right)
        if (g.dx < -0.001) hits.add((box.minX - point.x) / g.dx to GenieEdge.Left)
        if (g.dy > 0.001) hits.add((box.maxY - point.y) / g.dy to GenieEdge.Bottom)
        if (g.dy < -0.001) hits.add((box.minY - point.y) / g.dy to GenieEdge.Top)
        val first = hits.filter { it.first > 0.5 }.minByOrNull { it.first } ?: return null
        val end = GeniePoint(point.x + g.dx * first.first, point.y + g.dy * first.first)
        return Triple(box.clamp(end), first.second, first.first)
    }

    fun wander(point: GeniePoint, surface: GenieEdge, arena: GenieArena, gravity: GenieVector, random: () -> Double): Pair<GeniePoint, GenieEdge> {
        val box = arena.box
        val goal = if (random() < 0.55) {
            surface
        } else {
            val pick = random()
            val walls = GenieEdge.entries.filter { abs(lowness(it, gravity)) < 0.6 }
            when {
                pick < 0.5 || walls.isEmpty() -> floor(gravity)
                pick < 0.85 -> walls[min((random() * walls.size).toInt(), walls.size - 1)]
                else -> GenieEdge.entries.minByOrNull { lowness(it, gravity) } ?: GenieEdge.Top
            }
        }
        val along = random()
        var target = when (goal) {
            GenieEdge.Top, GenieEdge.Bottom -> GeniePoint(box.minX + box.width * along, point.y)
            GenieEdge.Left, GenieEdge.Right -> GeniePoint(point.x, box.minY + box.height * along)
        }
        if (goal == surface) {
            val reach = GenieCrawl.SHORT_HOP * (0.35 + 0.65 * random()) * (if (random() < 0.5) -1 else 1)
            target = when (goal) {
                GenieEdge.Top, GenieEdge.Bottom -> GeniePoint(point.x + reach, point.y)
                GenieEdge.Left, GenieEdge.Right -> GeniePoint(point.x, point.y + reach)
            }
        }
        return arena.on(goal, target) to goal
    }
}
