package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class UserPresencePolicyTest {
    fun testTheDialogIsOnlyForAPhoneWithNoSecureLock() {
        assertEq(UserPresenceMethod.Dialog, UserPresencePolicy.method(false, 26))
        assertEq(UserPresenceMethod.Dialog, UserPresencePolicy.method(false, 37))
        assertEq(UserPresenceMethod.DeviceCredential, UserPresencePolicy.method(true, 26))
        assertEq(UserPresenceMethod.DeviceCredential, UserPresencePolicy.method(true, 28))
        assertEq(UserPresenceMethod.Prompt, UserPresencePolicy.method(true, 29))
        assertEq(UserPresenceMethod.Prompt, UserPresencePolicy.method(true, 37))
    }

    fun testAnAuthenticationErrorNeverFallsBackToTheDialog() {
        val source = File("../app/src/main/kotlin/com/forkmesh/nativeapp/feature/account/projects/UserPresence.kt").readText()
        expect("continuation.resume(false)" in source)
        expect("resume(if" !in source)
        assertEq(1, Regex("showConfirmDialog").findAll(source).count())
    }
}
