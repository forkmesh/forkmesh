package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class TourCatalogTest {
    fun testTracksNest() {
        val essentials = TourCatalog.stops(TourTrack.ESSENTIALS).map { it.id }
        val advanced = TourCatalog.stops(TourTrack.ADVANCED).map { it.id }
        val everything = TourCatalog.stops(TourTrack.EVERYTHING).map { it.id }
        assertEq(listOf("home", "agents", "newAgent", "inbox", "hub"), essentials)
        assertEq(essentials, advanced.take(essentials.size))
        assertEq(advanced, everything.take(advanced.size))
        assertEq(10, advanced.size)
        assertEq(TourScene.entries.size, everything.size)
    }

    fun testEveryStopIsWritten() {
        assertEq(TourCatalog.stops.size, TourCatalog.stops.map { it.id }.toSet().size)
        for (stop in TourCatalog.stops) {
            expect(stop.title.isNotEmpty() && stop.message.isNotEmpty() && stop.tryIt.isNotEmpty() && stop.icon.isNotEmpty(), stop.id)
            assertEq(2, stop.tips.size, stop.id)
            assertEq(stop.jump == null, stop.jumpLabel.isEmpty(), stop.id)
            expect(stop.message.length <= 200, stop.id)
        }
    }

    fun testMissingTabDropsItsStop() {
        val installed = setOf("inbox", "code", "chat")
        val ids = TourCatalog.stops(TourTrack.EVERYTHING, installed::contains).map { it.id }
        expect("agents" !in ids && "newAgent" !in ids && "voice" !in ids)
        expect("home" in ids && "inbox" in ids)
    }

    fun testMissingPluginPointsAtTheMarket() {
        val stops = TourCatalog.stops(TourTrack.EVERYTHING, PackageGate.STARTER::contains)
        val workspace = stops.first { it.scene == TourScene.WORKSPACE }
        expect(workspace.fromMarket)
        assertEq(TourJump.Open(Route.Market(null)), workspace.jump)
        assertEq("Find it in the Market", workspace.jumpLabel)
        val chat = stops.first { it.scene == TourScene.CHAT }
        expect(!chat.fromMarket)
        assertEq(TourJump.Tab(AppTab.Chat), chat.jump)
    }

    fun testOnePluginIsEnough() {
        val workspace = TourCatalog.stops(TourTrack.EVERYTHING) { it == "tasks" }.first { it.scene == TourScene.WORKSPACE }
        expect(!workspace.fromMarket)
        assertEq(TourJump.Open(Route.Notes), workspace.jump)
    }

    fun testMinutes() {
        assertEq(1, TourCatalog.minutes(0))
        assertEq(2, TourCatalog.minutes(5))
        assertEq(6, TourCatalog.minutes(18))
    }

    fun testTrackIncludes() {
        expect(TourTrack.EVERYTHING.includes(TourTrack.ESSENTIALS))
        expect(TourTrack.ADVANCED.includes(TourTrack.ADVANCED))
        expect(!TourTrack.ESSENTIALS.includes(TourTrack.ADVANCED))
    }

    fun testPlaceRoundTrips() {
        val place = TourPlace(TourTrack.ADVANCED, "code")
        assertEq("advanced:code", place.encoded)
        assertEq(place, TourPlace.decode(place.encoded))
        assertEq(null, TourPlace.decode("advanced"))
        assertEq(null, TourPlace.decode("nope:code"))
        assertEq(null, TourPlace.decode("advanced:"))
        assertEq(6, TourCatalog.index(place, TourCatalog.stops(TourTrack.ADVANCED)))
        assertEq(null, TourCatalog.index(place, TourCatalog.stops(TourTrack.ESSENTIALS)))
    }

    fun testProgressSealsSmallerTracksAndRemembersThePlace() {
        val progress = TourProgress(MemoryPrefStore())
        assertEq(3, progress.left)
        assertEq(TourTrack.ADVANCED, progress.nextAfter(TourTrack.ESSENTIALS))
        progress.markDone(TourTrack.ADVANCED)
        assertEq(setOf(TourTrack.ESSENTIALS, TourTrack.ADVANCED), progress.done)
        assertEq(1, progress.left)
        assertEq(TourTrack.EVERYTHING, progress.nextAfter(TourTrack.ESSENTIALS))
        progress.markDone(TourTrack.EVERYTHING)
        assertEq(null, progress.nextAfter(TourTrack.ADVANCED))
        progress.remember(TourPlace(TourTrack.EVERYTHING, "chat"))
        assertEq(TourPlace(TourTrack.EVERYTHING, "chat"), progress.place)
        progress.remember(null)
        assertEq(null, progress.place)
        expect(!progress.seen)
        progress.markSeen()
        expect(progress.seen)
    }
}
