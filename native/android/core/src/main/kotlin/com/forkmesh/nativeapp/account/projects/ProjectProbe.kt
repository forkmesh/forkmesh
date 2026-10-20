package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import java.net.SocketTimeoutException
import javax.net.ssl.SSLException

enum class ProbeStatus { ok, invalidUrl, duplicate, notHttps, unreachable, timeout, insecure, notForkMesh, serverError, missingCore }

data class ProbeResult(
    val status: ProbeStatus,
    val project: Project? = null,
    val rev: String = "",
    val version: String = "",
    val statusCode: Int = 0,
    val features: RelayFeatureSet = RelayFeatureSet.undeclared(),
    val instanceName: String = "",
    val logoUrl: String = "",
    val branded: Boolean = false,
) {
    val ok: Boolean get() = status == ProbeStatus.ok

    val missingOptional: Set<RelayFeature>
        get() = RelayFeature.entries.filter { !it.core && !features.has(it) }.toSet()

    val message: String
        get() = when (status) {
            ProbeStatus.ok -> {
                val name = instanceName.ifEmpty { "ForkMesh" }
                if (version.isEmpty()) "Connected to $name." else "Connected to $name $version."
            }
            ProbeStatus.invalidUrl -> "That does not look like a host. Enter something like “relay.example.com” or “192.168.1.20:8787”."
            ProbeStatus.duplicate -> "This instance is already one of your projects."
            ProbeStatus.notHttps -> "Projects connect over https only. Enter the host, or an https:// address."
            ProbeStatus.unreachable -> "Could not reach that host. Check the address and your connection."
            ProbeStatus.timeout -> "That host did not answer in time. It may be offline, or behind a network this phone cannot see."
            ProbeStatus.insecure -> "The secure connection failed. That host’s certificate is not valid for this address."
            ProbeStatus.notForkMesh -> "That host answered, but it is not a ForkMesh relay."
            ProbeStatus.serverError ->
                if (statusCode == 0) "That relay is not answering right now. Try again in a moment." else "That relay answered $statusCode. Try again in a moment."
            ProbeStatus.missingCore ->
                "That relay is a ForkMesh relay, but it does not serve ${joinOr(features.missingCore.map { it.label })}, which the app cannot work without."
        }

    private fun joinOr(items: List<String>): String = when (items.size) {
        0 -> "anything"
        1 -> items.single()
        else -> items.dropLast(1).joinToString(", ") + " or " + items.last()
    }
}

class ProjectProbe(
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val timeoutMs: Int = 8_000,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    companion object {
        fun isSecureInput(input: String): Boolean {
            val raw = input.trim().lowercase()
            if (!raw.startsWith("http://") && !raw.startsWith("ws://")) return true
            return AppEnv.isLoopbackHost(ServerUrl.parse(raw).host)
        }
    }

    fun check(input: String, existing: Collection<Project> = emptyList(), label: String = "", worldUrl: String = ""): ProbeResult {
        if (!isSecureInput(input) || !isSecureInput(worldUrl)) return ProbeResult(ProbeStatus.notHttps)
        val candidate = Project.tryParse(input, label, worldUrl, clock()) ?: return ProbeResult(ProbeStatus.invalidUrl)
        if (existing.any { it.id == candidate.id }) return ProbeResult(ProbeStatus.duplicate)
        return verify(candidate)
    }

    fun verify(candidate: Project): ProbeResult {
        val request = HttpClient.Request.get(candidate.httpOrigin + "/api/version")
            .header("Accept", "application/json")
            .timeouts(timeoutMs, timeoutMs)
        val response = try {
            transport.send(request)
        } catch (_: SocketTimeoutException) {
            return ProbeResult(ProbeStatus.timeout)
        } catch (_: SSLException) {
            return ProbeResult(ProbeStatus.insecure)
        } catch (_: Exception) {
            return ProbeResult(ProbeStatus.unreachable)
        }
        val body: Map<String, Any?> = runCatching {
            @Suppress("UNCHECKED_CAST")
            Json.parse(response.text()) as? Map<String, Any?>
        }.getOrNull() ?: emptyMap()
        val identifiedByHeader = response.header("x-forkmesh-worker") != null
        if (response.status != 200) {
            return ProbeResult(if (identifiedByHeader) ProbeStatus.serverError else ProbeStatus.notForkMesh, statusCode = response.status)
        }
        val identified = identifiedByHeader || (body["ok"] == true && body.containsKey("rev") && body.containsKey("worker"))
        if (!identified) return ProbeResult(ProbeStatus.notForkMesh, statusCode = response.status)
        val features = RelayFeatureSet.fromVersionBody(body)
        if (!features.hasCore) return ProbeResult(ProbeStatus.missingCore, statusCode = response.status, features = features)
        val rev = (body["rev"] ?: "").toString()
        val instance = describe(candidate)
        val instanceName = (instance["name"] as? String).orEmpty().trim()
        val label = if (candidate.label == candidate.id && instanceName.isNotEmpty()) instanceName else candidate.label
        return ProbeResult(
            status = ProbeStatus.ok,
            statusCode = response.status,
            rev = rev,
            version = (body["version"] ?: "").toString(),
            features = features,
            instanceName = instanceName,
            logoUrl = logoUrl(candidate, instance),
            branded = instance["branded"] == true,
            project = candidate.copy(label = label, verifiedAt = clock(), verifiedRev = rev, features = features),
        )
    }

    private fun describe(candidate: Project): Map<String, Any?> = runCatching {
        val response = transport.send(
            HttpClient.Request.get(candidate.httpOrigin + "/api/instance")
                .header("Accept", "application/json")
                .timeouts(timeoutMs, timeoutMs),
        )
        @Suppress("UNCHECKED_CAST")
        if (response.status == 200) Json.parse(response.text()) as? Map<String, Any?> else null
    }.getOrNull() ?: emptyMap()

    private fun logoUrl(candidate: Project, instance: Map<String, Any?>): String = when {
        instance.containsKey("logo") -> sameOrigin(candidate, (instance["logo"] as? String).orEmpty().trim())
        instance["branded"] == true -> manifestLogo(candidate)
        else -> ""
    }

    private fun manifestLogo(candidate: Project): String = runCatching {
        val response = transport.send(
            HttpClient.Request.get(candidate.httpOrigin + "/favicon/site.webmanifest")
                .header("Accept", "application/manifest+json, application/json")
                .timeouts(timeoutMs, timeoutMs),
        )
        if (response.status != 200) return@runCatching ""
        val icons = (Json.parse(response.text()) as? Map<*, *>)?.get("icons") as? List<*>
        icons.orEmpty()
            .mapNotNull { ((it as? Map<*, *>)?.get("src") as? String)?.trim() }
            .firstOrNull { INSTANCE_LOGO.matches(it) }
            ?.let { candidate.httpOrigin + it }
            .orEmpty()
    }.getOrNull().orEmpty()

    private fun sameOrigin(candidate: Project, logo: String): String = when {
        logo.startsWith("/") && !logo.startsWith("//") -> candidate.httpOrigin + logo
        else -> ""
    }
}

private val INSTANCE_LOGO = Regex("/instance-logo\\.(?:png|jpg|webp)")
