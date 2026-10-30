package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.testing.assertEq

class PromptDeckTest {
    fun testTheNewestPromptIsOnTop() {
        val deck = PromptDeck.of(
            listOf(
                TranscriptBlock.Prompt("Fix the login page"),
                TranscriptBlock.Error("Done."),
                TranscriptBlock.Steer("ForkMesh (phone)", "  Also add a test \n"),
                TranscriptBlock.Steer("reviewer", "Not you"),
                TranscriptBlock.Prompt("   "),
            ),
        )
        assertEq(listOf("Also add a test", "Fix the login page"), deck.prompts)
        assertEq("Also add a test", deck.prompt(0))
    }

    fun testScrollingBackStopsAtTheFirstPrompt() {
        val deck = PromptDeck.said(listOf("one", "two", "three"))
        assertEq("3/3", deck.position(0))
        assertEq(1, deck.older(0))
        assertEq(2, deck.older(2))
        assertEq("1/3", deck.position(2))
        assertEq(0, deck.newer(0))
        assertEq("one", deck.prompt(9))
    }

    fun testTheStackShowsUpToTwoCardsBehind() {
        val deck = PromptDeck.said(listOf("one", "two", "three", "four"))
        assertEq(2, deck.behind(0))
        assertEq(1, deck.behind(2))
        assertEq(0, deck.behind(3))
        assertEq("", PromptDeck.said(listOf("only")).position(0))
    }

    fun testTheHeadlineIsOneLine() {
        assertEq("Fix the login page", PromptDeck.said(listOf("Fix\n\nthe   login\tpage")).headline(0))
        assertEq("", PromptDeck.said(emptyList()).headline(0))
    }
}
