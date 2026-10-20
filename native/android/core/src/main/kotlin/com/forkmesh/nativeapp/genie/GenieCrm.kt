package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.market.createCrmDeckLink

data class GenieCrmOrganization(val data: CrmOrganizationData, val memberEmails: List<String> = emptyList()) {
    companion object {
        fun fromJson(json: JsonMap) = GenieCrmOrganization(
            CrmOrganizationData.fromJson(json),
            json.strings("memberEmails").filter { it.contains('@') },
        )
    }
}

class GenieOutreachProblem(message: String) : Exception(message)

class GenieCrm(private val client: PackagesClient, private val sender: () -> String = { "" }) {
    companion object {
        const val MOST_ORGANIZATIONS = 5
    }

    fun organizations(): List<GenieCrmOrganization>? {
        val names = runCatching { client.crmOrganizations().filter { it.installed }.map { it.name } }.getOrNull() ?: return null
        return names.take(MOST_ORGANIZATIONS).mapNotNull { name ->
            val tz = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000
            runCatching { GenieCrmOrganization.fromJson(client.call("GET", "/api/crm/" + client.segment(name), listOf("tz" to tz.toString()))) }.getOrNull()
        }
    }

    fun pick(skip: Set<String>, random: () -> Double = { Math.random() }): GenieOutreachPick? {
        val loaded = organizations() ?: return null
        return GenieOutreach.pick(loaded, skip, sender(), random)
    }

    fun best(skip: Set<String>, only: GenieOutreachChannel? = null, zohoOnly: Boolean = false): GenieOutreachPick? {
        val loaded = organizations() ?: return null
        val reachable = if (zohoOnly) loaded.filter { it.data.settings.zoho.connected } else loaded
        return GenieOutreach.best(reachable, skip, sender(), only)
    }

    fun deck(pick: GenieOutreachPick): String {
        try {
            val link = client.createCrmDeckLink(pick.organization, pick.contact.id)
            if (!link.active || !link.url.startsWith("https://")) throw GenieOutreachProblem("The CRM did not return a deck link.")
            return link.url
        } catch (error: PackagesException) {
            if (error.code == "deck_needs_investor_pipeline") {
                throw GenieOutreachProblem("Move ${pick.contact.displayName} to an investor pipeline first so they can get a deck link.")
            }
            throw GenieOutreachProblem(error.message ?: "The CRM refused the deck link.")
        }
    }

    fun autoEmail(organization: String, body: Map<String, Any?>): ApiJsonResult =
        client.transport.send("POST", "/api/crm/" + client.segment(organization) + "/outreach/auto", emptyList(), body)

    fun autoSend(pick: GenieOutreachPick, writers: List<GenieOutreachWriter>, stage: (GenieAutoSendStage) -> Unit = {}): GenieAutoSendOutcome =
        try {
            val body = linkedMapOf<String, Any?>("contactId" to client.item(pick.contact.id))
            stage(GenieAutoSendStage.Writing)
            var reply = autoEmail(pick.organization, body)
            val code = reply.body.str("error")
            val prompt = reply.body.str("prompt")
            if ((code == "ai_unavailable" || code == "draft_unusable") && prompt.isNotEmpty()) {
                val draft = GenieAutoSend.write(prompt, writers)
                if (draft != null) {
                    body["subject"] = draft.first
                    body["text"] = draft.second
                    body["model"] = draft.third
                    stage(GenieAutoSendStage.Sending)
                    reply = autoEmail(pick.organization, body)
                }
            }
            val outcome = GenieAutoSend.outcome(reply, pick)
            if (outcome is GenieAutoSendOutcome.Sent) stage(GenieAutoSendStage.Sent)
            outcome
        } catch (error: Exception) {
            GenieAutoSendOutcome.Pause(PackagesException.describe(error))
        }

    fun rate(receipt: GenieAutoSendReceipt, rating: Int, note: String) {
        client.call(
            "POST",
            "/api/crm/" + client.segment(receipt.organization) + "/outreach/rate",
            body = linkedMapOf("emailId" to client.item(receipt.id), "rating" to rating, "note" to note),
        )
    }
}
