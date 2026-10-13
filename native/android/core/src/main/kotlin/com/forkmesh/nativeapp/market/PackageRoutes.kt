package com.forkmesh.nativeapp.market

enum class LedgerSection(val wire: String, val title: String) {
    RECORDS("records", "Public ledger"),
    REWARDS("rewards", "Rewards"),
    TIPS("tips", "Send"),
    MINE("mine", "My records"),
    TREASURY("treasury", "Treasury");

    companion object {
        fun of(route: String?): LedgerSection = entries.firstOrNull { it.wire == route?.lowercase() } ?: RECORDS
    }
}

enum class MarketingSection(val wire: String, val title: String) {
    OVERVIEW("overview", "Overview"),
    PLAN("plan", "Content"),
    CAMPAIGNS("campaigns", "Campaigns"),
    CHANNELS("channels", "Channels"),
    ADS("ads", "Ads"),
    GROWTH("growth", "Growth"),
    REDDIT("reddit", "Reddit"),
    PERFORMANCE("performance", "Performance");

    companion object {
        fun of(route: String?): MarketingSection = when (val value = route?.lowercase()) {
            "dashboard" -> OVERVIEW
            "content" -> PLAN
            "ads-manager" -> ADS
            "reddit-desk" -> REDDIT
            else -> entries.firstOrNull { it.wire == value } ?: OVERVIEW
        }
    }
}

data class PackageHubRow(val id: String, val title: String, val destination: String)

data class PipelineTarget(val organization: String?, val flow: String?)

object PackageRoutes {
    val BUSINESS_PACKAGE_IDS = listOf("crm", "marketing", "analytics", "competitor", "ledger")

    fun destination(pkg: MarketPackage): String? = (pkg.phoneSurface as? PackagePhoneSurface.Native)?.destination

    fun hubRows(installed: Set<String>): List<PackageHubRow> {
        val rows = mutableListOf(PackageHubRow("market", "Market", "market"), PackageHubRow("crm", "CRM", "crm"))
        for ((id, title, destination) in listOf(
            Triple("marketing", "Marketing", "marketing"),
            Triple("analytics", "Analytics", "analytics"),
            Triple("competitor", "Competitors", "competitors"),
        )) if (id in installed) rows.add(PackageHubRow(id, title, destination))
        rows.add(PackageHubRow("ads", "Ads", "ads"))
        rows.add(PackageHubRow("ledger", "Ledger & tips", "ledger"))
        return rows
    }

    fun organizationFromPipeline(pipeline: String?): PipelineTarget {
        if (pipeline.isNullOrEmpty()) return PipelineTarget(null, null)
        val parts = pipeline.split('/', limit = 2)
        if (parts.size == 2 && PackagesText.isOrgName(parts[0])) return PipelineTarget(parts[0], parts[1].ifEmpty { null })
        return PipelineTarget(null, pipeline)
    }
}
