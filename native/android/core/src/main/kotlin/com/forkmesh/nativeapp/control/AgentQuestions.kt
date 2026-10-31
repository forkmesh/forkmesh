package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

data class AgentQuestionOption(val label: String, val detail: String) {
    companion object {
        fun fromJson(json: JsonMap): AgentQuestionOption =
            AgentQuestionOption(json.str("label"), json.str("description"))
    }
}

data class AgentQuestionField(
    val id: String,
    val title: String,
    val multiSelect: Boolean,
    val isSecret: Boolean,
    val options: List<AgentQuestionOption>,
) {
    companion object {
        fun fromJson(json: JsonMap, index: Int): AgentQuestionField = AgentQuestionField(
            id = index.toString(),
            title = json.str("question", json.str("header")),
            multiSelect = json.bool("multiSelect"),
            isSecret = json.bool("isSecret"),
            options = json.maps("options").map(AgentQuestionOption::fromJson),
        )
    }
}

data class AgentQuestion(val id: String, val fields: List<AgentQuestionField>) {
    fun answer(selections: Map<String, Set<String>>, others: Map<String, String>): String {
        val lines = fields.mapNotNull { field ->
            val values = selections[field.id].orEmpty().toMutableList()
            others[field.id]?.trim()?.takeIf { it.isNotEmpty() }?.let { values.add(it) }
            if (values.isEmpty()) return@mapNotNull null
            val value = values.sorted().joinToString(", ")
            if (fields.size == 1) value else "${field.title} → $value"
        }
        return if (lines.size == fields.size) lines.joinToString("\n") else ""
    }

    companion object {
        const val COMPOSER_HINT = "Type your answer for the agent"

        fun fromJson(json: JsonMap): AgentQuestion? {
            val id = json.str("id")
            val fields = json.obj("input").maps("questions").mapIndexed { index, field -> AgentQuestionField.fromJson(field, index) }
            return if (id.isEmpty() || fields.isEmpty()) null else AgentQuestion(id, fields)
        }

        fun listFromJson(json: JsonMap): List<AgentQuestion> = json.maps("questions").mapNotNull(::fromJson)
    }
}
