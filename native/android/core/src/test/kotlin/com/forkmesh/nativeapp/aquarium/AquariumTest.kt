package com.forkmesh.nativeapp.aquarium

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PhonePackages
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.hypot

class AquariumTest {
    private class Dice(var state: Long) {
        fun next(): Double {
            state = state * 6364136223846793005L + 1442695040888963407L
            return (state ushr 11).toDouble() / (1L shl 53).toDouble()
        }
    }

    private val screen = AquariumRect(0.0, 0.0, 390.0, 844.0)

    private val page: List<GenieLandmark> = listOf(
        GenieLandmark(GenieLandmarkKind.IMAGE, AquariumRect(16.0, 60.0, 36.0, 36.0), "ForkMesh logo"),
        GenieLandmark(GenieLandmarkKind.BUTTON, AquariumRect(330.0, 60.0, 44.0, 36.0), "Settings"),
        GenieLandmark(GenieLandmarkKind.HEADING, AquariumRect(20.0, 130.0, 180.0, 30.0), "Agents"),
        GenieLandmark(GenieLandmarkKind.TEXT, AquariumRect(20.0, 230.0, 260.0, 20.0), "Three agents are running"),
        GenieLandmark(GenieLandmarkKind.BUTTON, AquariumRect(20.0, 330.0, 70.0, 40.0), "Merge"),
        GenieLandmark(GenieLandmarkKind.TOGGLE, AquariumRect(300.0, 330.0, 51.0, 31.0), "Wander"),
        GenieLandmark(GenieLandmarkKind.FIELD, AquariumRect(20.0, 450.0, 350.0, 36.0), "Search"),
        GenieLandmark(GenieLandmarkKind.LINK, AquariumRect(20.0, 560.0, 120.0, 22.0), "Read the docs"),
        GenieLandmark(GenieLandmarkKind.TAB, AquariumRect(0.0, 760.0, 130.0, 50.0), "Home"),
        GenieLandmark(GenieLandmarkKind.TAB, AquariumRect(130.0, 760.0, 130.0, 50.0), "Agents"),
        GenieLandmark(GenieLandmarkKind.TAB, AquariumRect(260.0, 760.0, 130.0, 50.0), "Chat"),
    )

    private fun swimmers(count: Int, waiting: Set<Int> = emptySet()): List<AquariumSwimmer> =
        (0 until count).map { AquariumSwimmer("a$it", "Agent $it", if (it % 2 == 0) "opus" else "sonnet", it in waiting) }

    private fun tank(swimmers: List<AquariumSwimmer>, dice: Dice, spider: AquariumSpider? = null): AquariumTank {
        val tank = AquariumTank(screen, 50.0, 750.0)
        tank.see(page)
        tank.spider = spider
        tank.roster(swimmers, dice::next)
        return tank
    }

    private fun isLog(event: AquariumEvent): Boolean = event is AquariumEvent.Log

    fun testLookComesFromNameAndModel() {
        assertEq(AquariumLook.of("Fix login", "opus"), AquariumLook.of("Fix login", "opus"))
        val species = HashSet<AquariumSpecies>()
        for (index in 0 until 60) {
            val look = AquariumLook.of("Task $index", "model-${index % 3}")
            species.add(look.species)
            expect(look.size >= 0.85 && look.size <= 1.2)
        }
        assertEq(AquariumSpecies.entries.size, species.size)
    }

    fun testThreeGuestsWhenNoAgentRuns() {
        val dice = Dice(1)
        val tank = tank(emptyList(), dice)
        assertEq(listOf("Bubbles", "Finn", "Coral"), tank.fish.map { it.name })
        tank.roster(swimmers(4), dice::next)
        repeat(300) { tank.step(1.0 / 30, dice::next) }
        assertEq(swimmers(4).map { it.id }.toSet(), tank.fish.map { it.id }.toSet())
    }

    fun testOneFishPerAgentAndTheyLeaveWithTheirAgent() {
        val dice = Dice(2)
        val tank = tank(swimmers(3), dice)
        repeat(90) { tank.step(1.0 / 30, dice::next) }
        assertEq(3, tank.fish.size)
        val events = tank.roster(swimmers(3).take(2), dice::next)
        expect(events.contains(AquariumEvent.Log("Agent 2 swims off")))
        assertEq("Bye!", tank.fish.firstOrNull { it.id == "a2" }?.speech)
        repeat(300) { tank.step(1.0 / 30, dice::next) }
        assertEq(listOf("a0", "a1"), tank.fish.map { it.id }.sorted())
        val many = tank(swimmers(20), dice)
        assertEq(AquariumTank.MOST, many.fish.size)
    }

    fun testWaitingFishHoverNearTheTopAndAskForYou() {
        val dice = Dice(3)
        val tank = tank(swimmers(1, setOf(0)), dice)
        var asked = false
        for (frame in 0 until 30 * 40) {
            tank.step(1.0 / 30, dice::next)
            val fish = tank.fish[0]
            expect(fish.act == null)
            if (fish.speech == "I need you!") asked = true
            if (frame > 30 * 8) expect(fish.point.y < tank.water.minY + 190)
        }
        expect(asked)
    }

    fun testEveryElementTrickPlaysBesideItsTargetAndNeverRestsOnAButton() {
        for (seed in listOf(4L, 11L, 23L, 57L)) elementTricks(seed)
    }

    private fun elementTricks(seed: Long) {
        val dice = Dice(seed)
        val tank = tank(swimmers(3), dice)
        val played = HashSet<AquariumTrick>()
        var logs = 0
        val overlapFor = HashMap<String, Double>()
        val dt = 1.0 / 30
        repeat(30 * 400) {
            logs += tank.step(dt, dice::next).count(::isLog)
            for (fish in tank.fish) {
                val box = fish.box
                val covers = GenieLandmarks.overlaps(box, tank.blockers)
                val act = fish.act
                if (act != null && fish.phase == AquariumPhase.ACTING) {
                    played.add(act.trick)
                    if (!(act.trick == AquariumTrick.STARTLE && tank.time - act.start >= 1)) {
                        expect(!covers, "${act.trick} covers a button at $box")
                        expect(!GenieLandmarks.overlaps(box, listOf(act.target)), "${act.trick} covers its target")
                    }
                }
                if (fish.phase == AquariumPhase.WANDERING && hypot(fish.point.x - fish.goal.x, fish.point.y - fish.goal.y) < 4 &&
                    hypot(fish.velocity.width, fish.velocity.height) < 2
                ) {
                    expect(!covers, "${fish.name} rests on a button at $box")
                }
                val speech = fish.speechBox
                if (speech != null) expect(!GenieLandmarks.overlaps(speech, tank.blockers), "speech covers a button")
                overlapFor[fish.id] = if (covers) (overlapFor[fish.id] ?: 0.0) + dt else 0.0
                if ((overlapFor[fish.id] ?: 0.0) > 0.6) expect(fish.opacity < 0.3)
                if (fish.phase != AquariumPhase.ENTERING && fish.phase != AquariumPhase.LEAVING) {
                    expect(tank.screen.contains(fish.point))
                }
            }
        }
        assertEq(AquariumTrick.element.toSet(), played)
        expect(AquariumTrick.element.size >= 12)
        expect(logs > 13)
    }

    fun testSpiderTricksMakeTheSpiderReact() {
        spiderTricks(5, AquariumSpider(AquariumPoint(300.0, 711.0), 92.0, true))
        spiderTricks(6, AquariumSpider(AquariumPoint(39.0, 400.0), 92.0, true))
    }

    private fun spiderTricks(seed: Long, spider: AquariumSpider) {
        val dice = Dice(seed)
        val tank = tank(swimmers(2), dice, spider)
        val played = HashSet<AquariumTrick>()
        val asks = ArrayList<AquariumSpiderAsk>()
        repeat(30 * 600) {
            for (event in tank.step(1.0 / 30, dice::next)) {
                if (event is AquariumEvent.Spider) asks.add(event.ask)
            }
            for (fish in tank.fish) {
                val act = fish.act
                if (fish.phase == AquariumPhase.ACTING && act != null) played.add(act.trick)
            }
            expect(tank.fish.count { it.act?.trick?.withSpider == true && it.phase == AquariumPhase.ACTING } <= 1)
        }
        assertEq(AquariumTrick.spider.toSet(), played.intersect(AquariumTrick.spider.toSet()))
        expect(AquariumTrick.spider.size >= 10)
        val wanted = listOf(
            AquariumSpiderAsk.Wave, AquariumSpiderAsk.Cheer, AquariumSpiderAsk.Giggle,
            AquariumSpiderAsk.Surprise, AquariumSpiderAsk.Dizzy, AquariumSpiderAsk.Nod,
        )
        for (ask in wanted) expect(asks.contains(ask), "never asked $ask")
        expect(asks.any { it is AquariumSpiderAsk.Walk })
        expect(asks.any { it is AquariumSpiderAsk.Look })
    }

    fun testNoSpiderTricksWhileTheSpiderIsBusyOrAway() {
        val dice = Dice(6)
        val tank = tank(swimmers(3), dice, AquariumSpider(AquariumPoint(300.0, 711.0), 92.0, false))
        for (frame in 0 until 30 * 120) {
            if (frame == 30 * 60) tank.spider = null
            for (event in tank.step(1.0 / 30, dice::next)) {
                expect(event !is AquariumEvent.Spider, "asked the spider while it was not free")
            }
            expect(!tank.spiderPlaying)
        }
    }

    fun testCalmTankSwimsSlowlyWithoutParticles() {
        val dice = Dice(7)
        val tank = tank(swimmers(3), dice)
        tank.calm = true
        val last = HashMap<String, AquariumPoint>()
        for (frame in 0 until 30 * 120) {
            tank.step(1.0 / 30, dice::next)
            expect(tank.particles.isEmpty())
            for (fish in tank.fish) {
                val before = last[fish.id]
                if (before != null && frame > 30 * 3) {
                    expect(hypot(fish.point.x - before.x, fish.point.y - before.y) * 30 <= 240 * AquariumTank.CALM_PACE + 1)
                }
                last[fish.id] = fish.point
            }
        }
    }

    fun testBesideSpotsKeepTheFishAndItsTargetApart() {
        val dice = Dice(8)
        val fish = AquariumFish("f", "Finn", "x", false, AquariumPoint.ZERO)
        val water = AquariumTank.water(screen, 50.0, 750.0)
        val blockers = GenieLandmarks.blockers(page)
        for (mark in page) {
            val spot = AquariumClear.spot(mark.rect, { fish.body(it) }, blockers, water, AquariumPoint(200.0, 400.0)) ?: continue
            expect(!GenieLandmarks.overlaps(fish.body(spot), blockers + mark.rect))
        }
        val point = AquariumClear.waypoint(water, { fish.body(it) }, blockers, water, dice::next)
        expect(AquariumClear.clear(fish.body(point), blockers, water))
        assertEq(AquariumTank.CROSSING, AquariumClear.opacity(AquariumRect(25.0, 335.0, 20.0, 20.0), blockers))
        assertEq(1.0, AquariumClear.opacity(AquariumRect(150.0, 600.0, 20.0, 20.0), blockers))
    }

    fun testTricksFitTheirElements() {
        val short = GenieLandmark(GenieLandmarkKind.TEXT, AquariumRect(0.0, 0.0, 100.0, 20.0), "Agents")
        val long = GenieLandmark(GenieLandmarkKind.TEXT, AquariumRect(0.0, 0.0, 100.0, 20.0), "word ".repeat(10))
        val blank = GenieLandmark(GenieLandmarkKind.HEADING, AquariumRect(0.0, 0.0, 40.0, 20.0), "")
        expect(AquariumTrick.COUNT.fits(short))
        expect(!AquariumTrick.COUNT.fits(long))
        expect(!AquariumTrick.READ.fits(blank))
        expect(!AquariumTrick.MEASURE.fits(blank))
        expect(!AquariumTrick.POLISH.fits(short))
        expect(AquariumTrick.POLISH.fits(GenieLandmark(GenieLandmarkKind.BUTTON, AquariumRect(0.0, 0.0, 60.0, 30.0))))
        for (trick in AquariumTrick.element) expect(trick.fits.isNotEmpty(), "$trick")
    }

    fun testAquariumIsSwitchedPerDeviceAndStartsOff() {
        val prefs = MemoryPrefStore()
        assertEq(false, PhonePackages.isOn(PhonePackages.AQUARIUM, prefs, legacy = true))
        assertEq(false, PhonePackages.isOn(PhonePackages.AQUARIUM, null, legacy = true))
        assertEq(null, prefs.getBool("packages/aquarium/on"))
        PhonePackages.set("aquarium", true, prefs)
        assertEq(true, PhonePackages.isOn("aquarium", prefs, legacy = false))
        assertEq(true, prefs.getBool("packages/aquarium/on"))
        assertEq(null, PhonePackages.isOn("tour", prefs, legacy = true))
        PhonePackages.set(PhonePackages.AQUARIUM, false, prefs)
        assertEq(false, prefs.getBool("packages/aquarium/on"))
        expect(PhonePackages.DEVICE_SWITCHED.contains("aquarium"))
        expect(PhonePackages.DEVICE_SWITCHED.contains("sign-in"))
    }

    fun testAquariumReadsAsNotInstalledUntilTurnedOn() {
        val prefs = MemoryPrefStore()
        val model = MarketModel(PackagesClient(com.forkmesh.nativeapp.market.FakeTransport())) { PhonePackages.isOn(it, prefs, legacy = true) }
        val aquarium = MarketPackage(id = "aquarium", name = "Aquarium", desktop = true, bundled = true)
        expect(!model.isInstalled(aquarium))
        PhonePackages.set("aquarium", true, prefs)
        expect(model.isInstalled(aquarium))
    }
}
