package com.forkmesh.nativeapp.core

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AppReturnGateTest {
    fun testAQuickInactiveStepIsNotAReturn() {
        val gate = AppReturnGate(away = false)
        assertEq(false, gate.isReturn(AppVisibility.INACTIVE))
        assertEq(false, gate.isReturn(AppVisibility.RESUMED))
    }

    fun testAReturnIsOnlyAResumeAfterTheBackground() {
        val gate = AppReturnGate(away = false)
        assertEq(false, gate.isReturn(AppVisibility.INACTIVE))
        assertEq(false, gate.isReturn(AppVisibility.HIDDEN))
        assertEq(false, gate.isReturn(AppVisibility.INACTIVE))
        assertEq(true, gate.isReturn(AppVisibility.RESUMED))
        assertEq(false, gate.isReturn(AppVisibility.RESUMED))
    }

    fun testTheFirstResumeAfterStartingCounts() {
        val gate = AppReturnGate()
        assertEq(true, gate.isReturn(AppVisibility.RESUMED))
        assertEq(false, gate.isReturn(AppVisibility.RESUMED))
    }

    fun testOnlyHiddenIsInvisible() {
        expect(AppVisibility.RESUMED.visible)
        expect(AppVisibility.INACTIVE.visible)
        expect(!AppVisibility.HIDDEN.visible)
    }
}
