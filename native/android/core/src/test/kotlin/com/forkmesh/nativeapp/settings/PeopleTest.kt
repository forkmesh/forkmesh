package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sqrt

class PeopleTest {
    private fun fixture(): Map<String, Any?> {
        var dir: File? = File(System.getProperty("user.dir")).absoluteFile
        val relative = "marketplace/packages/people/tests/fixtures/rings.json"
        while (dir != null && !File(dir, relative).isFile) dir = dir.parentFile
        expect(dir != null, "people fixture")
        return Json.parseObject(File(dir, relative).readText())
    }

    fun testEveryCaseMatchesTheSharedFixture() {
        val fixture = fixture()
        val now = fixture.long("now")
        assertEq(fixture.strings("rings"), PeopleRing.entries.map { it.id })
        for (item in fixture.maps("cases")) {
            val user = item.obj("user")
            val member = PeopleMember.parse(user)
            val expected = item["member"]
            if (expected == null) {
                expect(member == null, "${user.text("name")} should be skipped")
                continue
            }
            @Suppress("UNCHECKED_CAST")
            val want = expected as Map<String, Any?>
            val parsed = member ?: throw AssertionError("${want.text("name")} should parse")
            assertEq(want.text("name"), parsed.name)
            assertEq(want.int("contributions"), parsed.contributions, parsed.name)
            assertEq(want.int("nodeCount"), parsed.nodeCount, parsed.name)
            assertEq(want.flag("hasPayoutAddress"), parsed.hasPayoutAddress, parsed.name)
            assertEq(want.text("avatar"), parsed.avatar, parsed.name)
            assertEq(item.int("score"), PeopleRings.score(parsed), parsed.name)
            assertEq(item.text("ring"), PeopleRings.ring(parsed, now).id, parsed.name)
        }
    }

    fun testSectionsKeepTheFixtureOrder() {
        val fixture = fixture()
        val members = fixture.maps("cases").mapNotNull { PeopleMember.parse(it.obj("user")) }
        val sections = PeopleDirectory.sections(members, fixture.long("now"))
        val expected = fixture.maps("order")
        assertEq(expected.map { it.text("ring") }, sections.map { it.ring.id })
        sections.zip(expected).forEach { (section, want) -> assertEq(want.strings("names"), section.people.map { it.member.name }) }
    }

    fun testLayoutMatchesTheFixture() {
        for (layout in fixture().maps("layout")) {
            val size = (layout["size"] as Number).toDouble()
            val seat = (layout["seat"] as Number).toDouble()
            val label = (layout["label"] as Number).toDouble()
            val radii = layout.list("radii").map { (it as Number).toDouble() }
            val capacities = layout.list("capacities").map { (it as Number).toInt() }
            for (ring in PeopleRing.entries.indices) {
                expect(abs(PeopleRingLayout.radius(ring, size) - radii[ring]) < 1e-4, "radius $ring at $size")
                assertEq(capacities[ring], PeopleRingLayout.capacity(ring, size, seat, label), "capacity $ring at $size")
            }
        }
    }

    fun testAFullRingEndsWithAnOverflowSeat() {
        val people = (0 until 40).map { PeopleMember(name = "p$it", activityBucket = "120d") }
        val seats = PeopleRingLayout.seats(PeopleDirectory.sections(people, 0), 360.0, 30.0, 72.0)
        val capacity = PeopleRingLayout.capacity(PeopleRing.EMBERS.ordinal, 360.0, 30.0, 72.0)
        assertEq(capacity, seats.size)
        assertEq(null, seats.last().person)
        assertEq(40 - (capacity - 1), seats.last().overflow)
        val r = PeopleRingLayout.radius(4, 360.0)
        val top = -r * cos(PeopleRingLayout.labelGap(4, 360.0, 30.0, 72.0))
        for (seat in seats) {
            expect(abs(sqrt((seat.x - 180) * (seat.x - 180) + (seat.y - 180) * (seat.y - 180)) - r) < 1e-3, "seat on its ring")
            expect(seat.y - 180 > top - 1e-3, "seat clear of the label")
        }
    }

    fun testFilterMatchesNamesWithoutTheAtSign() {
        val sections = PeopleDirectory.sections(listOf(PeopleMember("Ada"), PeopleMember("bob")), 0)
        assertEq(listOf("Ada"), PeopleDirectory.filter(sections, " @ad ").flatMap { it.people }.map { it.member.name })
    }

    fun testLoadDropsDuplicatesAndNodes() {
        val api = FakeAccountEndpoints()
        api.answers["GET ${PeopleModel.DIRECTORY_PATH}"] = Result.success(
            mapOf("ok" to true, "users" to listOf(mapOf("name" to "ada", "activityBucket" to "hour"), mapOf("name" to "Ada"), mapOf("name" to "n1", "kind" to "node"))),
        )
        val model = PeopleModel(api, { "Ada" }, { 0L })
        model.load()
        expect(model.loaded)
        assertEq(listOf("ada"), model.members.map { it.name })
        assertEq("ada", model.viewerName)
        assertEq(listOf(PeopleRing.GLOWING), model.visible("", PeopleRing.GLOWING).map { it.ring })
    }

    fun testLoadFailureKeepsAnError() {
        val api = FakeAccountEndpoints()
        api.answers["GET ${PeopleModel.DIRECTORY_PATH}"] = Result.failure(AccountEndpointException("offline"))
        val model = PeopleModel(api, { "" })
        model.load()
        expect(!model.loaded)
        expect(model.error.isNotEmpty())
    }

    fun testProfileAsksAsTheViewerAndFollowsAndUnfollows() {
        val api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/bob?viewer=ada"] = Result.success(
            mapOf("ok" to true, "name" to "bob", "profileBio" to "hi", "social" to mapOf("followers" to 2, "following" to 1, "isFollowing" to false)),
        )
        api.answers["POST /api/accounts/bob/follow"] = Result.success(mapOf("ok" to true, "followers" to 3, "isFollowing" to true))
        api.answers["DELETE /api/accounts/bob/follow"] = Result.success(mapOf("ok" to true, "followers" to 2, "isFollowing" to false))
        val model = PeopleProfileModel("Bob", "Ada", api)
        model.load()
        assertEq("hi", model.profile?.bio)
        assertEq(2, model.profile?.followers)
        model.toggleFollow()
        assertEq(true, model.profile?.isFollowing)
        assertEq(3, model.profile?.followers)
        model.toggleFollow()
        assertEq(false, model.profile?.isFollowing)
        assertEq(
            listOf("GET /api/accounts/bob?viewer=ada", "POST /api/accounts/bob/follow", "DELETE /api/accounts/bob/follow"),
            api.calls.map { "${it.method} ${it.path}" },
        )
    }

    fun testNobodyFollowsThemselvesOrWhileSignedOut() {
        val api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/ada?viewer=ada"] = Result.success(mapOf("ok" to true, "name" to "ada"))
        val me = PeopleProfileModel("ada", "ada", api)
        me.load()
        expect(me.isSelf)
        me.toggleFollow()
        api.answers["GET /api/accounts/bob"] = Result.success(mapOf("ok" to true, "name" to "bob"))
        val guest = PeopleProfileModel("bob", "", api)
        guest.load()
        guest.toggleFollow()
        assertEq(listOf("GET", "GET"), api.calls.map { it.method })
    }

    fun testUnverifiedEmailExplainsTheRefusal() {
        val api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/bob?viewer=ada"] = Result.success(mapOf("ok" to true, "name" to "bob"))
        api.answers["POST /api/accounts/bob/follow"] = Result.failure(AccountEndpointException("email_not_verified"))
        val model = PeopleProfileModel("bob", "ada", api)
        model.load()
        model.toggleFollow()
        assertEq("Verify your email to follow people.", model.actionError)
        assertEq(false, model.profile?.isFollowing)
    }
}
