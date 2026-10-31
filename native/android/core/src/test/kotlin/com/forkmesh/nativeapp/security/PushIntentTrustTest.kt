package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PushIntentTrustTest {
    private val planted = ForkNotification(id = "x1", kind = "mention", title = "Re-verify your account", href = "https://evil.example/login")
    private val row = Json.stringify(planted.toJson())

    fun testAnOpenIntentWithoutTheInstallTokenOpensNothing() {
        expect(PushTarget.fromIntent(false, "proj-2", "x1", "https://evil.example/login", null, row) == null)
        expect(PushTarget.fromIntent(false, null, null, null, "agent-7", null) == null)
    }

    fun testAnOpenIntentWithTheTokenStillOpens() {
        val target = PushTarget.fromIntent(true, null, "x1", null, null, row)
        expect(target != null)
        assertEq("x1", target!!.pingId)
        assertEq(planted.href, target.ping?.href)
    }

    fun testTheRowCarriedInTheIntentIsNeverTheCachedRow() {
        val target = PushTarget.fromIntent(true, null, "x1", null, null, row)!!
        expect(target.cachedRow { null } == null, "an intent row is not cached when the app has no such ping")
        val stored = planted.copy(title = "Real title", href = "/inbox")
        assertEq(stored, target.cachedRow { id -> if (id == "x1") stored else null })
        expect(PushTarget("", "", "/inbox").cachedRow { stored } == null)
    }
}
