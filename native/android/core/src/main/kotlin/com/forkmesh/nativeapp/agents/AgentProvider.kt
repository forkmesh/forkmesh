package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.net.HttpClient

data class AgentToolCall(val id: String, val name: String, val arguments: Map<String, Any?>)

class AgentTurn(
    val text: String,
    val toolCalls: List<AgentToolCall>,
    val raw: Any,
    val inputTokens: Int = 0,
    val outputTokens: Int = 0,
    val refusal: String? = null,
) {
    val wantsTools: Boolean get() = toolCalls.isNotEmpty()
}

class AgentProviderException(override val message: String, val statusCode: Int? = null) : RuntimeException(message) {
    val isRetryable: Boolean
        get() = statusCode == null || statusCode == 408 || statusCode == 409 || statusCode == 429 || statusCode in 500..599

    override fun toString(): String = message
}

interface AgentProvider {
    val id: String
    val model: String

    fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn

    fun appendAssistant(history: MutableList<Any>, turn: AgentTurn) {
        history.add(turn.raw)
    }

    fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>)

    fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment> = emptyList())
}

private fun int(value: Any?): Int = (value as? Number)?.toInt() ?: 0

private fun post(transport: HttpTransport, url: String, headers: Map<String, String>, body: Any?): HttpClient.Response {
    val request = HttpClient.Request("POST", url).json(body)
    for ((name, value) in headers) request.header(name, value)
    request.timeouts(20_000, 180_000)
    return transport.send(request)
}

private fun decode(response: HttpClient.Response, label: String): JsonMap {
    val decoded: JsonMap = try {
        Json.parse(String(response.body, Charsets.UTF_8)).asJsonMap()
    } catch (_: Exception) {
        emptyMap()
    }
    if (response.status !in 200..299) {
        val error = decoded["error"]
        val message = if (error is Map<*, *>) jsonText(error["message"] ?: "request failed") else "request failed"
        throw AgentProviderException("$label API error ${response.status}: $message", response.status)
    }
    return decoded
}

class AnthropicProvider(
    private val apiKey: String,
    override val model: String = DEFAULT_MODEL,
    private val maxTokens: Int = 16000,
    private val baseUrl: String = "https://api.anthropic.com/v1/messages",
) : AgentProvider {
    companion object {
        const val DEFAULT_MODEL = "claude-opus-5-5"
        const val API_VERSION = "2023-06-01"
        private const val SERVER_SIDE_FALLBACK_BETA = "server-side-fallback-2026-07-01"
        private val FALLBACK_MODELS = setOf("claude-opus-5", "claude-fable-5")
    }

    override val id: String get() = "claude-api"

    fun requestBody(systemPrompt: String, history: List<Any>, tools: List<AgentTool>): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>(
            "model" to model,
            "max_tokens" to maxTokens,
            "system" to systemPrompt,
            "messages" to history,
            "tools" to tools.map { linkedMapOf("name" to it.name, "description" to it.description, "input_schema" to it.schema) },
        )
        if (model in FALLBACK_MODELS) body["fallbacks"] = "default"
        return body
    }

    override fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn {
        val headers = linkedMapOf("x-api-key" to apiKey, "anthropic-version" to API_VERSION)
        if (model in FALLBACK_MODELS) headers["anthropic-beta"] = SERVER_SIDE_FALLBACK_BETA
        return parse(decode(post(transport, baseUrl, headers, requestBody(systemPrompt, history, tools)), "Claude"))
    }

    fun parse(decoded: JsonMap): AgentTurn {
        val blocks = decoded["content"] as? List<*> ?: emptyList<Any?>()
        val text = StringBuilder()
        val calls = ArrayList<AgentToolCall>()
        for (block in blocks) {
            val map = block as? Map<*, *> ?: continue
            when (map["type"]) {
                "text" -> text.append(jsonText(map["text"]))
                "tool_use" -> calls.add(AgentToolCall(jsonText(map["id"]), jsonText(map["name"]), map["input"].asJsonMap()))
            }
        }
        val usage = decoded["usage"] as? Map<*, *>
        return AgentTurn(
            text = text.toString(),
            toolCalls = calls,
            raw = linkedMapOf("role" to "assistant", "content" to blocks.filterIsInstance<Map<*, *>>()),
            inputTokens = int(usage?.get("input_tokens")),
            outputTokens = int(usage?.get("output_tokens")),
            refusal = if (decoded["stop_reason"] == "refusal") refusalMessage(decoded["stop_details"]) else null,
        )
    }

    private fun refusalMessage(details: Any?): String {
        if (details is Map<*, *>) {
            val explanation = jsonText(details["explanation"]).trim()
            if (explanation.isNotEmpty()) return explanation
            val category = jsonText(details["category"]).trim()
            if (category.isNotEmpty()) return "The model declined this request ($category)."
        }
        return "The model declined this request."
    }

    override fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>) {
        history.add(
            linkedMapOf(
                "role" to "user",
                "content" to results.map { result ->
                    val item = linkedMapOf<String, Any?>("type" to "tool_result", "tool_use_id" to result.callId, "content" to result.output)
                    if (result.isError) item["is_error"] = true
                    item
                },
            ),
        )
    }

    override fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment>) {
        if (images.isEmpty()) {
            history.add(linkedMapOf("role" to "user", "content" to text))
            return
        }
        val content = ArrayList<Any>()
        for (image in images) {
            content.add(
                linkedMapOf(
                    "type" to "image",
                    "source" to linkedMapOf("type" to "base64", "media_type" to image.mediaType, "data" to image.base64Data),
                ),
            )
        }
        content.add(linkedMapOf("type" to "text", "text" to text))
        history.add(linkedMapOf("role" to "user", "content" to content))
    }
}

class OpenAiProvider(
    private val apiKey: String,
    override val model: String = DEFAULT_MODEL,
    private val maxTokens: Int = 16000,
    private val baseUrl: String = "https://api.openai.com/v1/chat/completions",
) : AgentProvider {
    companion object {
        const val DEFAULT_MODEL = "gpt-5.3-codex"
    }

    override val id: String get() = "openai"

    fun requestBody(systemPrompt: String, history: List<Any>, tools: List<AgentTool>): Map<String, Any?> = linkedMapOf(
        "model" to model,
        "max_completion_tokens" to maxTokens,
        "messages" to listOf<Any>(linkedMapOf("role" to "system", "content" to systemPrompt)) + history,
        "tools" to tools.map {
            linkedMapOf(
                "type" to "function",
                "function" to linkedMapOf("name" to it.name, "description" to it.description, "parameters" to it.schema),
            )
        },
    )

    override fun send(systemPrompt: String, history: List<Any>, tools: List<AgentTool>, transport: HttpTransport): AgentTurn {
        val headers = mapOf("authorization" to "Bearer $apiKey")
        return parse(decode(post(transport, baseUrl, headers, requestBody(systemPrompt, history, tools)), "OpenAI"))
    }

    fun parse(decoded: JsonMap): AgentTurn {
        val choices = decoded["choices"] as? List<*>
        val message = (choices?.firstOrNull() as? Map<*, *>)?.get("message") as? Map<*, *>
        val calls = ArrayList<AgentToolCall>()
        for (call in message?.get("tool_calls") as? List<*> ?: emptyList<Any?>()) {
            val map = call as? Map<*, *> ?: continue
            val function = map["function"] as? Map<*, *> ?: continue
            val args = try {
                Json.parse(jsonText(function["arguments"] ?: "{}")).asJsonMap()
            } catch (_: Exception) {
                emptyMap()
            }
            calls.add(AgentToolCall(jsonText(map["id"]), jsonText(function["name"]), args))
        }
        val usage = decoded["usage"] as? Map<*, *>
        return AgentTurn(
            text = if (message != null) jsonText(message["content"]) else "",
            toolCalls = calls,
            raw = message?.let { LinkedHashMap(it.asJsonMap()) } ?: linkedMapOf("role" to "assistant", "content" to ""),
            inputTokens = int(usage?.get("prompt_tokens")),
            outputTokens = int(usage?.get("completion_tokens")),
            refusal = message?.get("refusal") as? String,
        )
    }

    override fun appendToolResults(history: MutableList<Any>, results: List<AgentToolResult>) {
        for (result in results) {
            history.add(linkedMapOf("role" to "tool", "tool_call_id" to result.callId, "content" to result.output))
        }
    }

    override fun appendUser(history: MutableList<Any>, text: String, images: List<PromptAttachment>) {
        if (images.isEmpty()) {
            history.add(linkedMapOf("role" to "user", "content" to text))
            return
        }
        val content = ArrayList<Any>()
        content.add(linkedMapOf("type" to "text", "text" to text))
        for (image in images) {
            content.add(
                linkedMapOf(
                    "type" to "image_url",
                    "image_url" to linkedMapOf("url" to "data:${image.mediaType};base64,${image.base64Data}"),
                ),
            )
        }
        history.add(linkedMapOf("role" to "user", "content" to content))
    }
}
