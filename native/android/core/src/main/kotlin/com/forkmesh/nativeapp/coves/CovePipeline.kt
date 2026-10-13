package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.json.Json

data class CovePipelineStage(val id: String, val name: String, val color: String)

data class CovePipelineLead(
    val id: String,
    val name: String,
    val company: String = "",
    val value: Double = 0.0,
    val owner: String = "",
    val stage: String,
    val updatedAt: String = "",
)

data class CovePipelineTotal(val stage: CovePipelineStage, val count: Int, val value: Double)

class CovePipelineException(message: String) : Exception(message)

class CovePipeline private constructor(
    private val root: Map<String, Any?>,
    val stages: List<CovePipelineStage>,
    leads: List<CovePipelineLead>,
    history: List<Any?>,
) {
    var leads: List<CovePipelineLead> = leads
        private set
    var history: List<Any?> = history
        private set

    fun stageName(id: String): String = stages.firstOrNull { it.id == id }?.name ?: id

    val body: String
        get() {
            val out = LinkedHashMap(root)
            out["stages"] = stages.map { linkedMapOf("color" to it.color, "id" to it.id, "name" to it.name) }
            out["leads"] = leads.map {
                linkedMapOf(
                    "company" to it.company, "id" to it.id, "name" to it.name, "owner" to it.owner,
                    "stage" to it.stage, "updatedAt" to it.updatedAt, "value" to it.value,
                )
            }
            out["history"] = history
            out["version"] = 1L
            return CoveJson.pretty(out) + "\n"
        }

    private fun record(lead: CovePipelineLead, from: String, to: String, actor: String, action: String, nowMs: Long) {
        history = history + linkedMapOf(
            "action" to action, "at" to CoveFiles.isoNow(nowMs), "by" to actor, "from" to from,
            "lead" to lead.name.trim(), "leadId" to lead.id, "to" to to,
        )
    }

    fun addLead(name: String = "New lead", actor: String, nowMs: Long = System.currentTimeMillis(), id: String = CoveStore.newId()): CovePipelineLead {
        val stage = stages.firstOrNull()?.id.orEmpty()
        val lead = CovePipelineLead(id = id, name = name, owner = actor, stage = stage, updatedAt = CoveFiles.isoNow(nowMs))
        leads = leads + lead
        record(lead, "", stage, actor, "created", nowMs)
        return lead
    }

    fun move(leadId: String, to: String, actor: String, nowMs: Long = System.currentTimeMillis()) {
        val index = leads.indexOfFirst { it.id == leadId }
        if (index < 0 || stages.none { it.id == to }) return
        val from = leads[index].stage
        if (from == to) return
        val moved = leads[index].copy(stage = to, updatedAt = CoveFiles.isoNow(nowMs))
        leads = leads.toMutableList().also { it[index] = moved }
        record(moved, from, to, actor, "moved", nowMs)
    }

    fun update(lead: CovePipelineLead, nowMs: Long = System.currentTimeMillis()) {
        val index = leads.indexOfFirst { it.id == lead.id }
        if (index < 0) return
        val updated = lead.copy(stage = leads[index].stage, updatedAt = CoveFiles.isoNow(nowMs))
        leads = leads.toMutableList().also { it[index] = updated }
    }

    fun delete(leadId: String, actor: String, nowMs: Long = System.currentTimeMillis()) {
        val index = leads.indexOfFirst { it.id == leadId }
        if (index < 0) return
        val lead = leads[index]
        leads = leads.toMutableList().also { it.removeAt(index) }
        record(lead, lead.stage, "", actor, "deleted", nowMs)
    }

    fun totals(): List<CovePipelineTotal> = stages.map { stage ->
        val inStage = leads.filter { it.stage == stage.id }
        CovePipelineTotal(stage, inStage.size, inStage.sumOf { it.value })
    }

    companion object {
        fun parse(body: String): CovePipeline {
            val value = try {
                Json.parse(body)
            } catch (_: Exception) {
                throw CovePipelineException("Invalid pipeline JSON.")
            }
            val obj = (value as? Map<*, *>)?.entries?.associate { it.key.toString() to it.value }?.let { LinkedHashMap(it) }
                ?: throw CovePipelineException("The pipeline root must be a JSON object.")
            val rawStages = obj["stages"] as? List<*> ?: emptyList<Any?>()
            if (rawStages.isEmpty()) throw CovePipelineException("A pipeline needs at least one stage.")
            val seen = HashSet<String>()
            val stages = rawStages.map { raw ->
                val stage = raw as? Map<*, *> ?: emptyMap<String, Any?>()
                val id = (stage["id"] as? String).orEmpty().trim()
                val name = (stage["name"] as? String).orEmpty().trim()
                if (id.isEmpty() || name.isEmpty() || !seen.add(id)) {
                    throw CovePipelineException("Every pipeline stage needs a unique id and name.")
                }
                CovePipelineStage(id, name, (stage["color"] as? String).orEmpty())
            }
            if (obj.containsKey("leads") && obj["leads"] !is List<*>) throw CovePipelineException("Pipeline leads and history must be JSON arrays.")
            if (obj.containsKey("history") && obj["history"] !is List<*>) throw CovePipelineException("Pipeline leads and history must be JSON arrays.")
            if (!obj.containsKey("leads")) obj["leads"] = emptyList<Any?>()
            if (!obj.containsKey("history")) obj["history"] = emptyList<Any?>()
            if (!obj.containsKey("version")) obj["version"] = 1L
            val leads = (obj["leads"] as List<*>).mapNotNull { it as? Map<*, *> }.map { lead ->
                CovePipelineLead(
                    id = (lead["id"] as? String).orEmpty(),
                    name = (lead["name"] as? String).orEmpty(),
                    company = (lead["company"] as? String).orEmpty(),
                    value = (lead["value"] as? Number)?.toDouble() ?: 0.0,
                    owner = (lead["owner"] as? String).orEmpty(),
                    stage = (lead["stage"] as? String).orEmpty(),
                    updatedAt = (lead["updatedAt"] as? String).orEmpty(),
                )
            }
            return CovePipeline(obj, stages, leads, obj["history"] as List<Any?>)
        }
    }
}
