package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CrmPagingTest {
    private class PagingTransport : PackagesTransport {
        val sent = mutableListOf<Triple<String, String, Map<String, String>>>()
        val replies = HashMap<String, ArrayDeque<ApiJsonResult>>()

        fun answer(key: String, body: Map<String, Any?>, status: Int = 200) {
            replies.getOrPut(key) { ArrayDeque() }.addLast(ApiJsonResult(status, body))
        }

        override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
            val params = query.toMap()
            sent.add(Triple(method, path, params))
            val cursor = params["cursor"]
            val key = if (cursor != null) "$path?cursor=$cursor" else path
            val queue = replies[key] ?: return ApiJsonResult(404, mapOf("error" to "not_found"))
            return if (queue.size > 1) queue.removeFirst() else queue.first()
        }

        fun queries(path: String) = sent.filter { it.second == path }.map { it.third }
    }

    private fun hex(index: Int) = "%032x".format(index)

    private fun contact(index: Int, stage: String = "lead") =
        mapOf("id" to hex(index), "organization" to "acme", "name" to "Contact $index", "flow" to "customers", "stage" to stage)

    private fun firstPage(org: String = "acme", next: String = "c1", total: Int = 5, revision: String = "r1") = mapOf(
        "organization" to org, "role" to "owner",
        "contacts" to listOf(contact(1, "customer"), contact(2)),
        "summary" to mapOf("contacts" to total, "sent" to 3),
        "contactsNext" to next, "contactsTotal" to total, "revision" to revision,
    )

    private fun model(transport: PagingTransport, jobs: MutableList<() -> Unit>? = null): CrmModel {
        transport.answer("/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "role" to "owner", "installed" to true))))
        return CrmModel(PackagesClient(transport), "acme").also { model ->
            if (jobs != null) model.background = { jobs.add(it) }
            else model.background = { it() }
        }
    }

    fun testTheSummaryAsksForTheFirst250Contacts() {
        val transport = PagingTransport()
        transport.answer("/api/crm/acme", firstPage(next = ""))
        model(transport).load()
        assertEq("250", transport.queries("/api/crm/acme").single()["contacts"])
        val full = PagingTransport()
        full.answer("/api/crm/acme", firstPage(next = ""))
        PackagesClient(full).crm("acme")
        expect(full.queries("/api/crm/acme").single()["contacts"] == null)
    }

    fun testParsesTheCursorAndTotalAndCountsCustomers() {
        val data = CrmOrganizationData.fromJson(firstPage())
        assertEq("c1", data.contactsNext)
        assertEq(5, data.contactsTotal)
        assertEq(5, data.counts.contacts)
        assertEq(1, data.counts.customers)
        expect(!data.contactsComplete)
        val legacy = CrmOrganizationData.fromJson(mapOf("contacts" to listOf(contact(1)), "summary" to mapOf("customers" to 7)))
        assertEq("", legacy.contactsNext)
        assertEq(1, legacy.contactsTotal)
        assertEq(7, legacy.counts.customers)
        expect(legacy.contactsComplete)
    }

    fun testPagingFollowsCursorsAndSkipsKnownContacts() {
        val transport = PagingTransport()
        transport.answer("/api/crm/acme", firstPage())
        transport.answer("/api/crm/acme/contacts?cursor=c1", mapOf("ok" to true, "contacts" to listOf(contact(2), contact(3, "customer")), "next" to "c2"))
        transport.answer("/api/crm/acme/contacts?cursor=c2", mapOf("ok" to true, "contacts" to listOf(contact(4), contact(5, "customer")), "next" to ""))
        val jobs = mutableListOf<() -> Unit>()
        val model = model(transport, jobs)
        var pages = 0
        model.onContactsChanged = { pages++ }
        model.load()
        assertEq(2, model.data?.contacts?.size)
        expect(model.contactsLoading)
        assertEq("Loading contacts, 2 of 5", model.contactsStatus)
        jobs.single()()
        assertEq((1..5).map(::hex), model.data?.contacts?.map { it.id })
        assertEq(3, model.data?.counts?.customers)
        assertEq(2, pages)
        expect(!model.contactsLoading)
        assertEq("", model.contactsStatus)
        assertEq(
            listOf(mapOf("limit" to "1000", "cursor" to "c1"), mapOf("limit" to "1000", "cursor" to "c2")),
            transport.queries("/api/crm/acme/contacts"),
        )
        transport.answer("/api/crm/acme", mapOf("ok" to true, "unchanged" to true, "revision" to "r1"))
        transport.replies["/api/crm/acme"]?.removeFirst()
        model.load()
        assertEq("r1", transport.queries("/api/crm/acme").last()["since"])
        assertEq(5, model.data?.contacts?.size)
    }

    fun testANewerLoadDropsAnOlderBackgroundLoad() {
        val transport = PagingTransport()
        transport.answer("/api/crm/acme", firstPage())
        transport.answer("/api/crm/acme/contacts?cursor=c1", mapOf("ok" to true, "contacts" to listOf(contact(3)), "next" to ""))
        transport.answer("/api/crm/beta", firstPage(org = "beta", next = "", total = 2))
        val jobs = mutableListOf<() -> Unit>()
        val model = model(transport, jobs)
        model.load()
        model.organization = "beta"
        model.load()
        jobs.single()()
        assertEq("beta", model.data?.organization)
        assertEq(2, model.data?.contacts?.size)
        expect(!model.contactsLoading)
        val again = PagingTransport()
        again.answer("/api/crm/acme", firstPage())
        again.answer("/api/crm/acme/contacts?cursor=c1", mapOf("ok" to true, "contacts" to listOf(contact(3)), "next" to ""))
        val refreshed = model(again, jobs.also { it.clear() })
        refreshed.load()
        refreshed.load()
        assertEq(2, jobs.size)
        jobs[0]()
        assertEq(2, refreshed.data?.contacts?.size)
        jobs[1]()
        assertEq(3, refreshed.data?.contacts?.size)
    }

    fun testAFailedPageClearsTheRevisionSoTheNextLoadReadsEverything() {
        val transport = PagingTransport()
        transport.answer("/api/crm/acme", firstPage())
        transport.answer("/api/crm/acme/contacts?cursor=c1", mapOf("error" to "server_error"), status = 500)
        val model = model(transport)
        model.load()
        expect(!model.contactsLoading)
        assertEq("", model.data?.revision)
        expect(model.errorText.startsWith("Some contacts did not load"))
        assertEq(2, model.data?.contacts?.size)
        model.load()
        expect(transport.queries("/api/crm/acme").last()["since"] == null)
    }
}
