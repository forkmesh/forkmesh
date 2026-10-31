package com.forkmesh.nativeapp.market

enum class CrmSection(val title: String, val icon: String) {
    HOME("Home", "home"),
    CONTACTS("Contacts", "people"),
    PIPELINE("Pipeline", "view_week"),
    ACTIVITY("Activity", "history"),
    MORE("More", "more_horiz");

    companion object {
        fun of(tab: CrmTab): CrmSection = if (tab.isContacts) CONTACTS else ACTIVITY
    }
}

enum class CrmDealState {
    OPEN, WON, ENDED;

    companion object {
        fun of(stage: String): CrmDealState = when (stage) {
            in CrmNextContact.WON_STAGES -> WON
            in CrmNextContact.ENDED_STAGES -> ENDED
            else -> OPEN
        }
    }
}

data class CrmDashboard(
    val contacts: Int = 0,
    val newThisWeek: Int = 0,
    val openDeals: Int = 0,
    val openValue: Long = 0,
    val wonDeals: Int = 0,
    val wonValue: Long = 0,
    val neverContacted: Int = 0,
    val quiet: Int = 0,
) {
    val toContact: Int get() = neverContacted + quiet

    companion object {
        private const val DAY_MS = 86_400_000L

        fun of(rows: List<CrmContact>, now: Long): CrmDashboard {
            var newThisWeek = 0
            var openDeals = 0
            var openValue = 0L
            var wonDeals = 0
            var wonValue = 0L
            var neverContacted = 0
            var quiet = 0
            for (contact in rows) {
                if (contact.createdAt > 0 && now - contact.createdAt <= 7 * DAY_MS) newThisWeek++
                when (CrmDealState.of(contact.stage)) {
                    CrmDealState.WON -> {
                        wonDeals++
                        wonValue += contact.value
                    }
                    CrmDealState.OPEN -> {
                        if (contact.value > 0) {
                            openDeals++
                            openValue += contact.value
                        }
                        if (contact.lastContactedAt <= 0) {
                            neverContacted++
                        } else if (now - contact.lastContactedAt >= CrmNextContact.FOLLOW_UP_DAYS * DAY_MS) {
                            quiet++
                        }
                    }
                    CrmDealState.ENDED -> Unit
                }
            }
            return CrmDashboard(rows.size, newThisWeek, openDeals, openValue, wonDeals, wonValue, neverContacted, quiet)
        }

        fun greeting(hour: Int): String = when {
            hour < 5 || hour >= 18 -> "Good evening"
            hour < 12 -> "Good morning"
            else -> "Good afternoon"
        }

        fun pipelineFlow(data: CrmOrganizationData, active: CrmFlow?): CrmFlow? {
            if (active != null) return active
            val counts = data.contacts.groupingBy { it.flow }.eachCount()
            return data.flows.maxByOrNull { counts[it.id] ?: 0 }
        }
    }
}
