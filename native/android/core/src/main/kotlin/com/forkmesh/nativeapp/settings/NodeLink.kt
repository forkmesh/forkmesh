package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import java.net.URI
import java.net.URLDecoder

data class NodeLinkGrant(val node: String, val ts: String, val sig: String, val host: String) {
    val timestampMillis: Long get() = ts.toLongOrNull() ?: 0L

    fun ageMillis(nowMillis: Long): Long = nowMillis - timestampMillis

    fun isFresh(nowMillis: Long): Boolean = kotlin.math.abs(ageMillis(nowMillis)) <= NodeLink.MAX_SKEW_MILLIS

    fun body(user: String): Map<String, Any?> = linkedMapOf("nodeName" to node, "user" to user.lowercase(), "ts" to ts, "sig" to sig)
}

sealed class NodeLinkParse {
    data class Grant(val grant: NodeLinkGrant) : NodeLinkParse()
    data class InstallerCode(val code: String) : NodeLinkParse()
    data class Invalid(val message: String) : NodeLinkParse()
}

sealed class NodeLinkOutcome {
    data class Linked(val node: String, val nodes: List<String>) : NodeLinkOutcome()
    data class AlreadyLinked(val node: String) : NodeLinkOutcome()
    data class Failed(val message: String) : NodeLinkOutcome()
}

object NodeLink {
    const val MAX_SKEW_MILLIS = 5L * 60 * 1000
    const val PATH = "/api/accounts/link-grant"

    fun isNodeName(name: String): Boolean {
        if (name.isEmpty() || name.length > 63) return false
        val first = name[0]
        if (first !in 'a'..'z' && first !in '0'..'9') return false
        return name.all { it in 'a'..'z' || it in '0'..'9' || it == '-' || it == '_' }
    }

    fun parse(scanned: String): NodeLinkParse {
        val text = scanned.trim()
        if (text.isEmpty()) return NodeLinkParse.Invalid("Scan the code the desktop shows, or paste its link.")
        if (text.length == 6 && text.all { it in '0'..'9' }) return NodeLinkParse.InstallerCode(text)
        if (text.length > 2048) return NodeLinkParse.Invalid("That is not a ForkMesh link.")
        val uri = try {
            URI(text)
        } catch (_: Exception) {
            return NodeLinkParse.Invalid("That is not a ForkMesh link.")
        }
        val query = HashMap<String, String>()
        for (pair in (uri.rawQuery ?: "").split('&')) {
            if (pair.isEmpty()) continue
            val key = URLDecoder.decode(pair.substringBefore('='), "UTF-8")
            val value = URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8")
            query.putIfAbsent(key, value)
        }
        val node = query["link_node"].orEmpty().lowercase()
        val ts = query["link_ts"].orEmpty()
        val sig = query["link_sig"].orEmpty()
        if (node.isEmpty() && ts.isEmpty() && sig.isEmpty()) return NodeLinkParse.Invalid("That link does not link a node.")
        if (!isNodeName(node)) return NodeLinkParse.Invalid("The link names no valid node.")
        if (ts.isEmpty() || ts.length > 20 || !ts.all { it in '0'..'9' }) return NodeLinkParse.Invalid("The link has no valid time.")
        if (sig.isEmpty() || sig.length > 200) return NodeLinkParse.Invalid("The link is not signed.")
        val scheme = uri.scheme?.lowercase().orEmpty()
        if (scheme != "https" && scheme != "http" && scheme != "forkmesh") return NodeLinkParse.Invalid("That is not a ForkMesh link.")
        return NodeLinkParse.Grant(NodeLinkGrant(node, ts, sig, uri.host?.lowercase().orEmpty()))
    }

    private fun canonicalHost(host: String): String =
        if (host in ProjectDefaults.PROD_HOST_ALIASES || ProjectDefaults.isNextHost(host)) ProjectDefaults.PROD_HOST else host

    fun hostMatches(grant: NodeLinkGrant, serverOrigin: String): Boolean {
        if (grant.host.isEmpty()) return true
        val host = try {
            URI(serverOrigin).host?.lowercase().orEmpty()
        } catch (_: Exception) {
            ""
        }
        if (host.isEmpty()) return true
        return host == grant.host || canonicalHost(host) == canonicalHost(grant.host)
    }

    fun outcome(map: Map<String, Any?>): NodeLinkOutcome {
        val node = map.text("nodeId").ifEmpty { map.text("node") }
        if (map.flag("alreadyLinked")) return NodeLinkOutcome.AlreadyLinked(node)
        if (map.flag("linked")) return NodeLinkOutcome.Linked(node, map.strings("nodes"))
        return NodeLinkOutcome.Failed("The relay did not link the node.")
    }
}

class NodeLinkModel(
    private val client: AccountEndpointCalling,
    private val user: () -> String,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    @Volatile var busy = false
        private set
    @Volatile var outcome: NodeLinkOutcome? = null
        private set
    @Volatile var pending: NodeLinkGrant? = null
        private set
    @Volatile var error = ""
        private set

    fun review(scanned: String) {
        outcome = null
        pending = null
        when (val parsed = NodeLink.parse(scanned)) {
            is NodeLinkParse.Grant -> {
                val grant = parsed.grant
                when {
                    !grant.isFresh(clock()) -> error = "That link has expired. Ask the desktop for a new one."
                    !NodeLink.hostMatches(grant, client.serverOrigin) -> error = "That link is for ${grant.host}, not the server this app uses."
                    else -> {
                        error = ""
                        pending = grant
                    }
                }
            }
            is NodeLinkParse.InstallerCode -> error = "That is an installer code. Enter it on the desktop that installed the node."
            is NodeLinkParse.Invalid -> error = parsed.message
        }
    }

    fun cancel() {
        pending = null
    }

    fun confirm(): NodeLinkOutcome? {
        val grant = pending ?: return null
        val account = user()
        if (!NodeLink.isNodeName(account.lowercase())) {
            error = "Sign in to a user account first."
            return null
        }
        busy = true
        return try {
            val result = NodeLink.outcome(client.accountCall("POST", NodeLink.PATH, grant.body(account)))
            outcome = result
            pending = null
            error = if (result is NodeLinkOutcome.Failed) result.message else ""
            result
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not link the node.")
            null
        } finally {
            busy = false
        }
    }
}
