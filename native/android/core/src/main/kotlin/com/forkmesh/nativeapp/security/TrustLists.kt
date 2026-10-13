package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.Utf8

class TrustListError(val code: String) : Exception("TrustListError($code)")

@Suppress("UNCHECKED_CAST")
object TrustLists {
    const val SCHEMA = "forkmesh-trust-list-v1"
    const val REVOCATION_SCHEMA = "forkmesh-trust-list-revocation-v1"
    const val APPEAL_SCHEMA = "forkmesh-trust-list-appeal-v1"
    const val RESOLUTION_SCHEMA = "forkmesh-trust-resolution-v1"
    private const val LIST_ID_DOMAIN = "forkmesh-trust-list-id-v1"

    private val ROLES = listOf("maintainer", "moderator", "mirror-operator", "organization", "reviewer", "security-researcher")
    private val STANCES = listOf("distrust", "trust")
    private val REVOCATION_REASONS = listOf("compromised", "malicious", "retired", "superseded")
    private val LIST_STATES = listOf("active", "conflicted", "expired", "missing", "recovered", "revoked")

    private const val SAFE_INTEGER = 9007199254740991L
    private const val MAX_DOCUMENT_BYTES = 262144
    private const val MAX_ENTRIES = 2000
    private const val MAX_EDGES = 16
    private const val MAX_COMMUNITIES = 64
    private const val MAX_REVOCATION_KEYS = 4
    private const val MAX_NOTE = 280
    private const val MAX_STATEMENT = 1000
    private const val MAX_LIFETIME_MS = 400L * 24 * 60 * 60 * 1000
    private const val MAX_PATH_LISTS = 4
    private const val FUTURE_SKEW_MS = 300000L
    private const val MAX_NESTING = 8

    private val LIST_KEYS = setOf(
        "schema", "issuer", "name", "version", "previous", "issuedAt", "expiresAt",
        "roles", "communities", "revocationKeys", "entries", "edges", "signature",
    )
    private val REVOCATION_KEYS = setOf(
        "schema", "list", "issuer", "name", "fromVersion", "revokedBy", "revokedAt", "reason", "successor", "signature",
    )
    private val APPEAL_KEYS = setOf("schema", "subject", "list", "version", "role", "scope", "statement", "issuedAt", "signature")
    private val ENTRY_KEYS = setOf("subject", "role", "scope", "stance", "note")
    private val EDGE_KEYS = setOf("kind", "issuer", "name", "roles", "communities", "expiresAt")
    private val SUCCESSOR_KEYS = setOf("issuer", "name")

    private val KEY_TEXT = Regex("[A-Za-z0-9_-]{43}")
    private val SIG_TEXT = Regex("[A-Za-z0-9_-]{86}")
    private val SLUG = Regex("[a-z0-9][a-z0-9-]{0,63}")
    private val OWNER = Regex("[a-z0-9][a-z0-9-]{0,38}")
    private val REPO = Regex("[A-Za-z0-9._-]{1,100}")
    private val DIGEST = Regex("sha256:[0-9a-f]{64}")
    private val LIST_URN = Regex("urn:forkmesh:trust-list:sha256:[0-9a-f]{64}")

    private fun reject(code: String): Nothing = throw TrustListError(code)

    private fun isText(value: String): Boolean {
        for (c in value) {
            val code = c.code
            if (code < 0x20 || code > 0x7e || code == 0x22 || code == 0x5c) return false
        }
        return true
    }

    fun canonicalJson(value: Any?): String = StringBuilder().also { writeCanonical(it, value) }.toString()

    private fun writeCanonical(out: StringBuilder, value: Any?) {
        when (value) {
            null -> out.append("null")
            is Boolean -> out.append(if (value) "true" else "false")
            is Long, is Int, is Short, is Byte -> {
                val n = (value as Number).toLong()
                if (n > SAFE_INTEGER || n < -SAFE_INTEGER) reject("noncanonical")
                out.append(n)
            }
            is String -> {
                if (!isText(value)) reject("noncanonical")
                out.append('"').append(value).append('"')
            }
            is List<*> -> {
                out.append('[')
                for ((index, item) in value.withIndex()) {
                    if (index > 0) out.append(',')
                    writeCanonical(out, item)
                }
                out.append(']')
            }
            is Map<*, *> -> {
                if (value.keys.any { it !is String }) reject("noncanonical")
                val keys = value.keys.map { it as String }.sorted()
                out.append('{')
                for ((index, key) in keys.withIndex()) {
                    if (!isText(key)) reject("noncanonical")
                    if (index > 0) out.append(',')
                    out.append('"').append(key).append("\":")
                    writeCanonical(out, value[key])
                }
                out.append('}')
            }
            else -> reject("noncanonical")
        }
    }

    private fun utf8SurrogatePass(text: String): ByteArray {
        val out = java.io.ByteArrayOutputStream(text.length + 16)
        var i = 0
        while (i < text.length) {
            var c = text[i].code
            if (c in 0xD800..0xDBFF && i + 1 < text.length && text[i + 1].code in 0xDC00..0xDFFF) {
                c = 0x10000 + ((c - 0xD800) shl 10) + (text[i + 1].code - 0xDC00)
                i++
            }
            when {
                c < 0x80 -> out.write(c)
                c < 0x800 -> {
                    out.write(0xC0 or (c shr 6))
                    out.write(0x80 or (c and 0x3F))
                }
                c < 0x10000 -> {
                    out.write(0xE0 or (c shr 12))
                    out.write(0x80 or ((c shr 6) and 0x3F))
                    out.write(0x80 or (c and 0x3F))
                }
                else -> {
                    out.write(0xF0 or (c shr 18))
                    out.write(0x80 or ((c shr 12) and 0x3F))
                    out.write(0x80 or ((c shr 6) and 0x3F))
                    out.write(0x80 or (c and 0x3F))
                }
            }
            i++
        }
        return out.toByteArray()
    }

    fun documentId(raw: String): String = "sha256:" + Sha256.hex(utf8SurrogatePass(raw))

    fun listId(issuer: String, name: String): String =
        "urn:forkmesh:trust-list:sha256:" + Sha256.hex(Utf8.encode("$LIST_ID_DOMAIN\n$issuer\n$name"))

    private fun signingText(document: Map<String, Any?>): String {
        val unsigned = LinkedHashMap<String, Any?>()
        for ((key, value) in document) if (key != "signature") unsigned[key] = value
        return "${document["schema"]}\n${canonicalJson(unsigned)}"
    }

    private fun validScope(value: Any?): Boolean {
        if (value !is String) return false
        if (value.startsWith("repo:")) {
            val rest = value.substring(5)
            val slash = rest.indexOf('/')
            if (slash < 0) return false
            val owner = rest.substring(0, slash)
            val repo = rest.substring(slash + 1)
            return OWNER.matches(owner) && REPO.matches(repo) && repo != "." && repo != ".."
        }
        if (value.startsWith("org:")) return OWNER.matches(value.substring(4))
        if (value.startsWith("community:")) return SLUG.matches(value.substring(10))
        return false
    }

    private fun covers(outer: String, inner: String): Boolean {
        if (outer == inner) return true
        return outer.startsWith("org:") && inner.startsWith("repo:${outer.substring(4)}/")
    }

    private fun covered(scopes: Iterable<String>, scope: String): Boolean = scopes.any { covers(it, scope) }

    private fun sortedSet(values: Iterable<String>): List<String> = values.toSet().sorted()

    private fun normalizeScopes(scopes: Iterable<String>): List<String> {
        val unique = sortedSet(scopes)
        return unique.filter { s -> unique.none { o -> o != s && covers(o, s) } }
    }

    private fun intersectScopes(left: List<String>, right: List<String>): List<String> {
        val kept = left.filter { covered(right, it) } + right.filter { covered(left, it) }
        return normalizeScopes(kept)
    }

    private fun sortedIntersection(left: Iterable<String>, right: Iterable<String>): List<String> {
        val other = right.toSet()
        return sortedSet(left.filter { it in other })
    }

    private fun strings(values: Any?): List<String> = (values as List<*>).map { it as String }

    private fun sameStrings(left: List<String>, right: List<*>): Boolean {
        if (left.size != right.size) return false
        for (i in left.indices) if (left[i] != right[i]) return false
        return true
    }

    private fun List<String>.containsValue(value: Any?): Boolean = value is String && value in this

    private fun safeInt(value: Any?, minimum: Long = 0): Boolean {
        val n = when (value) {
            is Long -> value
            is Int -> value.toLong()
            else -> return false
        }
        return minimum <= n && n <= SAFE_INTEGER
    }

    private fun asLong(value: Any?): Long = (value as Number).toLong()

    private fun sameKeys(map: Map<*, *>, keys: Set<String>): Boolean = map.size == keys.size && keys.all { map.containsKey(it) }

    private fun decodeFixed(value: Any?, shape: Regex, padding: String): ByteArray? {
        if (value !is String || !shape.matches(value)) return null
        val decoded = B64.decodeOrNull("$value$padding") ?: return null
        if (B64.encodeUrlNoPadding(decoded) != value) return null
        return decoded
    }

    private fun keyBytes(value: Any?): ByteArray? = decodeFixed(value, KEY_TEXT, "=")

    private fun signatureBytes(value: Any?): ByteArray? = decodeFixed(value, SIG_TEXT, "==")

    private fun sortedUnique(values: Any?, check: (Any?) -> Boolean, maximum: Int, empty: Boolean): Boolean {
        if (values !is List<*> || values.size > maximum) return false
        if (values.isEmpty() && !empty) return false
        if (!values.all(check)) return false
        for (i in 0 until values.size - 1) {
            if ((values[i] as String).compareTo(values[i + 1] as String) >= 0) return false
        }
        return true
    }

    private fun validRoles(values: Any?): Boolean =
        sortedUnique(values, { ROLES.containsValue(it) }, maximum = ROLES.size, empty = false)

    private fun validCommunities(values: Any?): Boolean =
        sortedUnique(values, ::validScope, maximum = MAX_COMMUNITIES, empty = false) &&
            sameStrings(normalizeScopes(strings(values)), values as List<*>)

    private fun verifySignature(document: Map<String, Any?>, signer: Any?) {
        val signature = signatureBytes(document["signature"]) ?: reject("invalid_signature_encoding")
        if (!Ed25519.isCanonicalScalar(signature, 32)) reject("invalid_signature")
        val publicKey = keyBytes(signer)
        val valid = try {
            publicKey != null && Ed25519.verifyStrict(publicKey, Utf8.encode(signingText(document)), signature)
        } catch (_: Exception) {
            reject("invalid_signature")
        }
        if (!valid) reject("invalid_signature")
    }

    private fun validateList(doc: Map<String, Any?>) {
        if (keyBytes(doc["issuer"]) == null) reject("invalid_issuer")
        val name = doc["name"]
        if (name !is String || !SLUG.matches(name)) reject("invalid_name")
        if (!safeInt(doc["version"], 1)) reject("invalid_version")
        val previous = doc["previous"]
        if (asLong(doc["version"]) == 1L) {
            if (previous != null) reject("invalid_previous")
        } else if (previous !is String || !DIGEST.matches(previous)) {
            reject("invalid_previous")
        }
        val issued = doc["issuedAt"]
        val expires = doc["expiresAt"]
        if (!safeInt(issued) || !safeInt(expires) || asLong(expires) <= asLong(issued) || asLong(expires) - asLong(issued) > MAX_LIFETIME_MS) {
            reject("invalid_time")
        }
        if (!validRoles(doc["roles"]) || !validCommunities(doc["communities"])) reject("invalid_scope")
        val keys = doc["revocationKeys"]
        if (!sortedUnique(keys, { keyBytes(it) != null }, maximum = MAX_REVOCATION_KEYS, empty = true) ||
            (keys as List<*>).contains(doc["issuer"])
        ) {
            reject("invalid_revocation_keys")
        }
        val docRoles = strings(doc["roles"])
        val docCommunities = strings(doc["communities"])

        val entries = doc["entries"]
        if (entries !is List<*> || entries.size > MAX_ENTRIES) reject("invalid_entry")
        var previousKey: String? = null
        for (entry in entries) {
            if (entry !is Map<*, *> ||
                !sameKeys(entry, ENTRY_KEYS) ||
                keyBytes(entry["subject"]) == null ||
                entry["subject"] == doc["issuer"] ||
                !ROLES.containsValue(entry["role"]) ||
                !validScope(entry["scope"]) ||
                !STANCES.containsValue(entry["stance"]) ||
                entry["note"] !is String ||
                (entry["note"] as String).length > MAX_NOTE
            ) {
                reject("invalid_entry")
            }
            val role = entry["role"] as String
            val scope = entry["scope"] as String
            if (role !in docRoles || !covered(docCommunities, scope)) reject("entry_outside_scope")
            val key = "${entry["subject"]}\n$role\n$scope"
            if (previousKey != null && key.compareTo(previousKey) <= 0) reject("unsorted_entries")
            previousKey = key
        }

        val edges = doc["edges"]
        if (edges !is List<*> || edges.size > MAX_EDGES) reject("invalid_edge")
        previousKey = null
        for (edge in edges) {
            if (edge !is Map<*, *> ||
                !sameKeys(edge, EDGE_KEYS) ||
                (edge["kind"] != "delegate" && edge["kind"] != "include") ||
                keyBytes(edge["issuer"]) == null ||
                edge["name"] !is String ||
                !SLUG.matches(edge["name"] as String) ||
                !validRoles(edge["roles"]) ||
                !validCommunities(edge["communities"]) ||
                (edge["kind"] == "delegate" && !safeInt(edge["expiresAt"])) ||
                (edge["kind"] == "include" && edge["expiresAt"] != null) ||
                (edge["issuer"] == doc["issuer"] && edge["name"] == doc["name"])
            ) {
                reject("invalid_edge")
            }
            if (!strings(edge["roles"]).all { it in docRoles } ||
                !strings(edge["communities"]).all { covered(docCommunities, it) }
            ) {
                reject("edge_outside_scope")
            }
            val key = "${edge["issuer"]}\n${edge["name"]}"
            if (previousKey != null && key.compareTo(previousKey) <= 0) reject("unsorted_edges")
            previousKey = key
        }

        verifySignature(doc, doc["issuer"])
    }

    private fun validateRevocation(doc: Map<String, Any?>) {
        if (keyBytes(doc["issuer"]) == null) reject("invalid_issuer")
        val name = doc["name"]
        if (name !is String || !SLUG.matches(name)) reject("invalid_name")
        if (doc["list"] != listId(doc["issuer"] as String, name)) reject("list_mismatch")
        if (!safeInt(doc["fromVersion"], 1)) reject("invalid_version")
        if (keyBytes(doc["revokedBy"]) == null) reject("invalid_revoker")
        if (!safeInt(doc["revokedAt"])) reject("invalid_time")
        if (!REVOCATION_REASONS.containsValue(doc["reason"])) reject("invalid_reason")
        val successor = doc["successor"]
        if (successor != null &&
            (successor !is Map<*, *> ||
                !sameKeys(successor, SUCCESSOR_KEYS) ||
                keyBytes(successor["issuer"]) == null ||
                successor["name"] !is String ||
                !SLUG.matches(successor["name"] as String) ||
                (successor["issuer"] == doc["issuer"] && successor["name"] == doc["name"]))
        ) {
            reject("invalid_successor")
        }
        verifySignature(doc, doc["revokedBy"])
    }

    private fun validateAppeal(doc: Map<String, Any?>) {
        if (keyBytes(doc["subject"]) == null) reject("invalid_subject")
        val list = doc["list"]
        if (list !is String || !LIST_URN.matches(list)) reject("invalid_list")
        if (!safeInt(doc["version"], 1)) reject("invalid_version")
        if (!ROLES.containsValue(doc["role"]) || !validScope(doc["scope"])) reject("invalid_scope")
        val statement = doc["statement"]
        if (statement !is String || statement.isEmpty() || statement.length > MAX_STATEMENT) reject("invalid_statement")
        if (!safeInt(doc["issuedAt"])) reject("invalid_time")
        verifySignature(doc, doc["subject"])
    }

    private fun nestingDepth(raw: String): Int {
        var depth = 0
        var deepest = 0
        var inString = false
        var escaped = false
        for (unit in raw) {
            if (inString) {
                if (escaped) {
                    escaped = false
                } else if (unit == '\\') {
                    escaped = true
                } else if (unit == '"') {
                    inString = false
                }
            } else if (unit == '"') {
                inString = true
            } else if (unit == '[' || unit == '{') {
                depth += 1
                if (depth > deepest) deepest = depth
            } else if (unit == ']' || unit == '}') {
                depth -= 1
            }
        }
        return deepest
    }

    private fun verifyDocumentInternal(raw: String): Map<String, Any?> {
        if (utf8SurrogatePass(raw).size > MAX_DOCUMENT_BYTES) reject("too_large")
        if (nestingDepth(raw) > MAX_NESTING) reject("invalid_json")
        val document = try {
            StrictJson(raw).parse()
        } catch (_: StrictJson.Invalid) {
            reject("invalid_json")
        }
        if (canonicalJson(document) != raw) reject("noncanonical")
        if (document !is Map<*, *>) reject("unsupported_schema")
        val doc = document as Map<String, Any?>
        when (doc["schema"]) {
            SCHEMA -> {
                if (!sameKeys(doc, LIST_KEYS)) reject("invalid_keys")
                validateList(doc)
            }
            REVOCATION_SCHEMA -> {
                if (!sameKeys(doc, REVOCATION_KEYS)) reject("invalid_keys")
                validateRevocation(doc)
            }
            APPEAL_SCHEMA -> {
                if (!sameKeys(doc, APPEAL_KEYS)) reject("invalid_keys")
                validateAppeal(doc)
            }
            else -> reject("unsupported_schema")
        }
        return doc
    }

    fun verifyDocument(raw: String): Map<String, Any?> = verifyDocumentInternal(raw)

    private class Doc(val id: String, val doc: Map<String, Any?>) {
        val version: Long get() = (doc["version"] as Number).toLong()
    }

    private class Group(
        val state: String,
        val head: Map<String, Any?>?,
        val headId: String?,
        val cutoff: Long?,
        val reasons: List<String>,
        val successor: Map<String, Any?>?,
        val conflicts: List<Map<String, Any?>>,
        val heldVersions: List<Long>,
    )

    private fun listGroupState(docs: List<Doc>, revocations: List<Doc>, now: Long): Group {
        val byVersion = HashMap<Long, MutableList<Doc>>()
        for (item in docs) byVersion.getOrPut(item.version) { ArrayList() }.add(item)
        val versions = byVersion.keys.sorted()
        val cutoffs = ArrayList<Long>()
        val reasons = HashSet<String>()
        val conflicts = ArrayList<Map<String, Any?>>()
        val lid: String? = if (docs.isEmpty()) null else listId(docs.first().doc["issuer"] as String, docs.first().doc["name"] as String)
        for (version in versions) {
            val atVersion = byVersion.getValue(version)
            if (atVersion.size > 1) {
                cutoffs.add(version)
                reasons.add("equivocation")
                conflicts.add(linkedMapOf("kind" to "equivocation", "listId" to lid, "version" to version))
                continue
            }
            val prior = byVersion[version - 1]
            if (prior != null && prior.size == 1) {
                if (atVersion.first().doc["previous"] != prior.first().id) {
                    cutoffs.add(version)
                    reasons.add("history_fork")
                    conflicts.add(linkedMapOf("kind" to "history_fork", "listId" to lid, "version" to version))
                }
            }
        }
        var successor: Map<String, Any?>? = null
        if (revocations.isNotEmpty()) {
            reasons.add("revoked")
            val ordered = revocations.sortedWith(compareBy<Doc> { asLong(it.doc["fromVersion"]) }.thenBy { it.id })
            cutoffs.add(asLong(ordered.first().doc["fromVersion"]))
            for (revocation in ordered) {
                val next = revocation.doc["successor"]
                if (next != null) {
                    successor = LinkedHashMap(next as Map<String, Any?>)
                    break
                }
            }
        }
        val cutoff = cutoffs.minOrNull()
        var headId: String? = null
        var head: Map<String, Any?>? = null
        for (version in versions.reversed()) {
            if (cutoff != null && version >= cutoff) continue
            val atVersion = byVersion.getValue(version)
            if (atVersion.size == 1) {
                headId = atVersion.first().id
                head = atVersion.first().doc
                break
            }
        }
        val state = when {
            head == null -> if (revocations.isNotEmpty()) "revoked" else if (docs.isNotEmpty()) "conflicted" else "missing"
            now >= asLong(head["expiresAt"]) -> "expired"
            cutoff != null -> "recovered"
            else -> "active"
        }
        return Group(state, head, headId, cutoff, reasons.sorted(), successor, conflicts, versions)
    }

    fun resolve(
        documents: List<String>,
        now: Long,
        follows: List<Map<String, Any?>> = emptyList(),
        overrides: List<Map<String, Any?>> = emptyList(),
    ): Map<String, Any?> {
        val unique = LinkedHashMap<String, String>()
        for (raw in documents) unique.putIfAbsent(documentId(raw), raw)
        val duplicates = documents.size - unique.size

        val rejected = ArrayList<Map<String, Any?>>()
        val lists = ArrayList<Doc>()
        val revocations = ArrayList<Doc>()
        val appeals = ArrayList<Doc>()
        for (docId in unique.keys.sorted()) {
            val doc = try {
                verifyDocumentInternal(unique.getValue(docId))
            } catch (error: TrustListError) {
                rejected.add(linkedMapOf("documentId" to docId, "code" to error.code))
                continue
            }
            val schema = doc["schema"]
            if (schema != REVOCATION_SCHEMA && asLong(doc["issuedAt"]) > now + FUTURE_SKEW_MS) {
                rejected.add(linkedMapOf("documentId" to docId, "code" to "issued_in_future"))
            } else if (schema == SCHEMA) {
                lists.add(Doc(docId, doc))
            } else if (schema == REVOCATION_SCHEMA) {
                revocations.add(Doc(docId, doc))
            } else {
                appeals.add(Doc(docId, doc))
            }
        }

        val docsByList = LinkedHashMap<String, MutableList<Doc>>()
        val names = HashMap<String, List<String>>()
        for (item in lists) {
            val issuer = item.doc["issuer"] as String
            val name = item.doc["name"] as String
            val lid = listId(issuer, name)
            docsByList.getOrPut(lid) { ArrayList() }.add(item)
            names[lid] = listOf(issuer, name)
        }

        val revocationsByList = LinkedHashMap<String, MutableList<Doc>>()
        for (item in revocations) {
            val revocation = item.doc
            val lid = revocation["list"] as String
            val revokedBy = revocation["revokedBy"]
            val authorized = revokedBy == revocation["issuer"] ||
                (docsByList[lid] ?: emptyList<Doc>()).any { listed ->
                    listed.version < asLong(revocation["fromVersion"]) &&
                        (listed.doc["revocationKeys"] as List<*>).contains(revokedBy)
                }
            if (!authorized) {
                rejected.add(linkedMapOf("documentId" to item.id, "code" to "unauthorized_revoker"))
                continue
            }
            revocationsByList.getOrPut(lid) { ArrayList() }.add(item)
            if (lid !in names) names[lid] = listOf(revocation["issuer"] as String, revocation["name"] as String)
        }

        val groups = HashMap<String, Group>()
        fun group(lid: String): Group = groups.getOrPut(lid) {
            listGroupState(docsByList[lid] ?: emptyList(), revocationsByList[lid] ?: emptyList(), now)
        }
        for (lid in docsByList.keys + revocationsByList.keys) group(lid)

        fun usable(lid: String): Map<String, Any?>? {
            val info = group(lid)
            return if (info.state == "active" || info.state == "recovered") info.head else null
        }

        val followed = HashSet<String>()
        val reached = HashSet<String>()
        var expiredDelegations = 0L
        var depthLimited = 0L
        var cycles = 0L
        val sources = HashMap<String, MutableMap<String, Map<String, Any?>>>()

        fun admit(lid: String, path: List<String>, roles: List<String>, communities: List<String>) {
            reached.add(lid)
            val head = usable(lid) ?: return
            for (raw in head["entries"] as List<*>) {
                val entry = raw as Map<*, *>
                val subject = entry["subject"] as String
                val role = entry["role"] as String
                val scope = entry["scope"] as String
                if (role !in roles || !covered(communities, scope)) continue
                val tupleKey = "$subject\n$role\n$scope"
                val candidate = linkedMapOf<String, Any?>(
                    "listId" to lid,
                    "version" to head["version"],
                    "stance" to entry["stance"],
                    "note" to entry["note"],
                    "path" to ArrayList(path),
                )
                val perList = sources.getOrPut(tupleKey) { HashMap() }
                val current = perList[lid]
                if (current == null) {
                    perList[lid] = candidate
                } else {
                    val currentPath = current["path"] as List<String>
                    if (path.size < currentPath.size ||
                        (path.size == currentPath.size && path.joinToString("\n") < currentPath.joinToString("\n"))
                    ) {
                        perList[lid] = candidate
                    }
                }
            }
            for (raw in head["edges"] as List<*>) {
                val edge = raw as Map<*, *>
                if (edge["kind"] == "delegate" && now >= asLong(edge["expiresAt"])) {
                    expiredDelegations += 1
                    continue
                }
                val edgeIssuer = edge["issuer"] as String
                val edgeName = edge["name"] as String
                val child = listId(edgeIssuer, edgeName)
                if (child !in names) names[child] = listOf(edgeIssuer, edgeName)
                if (child in path) {
                    cycles += 1
                    continue
                }
                if (path.size >= MAX_PATH_LISTS) {
                    depthLimited += 1
                    continue
                }
                var childRoles = sortedIntersection(roles, strings(edge["roles"]))
                var childCommunities = intersectScopes(communities, strings(edge["communities"]))
                val childHead = usable(child)
                if (childHead != null) {
                    childRoles = sortedIntersection(childRoles, strings(childHead["roles"]))
                    childCommunities = intersectScopes(childCommunities, strings(childHead["communities"]))
                }
                if (childRoles.isEmpty() || childCommunities.isEmpty()) {
                    reached.add(child)
                    continue
                }
                admit(child, path + child, childRoles, childCommunities)
            }
        }

        val followIds = ArrayList<String>()
        for (follow in follows) {
            val issuer = follow["issuer"] as String
            val name = follow["name"] as String
            val lid = listId(issuer, name)
            if (lid !in names) names[lid] = listOf(issuer, name)
            followIds.add(lid)
        }
        val followOrder = follows.indices.sortedWith(compareBy<Int> { followIds[it] }.thenBy { it })
        for (index in followOrder) {
            val lid = followIds[index]
            val follow = follows[index]
            followed.add(lid)
            val head = usable(lid)
            if (head == null) {
                reached.add(lid)
                continue
            }
            var roles = strings(head["roles"])
            var communities = strings(head["communities"])
            if (follow["roles"] != null) roles = sortedIntersection(roles, strings(follow["roles"]))
            if (follow["communities"] != null) communities = intersectScopes(communities, strings(follow["communities"]))
            if (roles.isNotEmpty() && communities.isNotEmpty()) {
                admit(lid, listOf(lid), roles, communities)
            } else {
                reached.add(lid)
            }
        }

        val overrideMap = LinkedHashMap<String, String>()
        for (o in overrides) overrideMap["${o["subject"]}\n${o["role"]}\n${o["scope"]}"] = o["stance"] as String
        val appealStatusCounts = linkedMapOf("open" to 0L, "resolved" to 0L, "unattached" to 0L)
        val assertions = ArrayList<MutableMap<String, Any?>>()
        val conflicts = ArrayList<Map<String, Any?>>()
        for (tupleKey in sortedSet(sources.keys + overrideMap.keys)) {
            val parts = tupleKey.split("\n")
            val subject = parts[0]
            val role = parts[1]
            val scope = parts[2]
            val perList = sources[tupleKey] ?: emptyMap<String, Map<String, Any?>>()
            val tupleSources = sortedSet(perList.keys).map { perList.getValue(it) }
            val stances = tupleSources.map { it["stance"] as String }.toSet()
            val stance = if (stances.isEmpty()) "none" else if (stances.size == 1) stances.single() else "conflict"
            val override = overrideMap[tupleKey]
            if (stance == "conflict") {
                conflicts.add(
                    linkedMapOf("kind" to "stance", "subject" to subject, "role" to role, "scope" to scope, "overridden" to (override != null)),
                )
            }
            assertions.add(
                linkedMapOf(
                    "subject" to subject,
                    "role" to role,
                    "scope" to scope,
                    "stance" to stance,
                    "override" to override,
                    "effective" to (override ?: stance),
                    "sources" to tupleSources,
                    "appeals" to ArrayList<Map<String, Any?>>(),
                ),
            )
        }

        val byTuple = HashMap<String, MutableMap<String, Any?>>()
        for (a in assertions) byTuple["${a["subject"]}\n${a["role"]}\n${a["scope"]}"] = a
        for (item in appeals) {
            val appeal = item.doc
            val appealList = appeal["list"] as String
            val appealVersion = asLong(appeal["version"])
            val assertion = byTuple["${appeal["subject"]}\n${appeal["role"]}\n${appeal["scope"]}"]
            if (assertion == null || (assertion["sources"] as List<Map<String, Any?>>).none { it["listId"] == appealList }) {
                appealStatusCounts["unattached"] = appealStatusCounts.getValue("unattached") + 1
                continue
            }
            val head = usable(appealList)
            var status = "open"
            if (head != null &&
                asLong(head["version"]) > appealVersion &&
                (head["entries"] as List<*>).none { raw ->
                    val e = raw as Map<*, *>
                    e["subject"] == appeal["subject"] && e["role"] == appeal["role"] && e["scope"] == appeal["scope"] && e["stance"] == "distrust"
                }
            ) {
                status = "resolved"
            }
            appealStatusCounts[status] = appealStatusCounts.getValue(status) + 1
            (assertion["appeals"] as MutableList<Map<String, Any?>>).add(
                linkedMapOf("appealId" to item.id, "listId" to appealList, "version" to appealVersion, "status" to status),
            )
        }

        val listRows = ArrayList<Map<String, Any?>>()
        val stateCounts = LinkedHashMap<String, Long>().also { counts -> LIST_STATES.forEach { counts[it] = 0L } }
        for (lid in sortedSet(groups.keys + followed + reached)) {
            val info = group(lid)
            conflicts.addAll(info.conflicts)
            val name = names.getValue(lid)
            stateCounts[info.state] = stateCounts.getValue(info.state) + 1
            val head = info.head
            listRows.add(
                linkedMapOf(
                    "listId" to lid,
                    "issuer" to name[0],
                    "name" to name[1],
                    "state" to info.state,
                    "followed" to (lid in followed),
                    "reachable" to (lid in reached),
                    "headVersion" to head?.get("version"),
                    "headDocumentId" to info.headId,
                    "expiresAt" to head?.get("expiresAt"),
                    "cutoffVersion" to info.cutoff,
                    "cutoffReasons" to info.reasons,
                    "successor" to info.successor,
                    "heldVersions" to info.heldVersions,
                ),
            )
        }

        val sortedConflicts = conflicts.map { it to canonicalJson(it) }.sortedBy { it.second }.map { it.first }
        val sortedRejected = rejected.sortedBy { "${it["documentId"]}\n${it["code"]}" }
        val rejectionCodes = LinkedHashMap<String, Long>()
        for (item in sortedRejected) {
            val code = item["code"] as String
            rejectionCodes[code] = (rejectionCodes[code] ?: 0L) + 1
        }

        return linkedMapOf(
            "schema" to RESOLUTION_SCHEMA,
            "now" to now,
            "lists" to listRows,
            "assertions" to assertions,
            "conflicts" to sortedConflicts,
            "rejected" to sortedRejected,
            "telemetry" to linkedMapOf(
                "documents" to unique.size.toLong(),
                "duplicateDocuments" to duplicates.toLong(),
                "rejectedDocuments" to sortedRejected.size.toLong(),
                "rejectionCodes" to rejectionCodes,
                "revocations" to revocationsByList.values.sumOf { it.size }.toLong(),
                "listStates" to stateCounts,
                "followedLists" to followed.size.toLong(),
                "reachableLists" to reached.size.toLong(),
                "assertions" to assertions.size.toLong(),
                "stanceConflicts" to sortedConflicts.count { it["kind"] == "stance" }.toLong(),
                "overrides" to overrideMap.size.toLong(),
                "appeals" to appealStatusCounts,
                "expiredDelegations" to expiredDelegations,
                "depthLimitedEdges" to depthLimited,
                "cycleEdges" to cycles,
            ),
        )
    }

    private class StrictJson(private val text: String) {
        class Invalid : Exception()

        private var pos = 0

        fun parse(): Any? {
            skipWhitespace()
            val value = readValue()
            skipWhitespace()
            if (pos != text.length) throw Invalid()
            return value
        }

        private fun skipWhitespace() {
            while (pos < text.length) {
                val c = text[pos]
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++ else return
            }
        }

        private fun readValue(): Any? {
            if (pos >= text.length) throw Invalid()
            return when (val c = text[pos]) {
                '{' -> readObject()
                '[' -> readArray()
                '"' -> readString()
                't' -> readWord("true", true)
                'f' -> readWord("false", false)
                'n' -> readWord("null", null)
                else -> if (c == '-' || c in '0'..'9') readNumber() else throw Invalid()
            }
        }

        private fun readWord(word: String, value: Any?): Any? {
            if (!text.startsWith(word, pos)) throw Invalid()
            pos += word.length
            return value
        }

        private fun readObject(): Map<String, Any?> {
            val map = LinkedHashMap<String, Any?>()
            pos++
            skipWhitespace()
            if (pos < text.length && text[pos] == '}') {
                pos++
                return map
            }
            while (true) {
                skipWhitespace()
                if (pos >= text.length || text[pos] != '"') throw Invalid()
                val key = readString()
                skipWhitespace()
                if (pos >= text.length || text[pos] != ':') throw Invalid()
                pos++
                skipWhitespace()
                map[key] = readValue()
                skipWhitespace()
                if (pos >= text.length) throw Invalid()
                when (text[pos++]) {
                    ',' -> continue
                    '}' -> return map
                    else -> throw Invalid()
                }
            }
        }

        private fun readArray(): List<Any?> {
            val list = ArrayList<Any?>()
            pos++
            skipWhitespace()
            if (pos < text.length && text[pos] == ']') {
                pos++
                return list
            }
            while (true) {
                skipWhitespace()
                list.add(readValue())
                skipWhitespace()
                if (pos >= text.length) throw Invalid()
                when (text[pos++]) {
                    ',' -> continue
                    ']' -> return list
                    else -> throw Invalid()
                }
            }
        }

        private fun readString(): String {
            pos++
            val out = StringBuilder()
            while (true) {
                if (pos >= text.length) throw Invalid()
                val c = text[pos++]
                when {
                    c == '"' -> return out.toString()
                    c == '\\' -> {
                        if (pos >= text.length) throw Invalid()
                        when (text[pos++]) {
                            '"' -> out.append('"')
                            '\\' -> out.append('\\')
                            '/' -> out.append('/')
                            'b' -> out.append('\b')
                            'f' -> out.append('\u000C')
                            'n' -> out.append('\n')
                            'r' -> out.append('\r')
                            't' -> out.append('\t')
                            'u' -> {
                                if (pos + 4 > text.length) throw Invalid()
                                var code = 0
                                repeat(4) {
                                    val digit = when (val h = text[pos++]) {
                                        in '0'..'9' -> h - '0'
                                        in 'a'..'f' -> h - 'a' + 10
                                        in 'A'..'F' -> h - 'A' + 10
                                        else -> throw Invalid()
                                    }
                                    code = code * 16 + digit
                                }
                                out.append(code.toChar())
                            }
                            else -> throw Invalid()
                        }
                    }
                    c.code < 0x20 -> throw Invalid()
                    else -> out.append(c)
                }
            }
        }

        private fun isDigit(index: Int): Boolean = index < text.length && text[index] in '0'..'9'

        private fun readNumber(): Any {
            val start = pos
            if (text[pos] == '-') pos++
            if (pos < text.length && text[pos] == '0') {
                pos++
            } else if (isDigit(pos)) {
                while (isDigit(pos)) pos++
            } else {
                throw Invalid()
            }
            var integral = true
            if (pos < text.length && text[pos] == '.') {
                pos++
                if (!isDigit(pos)) throw Invalid()
                while (isDigit(pos)) pos++
                integral = false
            }
            if (pos < text.length && (text[pos] == 'e' || text[pos] == 'E')) {
                pos++
                if (pos < text.length && (text[pos] == '+' || text[pos] == '-')) pos++
                if (!isDigit(pos)) throw Invalid()
                while (isDigit(pos)) pos++
                integral = false
            }
            val literal = text.substring(start, pos)
            return if (integral) literal.toLongOrNull() ?: literal.toDouble() else literal.toDouble()
        }
    }
}
