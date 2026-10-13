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
) {
    val ok: Boolean get() = status == ProbeStatus.ok

    val missingOptional: Set<RelayFeature>
        get() = RelayFeature.entries.filter { !it.core && !features.has(it) }.toSet()

    val message: String
        get() = when (status) {
            ProbeStatus.ok -> if (version.isEmpty()) "Connected. This is a ForkMesh relay." else "Connected to ForkMesh $version."
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
        return ProbeResult(
            status = ProbeStatus.ok,
            statusCode = response.status,
            rev = rev,
            version = (body["version"] ?: "").toString(),
            features = features,
            project = candidate.copy(verifiedAt = clock(), verifiedRev = rev, features = features),
        )
    }
}
