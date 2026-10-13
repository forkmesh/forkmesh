package com.forkmesh.nativeapp.control

data class AgentStopAllOutcome(val stopped: Int, val failed: Int, val error: String = "") {
    val message: String
        get() {
            if (stopped == 0 && failed == 0) return "No agents are running."
            if (failed == 0) return "Asked ${AgentStopAll.count(stopped)} to stop."
            val reason = if (error.isEmpty()) "" else " $error"
            if (stopped == 0) return "Could not stop ${AgentStopAll.count(failed)}.$reason"
            return "Asked ${AgentStopAll.count(stopped)} to stop; $failed did not.$reason"
        }
}

object AgentStopAll {
    fun targets(rows: List<UnifiedAgentRow>): List<UnifiedAgentRow> = rows.filter { it.isActive && it.canStop && !it.outside }

    fun count(n: Int): String = if (n == 1) "1 agent" else "$n agents"

    fun question(n: Int): String = if (n == 1) "Stop the running agent?" else "Stop all $n running agents?"
}

fun AgentControlling.stopAll(): AgentStopAllOutcome {
    val targets = AgentStopAll.targets(rows)
    var stopped = 0
    var error = ""
    for (row in targets) {
        try {
            stop(row)
            stopped++
        } catch (failure: Exception) {
            if (error.isEmpty()) error = ControlErrorText.describe(failure)
        }
    }
    return AgentStopAllOutcome(stopped, targets.size - stopped, error)
}
