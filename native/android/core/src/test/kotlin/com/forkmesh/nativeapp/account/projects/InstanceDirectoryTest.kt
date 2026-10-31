package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class InstanceDirectoryTest {
    private val listing = """
        {"ok":true,"instances":[
        {"id":"a","name":"Acme Code","url":"https://relay.acme.dev/","status":"online","listed":true,"logo":"/api/start/instances/0123456789abcdef0123456789abcdef/logo?v=7"},
        {"id":"b","name":"Hidden","url":"https://hidden.dev/","listed":false},
        {"id":"c","name":"Plain","url":"http://plain.dev/","listed":true},
        {"id":"d","name":"Odd logo","url":"https://odd.dev","listed":true,"logo":"https://evil.dev/x.png"},
        {"id":"e","name":"ForkMesh copy","url":"https://app.forkmesh.com/","listed":true}]}
    """.trimIndent()

    private fun reply(status: Int, body: String) = HttpClient.Response(status, emptyMap(), body.toByteArray())

    fun testTheDirectoryListsListedHttpsInstancesAfterForkMesh() {
        val asked = ArrayList<String>()
        val directory = InstanceDirectory(HttpTransport { request ->
            asked.add(request.url)
            reply(200, listing)
        })
        val instances = directory.list()
        assertEq("https://app.forkmesh.com/api/start/instances", asked.single())
        assertEq(listOf("app.forkmesh.com", "relay.acme.dev", "odd.dev"), instances.map { it.id })
        assertEq("https://app.forkmesh.com/api/start/instances/0123456789abcdef0123456789abcdef/logo?v=7", instances[1].logoUrl)
        expect(instances[1].online)
        assertEq("", instances[2].logoUrl)
        expect(!instances[2].online)
        assertEq("A", instances[1].initial)
    }

    fun testAnUnreachableDirectoryStillOffersForkMesh() {
        assertEq(listOf(InstanceDirectory.flagship), InstanceDirectory(HttpTransport { reply(503, "down") }).list())
        assertEq(listOf(InstanceDirectory.flagship), InstanceDirectory(HttpTransport { throw java.io.IOException("offline") }).list())
    }

    fun testTheLogoCandidatesComeInOrderAndSkipStockFaviconsForABrand() {
        val instances = InstanceDirectory(HttpTransport { reply(200, listing) }).list()
        val project = Project("relay.acme.dev", "Acme Code", "https://relay.acme.dev")
        val result = ProbeResult(ProbeStatus.ok, project = project, logoUrl = "https://relay.acme.dev/instance-logo.png")
        assertEq(
            listOf(
                "https://relay.acme.dev/instance-logo.png",
                "https://app.forkmesh.com/api/start/instances/0123456789abcdef0123456789abcdef/logo?v=7",
                "https://relay.acme.dev/favicon.ico",
                "https://relay.acme.dev/favicon/favicon.ico",
            ),
            InstanceDirectory.logoCandidates(result, instances),
        )
        assertEq(listOf("https://relay.acme.dev/instance-logo.png"), InstanceDirectory.logoCandidates(result.copy(branded = true), emptyList()))
        assertEq(emptyList<String>(), InstanceDirectory.logoCandidates(null, instances))
        assertEq(emptyList<String>(), InstanceDirectory.logoCandidates(ProbeResult(ProbeStatus.unreachable), instances))
    }
}
