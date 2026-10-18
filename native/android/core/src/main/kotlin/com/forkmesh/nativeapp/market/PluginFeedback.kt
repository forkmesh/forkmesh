package com.forkmesh.nativeapp.market

data class PluginFeedback(val stars: Int = 0, val text: String = "", val improvement: String = "") {
    val rates: Boolean get() = stars in 1..5
    val reports: Boolean get() = text.isNotBlank()
    val canSend: Boolean get() = rates || reports
    val canAskAgent: Boolean get() = reports

    fun asksImprovement(rules: ReviewRules): Boolean = rates && stars < rules.reviewRequiredBelow

    fun review(base: ReviewDraft, rules: ReviewRules): ReviewDraft? {
        if (!rates) return null
        if (!asksImprovement(rules)) return base.copy(stars = stars)
        return base.copy(
            stars = stars,
            problem = if (reports) text.trim() else base.problem,
            improvement = improvement.trim().ifEmpty { base.improvement },
        )
    }

    fun agentPrompt(name: String, packageId: String, screen: String, device: String = "Android"): String = buildString {
        append("Feedback on the ForkMesh plugin $name (package `$packageId`), sent from the $screen screen of the $device app:\n\n")
        append(text.trim())
        if (rates) append("\n\nRated $stars of 5 stars.")
        append("\n\nFind the cause in this repository and fix it, or say what you would change.")
    }

    companion object {
        const val HINT = "What works, what is missing, or what went wrong?"
        const val IMPROVEMENT_HINT = "What would make it five stars?"
        const val SEND = "Send"
        const val SENDING = "Sending…"
        const val AGENT = "Send to agent"
        const val NOTHING_TO_SEND = "Tap a star or write something first."

        fun thanks(name: String, rated: Boolean, reported: Boolean): String = when {
            rated && reported -> "Thanks — your rating and feedback on $name reached the ForkMesh team."
            rated -> "Thanks for rating $name."
            else -> "Thanks — your feedback on $name reached the ForkMesh team."
        }
    }
}
