package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.testing.assertEq

class CodeButtonTest {
    private val tool = RepoRef("alice", "tool")
    private val other = RepoRef("bob", "beta")

    fun testNoRepoInUse() {
        assertEq(CodeButtonPlan.NoRepo, CodeButton.plan(null, listOf(tool)))
    }

    fun testRepoScreenAtTheBottomOfTheStackIsKept() {
        assertEq(CodeButtonPlan.PopToRepo, CodeButton.plan(RepoRef("Alice", "Tool"), listOf(tool, null, other)))
        assertEq(CodeButtonPlan.PopToRepo, CodeButton.plan(tool, listOf(tool)))
    }

    fun testAnyOtherStackStartsOverFromTheRepoScreen() {
        assertEq(CodeButtonPlan.Reset, CodeButton.plan(tool, emptyList()))
        assertEq(CodeButtonPlan.Reset, CodeButton.plan(tool, listOf(other, tool, null)))
        assertEq(CodeButtonPlan.Reset, CodeButton.plan(tool, listOf(null, tool)))
    }

    fun testHighlight() {
        assertEq(null, CodeButton.selected(onCodeTab = false, depth = 3))
        assertEq(CodeBarItem.Repos, CodeButton.selected(onCodeTab = true, depth = 1))
        assertEq(CodeBarItem.Code, CodeButton.selected(onCodeTab = true, depth = 2))
    }
}
