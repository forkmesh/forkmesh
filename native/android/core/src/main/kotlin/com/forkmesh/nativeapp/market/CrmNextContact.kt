package com.forkmesh.nativeapp.market

data class CrmLeadMarks(
    val rank: Double = 0.0,
    val rankField: String = "",
    val score: Double = 0.0,
    val scoreField: String = "",
    val value: Long = 0,
) {
    companion object {
        fun of(contact: CrmContact): CrmLeadMarks {
            var marks = CrmLeadMarks(value = maxOf(0L, contact.value))
            for ((key, raw) in contact.fields.toSortedMap()) {
                val field = key.lowercase()
                val number = raw.trim().toDoubleOrNull() ?: continue
                if (!number.isFinite() || number <= 0) continue
                if ((field == "rank" || field.endsWith("_rank")) && (marks.rank <= 0 || number < marks.rank)) {
                    marks = marks.copy(rank = number, rankField = key)
                }
                if ((field == "score" || field.endsWith("_score")) && number > marks.score) {
                    marks = marks.copy(score = number, scoreField = key)
                }
            }
            return marks
        }
    }
}

data class CrmNextContact(val contact: CrmContact? = null, val reason: String = "", val due: Int = 0) {
    enum class Mode(val title: String, val note: String) {
        REACH_OUT("Reach out", "Best new lead to contact"),
        FOLLOW_UP("Follow up", "Someone already in conversation"),
    }

    companion object {
        const val REPLY_DAYS = 60L
        const val FOLLOW_UP_DAYS = 14L
        val WON_STAGES = setOf("won", "closed-won", "customer", "renewal", "committed", "signed", "paid", "active")
        val ENDED_STAGES = setOf(
            "passed", "rejected", "closed", "inactive", "hired", "lost", "closed-lost", "churned", "declined",
            "unsubscribed", "do-not-contact",
        )
        val UNSENT_STATUSES = setOf("draft", "sending", "failed", "reject")
        private const val DAY_MS = 86_400_000L
        private val EMAIL = Regex("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$")

        fun hasEmail(contact: CrmContact): Boolean = EMAIL.matches(contact.email)

        fun reachable(contact: CrmContact): Boolean =
            hasEmail(contact) || CrmText.profileUrl("mastodon", contact.mastodon) != null || CrmText.profileUrl("twitter", contact.twitter) != null

        private fun ago(at: Long, now: Long): String {
            val days = (now - at) / DAY_MS
            return if (days <= 0) "today" else if (days == 1L) "yesterday" else "$days days ago"
        }

        private fun number(value: Double): String =
            if (value == Math.rint(value) && kotlin.math.abs(value) < 1e15) value.toLong().toString() else value.toString()

        private fun before(a: List<Double>, aId: String, b: List<Double>, bId: String): Boolean {
            for (index in a.indices) {
                if (a[index] != b[index]) return a[index] < b[index]
            }
            return aId < bId
        }

        fun pick(mode: Mode, contacts: List<CrmContact>, activity: List<CrmActivity>, skipped: Set<String> = emptySet(), now: Long): CrmNextContact {
            val linked = activity.filter { it.contactId.isNotEmpty() }.groupBy { it.contactId }
            var best: CrmContact? = null
            var bestReason = ""
            var bestKey = emptyList<Double>()
            var due = 0
            for (contact in contacts) {
                if (contact.id in skipped || !reachable(contact)) continue
                val last = contact.lastContactedAt
                var heardAt = 0L
                var wroteAt = 0L
                var heardSubject = ""
                for (message in linked[contact.id].orEmpty()) {
                    if (message.incoming) {
                        if (message.at > heardAt) {
                            heardAt = message.at
                            heardSubject = message.subject
                        }
                    } else if (message.status.lowercase() !in UNSENT_STATUSES) {
                        wroteAt = maxOf(wroteAt, message.at)
                    }
                }
                val stage = contact.stage.lowercase()
                val step = contact.nextStep.trim()
                val waiting = heardAt >= now - REPLY_DAYS * DAY_MS && heardAt > wroteAt && heardAt >= last
                val closed = stage in WON_STAGES || stage in ENDED_STAGES
                var reason: String
                val key: List<Double>
                when (mode) {
                    Mode.REACH_OUT -> {
                        if (waiting || closed || last > 0) continue
                        val marks = CrmLeadMarks.of(contact)
                        val email = hasEmail(contact)
                        val qualifies = listOfNotNull(
                            step.takeIf { it.isNotEmpty() }?.let { "next step on the record: $it" },
                            marks.value.takeIf { it > 0 }?.let { "deal value ${CrmText.money(it)}" },
                            marks.rank.takeIf { it > 0 }?.let { "ranked ${number(it)} (${marks.rankField})" },
                            marks.score.takeIf { it > 0 }?.let { "score ${number(it)} (${marks.scoreField})" },
                            "an email address on file".takeIf { email },
                        )
                        reason = (if (contact.createdAt > 0) "Added ${ago(contact.createdAt, now)} and never contacted. " else "Never contacted. ") +
                            if (qualifies.isEmpty()) {
                                "Nothing on the record sets a new lead apart, so the longest on the list goes first."
                            } else {
                                "Best qualified new lead: ${qualifies.joinToString("; ")}."
                            }
                        key = listOf(
                            if (step.isEmpty()) 1.0 else 0.0, -marks.value.toDouble(),
                            if (marks.rank > 0) marks.rank else Double.POSITIVE_INFINITY, -marks.score,
                            if (email) 0.0 else 1.0, contact.createdAt.toDouble(),
                        )
                    }
                    Mode.FOLLOW_UP -> {
                        if (waiting) {
                            reason = "They wrote to us ${ago(heardAt, now)}" +
                                (if (heardSubject.isEmpty()) "" else " (“$heardSubject”)") +
                                " and have had no answer yet, so the next move is ours."
                            key = listOf(0.0, if (step.isEmpty()) 1.0 else 0.0, -heardAt.toDouble())
                        } else if (!closed && last > 0 && now - last >= FOLLOW_UP_DAYS * DAY_MS) {
                            reason = "Last contacted ${ago(last, now)} and nothing has come back since; " +
                                "a follow-up is due after $FOLLOW_UP_DAYS quiet days."
                            key = listOf(1.0, if (step.isEmpty()) 1.0 else 0.0, last.toDouble())
                        } else {
                            continue
                        }
                        if (step.isNotEmpty()) reason += " Next step on the record: $step."
                    }
                }
                due += 1
                if (best == null || before(key, contact.id, bestKey, best.id)) {
                    best = contact
                    bestReason = reason
                    bestKey = key
                }
            }
            return CrmNextContact(best, bestReason, due)
        }
    }
}
