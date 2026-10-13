package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.net.HttpClient

open class AgentQueueApi(
    private val origin: () -> String,
    private val sessionToken: () -> String,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val readOwner: (String, String) -> String = { owner, _ -> owner },
) {
    companion object {
        const val MAX_AGENT_PROMPT_CHARS = 8000

        fun from(api: ApiService, transport: HttpTransport = HttpTransport.DEFAULT): AgentQueueApi =
            AgentQueueApi({ api.gitOrigin }, { api.sessionTokenProvider?.invoke() ?: "" }, transport, api::readOwner)
    }

    val hasSession: Boolean get() = sessionToken().isNotEmpty()

    fun routeOwner(owner: String, repo: String, orgScope: Boolean = false): String =
        (if (orgScope) readOwner(owner, repo) else owner).trim()

    fun sessionsPath(owner: String, repo: String, sessionId: String = "", orgScope: Boolean = false): String {
        val base = if (orgScope) {
            "/api/orgs/${seg(routeOwner(owner, repo, orgScope))}/repos/${seg(repo)}/agent-bots"
        } else {
            "/api/repo/${seg(routeOwner(owner, repo, orgScope))}/${seg(repo)}/agent-sessions"
        }
        return base + if (sessionId.isEmpty()) "" else "/" + DartUri.encodeComponent(sessionId)
    }

    private fun seg(value: String): String = DartUri.encodeComponent(value.trim())

    open fun launchOptions(owner: String, repo: String): AgentLaunchOptions {
        if (!hasSession) return AgentLaunchOptions()
        try {
            return AgentLaunchOptions.fromJson(agentJson("GET", sessionsPath(owner, repo)))
        } catch (refusal: AgentLaunchException) {
            if (refusal.status != 403 && refusal.status != 404) throw refusal
            try {
                return AgentLaunchOptions.fromJson(agentJson("GET", sessionsPath(owner, repo, orgScope = true)), orgScope = true)
            } catch (_: AgentLaunchException) {
                throw refusal
            }
        }
    }

    open fun launch(
        owner: String,
        repo: String,
        prompt: String,
        provider: String = "auto",
        model: String = "",
        targetNode: String = "",
        title: String = "",
        createPr: Boolean = false,
        issueNumber: Int = 0,
        orgScope: Boolean = false,
        attachments: List<PromptAttachment> = emptyList(),
        baseBranch: String = "",
        branchName: String = "",
    ): AgentQueueResult {
        val text = promptOrPhotoNote(prompt, attachments)
        if (text.isEmpty()) throw AgentLaunchException("Write what the agent should do.")
        PromptAttachmentLimits.check(attachments)
        if (!hasSession) throw AgentLaunchException("Sign in before starting an agent.", "invalid_session", 401)
        if (text.length > MAX_AGENT_PROMPT_CHARS) {
            throw AgentLaunchException("That prompt is longer than the relay accepts. Trim it and try again.")
        }
        val body = linkedMapOf<String, Any?>("provider" to provider, "prompt" to text)
        if (model.trim().isNotEmpty()) body["model"] = model.trim()
        if (targetNode.trim().isNotEmpty()) body["targetNode"] = targetNode.trim()
        if (title.trim().isNotEmpty()) body["title"] = title.trim()
        body["createPr"] = createPr
        if (issueNumber > 0) body["issueNumber"] = issueNumber
        if (baseBranch.trim().isNotEmpty()) body["baseBranch"] = baseBranch.trim()
        if (branchName.trim().isNotEmpty()) body["branchName"] = branchName.trim()
        if (attachments.isNotEmpty()) body["attachments"] = attachments.map { it.toWire() }
        return AgentQueueResult.fromJson(agentJson("POST", sessionsPath(owner, repo, orgScope = orgScope), body))
    }

    open fun steer(
        owner: String,
        repo: String,
        sessionId: String,
        prompt: String,
        orgScope: Boolean = false,
        attachments: List<PromptAttachment> = emptyList(),
    ): AgentQueueResult {
        val text = promptOrPhotoNote(prompt, attachments)
        if (text.isEmpty()) throw AgentLaunchException("Write what the agent should do next.")
        PromptAttachmentLimits.check(attachments)
        val body = linkedMapOf<String, Any?>("prompt" to text)
        if (attachments.isNotEmpty()) body["attachments"] = attachments.map { it.toWire() }
        return AgentQueueResult.fromJson(agentJson("POST", sessionsPath(owner, repo, sessionId, orgScope), body))
    }

    open fun retry(owner: String, repo: String, session: AgentLaunchSession): AgentRetryResult {
        val prompt = session.retryPrompt
        if (prompt.isEmpty()) {
            throw AgentLaunchException(
                "This session did not keep the task it was given, so there is nothing to run again. Start a new one.",
            )
        }
        if (session.canResume) {
            return AgentRetryResult(steer(owner, repo, session.sessionId, prompt, session.orgScope), resumed = true)
        }
        return AgentRetryResult(
            launch(
                owner,
                repo,
                prompt = prompt,
                provider = session.provider.ifEmpty { "auto" },
                model = session.requestedModel,
                targetNode = session.targetNode,
                title = session.title,
                createPr = session.createPr,
                issueNumber = session.issueNumber,
                orgScope = session.orgScope,
            ),
            resumed = false,
        )
    }

    open fun cancel(owner: String, repo: String, sessionId: String, orgScope: Boolean = false) {
        agentJson("DELETE", sessionsPath(owner, repo, sessionId, orgScope))
    }

    open fun stop(owner: String, repo: String, sessionId: String, orgScope: Boolean = false): AgentStopResult =
        AgentStopResult.fromJson(agentJson("POST", sessionsPath(owner, repo, sessionId, orgScope), mapOf("action" to "stop")))

    open fun runnerDiff(owner: String, repo: String, sessionId: String, orgScope: Boolean, page: Int, pageSize: Int): JsonMap =
        agentJson("GET", sessionsPath(owner, repo, sessionId, orgScope) + "/diff?page=$page&pageSize=$pageSize")

    open fun sessionAction(owner: String, repo: String, sessionId: String, orgScope: Boolean, body: Map<String, Any?>): Pair<Int, JsonMap> =
        exchange("POST", sessionsPath(owner, repo, sessionId, orgScope), body)

    open fun accountSessions(): AgentAccountSessions {
        if (!hasSession) return AgentAccountSessions()
        return AgentAccountSessions.fromJson(agentJson("GET", "/api/account/agent-sessions"))
    }

    open fun session(owner: String, repo: String, sessionId: String, orgScope: Boolean = false): AgentLaunchSession =
        AgentLaunchSession.fromJson(agentJson("GET", sessionsPath(owner, repo, sessionId, orgScope)).obj("session"), orgScope)

    open fun liveRequest(owner: String, repo: String, sessionId: String, orgScope: Boolean = false): HttpClient.Request =
        HttpClient.Request("GET", origin().trimEnd('/') + sessionsPath(owner, repo, sessionId, orgScope) + "/live")
            .header("Accept", AgentLiveStream.CONTENT_TYPE)
            .header("Cache-Control", "no-cache")
            .bearer(sessionToken())
            .timeouts(15_000, AgentLiveStream.READ_TIMEOUT_MS)

    open fun live(
        owner: String,
        repo: String,
        sessionId: String,
        orgScope: Boolean = false,
        connector: AgentLiveConnector = AgentLiveConnector.DEFAULT,
        onEvent: (AgentLiveEvent) -> Unit,
    ): AgentLiveStream = AgentLiveStream({ liveRequest(owner, repo, sessionId, orgScope) }, connector, onEvent)

    open fun attachment(owner: String, repo: String, sessionId: String, attachmentId: String, orgScope: Boolean = false): ByteArray {
        if (!Regex(AgentAttachmentMeta.HEX_ID_PATTERN).matches(attachmentId)) {
            throw AgentAttachmentError("This attachment has no valid id.")
        }
        if (!hasSession) throw AgentAttachmentError("Sign in to see this photo.")
        val path = sessionsPath(owner, repo, sessionId, orgScope) + "/attachments/" + attachmentId
        val response = send("GET", path, mapOf("Accept" to "image/*"), null)
        when (response.status) {
            in 200..299 -> {
                AgentAttachmentMedia.checkImage(response.body)
                return response.body
            }
            401 -> throw AgentAttachmentError("Sign in again to see this photo.")
            403 -> throw AgentAttachmentError("Only the person who sent this photo can open it.")
            404 -> throw AgentAttachmentError("This relay does not keep photos for transcripts.")
            410 -> throw AgentAttachmentError("The relay no longer keeps this photo (they are kept for a week).")
            else -> throw AgentAttachmentError("The relay could not send this photo (HTTP ${response.status}).")
        }
    }

    private fun send(method: String, path: String, headers: Map<String, String>, body: ByteArray?): HttpClient.Response {
        val request = HttpClient.Request(method, origin().trimEnd('/') + path)
        for ((name, value) in headers) request.header(name, value)
        request.bearer(sessionToken())
        if (body != null) request.body = body
        request.timeouts(15_000, 30_000)
        return transport.send(request)
    }

    protected fun agentJson(method: String, path: String, body: Map<String, Any?>? = null): JsonMap {
        val (status, decoded) = exchange(method, path, body)
        if (status in 200..299) return decoded
        throw AgentLaunchException(agentMessage(decoded, status), jsonText(decoded["error"]), status)
    }

    private fun exchange(method: String, path: String, body: Map<String, Any?>?): Pair<Int, JsonMap> {
        val headers = LinkedHashMap<String, String>()
        headers["Accept"] = "application/json"
        if (body != null) headers["Content-Type"] = "application/json"
        if (method != "GET") headers["Origin"] = origin().trimEnd('/')
        val response = try {
            send(method, path, headers, body?.let { Json.stringify(it).toByteArray(Charsets.UTF_8) })
        } catch (error: Exception) {
            throw AgentLaunchException("Could not reach the relay: ${error.message ?: error}")
        }
        val decoded = try {
            @Suppress("UNCHECKED_CAST")
            Json.parse(String(response.body, Charsets.UTF_8)) as? JsonMap ?: emptyMap()
        } catch (_: Exception) {
            emptyMap()
        }
        return response.status to decoded
    }

    private fun agentMessage(body: JsonMap, status: Int): String {
        val message = jsonText(body["message"]).trim()
        if (message.isNotEmpty()) return message
        return when (val code = jsonText(body["error"]).trim()) {
            "origin_not_allowed" -> "This build cannot queue agent work against that relay."
            "invalid_session" -> "Sign in again before starting an agent."
            "forbidden" -> "Only the owner of this repository can start agents on its nodes."
            "" -> "The relay refused the agent request (HTTP $status)."
            else -> code.replace('_', ' ')
        }
    }
}
