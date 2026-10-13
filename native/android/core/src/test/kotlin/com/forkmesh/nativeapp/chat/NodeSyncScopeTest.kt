package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NodeSyncScopeTest {
    private val item = NodeSyncItem(owner = "ada", repo = "engine", kind = NodeSyncKind.ISSUES, id = 7, data = mapOf("number" to 3L, "title" to "Fix"))

    private fun engine(prefs: MemoryPrefStore, project: () -> String): NodeSyncEngine {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return NodeSyncEngine(settings, Identity.fromSeed(Bytes.random(32)), session = { null }, prefs = prefs, loop = EventLoop("scope", virtualTime = true), projectId = project)
    }

    fun testALegacyCarryMovesIntoTheActiveProjectOnce() {
        val prefs = MemoryPrefStore()
        prefs.putString(NodeSyncEngine.CARRIED_PREFS_KEY, Json.stringify(listOf(item.toJson())))
        val sync = engine(prefs) { "next" }
        try {
            sync.loadCarried()
            assertEq(1, sync.carried.size)
            assertEq(null, prefs.getString(NodeSyncEngine.CARRIED_PREFS_KEY))
            expect((prefs.getString("nodeSync/carried/next") ?: "").isNotEmpty())
        } finally {
            sync.dispose()
        }
    }

    fun testEachProjectSeesOnlyItsOwnCarry() {
        val prefs = MemoryPrefStore()
        prefs.putString(NodeSyncEngine.carriedPrefsKeyFor("prod"), Json.stringify(listOf(item.toJson())))
        var project = "next"
        val sync = engine(prefs) { project }
        try {
            sync.loadCarried()
            assertEq(0, sync.carried.size)
            project = "prod"
            sync.loadCarried()
            assertEq(1, sync.carried.size)
            sync.clearCarried()
            assertEq(null, prefs.getString(NodeSyncEngine.carriedPrefsKeyFor("prod")))
        } finally {
            sync.dispose()
        }
    }

    fun testAnUnscopedEngineKeepsTheLegacySlot() {
        assertEq(NodeSyncEngine.CARRIED_PREFS_KEY, NodeSyncEngine.carriedPrefsKeyFor(""))
    }
}
