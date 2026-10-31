package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.testing.assertEq

class ShellPanesTest {
    fun testPhonesAndCoverScreensKeepTheStack() {
        assertEq(ShellPaneMetrics.Phone, ShellPanes.metrics(412.0, 915.0))
        assertEq(ShellPaneMetrics.Phone, ShellPanes.metrics(915.0, 412.0))
        assertEq(ShellPaneMetrics.Phone, ShellPanes.metrics(466.0, 678.0))
    }

    fun testAFoldedOutInnerDisplaySplitsInBothPoses() {
        assertEq(ShellPaneMetrics(wide = true, list = 280.0), ShellPanes.metrics(673.0, 841.0))
        assertEq(ShellPaneMetrics(wide = true, list = 306.0), ShellPanes.metrics(841.0, 673.0, content = 765.0))
    }

    fun testListWidthStaysInBounds() {
        assertEq(280.0, ShellPanes.listWidth(500.0))
        assertEq(380.0, ShellPanes.listWidth(2000.0))
    }

    fun testChatKeepsItsOwnSidebar() {
        val wide = ShellPanes.metrics(673.0, 841.0)
        assertEq(false, ShellPanes.splits(AppTab.Chat, wide))
        assertEq(true, ShellPanes.splits(AppTab.Code, wide))
        assertEq(false, ShellPanes.splits(AppTab.Code, ShellPaneMetrics.Phone))
    }
}
