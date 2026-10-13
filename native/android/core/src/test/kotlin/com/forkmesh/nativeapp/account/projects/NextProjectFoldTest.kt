package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.push.BackgroundLinkKeys
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NextProjectFoldTest {
    private val prod = ProjectDefaults.PROD_HOST
    private val next = ProjectDefaults.NEXT_HOST
    private val ns = "p/$next/"

    private class MemoryFoldPrefs(seed: Map<String, Any> = emptyMap()) : FoldPrefs {
        val values = LinkedHashMap<String, Any>(seed)

        override fun getString(key: String): String? = values[key] as? String

        override fun putString(key: String, value: String) {
            values[key] = value
        }

        override fun getBool(key: String): Boolean? = values[key] as? Boolean

        override fun putBool(key: String, value: Boolean) {
            values[key] = value
        }

        override fun getDouble(key: String): Double? = (values[key] as? Number)?.toDouble()

        override fun putDouble(key: String, value: Double) {
            values[key] = value
        }

        override fun contains(key: String): Boolean = values.containsKey(key)

        override fun remove(key: String) {
            values.remove(key)
        }

        override fun keys(): Set<String> = values.keys.toSet()

        override fun copyRaw(from: String, to: String): Boolean {
            val value = values[from] ?: return false
            values[to] = value
            return true
        }
    }

    private fun session(name: String) = "{\"nodeName\":\"$name\",\"sessionToken\":\"t-$name\"}"

    private fun listJson(vararg ids: String): String = Json.stringify(
        ids.map { id ->
            linkedMapOf<String, Any?>(
                "id" to id,
                "label" to id,
                "serverUrl" to "wss://$id/ws",
                "namespace" to if (id == prod) "" else ProjectScope.namespaceFor(id),
            )
        },
    )

    private fun prefsWith(active: String, vararg ids: String) = MemoryFoldPrefs(
        mapOf(ProjectDirectory.LIST_KEY to listJson(*ids), ProjectDirectory.ACTIVE_KEY to active),
    )

    private fun ids(prefs: PrefStore): List<String> =
        (Json.parse(prefs.getString(ProjectDirectory.LIST_KEY)!!) as List<*>).map { (it as Map<*, *>)["id"].toString() }

    fun testASignedInNextFoldsIntoForkMeshWithItsKeysAndSettings() {
        val prefs = prefsWith(next, prod, next, "relay.example.com")
        prefs.values["${ns}profile/name"] = "Next Alice"
        prefs.values["profile/name"] = "Old Alice"
        prefs.values["${ns}ads/hidden"] = true
        prefs.values[BackgroundLinkKeys.enabled(next)] = false
        prefs.values[BackgroundLinkKeys.ledger(next)] = "{\"seen\":[\"p1\"]}"
        prefs.values[AuthController.revokedBannerKeyFor(next)] = true
        val secrets = MemorySecretStore(
            mapOf(
                AuthController.sessionKeyFor(next) to session("alice"),
                "${ns}identity/ed25519Seed" to "next-seed",
                "${ns}vault/alice/quickUnlock" to "q",
                "identity/ed25519Seed" to "old-seed",
            ),
        )

        val result = NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }

        assertEq(listOf(next), result.folded)
        assertEq(next, result.movedFrom)
        assertEq(listOf("identity/ed25519Seed"), result.replacedSecrets)
        assertEq(listOf(prod, "relay.example.com"), ids(prefs))
        assertEq(prod, prefs.getString(ProjectDirectory.ACTIVE_KEY))
        assertEq(session("alice"), secrets.read(AuthController.sessionKeyFor(prod)))
        assertEq(null, secrets.read(AuthController.sessionKeyFor(next)))
        assertEq("next-seed", secrets.read("identity/ed25519Seed"))
        assertEq("old-seed", secrets.read("${NextProjectFold.REPLACED_PREFIX}identity/ed25519Seed"))
        assertEq("q", secrets.read("vault/alice/quickUnlock"))
        expect(secrets.snapshot().keys.none { it.startsWith(ns) })
        assertEq("Old Alice", prefs.getString("profile/name"))
        assertEq("Next Alice", prefs.getString("${ns}profile/name"))
        assertEq(true, prefs.getBool("ads/hidden"))
        assertEq(false, prefs.getBool(BackgroundLinkKeys.enabled(prod)))
        assertEq("{\"seen\":[\"p1\"]}", prefs.getString(BackgroundLinkKeys.ledger(prod)))
        assertEq(true, prefs.getBool(AuthController.revokedBannerKeyFor(prod)))
        expect(!prefs.contains(BackgroundLinkKeys.enabled(next)))

        val again = NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }
        assertEq(emptyList<String>(), again.folded)
        assertEq(session("alice"), secrets.read(AuthController.sessionKeyFor(prod)))
    }

    fun testWhenBothAreSignedInForkMeshWinsAndNextKeysStay() {
        val prefs = prefsWith(prod, prod, next)
        val secrets = MemorySecretStore(
            mapOf(
                AuthController.sessionKeyFor(prod) to session("bob"),
                AuthController.sessionKeyFor(next) to session("alice"),
                "${ns}identity/ed25519Seed" to "next-seed",
            ),
        )
        val result = NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }
        assertEq(listOf(next), result.keptKeysOf)
        assertEq(null, result.movedFrom)
        assertEq(listOf(prod), ids(prefs))
        assertEq(session("bob"), secrets.read(AuthController.sessionKeyFor(prod)))
        assertEq(session("alice"), secrets.read(AuthController.sessionKeyFor(next)))
        assertEq("next-seed", secrets.read("${ns}identity/ed25519Seed"))
    }

    fun testAnUnusedNextEntryIsDroppedAndActiveFollowsToForkMesh() {
        val prefs = prefsWith(next, prod, next)
        val secrets = MemorySecretStore(mapOf("${ns}identity/ed25519Seed" to "next-seed"))
        NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }
        assertEq(listOf(prod), ids(prefs))
        assertEq(prod, prefs.getString(ProjectDirectory.ACTIVE_KEY))
        assertEq("next-seed", secrets.read("${ns}identity/ed25519Seed"))
    }

    fun testAGuestOnNextStaysAGuestOnForkMesh() {
        val prefs = prefsWith(next, prod, next)
        prefs.putBool(AuthController.guestModeKeyFor(next), true)
        NextProjectFold.run(prefs, MemorySecretStore()) { emptySet() }
        assertEq(true, prefs.getBool(AuthController.guestModeKeyFor(prod)))
        expect(!prefs.contains(AuthController.guestModeKeyFor(next)))
        NextProjectFold.run(prefs, MemorySecretStore()) { emptySet() }
        assertEq(true, prefs.getBool(AuthController.guestModeKeyFor(prod)))
    }

    fun testAGuestFlagOnNextDoesNotOverrideAForkMeshSession() {
        val prefs = prefsWith(next, prod, next)
        prefs.putBool(AuthController.guestModeKeyFor(next), true)
        val secrets = MemorySecretStore(mapOf(AuthController.sessionKeyFor(prod) to session("alice")))
        NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }
        expect(!prefs.contains(AuthController.guestModeKeyFor(prod)))
    }

    fun testALegacyUnscopedForkMeshSessionAlsoCountsAsSignedIn() {
        val prefs = prefsWith(next, prod, next)
        val secrets = MemorySecretStore(
            mapOf(AuthController.SESSION_KEY to session("bob"), AuthController.sessionKeyFor(next) to session("alice")),
        )
        NextProjectFold.run(prefs, secrets) { secrets.snapshot().keys }
        assertEq(session("alice"), secrets.read(AuthController.sessionKeyFor(next)))
        assertEq(null, secrets.read(AuthController.sessionKeyFor(prod)))
    }

    fun testFoldedDirectoryLoadsWithoutNextAndOtherNextHostsFoldToo() {
        expect(ProjectDefaults.isNextHost("next.forkmesh.com"))
        expect(ProjectDefaults.isNextHost("app.next.forkmesh.com:443"))
        expect(!ProjectDefaults.isNextHost("app.forkmesh.com"))
        expect(!ProjectDefaults.isNextHost("nextcloud.example.com"))
        val prefs = prefsWith("next.forkmesh.com", prod, "next.forkmesh.com")
        NextProjectFold.run(prefs, MemorySecretStore()) { emptySet() }
        val directory = ProjectDirectory.load(prefs)
        assertEq(listOf(prod), directory.projects.map { it.id })
        assertEq(prod, directory.activeId)
    }

    fun testNotificationsTappedAfterTheFoldOpenForkMesh() {
        assertEq(prod, PushTarget.from(next, "p1", "")!!.projectId)
        assertEq("relay.example.com", PushTarget.from("relay.example.com", "p1", "")!!.projectId)
    }
}
