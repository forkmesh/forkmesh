package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

private fun PackagesClient.crmPath(organization: String, rest: String = "") = "/api/crm/" + segment(organization) + rest

fun PackagesClient.bulkUpdateCrmContacts(organization: String, edits: List<Map<String, Any?>>): Pair<List<CrmContact>, List<String>> {
    if (edits.isEmpty()) return emptyList<CrmContact>() to emptyList()
    val reply = call("POST", crmPath(organization, "/contacts/bulk"), body = mapOf("edits" to edits))
    return reply.maps("contacts").map(CrmContact::fromJson) to reply.strings("missing")
}

fun PackagesClient.mergeCrmContacts(organization: String, duplicate: String, kept: String): CrmContact =
    CrmContact.fromJson(call("POST", crmPath(organization, "/contacts/merge"), body = mapOf("from" to item(duplicate), "into" to item(kept))).obj("contact"))

fun PackagesClient.deleteCrmColumn(organization: String, field: String) {
    call("POST", crmPath(organization, "/contacts/delete-column"), body = mapOf("field" to field))
}

fun PackagesClient.exportCrmContacts(organization: String): Pair<String, String> {
    val reply = call("GET", crmPath(organization, "/contacts/export"))
    return reply.str("filename", "forkmesh-crm-$organization.csv") to reply.str("csv")
}

fun PackagesClient.previewCrmImport(organization: String, csv: String): CrmImportPreview =
    CrmImportPreview.fromJson(call("POST", crmPath(organization, "/contacts/import"), body = mapOf("preview" to true, "csv" to csv)))

fun PackagesClient.importCrmContacts(organization: String, body: Map<String, Any?>): JsonMap = call("POST", crmPath(organization, "/contacts/import"), body = body)

fun PackagesClient.crmEmail(organization: String, id: String): CrmActivity =
    CrmActivity.fromJson(call("GET", crmPath(organization, "/emails/" + item(id))).obj("email"))

fun PackagesClient.sendCrmEmail(organization: String, draft: CrmEmailDraft): CrmActivity =
    CrmActivity.fromJson(call("POST", crmPath(organization, "/emails/send"), body = draft.sendBody).obj("email"))

fun PackagesClient.logCrmEmail(organization: String, body: Map<String, Any?>): CrmActivity =
    CrmActivity.fromJson(call("POST", crmPath(organization, "/emails"), body = body).obj("email"))

fun PackagesClient.updateCrmEmail(organization: String, id: String, status: String): CrmActivity =
    CrmActivity.fromJson(call("PATCH", crmPath(organization, "/emails/" + item(id)), body = mapOf("status" to status)).obj("email"))

fun PackagesClient.deleteCrmEmail(organization: String, id: String) {
    call("DELETE", crmPath(organization, "/emails/" + item(id)), body = emptyMap())
}

fun PackagesClient.moveCrmMail(organization: String, id: String, box: String): CrmMailMessage =
    CrmMailMessage.fromJson(call("PATCH", crmPath(organization, "/mail/" + item(id)), body = mapOf("box" to box)).obj("message"))

fun PackagesClient.deleteCrmMail(organization: String, id: String) {
    call("DELETE", crmPath(organization, "/mail/" + item(id)), body = emptyMap())
}

fun PackagesClient.importCrmMail(organization: String, limit: Int = 200): JsonMap =
    call("POST", crmPath(organization, "/mail/import"), body = mapOf("limit" to limit))

fun PackagesClient.connectCrmZoho(organization: String): String =
    call("POST", crmPath(organization, "/zoho/connect"), body = emptyMap()).str("authorizationUrl")

fun PackagesClient.exchangeCrmZoho(organization: String, code: CrmZohoCode): CrmSettings =
    CrmSettings.fromJson(call("POST", crmPath(organization, "/zoho/exchange"), body = code.body).obj("settings"))

fun PackagesClient.saveCrmSettings(organization: String, body: Map<String, Any?>): CrmSettings =
    CrmSettings.fromJson(call("PUT", crmPath(organization, "/settings"), body = body).obj("settings"))

fun PackagesClient.saveCrmFlows(organization: String, flows: List<CrmFlow>): List<CrmFlow> =
    call("PUT", crmPath(organization, "/settings"), body = CrmFlowDraft.settingsBody(flows)).obj("settings").maps("flows").map(CrmFlow::fromJson)

fun PackagesClient.createCrmView(organization: String, body: Map<String, Any?>): CrmView =
    CrmView.fromJson(call("POST", crmPath(organization, "/views"), body = body).obj("view"))

fun PackagesClient.updateCrmView(organization: String, id: String, body: Map<String, Any?>): CrmView =
    CrmView.fromJson(call("PATCH", crmPath(organization, "/views/" + item(id)), body = body).obj("view"))

fun PackagesClient.deleteCrmView(organization: String, id: String) {
    call("DELETE", crmPath(organization, "/views/" + item(id)), body = emptyMap())
}

fun PackagesClient.orderCrmViews(organization: String, order: List<String>): List<CrmView> =
    call("POST", crmPath(organization, "/views/order"), body = mapOf("order" to order)).maps("views").map(CrmView::fromJson)

fun PackagesClient.crmListActivity(organization: String, viewId: String, before: Long = 0): Pair<List<CrmListActivityEntry>, Boolean> {
    val query = mutableListOf("view" to item(viewId))
    if (before > 0) query.add("before" to before.toString())
    val reply = call("GET", crmPath(organization, "/views/activity"), query)
    return reply.maps("entries").map { CrmListActivityEntry(it.long("at"), it.str("actor"), it.str("action"), it.str("text"), it.int("count", 1)) } to reply.bool("more")
}

fun PackagesClient.recordCrmListView(organization: String, viewId: String) {
    call("POST", crmPath(organization, "/views/activity"), body = mapOf("view" to item(viewId)))
}

fun PackagesClient.copyCrmSharedView(organization: String, viewId: String): CrmImportResult =
    CrmImportResult.fromJson(call("POST", crmPath(organization, "/views/copy"), body = mapOf("viewId" to item(viewId))))

fun PackagesClient.crmSharedView(viewId: String): Pair<CrmView, List<CrmContact>> {
    val reply = call("GET", "/api/crm/-/shared/" + item(viewId))
    return CrmView.fromJson(reply.obj("view")) to reply.maps("contacts").map(CrmContact::fromJson)
}

fun PackagesClient.updateCrmSharedContact(viewId: String, contactId: String, changes: Map<String, Any?>): CrmContact =
    CrmContact.fromJson(call("PATCH", "/api/crm/-/shared/${item(viewId)}/contacts/${item(contactId)}", body = changes).obj("contact"))

fun PackagesClient.sendCrmOutreach(organization: String, draft: CrmEmailDraft): Int =
    call("POST", crmPath(organization, "/outreach/send"), body = draft.outreachBody).int("sentToday")

fun PackagesClient.updateCrmOutreachTeam(organization: String, add: Boolean, name: String): List<CrmOutreachMember> =
    call("POST", crmPath(organization, "/outreach/team"), body = linkedMapOf("action" to if (add) "add" else "remove", "name" to name.lowercase()))
        .maps("members").map(CrmOutreach::memberOf)

fun PackagesClient.createCrmDeckLink(organization: String, contactId: String): CrmDeckLink =
    CrmDeckLink.fromJson(call("POST", crmPath(organization, "/decks"), body = mapOf("contactId" to item(contactId))).obj("link"))

fun PackagesClient.crmDeckDetail(organization: String, token: String): CrmDeckDetail {
    if (!CrmText.isDeckToken(token)) throw PackagesException("deck_link_not_found", 404)
    return CrmDeckDetail.fromJson(call("GET", crmPath(organization, "/decks/$token")).obj("deck"))
}

fun PackagesClient.revokeCrmDeckLink(organization: String, token: String) {
    if (!CrmText.isDeckToken(token)) throw PackagesException("deck_link_not_found", 404)
    call("DELETE", crmPath(organization, "/decks/$token"), body = emptyMap())
}

class CrmModel(private val client: PackagesClient, var organization: String = "", var flowId: String? = null) {
    companion object {
        fun newRequestId(): String = java.util.UUID.randomUUID().toString().replace("-", "").lowercase()
    }


    var organizations: List<PackageOrganization> = emptyList()
        private set
    var data: CrmOrganizationData? = null
        private set
    var errorText: String = ""
        private set
    var notice: String = ""
    var query: String = ""
    var tab: CrmTab? = null
    var filter = CrmFilter()
    val nextSkipped = mutableSetOf<String>()
    var existing: CrmContact? = null
    var section: CrmSection = CrmSection.HOME
    var now: () -> Long = { System.currentTimeMillis() }
    var retryDelays: List<Long> = listOf(400, 1_500)
    var sleep: (Long) -> Unit = { Thread.sleep(it) }
    private val memory = HashMap<String, CrmOrganizationData>()
    private val listViewsRecorded = HashMap<String, Long>()

    val installedOrganizations: List<PackageOrganization> get() = organizations.filter { it.installed }

    val current: PackageOrganization? get() = organizations.firstOrNull { it.name == organization }

    var organizationsLoaded: Boolean = false
        private set

    val needsOrganization: Boolean get() = organizationsLoaded && organizations.isEmpty()

    val needsInstall: Boolean get() = current?.let { !it.installed } ?: false

    val canInstall: Boolean get() = current?.let { !it.installed && it.canManage } ?: false

    val canManage: Boolean get() = data?.canManageSettings ?: false

    val flow: CrmFlow get() = data?.flow(flowId) ?: CrmFlow.FALLBACK

    val activeTab: CrmTab
        get() {
            val data = data ?: return tab ?: CrmTab.Activity
            tab?.let { if (exists(it, data)) return it }
            flowId?.let { id -> data.tabForFlow(id)?.let { return it } }
            return data.contactTabs.firstOrNull()?.tab ?: CrmTab.Activity
        }

    private fun exists(tab: CrmTab, data: CrmOrganizationData): Boolean = when {
        tab.isContacts -> data.contactTabs.any { it.tab == tab }
        tab == CrmTab.Shared -> data.sharedViews.isNotEmpty()
        else -> true
    }

    val activeFlow: CrmFlow? get() = data?.flowFor(activeTab)

    val activeView: CrmView? get() = data?.viewFor(activeTab)

    val activeTitle: String
        get() = data?.let { data -> (data.contactTabs + data.workspaceTabs).firstOrNull { it.tab == activeTab }?.title } ?: "CRM"

    val tabContacts: List<CrmContact> get() = data?.contactsFor(activeTab).orEmpty()

    val visibleContacts: List<CrmContact> get() = filter.copy(query = query).apply(tabContacts, activeFlow)

    fun select(next: CrmTab) {
        tab = next
        filter = filter.copy(stage = "")
        data?.flowFor(next)?.let { flowId = it.id }
    }

    val defaultTab: CrmTab get() = data?.contactTabs?.firstOrNull()?.tab ?: CrmTab.Activity

    fun show(next: CrmSection) {
        section = next
        val data = data ?: return
        when (next) {
            CrmSection.CONTACTS -> if (!activeTab.isContacts) select(defaultTab)
            CrmSection.PIPELINE -> if (activeFlow == null) data.contactTabs.firstOrNull { it.flow != null }?.let { select(it.tab) }
            CrmSection.ACTIVITY -> if (activeTab.isContacts) select(CrmTab.Activity)
            CrmSection.HOME, CrmSection.MORE -> Unit
        }
    }

    val dashboard: CrmDashboard get() = CrmDashboard.of(data?.contacts.orEmpty(), now())

    fun recordListView(viewId: String) {
        val stamp = now()
        val last = listViewsRecorded[viewId]
        if (last != null && stamp - last < 30 * 60 * 1000) return
        listViewsRecorded[viewId] = stamp
        runCatching { client.recordCrmListView(organization, viewId) }
    }

    fun refresh() {
        try {
            organizations = client.crmOrganizations()
            organizationsLoaded = true
            if (organization.isEmpty() || organizations.none { it.name == organization }) {
                organization = installedOrganizations.firstOrNull()?.name ?: organizations.firstOrNull()?.name ?: ""
            }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            return
        }
        load()
    }

    fun load() {
        if (!PackagesText.isOrgName(organization)) {
            data = null
            return
        }
        val cached = memory[organization]
        data = cached
        try {
            client.crm(organization, cached?.revision ?: "")?.let { fresh ->
                memory[organization] = fresh
                data = fresh
            }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
        }
    }

    fun contact(id: String): CrmContact? = data?.contacts?.firstOrNull { it.id == id }

    fun clearError() {
        errorText = ""
        existing = null
    }

    private fun remember(next: CrmOrganizationData?) {
        data = next
        next?.let { memory[organization] = it }
    }

    private fun <T> perform(work: () -> T): T? {
        if (!PackagesText.isOrgName(organization)) return null
        return try {
            work().also { errorText = "" }
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            null
        }
    }

    fun save(edit: CrmContactEdit, original: CrmContact?, addTo: String? = null, requestId: String = ""): CrmContact? {
        existing = null
        if (!edit.isValid || !PackagesText.isOrgName(organization)) {
            errorText = PackagesException.sentence("name_or_email_required")
            return null
        }
        if (original != null) return update(original, edit.changes(original))
        val org = organization
        val fields = LinkedHashMap(edit.changes(null))
        if (addTo != null && PackagesText.isHexId(addTo)) fields["views"] = listOf(addTo)
        if (PackagesText.isHexId(requestId)) fields["requestId"] = requestId
        var attempt = 0
        while (true) {
            try {
                val saved = client.createCrmContact(org, fields)
                errorText = ""
                if (org != organization) return saved
                remember(data?.replacing(saved))
                return saved
            } catch (thrown: Exception) {
                val found = (thrown as? PackagesException)?.let(::duplicate)
                if (found != null) {
                    existing = found
                    val label = found.name.ifEmpty { found.email }.ifEmpty { "A contact" }
                    errorText = "$label already uses that email address, so they are already in the CRM."
                    return null
                }
                if (!PackagesText.isHexId(requestId) || !PackagesException.isTransient(thrown) || attempt >= retryDelays.size) {
                    errorText = PackagesException.describe(thrown)
                    return null
                }
                sleep(retryDelays[attempt])
                attempt++
            }
        }
    }

    private fun duplicate(error: PackagesException): CrmContact? {
        val id = error.fields["contactId"].orEmpty()
        if (error.code != "contact_email_exists" || !PackagesText.isHexId(id)) return null
        return contact(id) ?: CrmContact(
            id = id, organization = organization, name = error.fields["contactName"].orEmpty(), email = error.fields["contactEmail"].orEmpty(),
        )
    }

    fun addDetails(edit: CrmContactEdit, found: CrmContact, addTo: String? = null): CrmContact? {
        if (contact(found.id) == null) load()
        val current = contact(found.id) ?: found
        val changes = LinkedHashMap(edit.additions(current))
        if (addTo != null && PackagesText.isHexId(addTo) && addTo !in current.views) changes["views"] = listOf(addTo)
        val saved = update(current, changes)
        if (saved != null) existing = null
        return saved
    }

    fun update(contact: CrmContact, changes: Map<String, Any?>): CrmContact? {
        if (changes.isEmpty()) return contact
        val saved = perform { client.updateCrmContact(CrmContactRef(organization, contact.id), changes) } ?: return null
        remember(data?.replacing(saved))
        return saved
    }

    fun move(contact: CrmContact, stage: String): Boolean = stage == contact.stage || update(contact, mapOf("stage" to stage)) != null

    fun moveFlow(contact: CrmContact, flow: String): Boolean {
        val data = data ?: return false
        if (flow == contact.flow) return true
        return update(contact, CrmBulkAction.Flow(flow).edit(contact, data.flows) - "id") != null
    }

    fun delete(contact: CrmContact): Boolean {
        perform { client.deleteCrmContact(CrmContactRef(organization, contact.id)) } ?: return false
        remember(data?.removing(contact.id))
        return true
    }

    fun deleteAll(ids: List<String>): Int {
        var removed = 0
        for (id in ids) {
            val contact = contact(id) ?: continue
            if (delete(contact)) removed++ else break
        }
        if (removed > 0) notice = if (removed == 1) "Deleted 1 contact." else "Deleted $removed contacts."
        return removed
    }

    fun bulk(action: CrmBulkAction, ids: List<String>): Boolean {
        val data = data ?: return false
        return sendBulk(ids.mapNotNull { id -> data.contactById(id)?.let { action.edit(it, data.flows) } })
    }

    fun moveToPipelineList(ids: List<String>, view: CrmView): Boolean {
        val data = data ?: return false
        if (!view.isPipeline) return false
        val flow = data.flows.firstOrNull { it.id == view.flow } ?: return false
        val siblings = data.pipelineLists[view.flow].orEmpty().filter { it != view.id }
        return sendBulk(ids.mapNotNull { id ->
            val contact = data.contactById(id) ?: return@mapNotNull null
            val stage = if (contact.flow == flow.id && contact.stage in flow.stages) contact.stage else flow.stages.firstOrNull() ?: "lead"
            linkedMapOf<String, Any?>("id" to id, "flow" to flow.id, "stage" to stage, "views" to listOf(view.id)).also {
                if (siblings.isNotEmpty()) it["removeViews"] = siblings
            }
        })
    }

    fun setOnlyList(ids: List<String>, viewId: String): Boolean = sendBulk(ids.map { mapOf("id" to it, "setViews" to listOf(viewId)) })

    fun markContacted(ids: List<String>): Boolean = bulk(CrmBulkAction.Contacted(now()), ids)

    private fun sendBulk(edits: List<Map<String, Any?>>): Boolean {
        if (edits.isEmpty()) return false
        val (contacts, missing) = perform { client.bulkUpdateCrmContacts(organization, edits) } ?: return false
        var next = data
        contacts.filter { PackagesText.isHexId(it.id) }.forEach { next = next?.replacing(it) }
        missing.forEach { next = next?.removing(it) }
        remember(next)
        return true
    }

    fun merge(duplicate: CrmContact, kept: CrmContact): CrmContact? {
        val merged = perform { client.mergeCrmContacts(organization, duplicate.id, kept.id) } ?: return null
        val current = data ?: return merged
        remember(
            current.copy(
                contacts = current.contacts.filter { it.id != duplicate.id },
                activity = current.activity.map { if (it.contactId == duplicate.id) it.copy(contactId = kept.id) else it },
                decks = current.decks.map { if (it.contactId == duplicate.id) it.copy(contactId = kept.id) else it },
            ).replacing(merged),
        )
        notice = "Merged ${duplicate.displayName} into ${merged.displayName}."
        return merged
    }

    fun install(): Boolean {
        if (!canInstall) return false
        try {
            client.install("crm", organization)
            organizations = organizations.map { if (it.name == organization) it.copy(installed = true) else it }
            errorText = ""
        } catch (thrown: Exception) {
            errorText = PackagesException.describe(thrown)
            return false
        }
        load()
        return data != null
    }

    fun forget() {
        memory.clear()
        listViewsRecorded.clear()
        data = null
        tab = null
    }

    fun emailDetail(id: String): CrmActivity? = perform { client.crmEmail(organization, id) }

    private fun absorb(email: CrmActivity) {
        val current = data ?: return
        val stored = email.copy(text = "", html = "")
        val index = current.activity.indexOfFirst { it.id == email.id }
        val activity = if (index >= 0) current.activity.toMutableList().also { it[index] = stored } else listOf(stored) + current.activity
        var next = current.copy(activity = activity)
        if (email.delivered || email.status == "external") {
            next.contactById(email.contactId)?.let { contact ->
                next = next.replacing(contact.copy(lastContactedAt = maxOf(contact.lastContactedAt, email.at)))
            }
        }
        remember(next)
    }

    fun sendEmail(draft: CrmEmailDraft): CrmActivity? {
        val sent = perform { client.sendCrmEmail(organization, draft) } ?: return null
        absorb(sent)
        notice = "Sent to ${draft.to}."
        if (draft.noteDraft && PackagesText.isHexId(draft.contactId)) {
            val sentNotice = notice
            markNoteDraftSent(draft.contactId, stampContacted = false)
            notice = sentNotice + " " + notice
        }
        return sent
    }

    fun nextContact(mode: CrmNextContact.Mode, everyone: Boolean = false): CrmNextContact =
        CrmNextContact.pick(mode, if (everyone) data?.contacts.orEmpty() else tabContacts, data?.activity.orEmpty(), nextSkipped.toSet(), now())

    val noteDrafts: List<CrmNoteDraft> get() = CrmNoteDraft.drafts(data?.contacts.orEmpty(), data?.activity.orEmpty())

    fun markNoteDraftSent(contactId: String, stampContacted: Boolean): Boolean {
        val contact = contact(contactId) ?: return false
        val day = java.time.Instant.ofEpochMilli(now()).atZone(java.time.ZoneId.systemDefault()).toLocalDate().toString()
        val notes = CrmNoteDraft.markedSent(contact.notes, day) ?: return false
        val changes = linkedMapOf<String, Any?>("notes" to notes)
        if (stampContacted) changes["lastContactedAt"] = now()
        update(contact, changes) ?: return false
        notice = "Draft marked sent in ${contact.name.ifEmpty { "the contact" }}'s notes."
        return true
    }

    fun restoreColumn(field: String): Boolean {
        val settings = data?.settings ?: return false
        val saved = perform { client.saveCrmSettings(organization, mapOf("removedColumns" to settings.removedColumns.filter { it != field })) } ?: return false
        data?.let { remember(it.copy(settings = saved)) }
        notice = "Column ${CrmColumnCatalog.label(field)} restored."
        return true
    }

    fun exchangeZoho(code: CrmZohoCode): Boolean {
        if (!code.isComplete) {
            errorText = PackagesException.sentence("zoho_code_incomplete")
            return false
        }
        val saved = perform { client.exchangeCrmZoho(organization, code) } ?: return false
        data?.let { remember(it.copy(settings = saved)) }
        notice = "Zoho Mail connected (${saved.zoho.region}). Sync mail to read it."
        return true
    }

    fun sendOutreach(draft: CrmEmailDraft): Boolean {
        val sentToday = perform { client.sendCrmOutreach(organization, draft) } ?: return false
        data?.let { remember(it.copy(outreach = it.outreach.copy(sentToday = sentToday))) }
        notice = "Outreach sent to ${draft.to}."
        load()
        return true
    }

    fun logEmail(to: String, subject: String, contactId: String, direction: String = "outbound"): CrmActivity? {
        val at = now()
        val body = linkedMapOf<String, Any?>(
            "to" to to, "subject" to subject, "externalId" to "mobile-$at", "status" to "external", "direction" to direction,
            "provider" to "other", "sentAt" to at,
        )
        if (PackagesText.isHexId(contactId)) body["contactId"] = contactId
        val logged = perform { client.logCrmEmail(organization, body) } ?: return null
        absorb(logged)
        return logged
    }

    fun setEmailStatus(email: CrmActivity, status: String): CrmActivity? {
        val updated = perform { client.updateCrmEmail(organization, email.id, status) } ?: return null
        absorb(updated)
        return updated
    }

    fun deleteEmail(email: CrmActivity): Boolean {
        perform { client.deleteCrmEmail(organization, email.id) } ?: return false
        data?.let { remember(it.copy(activity = it.activity.filter { item -> item.id != email.id })) }
        return true
    }

    fun moveMail(message: CrmMailMessage, box: String): Boolean {
        val moved = perform { client.moveCrmMail(organization, message.id, box) } ?: return false
        data?.let { remember(it.copy(mail = it.mail.map { item -> if (item.id == moved.id) moved else item })) }
        return true
    }

    fun deleteMail(message: CrmMailMessage): Boolean {
        perform { client.deleteCrmMail(organization, message.id) } ?: return false
        data?.let { remember(it.copy(mail = it.mail.filter { item -> item.id != message.id })) }
        return true
    }

    fun syncMail(): String? {
        val data = data ?: return null
        if (!data.settings.zoho.connected) {
            if (!data.canManageSettings || !data.settings.zoho.oneClick) {
                errorText = "Zoho Mail is not connected. An owner or admin connects it in CRM settings."
                return null
            }
            return perform { client.connectCrmZoho(organization) }?.ifEmpty { null }
        }
        val reply = perform { client.importCrmMail(organization) } ?: return null
        val created = reply.int("created")
        val updated = reply.int("updated")
        notice = if (created + updated == 0) "Mail is up to date." else "Mail: $created new, $updated updated."
        load()
        return null
    }

    fun connectZoho(): String? = perform { client.connectCrmZoho(organization) }?.ifEmpty { null }

    fun saveSettings(edit: CrmSettingsEdit): Boolean {
        val settings = perform { client.saveCrmSettings(organization, edit.body) } ?: return false
        data?.let { remember(it.copy(settings = settings)) }
        notice = "Settings saved."
        return true
    }

    fun saveFlow(draft: CrmFlowDraft): CrmFlow? {
        val data = data ?: return null
        val flow = draft.flow ?: run {
            errorText = "Give the pipeline a name."
            return null
        }
        val previous = data.flows.firstOrNull { it.id == flow.id }
        val flows = if (draft.isNew) {
            if (previous != null) {
                errorText = "A pipeline with that name already exists."
                return null
            }
            if (data.flows.size >= 12) {
                errorText = "An organization can have up to 12 pipelines."
                return null
            }
            data.flows + flow
        } else {
            data.flows.map { if (it.id == flow.id) flow else it }
        }
        val saved = perform { client.saveCrmFlows(organization, flows) } ?: return null
        remember(this.data?.copy(flows = saved.ifEmpty { listOf(CrmFlow.FALLBACK) }))
        if (previous != null && !draft.isNew) {
            val renames = previous.stages.zip(flow.stages).filter { (old, new) -> old != new && new !in previous.stages }
            val removed = previous.stages.toSet() - flow.stages.toSet()
            val edits = data.contacts.filter { it.flow == flow.id && it.stage in removed }.map { contact ->
                mapOf("id" to contact.id, "stage" to (renames.firstOrNull { it.first == contact.stage }?.second ?: flow.stages.firstOrNull() ?: "lead"))
            }
            if (edits.isNotEmpty()) sendBulk(edits)
        }
        if (draft.isNew) {
            val view = perform { client.createCrmView(organization, linkedMapOf("name" to flow.name, "flow" to flow.id, "visibility" to "organization")) }
            if (view != null) {
                remember(this.data?.let { it.copy(views = it.views + view) })
                tab = CrmTab.Sheet(view.id)
            } else {
                tab = CrmTab.Pipeline(flow.id)
            }
        }
        notice = if (draft.isNew) "Pipeline ${flow.name} created." else "Pipeline ${flow.name} saved."
        return flow
    }

    fun deleteFlow(flowId: String): Boolean {
        val data = data ?: return false
        if (data.flows.size <= 1) {
            errorText = "The last pipeline can't be deleted."
            return false
        }
        val saved = perform { client.saveCrmFlows(organization, data.flows.filter { it.id != flowId }) } ?: return false
        remember(this.data?.copy(flows = saved))
        for (view in data.views.filter { it.flow == flowId }) {
            if (perform { client.deleteCrmView(organization, view.id) } != null) {
                remember(this.data?.let { it.copy(views = it.views.filter { item -> item.id != view.id }) })
            }
        }
        tab = null
        notice = "Pipeline deleted."
        return true
    }

    fun reorderFlows(order: List<String>): Boolean {
        val data = data ?: return false
        val flows = order.mapNotNull { id -> data.flows.firstOrNull { it.id == id } }
        if (flows.size != data.flows.size) return false
        val saved = perform { client.saveCrmFlows(organization, flows) } ?: return false
        remember(this.data?.copy(flows = saved))
        return true
    }

    private fun replaceView(view: CrmView) {
        data?.let { current ->
            val index = current.views.indexOfFirst { it.id == view.id }
            remember(current.copy(views = if (index >= 0) current.views.toMutableList().also { it[index] = view } else current.views + view))
        }
    }

    fun createList(name: String, flow: String = "", visibility: String = "organization"): CrmView? {
        val body = linkedMapOf<String, Any?>("name" to name.trim().ifEmpty { "Untitled sheet" }, "visibility" to visibility)
        if (flow.isNotEmpty()) body["flow"] = flow
        val view = perform { client.createCrmView(organization, body) } ?: return null
        replaceView(view)
        tab = CrmTab.Sheet(view.id)
        return view
    }

    fun updateList(view: CrmView, body: Map<String, Any?>): CrmView? {
        val updated = perform { client.updateCrmView(organization, view.id, body) } ?: return null
        replaceView(updated)
        return updated
    }

    fun renameTab(item: CrmTabItem, name: String): Boolean {
        val title = name.trim()
        if (title.isEmpty()) return false
        item.view?.let { return updateList(it, mapOf("name" to title)) != null }
        val flow = item.flow ?: return false
        return saveFlow(CrmFlowDraft.of(flow).copy(name = title)) != null
    }

    fun share(item: CrmTabItem, names: List<String>, access: String, visibility: String): Boolean {
        val clean = names.map { it.trim().lowercase() }.filter { it.isNotEmpty() }.distinct()
        val body = linkedMapOf<String, Any?>("sharedWith" to clean, "shareAccess" to access, "visibility" to visibility)
        item.view?.let { return updateList(it, body) != null }
        val flow = item.flow ?: return false
        body["name"] = flow.name
        body["flow"] = flow.id
        val view = perform { client.createCrmView(organization, body) } ?: return false
        replaceView(view)
        tab = CrmTab.Sheet(view.id)
        return true
    }

    fun setColumns(view: CrmView, columns: List<CrmColumn>): Boolean = updateList(view, mapOf("columns" to columns.map { it.json })) != null

    fun deleteList(view: CrmView): Boolean {
        perform { client.deleteCrmView(organization, view.id) } ?: return false
        data?.let { current ->
            remember(
                current.copy(
                    views = current.views.filter { it.id != view.id },
                    contacts = current.contacts.map { if (view.id in it.views) it.copy(views = it.views - view.id) else it },
                ),
            )
        }
        if (tab == CrmTab.Sheet(view.id)) tab = null
        notice = "Deleted ${view.name}."
        return true
    }

    fun moveList(view: CrmView, offset: Int): Boolean {
        val order = data?.views?.map { it.id }?.toMutableList() ?: return false
        val index = order.indexOf(view.id)
        val target = index + offset
        if (index < 0 || target !in order.indices) return false
        order[index] = order[target].also { order[target] = order[index] }
        return reorderLists(order)
    }

    fun reorderLists(order: List<String>): Boolean {
        val views = perform { client.orderCrmViews(organization, order) } ?: return false
        data?.let { remember(it.copy(views = views.filter { view -> PackagesText.isHexId(view.id) })) }
        return true
    }

    fun mergeList(source: CrmView, target: CrmView): Boolean {
        val data = data ?: return false
        if (source.id == target.id) return false
        val members = data.contacts.filter { data.shows(it, source) }.map { it.id }
        if (members.isNotEmpty()) {
            val ok = if (target.isPipeline) moveToPipelineList(members, target)
            else sendBulk(members.map { mapOf("id" to it, "views" to listOf(target.id), "removeViews" to listOf(source.id)) })
            if (!ok) return false
        }
        val known = target.columns.map { it.field }.toSet()
        val extra = source.columns.filter { it.field !in known }
        if (extra.isNotEmpty()) setColumns(target, target.columns + extra)
        if (!deleteList(source)) return false
        tab = CrmTab.Sheet(target.id)
        notice = "Merged ${source.name} into ${target.name}."
        return true
    }

    fun listActivity(view: CrmView, before: Long = 0): Pair<List<CrmListActivityEntry>, Boolean> =
        perform { client.crmListActivity(organization, view.id, before) } ?: (emptyList<CrmListActivityEntry>() to false)

    fun deleteColumn(field: String): Boolean {
        perform { client.deleteCrmColumn(organization, field) } ?: return false
        load()
        notice = "Column ${CrmColumnCatalog.label(field)} deleted."
        return true
    }

    fun deckLink(contact: CrmContact): CrmDeckLink? {
        val link = perform { client.createCrmDeckLink(organization, contact.id) } ?: return null
        data?.let { current ->
            val index = current.decks.indexOfFirst { it.token == link.token }
            remember(current.copy(decks = if (index >= 0) current.decks.toMutableList().also { it[index] = link } else listOf(link) + current.decks))
        }
        return link
    }

    fun deckDetail(link: CrmDeckLink): CrmDeckDetail? = perform { client.crmDeckDetail(organization, link.token) }

    fun revoke(link: CrmDeckLink): Boolean {
        perform { client.revokeCrmDeckLink(organization, link.token) } ?: return false
        data?.let { current -> remember(current.copy(decks = current.decks.map { if (it.token == link.token) it.copy(active = false) else it })) }
        return true
    }

    fun setOutreachMember(name: String, allowed: Boolean): Boolean {
        val members = perform { client.updateCrmOutreachTeam(organization, allowed, name) } ?: return false
        data?.let { remember(it.copy(outreach = it.outreach.copy(members = members))) }
        return true
    }

    fun exportCsv(): Pair<String, String>? = perform { client.exportCrmContacts(organization) }

    fun previewImport(csv: String): CrmImportPreview? = perform { client.previewCrmImport(organization, CrmCsv.preview(csv)) }

    fun importCsv(csv: String, fileName: String, sheetName: String, viewId: String = "", mapping: List<String>, progress: (Int, Int) -> Unit = { _, _ -> }): CrmImportResult? {
        val split = CrmCsv.split(csv)
        if (split.rows == 0) {
            errorText = PackagesException.sentence("csv_required")
            return null
        }
        if (split.rows > 2000) {
            errorText = "The file has more than 2000 rows."
            return null
        }
        if (!PackagesText.isOrgName(organization)) return null
        val importId = "mobile-${now()}"
        var total = CrmImportResult(viewId = viewId)
        var offset = 0
        split.parts.forEachIndexed { part, chunk ->
            progress(part, split.parts.size)
            val batch = linkedMapOf<String, Any?>(
                "id" to importId, "part" to part, "parts" to split.parts.size, "offset" to offset, "rows" to split.rows,
                "created" to total.created, "updated" to total.updated, "skipped" to total.skipped,
            )
            val body = linkedMapOf<String, Any?>("csv" to chunk, "fileName" to fileName, "sheetName" to sheetName, "mapping" to mapping, "import" to batch)
            if (PackagesText.isHexId(total.viewId)) body["viewId"] = total.viewId
            try {
                val result = CrmImportResult.fromJson(client.importCrmContacts(organization, body))
                total = total.copy(
                    created = total.created + result.created, updated = total.updated + result.updated, skipped = total.skipped + result.skipped,
                    errors = total.errors + result.errors, viewId = if (PackagesText.isHexId(result.viewId)) result.viewId else total.viewId,
                )
            } catch (thrown: Exception) {
                errorText = PackagesException.describe(thrown)
                runCatching { client.importCrmContacts(organization, body + mapOf("csv" to "", "import" to batch + ("failed" to errorText))) }
                load()
                return null
            }
            offset += CrmCsv.split(chunk).rows
        }
        progress(split.parts.size, split.parts.size)
        errorText = ""
        load()
        if (PackagesText.isHexId(total.viewId)) tab = CrmTab.Sheet(total.viewId)
        notice = total.sentence
        return total
    }

    fun sharedView(view: CrmView): Pair<CrmView, List<CrmContact>>? = try {
        client.crmSharedView(view.id)
    } catch (thrown: Exception) {
        errorText = PackagesException.describe(thrown)
        null
    }

    fun copySharedView(view: CrmView): CrmImportResult? {
        val result = perform { client.copyCrmSharedView(organization, view.id) } ?: return null
        load()
        if (PackagesText.isHexId(result.viewId)) tab = CrmTab.Sheet(result.viewId)
        notice = "Copied ${view.name}: ${result.sentence}"
        return result
    }

    fun updateSharedContact(view: CrmView, contact: CrmContact, changes: Map<String, Any?>): CrmContact? =
        perform { client.updateCrmSharedContact(view.id, contact.id, changes) }
}
