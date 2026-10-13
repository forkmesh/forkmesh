package com.forkmesh.nativeapp.hosts

data class AddHostForm(
    val options: HostCreateOptions,
    val name: String = "",
    val planId: String = initialPlan(options),
    val region: String = defaultRegion(options, initialPlan(options)),
) {
    val plan: HostPlanChoice? get() = options.plans.firstOrNull { it.id == planId }

    val regions: List<String> get() = plan?.regions ?: emptyList()

    fun choosePlan(id: String): AddHostForm {
        val next = copy(planId = id)
        return if (region in next.regions) next else next.copy(region = defaultRegion(options, id))
    }

    val effectiveName: String get() = name.trim().ifEmpty { options.suggestedName }

    val problem: String?
        get() {
            if (!options.vultr) return "The desktop has no Vultr API key. Add one on the desktop's Hosts page."
            if (options.plans.isEmpty()) return "Vultr offers no plan a mirror can run on right now."
            if (plan == null) return "Pick a plan."
            if (region.isEmpty()) return "Pick a region."
            val typed = name.trim()
            if (typed.isNotEmpty() && !validName(typed)) {
                return "Use letters, digits, dots, dashes or underscores, starting with a letter or digit."
            }
            return null
        }

    val requestedPlan: String get() = if (planId == options.defaultPlan) "" else planId

    val requestedRegion: String
        get() = if (planId == options.defaultPlan && region == options.defaultRegion) "" else region

    companion object {
        private fun isAsciiAlnum(c: Char): Boolean = c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9'

        fun validName(name: String): Boolean {
            if (name.isEmpty() || name.length > 63) return false
            if (!isAsciiAlnum(name[0])) return false
            return name.all { isAsciiAlnum(it) || it == '.' || it == '_' || it == '-' }
        }

        private fun initialPlan(options: HostCreateOptions): String =
            options.defaultPlan.ifEmpty { options.plans.firstOrNull()?.id ?: "" }

        private fun defaultRegion(options: HostCreateOptions, id: String): String {
            val regions = options.plans.firstOrNull { it.id == id }?.regions ?: emptyList()
            if (id == options.defaultPlan && options.defaultRegion in regions) return options.defaultRegion
            return regions.firstOrNull() ?: ""
        }
    }
}
