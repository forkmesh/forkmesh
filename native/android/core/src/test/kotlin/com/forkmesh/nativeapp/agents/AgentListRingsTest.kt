package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentListRingsTest {
    fun testARingWhileIdleLoadsAtOnce() {
        val rings = AgentListRings()
        expect(rings.ring())
    }

    fun testARingBeforeTheReadsGoOutIsAnsweredByThem() {
        val rings = AgentListRings()
        val token = rings.begin()
        expect(!rings.ring())
        rings.readsSent(token)
        expect(!rings.finish(token))
        expect(!rings.isLoading)
    }

    fun testARingAfterTheReadsGoOutQueuesOneMoreLoad() {
        val rings = AgentListRings()
        val token = rings.begin()
        rings.readsSent(token)
        expect(!rings.ring())
        expect(!rings.ring())
        expect(rings.finish(token))
        expect(!rings.finish(token))
    }

    fun testALoadThatIsReplacedNeverClearsTheNewOne() {
        val rings = AgentListRings()
        val old = rings.begin()
        val fresh = rings.begin()
        rings.abandon(old)
        expect(rings.isLoading)
        rings.ring()
        rings.readsSent(old)
        expect(rings.finish(fresh))
    }

    fun testALoadThatFailsStillReportsALateRing() {
        val rings = AgentListRings()
        val token = rings.begin()
        rings.ring()
        expect(rings.finish(token))
        expect(rings.ring())
    }

    fun testAStartTargetNamesItsRepositoryOnlyOnTheMesh() {
        val flagship = AgentStartTarget(repo = Repository(owner = "mirror35", name = "forkmesh"))
        assertEq(com.forkmesh.nativeapp.navigation.RepoRef("mirror35", "forkmesh"), flagship.ref)
        assertEq(null, AgentStartTarget(record = com.forkmesh.nativeapp.localrepos.LocalRepoRecord("scratch", "/x/scratch", 0)).ref)
    }
}
