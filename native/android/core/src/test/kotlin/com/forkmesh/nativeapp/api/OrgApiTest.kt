package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class OrgApiTest {
    private fun api(transport: FakeTransport) =
        ApiService(TestSettings(), transport).also { it.sessionTokenProvider = { "tok" } }

    fun testMyOrgsUnwrapsTheOrgsArray() {
        val transport = FakeTransport { request ->
            assertEq("GET", request.method)
            assertEq("/api/orgs", request.path())
            jsonResponse(mapOf("ok" to true, "orgs" to listOf(mapOf("name" to "acme", "role" to "owner"), mapOf("name" to "globex", "role" to "member"), mapOf("role" to "x"))))
        }
        val orgs = api(transport).myOrgs()
        assertEq(listOf("acme", "globex"), orgs.map { it.name })
        expect(orgs.first().canManage)
        expect(!orgs.last().canManage)
        assertEq("Bearer tok", transport.requests.single().headerValue("Authorization"))
        assertEq(null, transport.requests.single().headerValue("Content-Type"))
    }

    fun testCreateOrgPostsThePayloadAndReturnsTheNewOrg() {
        val transport = FakeTransport { request ->
            assertEq("POST", request.method)
            assertEq("/api/orgs", request.path())
            assertEq(mapOf("name" to "acme", "displayName" to "Acme Inc", "description" to "Things"), request.bodyJson())
            jsonResponse(mapOf("ok" to true, "org" to "acme", "role" to "owner"), 201)
        }
        val created = api(transport).createOrg(name = "ACME", displayName = "Acme Inc", description = "Things")
        assertEq("acme", created.name)
        assertEq("owner", created.role)

        transport.handler = { jsonResponse(mapOf("ok" to true), 201) }
        val fallback = api(FakeTransport { jsonResponse(mapOf("ok" to true), 201) }).createOrg(name = "Beta ")
        assertEq("Beta ", fallback.name)
        assertEq("owner", fallback.role)
    }

    fun testCreateOrgSurfacesTheWorkerErrorCodeAsAMessage() {
        val api = api(FakeTransport { jsonResponse(mapOf("error" to "org_name_taken"), 409) })
        val error = assertFails<OrgApiException> { api.createOrg(name = "taken") }
        assertEq("org_name_taken", error.code)
        assertEq("That name is already taken.", error.message)
        assertEq("That name is already taken.", error.toString())

        val bare = assertFails<OrgApiException> { api(FakeTransport { textResponse("oops", 500) }).myOrgs() }
        assertEq("http_500", bare.code)
        assertEq("Request failed.", bare.message)
    }

    fun testMemberTeamAndRepoWritesUseTheRightMethodPathAndBody() {
        val transport = FakeTransport { request ->
            if (request.path().endsWith("/members") && request.method == "GET") {
                jsonResponse(mapOf("ok" to true, "members" to listOf(mapOf("name" to "alice", "role" to "owner", "since" to 1), mapOf("name" to "bob", "role" to "member", "since" to 2))))
            } else if (request.path().endsWith("/teams") && request.method == "GET") {
                jsonResponse(mapOf("teams" to listOf(mapOf("team" to "Core", "permission" to "write", "members" to 2))))
            } else if (request.path().endsWith("/repos") && request.method == "GET") {
                jsonResponse(mapOf("repos" to listOf(mapOf("repo" to "Widget", "node" to "alice"))))
            } else if (request.method == "GET") {
                jsonResponse(mapOf("org" to "acme", "displayName" to "Acme", "viewerRole" to "owner"))
            } else {
                jsonResponse(mapOf("ok" to true))
            }
        }
        val api = api(transport)
        assertEq(listOf("alice", "bob"), api.orgMembers("acme").map { it.name })
        api.setOrgMember("acme", "Carol", role = "admin")
        api.removeOrgMember("acme", "Bob")
        api.setOrgTeam("acme", "Core", permission = "write")
        api.deleteOrgTeam("acme", "Core")
        api.linkOrgRepo("acme", "Widget", node = "Alice")
        api.unlinkOrgRepo("acme", "Widget")
        api.addOrgTeamMember("acme", "core", "Alice")
        api.removeOrgTeamMember("acme", "core", "Alice")
        api.deleteOrg("acme")

        val writes = transport.requests.filter { it.method != "GET" }
        fun expectWrite(i: Int, method: String, path: String, body: Map<String, Any?>) {
            assertEq(method, writes[i].method, "write $i")
            assertEq(path, writes[i].path(), "write $i")
            assertEq(body, writes[i].bodyJson(), "write $i")
            assertEq("application/json", writes[i].headerValue("Content-Type"))
        }
        expectWrite(0, "POST", "/api/orgs/acme/members", mapOf("member" to "carol", "role" to "admin"))
        expectWrite(1, "DELETE", "/api/orgs/acme/members", mapOf("member" to "bob"))
        expectWrite(2, "POST", "/api/orgs/acme/teams", mapOf("team" to "core", "permission" to "write"))
        expectWrite(3, "DELETE", "/api/orgs/acme/teams", mapOf("team" to "core"))
        expectWrite(4, "POST", "/api/orgs/acme/repos", mapOf("repo" to "widget", "node" to "alice"))
        expectWrite(5, "DELETE", "/api/orgs/acme/repos", mapOf("repo" to "widget"))
        expectWrite(6, "POST", "/api/orgs/acme/teams/core/members", mapOf("member" to "alice"))
        expectWrite(7, "DELETE", "/api/orgs/acme/teams/core/members", mapOf("member" to "alice"))
        expectWrite(8, "DELETE", "/api/orgs/acme", emptyMap())

        assertEq(listOf("core"), api.orgTeams("ACME").map { it.team })
        assertEq(listOf("widget"), api.orgRepos("acme").map { it.repo })
        assertEq("Acme", api.orgProfile("acme").title)
        assertEq("/api/orgs/a%20b/teams/c/members", run { api.orgTeamMembers("A B", "c"); transport.requests.last().path() })
    }
}
