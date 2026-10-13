import Foundation
import XCTest

@testable import FMControl

@MainActor
final class AgentTranscriptFormatTests: XCTestCase {
    func testReadableQuestionData() {
        let source = #"{"questions":[{"question":"Destination?","options":[{"label":"Staging","description":"Preview environment"}]}]}"#
        let result = TranscriptJson.readable(source)
        XCTAssertNotNil(result)
        XCTAssertTrue(result?.contains("Destination?") == true)
        XCTAssertTrue(result?.contains("Staging") == true)
        XCTAssertTrue(result?.contains("Preview environment") == true)
        XCTAssertFalse(result?.contains("{\"questions\"") == true)
        XCTAssertNil(TranscriptJson.readable("ordinary output"))
    }

    func testDesktopLogBecomesStatusPromptToolAndText() async {
        let log = """
            ==> Agent: Claude Code
            ==> Creating worktree /w/agent-7

            > Fix the login bug
            please

            I'll look at **auth** first.

            [Read src/auth.ts]

            [Bash npm test]
            !! npm exited 1
            ==> User steering prompt
            try again
            ==> Stop requested by user.
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(log),
            [
                .status(["Agent: Claude Code", "Creating worktree /w/agent-7"]),
                .prompt("Fix the login bug\nplease"),
                .text("I'll look at **auth** first."),
                .tools([TranscriptTool("Read", "src/auth.ts"), TranscriptTool("Bash", "npm test")]),
                .error("npm exited 1"),
                .prompt("try again"),
                .status(["Stop requested by user."]),
            ])
    }

    func testMarkdownHeadingsListsCodeAndTables() async {
        let text = """
            ## Summary
            - one
              - nested
            2. second

            ```swift
            let x = 1

            print(x)
            ```
            | a | b |
            |---|---|
            ---
            [x]
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(text),
            [
                .heading(level: 2, text: "Summary"),
                .bullet(marker: "•", depth: 0, text: "one"),
                .bullet(marker: "•", depth: 1, text: "nested"),
                .bullet(marker: "2.", depth: 0, text: "second"),
                .code(language: "swift", text: "let x = 1\n\nprint(x)"),
                .table(TranscriptTable(header: ["a", "b"], rows: [], align: [.start, .start])),
                .rule,
                .text("[x]"),
            ])
    }

    func testTablesBecomeGridsWithAlignmentAndEscapes() async {
        let text = """
            | Item | Count | Note |
            | :--- | ---: | :-: |
            | `a|b` | 3 | x \\| y |
            | short |

            | not | a table |
            | no separator |
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(text),
            [
                .table(
                    TranscriptTable(
                        header: ["Item", "Count", "Note"],
                        rows: [["`a|b`", "3", "x | y"], ["short", "", ""]],
                        align: [.start, .end, .center])),
                .code(language: "", text: "| not | a table |\n| no separator |"),
            ])
    }

    func testTaskNotificationsBecomeCardsWithTheirResult() async {
        let log = """
            ==> Task completed: Agent "Fix runner" finished
                Done.

                | a | b |
                |---|---|
                | 1 | 2 |

            ==> Task failed: Background command "make" failed with exit code 2
            ==> Task update: Monitor event: "build"
                EXIT 0
            Back to the reply.
            ==> Task cost: $0.12
            """
        let blocks = AgentTranscriptFormat.blocks(log)
        XCTAssertEqual(
            blocks,
            [
                .task(TranscriptTask(status: "completed", summary: "Agent \"Fix runner\" finished", body: "Done.\n\n| a | b |\n|---|---|\n| 1 | 2 |")),
                .task(TranscriptTask(status: "failed", summary: "Background command \"make\" failed with exit code 2", body: "")),
                .task(TranscriptTask(status: "update", summary: "Monitor event: \"build\"", body: "EXIT 0")),
                .text("Back to the reply."),
                .status(["Task cost: $0.12"]),
            ])
        let tones: [TranscriptTaskTone] = blocks.prefix(3).compactMap {
            if case .task(let task) = $0 { return task.tone }
            return nil
        }
        XCTAssertEqual(tones, [.done, .failed, .running])
        guard case .task(let first) = blocks[0] else { return XCTFail("no task") }
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(first.body),
            [.text("Done."), .table(TranscriptTable(header: ["a", "b"], rows: [["1", "2"]], align: [.start, .start]))])
    }

    func testSteersAndPeerMessagesAreNamedBubbles() async {
        let log = """
            > ⟵ ForkMesh (phone): Also run the tests

            > ⟵ a-13: ping from a

            > ⟵ no colon here
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(log),
            [
                .steer(from: "ForkMesh (phone)", text: "Also run the tests"),
                .steer(from: "a-13", text: "ping from a"),
                .prompt("⟵ no colon here"),
            ])
        XCTAssertTrue(TranscriptBlock.steerFromYou("ForkMesh (phone)"))
        XCTAssertFalse(TranscriptBlock.steerFromYou("a-13"))
    }

    func testToolRunsGroupUntilSomethingElseArrives() async {
        XCTAssertEqual(
            AgentTranscriptFormat.blocks("[Read a]\n\n[Grep b]\n\n[Bash make]\n\nDone.\n\n[Edit c]"),
            [
                .tools([TranscriptTool("Read", "a"), TranscriptTool("Grep", "b"), TranscriptTool("Bash", "make")]),
                .text("Done."),
                .tools([TranscriptTool("Edit", "c")]),
            ])
    }

    func testRunnerToolLinesBecomeChips() async {
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(
                "Checking workflows.\n\n\u{25B8} Bash: cd /var/lib/forkmesh-runner/work/6d763db74bc070caa14fbd14/app && ls\n\n\u{25B8} Read: server/actions.go\n\n\u{25B8} TodoWrite\n\nDone.\n\n\u{25B8} not a tool line"),
            [
                .text("Checking workflows."),
                .tools([
                    TranscriptTool("Bash", "cd /var/lib/forkmesh-runner/work/6d763db74bc070caa14fbd14/app && ls"),
                    TranscriptTool("Read", "server/actions.go"),
                    TranscriptTool("TodoWrite", ""),
                ]),
                .text("Done."),
                .text("\u{25B8} not a tool line"),
            ])
    }

    func testMultiLineToolCallsFoldIntoOneChip() async {
        let log = """
            [Bash timeout 60 python3 -u - <<'EOF'
            import next_common as c

            cmd=["ssh","-v"]
            p=c.run(cmd)…]

            [Bash cd tools && python3 - <<'EOF'
            x = [1, 2]
            print(x)
            EOF]

            [Read a.py]

            Done.
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(log),
            [
                .tools([
                    TranscriptTool("Bash", "timeout 60 python3 -u - <<'EOF'\nimport next_common as c\n\ncmd=[\"ssh\",\"-v\"]\np=c.run(cmd)…"),
                    TranscriptTool("Bash", "cd tools && python3 - <<'EOF'\nx = [1, 2]\nprint(x)\nEOF"),
                    TranscriptTool("Read", "a.py"),
                ]),
                .text("Done."),
            ])
    }

    func testAnOpenBracketThatIsNotAToolStaysText() async {
        XCTAssertEqual(
            AgentTranscriptFormat.blocks("[see the docs](https://x.dev) for more\nand here\n\nNext."),
            [.text("[see the docs](https://x.dev) for more\nand here"), .text("Next.")])
        let long = "[Note " + String(repeating: "word ", count: 80)
        XCTAssertEqual(AgentTranscriptFormat.blocks(long + "\nmore\n\nNext."), [.text(long + "\nmore"), .text("Next.")])
        XCTAssertEqual(AgentTranscriptFormat.blocks("[Bash make\nall"), [.text("[Bash make\nall")])
    }

    func testTheCacheFoldsAMultiLineToolOnceItCloses() async {
        var cache = TranscriptBlockCache()
        let text = "[Bash make \\\n  all]\n\nOk.\n"
        for end in 1...text.count {
            let part = String(text.prefix(end))
            XCTAssertEqual(cache.update(part), AgentTranscriptFormat.blocks(part), "at \(end)")
        }
        XCTAssertEqual(cache.update(text), [.tools([TranscriptTool("Bash", "make \\\n  all")]), .text("Ok.")])
    }

    func testJsonIsPrettyPrinted() async {
        let log = """
            ```json
            {"a":1,"b":[true,null,"x,y"],"c":{}}
            ```

            ```
            [1, 2.5e3, -0.1]
            ```

            {"k": "v"}

            ```json
            {not json}
            ```
            """
        XCTAssertEqual(
            AgentTranscriptFormat.blocks(log),
            [
                .json("{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null,\n    \"x,y\"\n  ],\n  \"c\": {}\n}"),
                .json("[\n  1,\n  2.5e3,\n  -0.1\n]"),
                .json("{\n  \"k\": \"v\"\n}"),
                .code(language: "json", text: "{not json}"),
            ])
        XCTAssertNil(TranscriptJson.pretty("{\"a\":01}"))
        XCTAssertNil(TranscriptJson.pretty("{\"a\":1} x"))
        XCTAssertEqual(TranscriptJson.pretty(#"{"q":"say \"hi\""}"#), "{\n  \"q\": \"say \\\"hi\\\"\"\n}")
    }

    func testAnUnclosedFenceStillShowsItsCode() async {
        XCTAssertEqual(
            AgentTranscriptFormat.blocks("```\nstreaming"),
            [.code(language: "", text: "streaming")])
    }

    func testInlineBoldCodeAndLinks() async {
        XCTAssertEqual(
            AgentTranscriptFormat.inline("Run `make` then **ship** it, see [docs](https://forkmesh.com/docs) or [x](file)."),
            [
                TranscriptInline("Run "),
                TranscriptInline("make", .code),
                TranscriptInline(" then "),
                TranscriptInline("ship", .bold),
                TranscriptInline(" it, see "),
                TranscriptInline("docs", .link, url: "https://forkmesh.com/docs"),
                TranscriptInline(" or [x](file)."),
            ])
        XCTAssertEqual(AgentTranscriptFormat.inline("a ** b `"), [TranscriptInline("a ** b `")])
    }

    func testInlineItalicAndBareLinks() async {
        XCTAssertEqual(
            AgentTranscriptFormat.inline("merged but *not pushed*, _also_ __bold__; see https://forkmesh.com/a."),
            [
                TranscriptInline("merged but "),
                TranscriptInline("not pushed", .italic),
                TranscriptInline(", "),
                TranscriptInline("also", .italic),
                TranscriptInline(" "),
                TranscriptInline("bold", .bold),
                TranscriptInline("; see "),
                TranscriptInline("https://forkmesh.com/a", .link, url: "https://forkmesh.com/a"),
                TranscriptInline("."),
            ])
        XCTAssertEqual(
            AgentTranscriptFormat.inline("snake_case_name and 2 * 3 * 4"),
            [TranscriptInline("snake_case_name and 2 * 3 * 4")])
    }

    func testTheCacheMatchesAFullParseAsTextArrives() async throws {
        let full = Array(try fixture("transcript.txt"))
        var cache = TranscriptBlockCache()
        var at = 0
        var step = 1
        while at < full.count {
            at = min(full.count, at + step)
            step = step % 97 + 13
            let text = String(full[0..<at])
            XCTAssertEqual(cache.update(text), AgentTranscriptFormat.blocks(text), "at \(at)")
        }
        XCTAssertEqual(cache.update("reset\n"), AgentTranscriptFormat.blocks("reset\n"))
    }

    func testTheSharedTranscriptParsesToTheSharedBlocks() async throws {
        let blocks = AgentTranscriptFormat.blocks(try fixture("transcript.txt"))
        var expected = try fixture("blocks.txt")
        while expected.hasSuffix("\n") { expected.removeLast() }
        XCTAssertEqual(blocks.map(describe).joined(separator: "\n"), expected)
    }

    private func firstLine(_ text: String) -> String {
        String(text.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
    }

    private func lineCount(_ text: String) -> Int {
        text.split(separator: "\n", omittingEmptySubsequences: false).count
    }

    private func describe(_ block: TranscriptBlock) -> String {
        switch block {
        case .status(let lines): return "status " + lines.joined(separator: " | ")
        case .error(let text): return "error " + text
        case .prompt(let text): return "prompt " + firstLine(text)
        case .steer(let from, let text):
            return "steer " + from + (TranscriptBlock.steerFromYou(from) ? " (you) " : " ") + text
        case .tools(let calls): return "tools " + calls.map(\.name).joined(separator: ",")
        case .heading(let level, let text): return "heading\(level) " + text
        case .bullet(let marker, let depth, let text): return "bullet \(marker) \(depth) " + text
        case .code(let language, let text): return "code \(language) \(lineCount(text))"
        case .json(let text): return "json \(lineCount(text))"
        case .table(let table):
            let align = table.align.map { $0 == .start ? "s" : $0 == .center ? "c" : "e" }.joined()
            return "table \(table.header.count)x\(table.rows.count) " + table.header.joined(separator: "|") + " " + align
        case .task(let task):
            let tone = task.tone == .done ? "done" : task.tone == .failed ? "failed" : "running"
            let kinds = AgentTranscriptFormat.blocks(task.body).map { describe($0).split(separator: " ", maxSplits: 1).first.map(String.init) ?? "" }
            return "task \(task.status) \(tone) " + task.summary + " [" + kinds.joined(separator: ", ") + "]"
        case .rule: return "rule"
        case .text(let text): return "text " + firstLine(text)
        }
    }

    private func fixture(_ name: String) throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { url.deleteLastPathComponent() }
        return try String(contentsOf: url.appendingPathComponent("conformance/agent-transcript/v1/\(name)"), encoding: .utf8)
    }
}
