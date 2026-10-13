package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq

class ShellPromptTargetsTest {
    private fun target(id: String) = ShellPromptTarget(id, id, "icon", "hint $id", "send") { _, _ -> "ok" }

    fun testLatestRegistrationRoutes() {
        val targets = ShellPromptTargets()
        var changes = 0
        val stop = targets.addListener { changes++ }
        assertEq(null, targets.routing)
        val first = Any()
        val second = Any()
        targets.register(first, target("calories"))
        targets.register(second, target("notes"))
        assertEq("notes", targets.routing?.id)
        targets.unregister(second)
        assertEq("calories", targets.routing?.id)
        targets.register(second, target("calories"))
        targets.unregister(first)
        assertEq("calories", targets.routing?.id)
        targets.unregister(second)
        assertEq(null, targets.active)
        assertEq(5, changes)
        targets.unregister(second)
        assertEq(5, changes)
        stop()
        targets.register(first, target("calories"))
        assertEq(5, changes)
    }

    fun testBypassFallsBackToTheShell() {
        val targets = ShellPromptTargets()
        val owner = Any()
        targets.register(owner, target("calories"))
        targets.bypass(true)
        assertEq(null, targets.routing)
        assertEq("calories", targets.active?.id)
        targets.register(owner, target("calories"))
        assertEq(false, targets.bypassed)
        targets.bypass(true)
        targets.unregister(owner)
        assertEq(false, targets.bypassed)
    }

    fun testMessages() {
        assertEq("Install first.", ShellPromptTargets.message(ShellPromptError("Install first.")))
        assertEq("nope", ShellPromptTargets.message(OrgApiException("x", "nope")))
        assertEq("That did not work.", ShellPromptTargets.message(IllegalStateException("boom")))
    }
}
