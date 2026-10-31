package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException

class CrmCreateTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val request = "d".repeat(32)

    private class QueueTransport : PackagesTransport {
        val sent = mutableListOf<FakeTransport.Sent>()
        val replies = HashMap<String, ArrayDeque<() -> ApiJsonResult>>()

        fun on(method: String, path: String, vararg answers: () -> ApiJsonResult) {
            replies.getOrPut("$method $path") { ArrayDeque() }.addAll(answers)
        }

        fun count(method: String, path: String): Int = sent.count { it.method == method && it.path == path }

        fun last(method: String, path: String) = sent.lastOrNull { it.method == method && it.path == path }

        override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
            sent.add(FakeTransport.Sent(method, path, query, body))
            val queue = replies["$method $path"] ?: return ApiJsonResult(200, mapOf("ok" to true))
            val answer = if (queue.size > 1) queue.removeFirst() else queue.first()
            return answer()
        }
    }

    private fun person(id: String, name: String): Map<String, Any?> = mapOf(
        "id" to id, "organization" to "acme", "name" to name, "email" to name.lowercase() + "@x.test",
        "company" to "Acme", "flow" to "customers", "stage" to "lead", "tags" to listOf("vip"),
    )

    private fun reply(status: Int, body: Map<String, Any?>): () -> ApiJsonResult = { ApiJsonResult(status, body) }

    private fun loaded(transport: QueueTransport): CrmModel {
        transport.on("GET", "/api/crm", reply(200, mapOf("organizations" to listOf(mapOf("name" to "acme", "role" to "owner", "installed" to true)))))
        transport.on(
            "GET", "/api/crm/acme",
            reply(
                200,
                mapOf(
                    "organization" to "acme", "role" to "owner", "contacts" to listOf(person(hexA, "Ada")),
                    "settings" to mapOf("flows" to listOf(mapOf("id" to "customers", "name" to "Customers", "stages" to listOf("lead", "customer")))),
                    "revision" to "r1",
                ),
            ),
        )
        val model = CrmModel(PackagesClient(transport))
        model.retryDelays = listOf(0, 0)
        model.sleep = {}
        model.refresh()
        return model
    }

    fun testCreateSendsTheRequestIdAndRetriesAServerBlip() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on(
            "POST", "/api/crm/acme/contacts",
            reply(503, mapOf("error" to "unavailable")),
            { throw IOException("reset") },
            reply(201, mapOf("contact" to person(request, "Dee"))),
        )
        val saved = model.save(CrmContactEdit(name = "Dee", email = "dee@x.test"), null, requestId = request)
        assertEq(request, saved?.id)
        assertEq(3, transport.count("POST", "/api/crm/acme/contacts"))
        assertEq(request, transport.last("POST", "/api/crm/acme/contacts")?.body?.get("requestId"))
        assertEq("Dee", model.contact(request)?.name)
        assertEq("", model.errorText)
    }

    fun testCreateWithoutARequestIdIsNotRetried() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on("POST", "/api/crm/acme/contacts", reply(503, mapOf("error" to "unavailable")))
        assertEq(null, model.save(CrmContactEdit(name = "Dee"), null))
        assertEq(1, transport.count("POST", "/api/crm/acme/contacts"))
        expect(model.errorText.isNotEmpty())
        expect(transport.last("POST", "/api/crm/acme/contacts")?.body?.containsKey("requestId") == false)
    }

    fun testRetriesStopAfterTheLastDelay() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on("POST", "/api/crm/acme/contacts", reply(502, mapOf("error" to "bad_gateway")))
        assertEq(null, model.save(CrmContactEdit(name = "Dee"), null, requestId = request))
        assertEq(3, transport.count("POST", "/api/crm/acme/contacts"))
    }

    fun testAnEmailAlreadyInTheCrmNamesThatContact() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on("POST", "/api/crm/acme/contacts", reply(409, mapOf("error" to "contact_email_exists", "contact" to person(hexA, "Ada"))))
        val saved = model.save(CrmContactEdit(name = "Ada L", email = "ada@x.test"), null, requestId = request)
        assertEq(null, saved)
        assertEq(hexA, model.existing?.id)
        assertEq("Acme", model.existing?.company)
        expect(model.errorText.startsWith("Ada already uses that email address"), model.errorText)
        assertEq(1, transport.count("POST", "/api/crm/acme/contacts"))
        model.clearError()
        assertEq(null, model.existing)
    }

    fun testAnUnknownDuplicateIsBuiltFromTheReply() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on("POST", "/api/crm/acme/contacts", reply(409, mapOf("error" to "contact_email_exists", "contact" to mapOf("id" to hexB, "name" to "", "email" to "zed@x.test"))))
        model.save(CrmContactEdit(email = "zed@x.test"), null, requestId = request)
        assertEq(hexB, model.existing?.id)
        assertEq("zed@x.test", model.existing?.email)
        expect(model.errorText.startsWith("zed@x.test already uses"), model.errorText)
    }

    fun testAddDetailsFillsOnlyBlanksAndAddsTheList() {
        val transport = QueueTransport()
        val model = loaded(transport)
        transport.on("PATCH", "/api/crm/acme/contacts/$hexA", reply(200, mapOf("contact" to person(hexA, "Ada"))))
        val details = CrmContactEdit(name = "Ada L", email = "ada@x.test", phone = "+1 555", company = "Other", tags = "vip, new", notes = "Met at a fair")
        val found = model.contact(hexA)!!
        model.existing = found
        val saved = model.addDetails(details, found, addTo = hexB)
        expect(saved != null)
        assertEq(null, model.existing)
        val body = transport.last("PATCH", "/api/crm/acme/contacts/$hexA")?.body.orEmpty()
        assertEq("+1 555", body["phone"])
        expect("company" !in body && "name" !in body && "stage" !in body && "flow" !in body && "email" !in body)
        assertEq(listOf("new"), body["addTags"])
        assertEq(listOf(hexB), body["views"])
        assertEq("Met at a fair", body["notes"])
    }

    fun testAdditionsAppendNotesAndKeepFilledFields() {
        val existing = CrmContact(id = hexA, name = "Ada", notes = "Old", value = 10, fields = mapOf("rank" to "2"), lastContactedAt = 5)
        val edit = CrmContactEdit(name = "Ada", notes = "New", value = 99, fields = mapOf("rank" to "1", "team" to "core"), lastContactedAt = 9)
        val body = edit.additions(existing)
        assertEq("Old\n\nNew", body["notes"])
        expect("value" !in body)
        assertEq(9L, body["lastContactedAt"])
        assertEq(mapOf("team" to "core"), body["fields"])
        expect(edit.copy(notes = "Old", lastContactedAt = 1, fields = emptyMap()).additions(existing).isEmpty())
    }

    fun testShowingASectionPicksAMatchingTab() {
        val transport = QueueTransport()
        val model = loaded(transport)
        assertEq(CrmSection.HOME, model.section)
        model.show(CrmSection.ACTIVITY)
        assertEq(CrmTab.Activity, model.activeTab)
        model.show(CrmSection.CONTACTS)
        expect(model.activeTab.isContacts)
        model.show(CrmSection.ACTIVITY)
        model.show(CrmSection.PIPELINE)
        assertEq("customers", model.activeFlow?.id)
        assertEq(CrmSection.PIPELINE, model.section)
    }

    fun testRequestIdsAreHex() {
        expect(PackagesText.isHexId(CrmModel.newRequestId()))
        expect(CrmModel.newRequestId() != CrmModel.newRequestId())
    }

    fun testIsTransient() {
        expect(PackagesException.isTransient(PackagesException("x", 502)))
        expect(PackagesException.isTransient(PackagesException("x", 408)))
        expect(PackagesException.isTransient(IOException("reset")))
        expect(!PackagesException.isTransient(PackagesException("contact_email_exists", 409)))
        expect(!PackagesException.isTransient(IllegalStateException()))
    }
}
