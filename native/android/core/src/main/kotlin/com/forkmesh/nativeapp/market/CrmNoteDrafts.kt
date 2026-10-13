package com.forkmesh.nativeapp.market

data class CrmNoteDraft(
    val contactId: String,
    val name: String,
    val to: String,
    val subject: String,
    val body: String,
    val thread: String,
    val mastodon: Boolean,
) {
    val canSend: Boolean get() = if (mastodon) thread.isNotEmpty() else '@' in to

    val kindTitle: String get() = if (mastodon) "Mastodon reply" else "Email"

    fun emailDraft(contact: CrmContact?): CrmEmailDraft =
        CrmEmailDraft(to = to.ifEmpty { contact?.email.orEmpty() }, subject = subject, body = body, contactId = contactId, noteDraft = true)

    companion object {
        const val MARK = "DRAFT, not sent."
        private val STATUS = Regex("https://[A-Za-z0-9.-]+/@[A-Za-z0-9_.]+/[0-9]{6,}")

        fun markAt(notes: String): Int {
            var at = notes.indexOf(MARK)
            while (at > 0 && notes[at - 1] != '\n') at = notes.indexOf(MARK, at + 1)
            return at
        }

        fun drafts(contacts: List<CrmContact>, activity: List<CrmActivity> = emptyList()): List<CrmNoteDraft> = contacts.mapNotNull { draft(it, activity) }

        fun draft(contact: CrmContact, activity: List<CrmActivity> = emptyList()): CrmNoteDraft? {
            val notes = contact.notes
            val at = markAt(notes)
            if (at < 0) return null
            val end = notes.indexOf("\n\n", at)
            val lines = notes.substring(at, if (end < 0) notes.length else end).split('\n').toMutableList()
            val mastodon = lines.removeAt(0).contains("mastodon", ignoreCase = true)
            var subject = ""
            if (lines.firstOrNull()?.startsWith("Subject:") == true) subject = lines.removeAt(0).removePrefix("Subject:").trim()
            val body = lines.joinToString("\n").trim()
            if (body.isEmpty()) return null
            var to = contact.email
            var thread = ""
            if (mastodon) {
                to = contact.mastodon
                thread = activity.firstOrNull { it.contactId == contact.id && it.provider == "mastodon" && it.providerUrl.startsWith("https://") }?.providerUrl
                    ?: STATUS.find(notes)?.value.orEmpty()
            }
            return CrmNoteDraft(contact.id, contact.name.ifEmpty { to }, to, subject, body, thread, mastodon)
        }

        fun markedSent(notes: String, day: String, by: String = ""): String? {
            val at = markAt(notes)
            if (at < 0) return null
            return notes.replaceRange(at, at + MARK.length, "DRAFT, sent $day" + (if (by.isEmpty()) "" else " by $by") + ".")
        }
    }
}

data class CrmZohoCode(val clientId: String = "", val clientSecret: String = "", val code: String = "", val region: String = "com") {
    val isComplete: Boolean get() = listOf(clientId, clientSecret, code).none { it.isBlank() }

    val body: Map<String, Any?>
        get() = linkedMapOf("clientId" to clientId.trim(), "clientSecret" to clientSecret.trim(), "code" to code.trim(), "region" to region)

    companion object {
        val REGIONS = listOf("com", "eu", "in", "com.au", "jp", "ca")
        const val CONSOLE_URL = "https://api-console.zoho.com/"
        const val SCOPES = "ZohoMail.messages.READ,ZohoMail.folders.READ,ZohoMail.accounts.READ"
    }
}
