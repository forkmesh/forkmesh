package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf

class CoveFilesTest {
    fun testTenTemplatesWithUniqueIdsAndPaths() {
        val templates = CoveFiles.templates()
        assertEq(10, templates.size)
        assertEq(10, templates.map { it.id }.toSet().size)
        assertEq(10, templates.map { it.defaultPath }.toSet().size)
        for (template in templates) {
            expect(template.title.isNotEmpty() && template.description.isNotEmpty() && template.mime.isNotEmpty())
            assertEq(template.editor, CoveFiles.editorKind(template.defaultPath, template.mime))
            when (template.editor) {
                CoveEditorKind.Spreadsheet -> expect(CoveFiles.parseCsv(template.body).isNotEmpty())
                CoveEditorKind.Pipeline -> assertEq(5, CovePipeline.parse(template.body).stages.size)
                CoveEditorKind.Text -> Unit
            }
        }
        assertEq("budget.csv", CoveFiles.template("budget")?.defaultPath)
        assertEq(null, CoveFiles.template("nope"))
    }

    fun testCsvMatchesDesktop() {
        val parsed = CoveFiles.parseCsv("Name,Notes,Amount\r\n\"Acme, Inc.\",\"Line one\nLine two\",\"12\"\"34\"\r\n")
        assertEq(listOf(listOf("Name", "Notes", "Amount"), listOf("Acme, Inc.", "Line one\nLine two", "12\"34")), parsed)
        assertEq(parsed, CoveFiles.parseCsv(CoveFiles.serializeCsv(parsed)))
        val serialized = CoveFiles.serializeCsv(listOf(listOf("a,b", "c\"d", "e"), listOf("", "x\ny")))
        assertEq("22612c62222c2263222264222c650d0a2c22780a79220d0a", hexOf(serialized.toByteArray()))
        assertEq(listOf(listOf("")), CoveFiles.parseCsv(""))
        assertEq(listOf(listOf("A", "", ""), listOf("", "", "")), CoveFiles.parseCsv("A,,\r\n,,\r\n"))
        assertEq(listOf(listOf("a"), listOf("b")), CoveFiles.parseCsv("a\nb"))
    }

    fun testShareUrlsMatchDesktop() {
        assertEq("forkmesh://cove/al%20ice/my%20repo/id-1", CoveFiles.shareUrl("al ice", "my repo", "id-1"))
        assertEq("", CoveFiles.shareUrl("", "r", "c"))
        assertEq(CoveLink("alice", "repo", "abc"), CoveFiles.parseShareUrl("forkmesh://cove/alice/repo/abc"))
        assertEq(CoveLink("al ice", "my repo", "id-1"), CoveFiles.parseShareUrl("forkmesh://cove/al%20ice/my%20repo/id-1"))
        assertEq(null, CoveFiles.parseShareUrl("forkmesh://note/alice/repo/abc"))
        assertEq(null, CoveFiles.parseShareUrl("forkmesh://cove/alice/abc"))
        assertEq("c", CoveFiles.parseRouteId("a/b/c")?.coveId)
        assertEq(null, CoveFiles.parseRouteId("a/b"))
    }

    fun testUniqueNames() {
        assertEq("notes-3.md", CoveFiles.uniqueName("notes.md", listOf("notes.md", "notes-2.md")))
        assertEq("a/leads-2.pipeline.json", CoveFiles.uniqueName("a/leads.pipeline.json", listOf("a/leads.pipeline.json")))
        assertEq("x.md", CoveFiles.uniqueName("x.md", emptyList()))
    }

    fun testPipelineEditing() {
        val pipeline = CovePipeline.parse(CoveFiles.defaultPipelineJson())
        val now = 1_700_000_000_000L
        val lead = pipeline.addLead(actor = "alice", nowMs = now, id = "lead-1")
        assertEq("new", lead.stage)
        pipeline.move("lead-1", "won", "alice", now)
        pipeline.move("lead-1", "missing", "alice", now)
        pipeline.update(pipeline.leads[0].copy(value = 1500.0, company = "Acme"), now)
        assertEq(1500.0, pipeline.totals().first { it.stage.id == "won" }.value)
        val reparsed = CovePipeline.parse(pipeline.body)
        assertEq("won", reparsed.leads.first().stage)
        assertEq("Acme", reparsed.leads.first().company)
        assertEq(2, reparsed.history.size)
        val last = reparsed.history.last() as Map<*, *>
        assertEq("moved", last["action"])
        assertEq("2023-11-14T22:13:20.000Z", last["at"])
        reparsed.delete("lead-1", "bob")
        expect(reparsed.leads.isEmpty())
        assertEq(3, reparsed.history.size)
    }

    fun testPipelineValidation() {
        fun error(body: String): String? = try {
            CovePipeline.parse(body)
            null
        } catch (failure: CovePipelineException) {
            failure.message
        }
        assertEq("The pipeline root must be a JSON object.", error("[]"))
        assertEq("Invalid pipeline JSON.", error("{"))
        assertEq("A pipeline needs at least one stage.", error("{\"stages\":[]}"))
        assertEq("Every pipeline stage needs a unique id and name.", error("{\"stages\":[{\"id\":\"a\",\"name\":\"A\"},{\"id\":\"a\",\"name\":\"B\"}]}"))
        assertEq("Pipeline leads and history must be JSON arrays.", error("{\"stages\":[{\"id\":\"a\",\"name\":\"A\"}],\"leads\":{}}"))
        assertFails<CovePipelineException> { CovePipeline.parse("{\"stages\":[{\"id\":\"\",\"name\":\"A\"}]}") }
    }
}
