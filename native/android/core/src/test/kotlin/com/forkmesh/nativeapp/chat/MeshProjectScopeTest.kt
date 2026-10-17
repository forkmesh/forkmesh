package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MeshProjectScopeTest {
    private fun meshSeal(scope: ProjectScope, base: MemorySecretStore): Pair<String, Any?> {
        val secrets = scope.secrets(base)
        val identity = Identity.loadOrCreate(secrets)
        val seal = RoomSeal.loadOrCreate(MESH_ROOM_NAME, scope = MESH_ROOM_NAME, policy = SealPolicy.OPEN, acct = identity.nodeId, secureStore = secrets)
        return identity.nodeId to seal.keyFrame()["kid"]
    }

    fun testEachProjectHasItsOwnMeshIdentityAndSeal() {
        val base = MemorySecretStore()
        val production = meshSeal(ProjectScope.legacy, base)
        val nextId = "https://app.next.forkmesh.com"
        val next = meshSeal(ProjectScope(nextId, ProjectScope.namespaceFor(nextId)), base)
        expect(production.first != next.first, "node ids differ")
        expect(production.second != next.second, "seal key ids differ")
        expect(base.read(ChatDeviceKeys.IDENTITY_STORE_KEY) != null, "production keeps the legacy namespace, as Flutter")
        expect(base.read(ProjectScope.namespaceFor(nextId) + ChatDeviceKeys.IDENTITY_STORE_KEY) != null, "next is under p/<id>/")
    }

    fun testReloadingAProjectKeepsItsSeal() {
        val base = MemorySecretStore()
        val id = "https://self.example"
        val scope = ProjectScope(id, ProjectScope.namespaceFor(id))
        assertEq(meshSeal(scope, base), meshSeal(scope, base))
    }
}
