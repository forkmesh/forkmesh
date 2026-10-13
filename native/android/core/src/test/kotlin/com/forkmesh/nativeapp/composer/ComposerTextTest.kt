package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ComposerTextTest {
    fun testSlashCommandsMatchABareWordOnly() {
        val all = SlashCommands.builtIn(canPickModel = true, canAttach = true, canDictate = true)
        assertEq(listOf("model", "photo", "camera", "screenshot", "dictate"), all.map { it.name })
        assertEq(listOf("screenshot"), SlashCommands.matching("/s", all).map { it.name })
        assertEq(all, SlashCommands.matching("/", all))
        assertEq(listOf("photo"), SlashCommands.matching("/PH", all).map { it.name })
        assertEq(listOf("camera"), SlashCommands.matching("/c", all).map { it.name })
        assertEq(emptyList<SlashCommand>(), SlashCommands.matching("/photo now", all))
        assertEq(emptyList<SlashCommand>(), SlashCommands.matching("photo", all))
        assertEq(emptyList<SlashCommand>(), SlashCommands.matching("/zzz", all))
    }

    fun testSurfacesOnlyOfferWhatTheyHonour() {
        assertEq(listOf("dictate"), SlashCommands.builtIn(false, false, true).map { it.name })
        assertEq(emptyList<String>(), SlashCommands.builtIn(false, false, false).map { it.name })
    }

    fun testFixLeadsWithTheAskAndKeepsWhatWasTyped() {
        assertEq("Please fix: the login button overlaps the logo", ComposerRules.fixPrompt("the login button overlaps the logo"))
        assertEq("Please fix: crash on save", ComposerRules.fixPrompt("  crash on save\n"))
        assertEq("Please fix: line one\nline two", ComposerRules.fixPrompt("line one\nline two"))
        assertEq("Please fix", ComposerRules.fixPrompt(" \n "))
    }

    fun testDictationAppendsToWhatWasTyped() {
        assertEq("hello", DictationText.merge("", "hello"))
        assertEq("fix the bug in login", DictationText.merge("fix the bug", "in login"))
        assertEq("line\nnext", DictationText.merge("line\n", "next"))
        assertEq("typed", DictationText.merge("typed", ""))
    }

    fun testSendAndStopRules() {
        expect(ComposerRules.showsStop(running = true, text = "", attachmentCount = 0, canStop = true))
        expect(!ComposerRules.showsStop(running = true, text = "steer", attachmentCount = 0, canStop = true))
        expect(!ComposerRules.showsStop(running = true, text = "", attachmentCount = 1, canStop = true))
        expect(!ComposerRules.showsStop(running = true, text = "", attachmentCount = 0, canStop = false))
        expect(ComposerRules.canSend("x", 0, busy = false, sendable = true))
        expect(ComposerRules.canSend(" ", 2, busy = false, sendable = true))
        expect(!ComposerRules.canSend("x", 0, busy = true, sendable = true))
        expect(!ComposerRules.canSend("  ", 0, busy = false, sendable = true))
        expect(!ComposerRules.canSend("x", 0, busy = false, sendable = false))
        assertEq(AgentQueueApi.MAX_AGENT_PROMPT_CHARS, ComposerRules.clamp("a".repeat(9000)).length)
    }

    fun testDraftsSurviveARestartAndClearWhenBlank() {
        val prefs = MemoryPrefStore()
        val drafts = PromptDraftStore(prefs)
        drafts.save(PromptDraftStore.SHELL, "half a thought")
        assertEq("half a thought", PromptDraftStore(prefs).load(PromptDraftStore.SHELL))
        assertEq("half a thought", prefs.getString("composer/draft/shell"))
        drafts.save(PromptDraftStore.SHELL, "   ")
        expect(!prefs.contains("composer/draft/shell"))
        drafts.save(PromptDraftStore.agentSurface("alice/app#3"), "steer it")
        assertEq("steer it", drafts.load("agent/alice/app#3"))
        drafts.clear("agent/alice/app#3")
        assertEq("", drafts.load("agent/alice/app#3"))
        drafts.save("", "ignored")
        expect(prefs.snapshot().isEmpty())
    }
}
