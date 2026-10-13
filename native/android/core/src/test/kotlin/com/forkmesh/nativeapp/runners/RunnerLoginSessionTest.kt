package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class RunnerLoginSessionTest {
    private val loginId = "0123456789abcdef0123456789abcdef"

    private fun json(text: String): JsonMap = Json.parse(text).asJsonMap()

    private fun b64(text: String): String = Base64.getEncoder().encodeToString(text.toByteArray())

    fun testStartReplies() {
        val started = RunnerLoginSession("FMNEXT-R1", "claude-code")
        assertEq("fmnext-r1", started.runner)
        assertEq(RunnerLoginSession.Start.Started, started.applyStartReply(201, json("""{"id":"$loginId","session":{"state":"pending"}}""")))
        assertEq(loginId, started.id)
        expect(started.open)
        val attached = RunnerLoginSession("fmnext-r1", "claude-code")
        assertEq(RunnerLoginSession.Start.Attached, attached.applyStartReply(409, json("""{"error":"login_active","id":"$loginId"}""")))
        expect(attached.open)
        val refused = RunnerLoginSession("fmnext-r1", "claude-code")
        assertEq(RunnerLoginSession.Start.Failed, refused.applyStartReply(403, json("{}")))
        expect(!refused.open)
        expect(refused.ended)
        expect(refused.error.startsWith("Only an administrator"))
        val badId = RunnerLoginSession("fmnext-r1", "claude-code")
        assertEq(RunnerLoginSession.Start.Failed, badId.applyStartReply(201, json("""{"id":"../x"}""")))
        val server = RunnerLoginSession("fmnext-r1", "codex")
        server.applyStartReply(409, json("""{"error":"runner_inactive","message":"Runner is revoked."}"""))
        assertEq("Runner is revoked.", server.statusText)
    }

    fun testOutputChunksInOrderAndSkipsBadOnes() {
        val session = RunnerLoginSession("fmnext-r1", "claude-code")
        session.attach(loginId)
        session.applyOutputReply(
            json(
                """{"state":"running","chunks":[
                  {"seq":2,"data":"${b64("world https://claude.ai/oauth/authorize?code=1.")}"},
                  {"seq":1,"data":"${b64("hello ")}"},
                  {"seq":3,"data":"!!!notbase64"}
                ],"more":true}""",
            ),
        )
        assertEq(2L, session.afterSeq)
        expect(session.more)
        assertEq("hello world https://claude.ai/oauth/authorize?code=1.", session.screenText)
        assertEq(listOf("https://claude.ai/oauth/authorize?code=1"), session.urls)
        session.applyOutputReply(json("""{"state":"running","chunks":[{"seq":2,"data":"${b64("dup")}"},{"seq":4,"data":"${b64("\r\nok")}"}],"truncated":true,"nextSeq":6}"""))
        assertEq(6L, session.afterSeq)
        expect(session.screenText.contains("[older sign-in output was trimmed]"))
        expect(!session.screenText.contains("dup"))
        expect(session.screenText.endsWith("ok"))
        session.applyOutputReply(json("""{"state":"succeeded","message":"","chunks":[]}"""))
        expect(!session.open)
        expect(session.ended)
        expect(session.statusText.startsWith("Signed in."))
    }

    fun testCancelAndStatus() {
        val session = RunnerLoginSession("fmnext-r1", "codex")
        session.attach(loginId)
        expect(session.needsCancelOnClose)
        session.applyCancelReply(json("""{"ok":true}"""))
        assertEq("cancelled", session.state)
        expect(!session.needsCancelOnClose)
        val gone = RunnerLoginSession("fmnext-r1", "codex")
        gone.attach(loginId)
        gone.applyOutputStatus(404)
        expect(gone.ended)
        gone.attach("nothex")
        assertEq(loginId, gone.id)
    }

    fun testTerminalText() {
        val raw = "\u001b[1mBold\u001b[0m line\r\nprogress 10%\rprogress 99%\n\u001b]0;title\u0007Paste code:\u0008"
        assertEq("Bold line\nprogress 99%\nPaste code:", RunnerLoginText.terminalText(raw))
        assertEq(
            listOf("https://auth.openai.com/codex/device"),
            RunnerLoginText.urls("Go to https://auth.openai.com/codex/device, then http://insecure.example and https://auth.openai.com/codex/device"),
        )
        assertEq(emptyList<String>(), RunnerLoginText.urls("https://user@evil.example/x"))
    }

    fun testInputValidation() {
        expect(RunnerLoginText.inputValid("abc#123\r"))
        expect(RunnerLoginText.inputValid("tab\there\n"))
        expect(RunnerLoginText.inputValid("emoji 😀"))
        expect(!RunnerLoginText.inputValid(""))
        expect(!RunnerLoginText.inputValid("\u001b[A"))
        expect(!RunnerLoginText.inputValid("x\u007f"))
        expect(!RunnerLoginText.inputValid("x​"))
        expect(RunnerLoginText.inputValid("a".repeat(4096)))
        expect(!RunnerLoginText.inputValid("a".repeat(4097)))
        assertEq("code#1\r", RunnerLoginText.codeSubmission("  code#1\r\n "))
        assertEq("", RunnerLoginText.codeSubmission(" \n"))
    }
}
