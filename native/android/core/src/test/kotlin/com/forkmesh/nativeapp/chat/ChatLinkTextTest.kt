package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatLinkTextTest {
    fun testIssueAndPullReferencesLinkBesideAddresses() {
        assertEq(
            listOf(
                ChatTextSegment("landed "),
                ChatTextSegment("PR #8", "forkmesh://forkmesh/forkmesh/pulls/8"),
                ChatTextSegment(" for "),
                ChatTextSegment("#7", "forkmesh://forkmesh/forkmesh/issues/7"),
                ChatTextSegment(", "),
                ChatTextSegment("bob/lib#3", "forkmesh://bob/lib/issues/3"),
                ChatTextSegment(" and "),
                ChatTextSegment("https://forkmesh.com/status", "https://forkmesh.com/status"),
                ChatTextSegment(" abc1234"),
            ),
            linkChatReferences(parseChatText("landed PR #8 for #7, bob/lib#3 and https://forkmesh.com/status abc1234")),
        )
        assertEq(
            listOf(ChatTextSegment("#7 and "), ChatTextSegment("bob/lib#3", "forkmesh://bob/lib/issues/3")),
            linkChatReferences(parseChatText("#7 and bob/lib#3"), null),
        )
    }

    fun testAPastedUrlLinksWithoutSwallowingTheSentencePunctuation() {
        assertEq(
            listOf(
                ChatTextSegment("status is at "),
                ChatTextSegment("https://forkmesh.com/status", "https://forkmesh.com/status"),
                ChatTextSegment(", see it."),
            ),
            parseChatText("status is at https://forkmesh.com/status, see it."),
        )
    }

    fun testAParenthesisTheAddressOpenedStaysPartOfTheAddress() {
        assertEq(
            listOf(
                ChatTextSegment("(see "),
                ChatTextSegment(
                    "https://en.wikipedia.test/wiki/Fork_(software)",
                    "https://en.wikipedia.test/wiki/Fork_(software)",
                ),
                ChatTextSegment(")"),
            ),
            parseChatText("(see https://en.wikipedia.test/wiki/Fork_(software))"),
        )
    }

    fun testMarkdownLinksShowTheirLabelAndPointAtTheirTarget() {
        assertEq(
            listOf(
                ChatTextSegment("read the "),
                ChatTextSegment("release notes", "https://forkmesh.com/b"),
                ChatTextSegment(" now"),
            ),
            parseChatText("read the [release notes](https://forkmesh.com/b) now"),
        )
    }

    fun testATargetThatCouldExecuteIsNotALinkAndItsWordsSurvive() {
        assertEq(listOf(ChatTextSegment("click me stays text")), parseChatText("[click me](javascript:alert(1)) stays text"))
        assertEq("", safeChatLink("java\tscript:alert(1)"))
        assertEq("", safeChatLink("/dashboard"))
        assertEq("mailto:hi@forkmesh.com", safeChatLink("mailto:hi@forkmesh.com"))
    }

    fun testTappingALinkOpensItsAddress() {
        val segments = parseChatText("see [the notes](https://forkmesh.com/blog) today")
        assertEq("see the notes today", segments.joinToString("") { it.text })
        val link = segments.single { it.isLink }
        assertEq("the notes", link.text)
        assertEq("https://forkmesh.com/blog", link.url)
    }

    fun testAMessageWithNoLinkStaysAPlainTextRun() {
        assertEq(listOf(ChatTextSegment("plain @jett message")), parseChatText("plain @jett message"))
        expect(parseChatText("plain @jett message").none { it.isLink })
    }

    fun testLinksAreNormalisedTheWayDartUriPrintsThem() {
        assertEq("https://example.com/Path", safeChatLink("HTTPS://Example.COM/Path"))
        assertEq("https://forkmesh.com/x", safeChatLink("https://forkmesh.com:443/x"))
        assertEq("https://x.test/a%20b", safeChatLink("https://x.test/a b"))
        assertEq("", safeChatLink("https:no-host"))
        assertEq("", safeChatLink("file:///etc/passwd"))
        assertEq("", safeChatLink("https://" + "a".repeat(2048)))
    }

    fun testFencedRunsSplitIntoProseAndCode() {
        assertEq(
            listOf(
                MessageBodySegment(code = false, text = "look:"),
                MessageBodySegment(code = true, text = "a  b\n c"),
                MessageBodySegment(code = false, text = "done"),
            ),
            messageBodySegments("look:\n```a  b\n c\n```\ndone"),
        )
        assertEq(listOf(MessageBodySegment(code = true, text = "tail")), messageBodySegments("```tail"))
        assertEq(listOf(MessageBodySegment(code = false, text = "plain")), messageBodySegments("plain"))
    }
}
