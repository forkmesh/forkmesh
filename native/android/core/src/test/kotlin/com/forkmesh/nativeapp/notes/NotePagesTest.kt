package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import kotlin.random.Random

class NotePagesTest {
    private fun note(id: String, title: String = id, emoji: String = "", unread: Boolean = false) = PageNote(id, title, emoji, unread = unread)

    fun testEmojiRules() {
        for (ok in listOf("📝", "👍🏽", "🇫🇷", "👩‍💻", "1️⃣", "#⃣", "*️⃣", "❤️", "🏳️‍🌈", "ö", "✅✅")) assertEq(true, NoteEmoji.isValid(ok), ok)
        for (bad in listOf("", "a", "📝a", "1", "#", "hi", "📝 📝", "\t", "\u0007", "1📝", "*")) assertEq(false, NoteEmoji.isValid(bad), bad)
        assertEq(false, NoteEmoji.isValid("😀".repeat(17)))
        assertEq(true, NoteEmoji.isValid("😀".repeat(16)))
        assertEq(false, NoteEmoji.isValid("👩‍👩‍👧‍👦👩‍👩‍👧‍👦👩‍👩‍👧‍👦"))
        assertEq(null, NoteEmoji.error("  📝 "))
        assertEq(null, NoteEmoji.error("  "))
        assertEq("Use a single emoji, such as 📝 or 🇫🇷.", NoteEmoji.error("ab"))
        assertEq("📝 Plan", NoteEmoji.titled(" 📝 ", "Plan"))
        assertEq("Plan", NoteEmoji.titled("", "Plan"))
        assertEq(40, NoteEmoji.common.size)
        assertEq(true, NoteEmoji.common.all(NoteEmoji::isValid))
        assertEq(NoteEmoji.common.size, NoteEmoji.common.toSet().size)
    }

    fun testParsesPagesAndDetail() {
        val list = NotePage.list(listOf(
            mapOf("id" to "p1", "title" to "", "emoji" to "📚", "accent" to "teal", "layout" to "continuous", "role" to "editor", "version" to 3.0, "entries" to 1L),
            mapOf("id" to "p2", "accent" to "neon", "layout" to "grid"),
            mapOf("title" to "no id"),
        ))
        assertEq(2, list.size)
        assertEq("Untitled page", list[0].displayTitle)
        assertEq(PageAccent.Teal, list[0].accent)
        assertEq(PageLayout.Continuous, list[0].layout)
        assertEq(true, list[0].canEdit)
        assertEq(false, list[0].isOwner)
        assertEq("1 entry", list[0].entriesLabel)
        assertEq(PageAccent.Blue, list[1].accent)
        assertEq(PageLayout.Outline, list[1].layout)
        assertEq(true, list[1].showEmoji)
        assertEq("0 entries", list[1].entriesLabel)
        val detail = NotePageDetail.from(mapOf(
            "page" to mapOf(
                "id" to "p1", "title" to "Handbook", "showEmoji" to false, "role" to "viewer",
                "outline" to listOf(
                    mapOf("id" to "e1", "kind" to "section", "depth" to 0, "label" to "Start", "collapsed" to true),
                    mapOf("id" to "e2", "kind" to "note", "depth" to 9.0, "noteId" to "n1"),
                    mapOf("id" to "e3", "kind" to "bogus"),
                ),
            ),
            "notes" to mapOf("n1" to mapOf("title" to "Intro", "emoji" to "👋", "unread" to true, "markdown" to "# Hi", "updatedAt" to 5L)),
        ))!!
        assertEq(listOf("e1", "e2"), detail.page.outline.map { it.id })
        assertEq(3, detail.page.outline[1].depth)
        assertEq(true, detail.page.outline[0].collapsed)
        assertEq(false, detail.page.showEmoji)
        assertEq(false, detail.page.canEdit)
        assertEq(2, detail.page.entryCount)
        val intro = detail.notes.getValue("n1")
        assertEq("n1", intro.id)
        assertEq("👋", intro.emoji)
        assertEq("# Hi", intro.markdown)
        assertEq(true, intro.unread)
        assertEq(null, PageNote.from(mapOf("title" to "x"))?.markdown)
        assertEq(null, NotePageDetail.from(emptyMap()))
        assertEq("Read", detail.withNote(intro.copy(title = "Read")).notes.getValue("n1").title)
    }

    fun testOutlineRowsAndCollapse() {
        val outline = listOf(
            PageEntry("s1", "section", 0, "Guides"),
            PageEntry("a", "note", 1, noteId = "n1"),
            PageEntry("b", "note", 1, label = "Renamed", noteId = "n2"),
            PageEntry("c", "note", 2, noteId = "gone"),
            PageEntry("s2", "section", 0, "Links", collapsed = true),
            PageEntry("d", "link", 1, "", url = "https://example.com"),
            PageEntry("s3", "section", 0, ""),
        )
        val notes = mapOf("n1" to note("n1", "One", "🧭", unread = true), "n2" to note("n2", "Two"))
        val rows = PageOutline.rows(outline, notes, showEmoji = true)
        assertEq(listOf("s1", "a", "b", "c", "s2", "s3"), rows.map { it.entry.id })
        assertEq(listOf("Guides", "One", "Renamed", "Locked note", "Links", "Untitled section"), rows.map { it.title })
        assertEq("🧭", rows[1].emoji)
        assertEq(true, rows[1].unread)
        assertEq(true, rows[3].locked)
        assertEq(false, rows[1].locked)
        assertEq(true, rows[4].collapsed)
        assertEq(true, rows[0].hasChildren)
        assertEq(false, rows[5].hasChildren)
        assertEq("", PageOutline.rows(outline, notes, showEmoji = false)[1].emoji)
        val opened = PageOutline.rows(outline, notes, true, toggled = setOf("s2", "s1"))
        assertEq(listOf("s1", "s2", "d", "s3"), opened.map { it.entry.id })
        assertEq("https://example.com", opened[2].title)
        assertEq(listOf("s1", "a", "b", "c", "s2", "d", "s3"), PageOutline.rows(PageOutline.toggle(outline, "s2"), notes, true).map { it.entry.id })
        val nested = listOf(
            PageEntry("top", "section", 0, "Top", collapsed = true),
            PageEntry("sub", "section", 1, "Sub"),
            PageEntry("x", "note", 2, noteId = "n1"),
            PageEntry("next", "section", 0, "Next"),
        )
        assertEq(listOf("top", "next"), PageOutline.rows(nested, notes, true).map { it.entry.id })
        assertEq("You don't have access to this note", PageOutline.LOCKED)
    }

    fun testReadingOrderAndNeighbours() {
        val outline = listOf(
            PageEntry("a", "note", 0, noteId = "n1"),
            PageEntry("s", "section", 0, "S"),
            PageEntry("b", "note", 0, noteId = "locked"),
            PageEntry("c", "note", 0, noteId = "n2"),
            PageEntry("d", "note", 0, noteId = "n1"),
        )
        val notes = mapOf("n1" to note("n1"), "n2" to note("n2"))
        assertEq(listOf("n1", "n2"), PageOutline.readable(outline, notes).map { it.second.id })
        assertEq("n1", PageOutline.firstNote(outline, notes))
        assertEq(null, PageOutline.firstNote(emptyList(), notes))
        assertEq(null to notes["n2"], PageOutline.neighbours(outline, notes, "n1"))
        assertEq(notes["n1"] to null, PageOutline.neighbours(outline, notes, "n2"))
        assertEq(null to null, PageOutline.neighbours(outline, notes, "zz"))
    }

    fun testOutlineEditing() {
        val random = Random(7)
        var outline = PageOutline.addNote(emptyList(), "n1", random)
        assertEq(0, outline[0].depth)
        assertEq(12, outline[0].id.length)
        assertEq(true, outline[0].id.all { it.isLetterOrDigit() })
        outline = PageOutline.addSection(outline, "  Part one ", random)
        assertEq("Part one", outline[1].label)
        outline = PageOutline.addNote(outline, "n2", random)
        assertEq(1, outline[2].depth)
        outline = PageOutline.addLink(outline, "Docs", "https://docs.example.com/a", random)
        assertEq(1, outline[3].depth)
        assertEq(outline, PageOutline.addLink(outline, "Bad", "http://plain.example.com", random))
        assertEq(outline, PageOutline.addSection(outline, "  ", random))
        assertEq(outline, PageOutline.addNote(outline, " ", random))
        assertEq(false, PageOutline.isLink("https://"))
        assertEq(true, PageOutline.isLink(" https://a.b/c?d=1 "))
        assertEq(0, PageOutline.indent(outline, 0)[0].depth)
        assertEq(2, PageOutline.indent(outline, 3)[3].depth)
        assertEq(2, PageOutline.indent(PageOutline.indent(outline, 3), 3)[3].depth)
        assertEq(0, PageOutline.outdent(outline, 2)[2].depth)
        assertEq(0, PageOutline.outdent(outline, 0)[0].depth)
        val deep = List(5) { PageEntry("e$it", "note", 3, noteId = "n") }
        assertEq(3, PageOutline.indent(deep, 2)[2].depth)
        val ids = outline.map { it.id }
        assertEq(listOf(ids[1], ids[2], ids[0], ids[3]), PageOutline.move(outline, 0, 2).map { it.id })
        assertEq(listOf(ids[3], ids[0], ids[1], ids[2]), PageOutline.move(outline, 3, -5).map { it.id })
        assertEq(outline, PageOutline.move(outline, 9, 0))
        assertEq(listOf(ids[0], ids[2], ids[3]), PageOutline.remove(outline, 1).map { it.id })
        assertEq(outline, PageOutline.remove(outline, 9))
        assertEq("New", PageOutline.rename(outline, 1, "  New ")[1].label)
        val full = List(PageOutline.MAX_ENTRIES) { PageEntry("x$it", "note", noteId = "n") }
        assertEq(false, PageOutline.canAdd(full))
        assertEq(full, PageOutline.addSection(full, "More", random))
    }

    fun testFieldsAndLinks() {
        val page = NotePage("p1", title = " T ", emoji = "📚", accent = PageAccent.Pink, layout = PageLayout.Continuous, showEmoji = false,
            outline = listOf(PageEntry("e1", "section", 0, "S")))
        val fields = page.fields()
        assertEq(listOf("title", "emoji", "description", "accent", "layout", "showEmoji", "outline"), fields.keys.toList())
        assertEq("T", fields["title"])
        assertEq("pink", fields["accent"])
        assertEq("continuous", fields["layout"])
        assertEq(false, fields["showEmoji"])
        assertEq(listOf(linkedMapOf<String, Any?>("id" to "e1", "kind" to "section", "depth" to 0, "label" to "S", "noteId" to "", "url" to "", "collapsed" to false)), fields["outline"])
        assertEq("https://h.example/dashboard/notes?page=p+1", PageOutline.accessUrl("https://h.example/", "p 1"))
        assertEq("https://h.example/dashboard/notes?page=p1&note=n2", PageOutline.accessUrl("https://h.example", "p1", "n2"))
        assertEq("A page holds at most 300 entries.", PageOutline.error(OrgApiException("too_many_entries", ""), "x"))
        assertEq("Someone else changed this page. Reload to see their changes, then make yours again.", PageOutline.error(PageConflictException(null), "x"))
        assertEq("fallback", PageOutline.error(IllegalStateException(), "fallback"))
    }

    fun testPageRequests() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                if (method == "PATCH" && path.endsWith("/pages/p2")) throw OrgApiException("version_conflict", "stale")
                return mapOf(
                    "pages" to listOf(mapOf("id" to "p1")),
                    "page" to mapOf("id" to path.substringAfter("/pages/", "p1").substringBefore('?').substringBefore('/').ifEmpty { "p1" }, "version" to 4L),
                    "notes" to emptyMap<String, Any?>(),
                    "shares" to listOf(mapOf("type" to "user", "name" to "bo", "role" to "viewer")),
                    "note" to mapOf("id" to "n1"),
                )
            }
        }
        val pages = NotePagesApi(api)
        assertEq(listOf("p1"), pages.list().map { it.id })
        pages.create(mapOf("title" to "New"))
        pages.get("p1")
        pages.get("p1", content = false)
        assertEq(4L, pages.save("p1", 3, mapOf("title" to "T")).version)
        val conflict = try {
            pages.save("p2", 1, mapOf("title" to "T"))
            null
        } catch (error: PageConflictException) {
            error
        }
        assertEq("p2", conflict?.latest?.id)
        pages.addShare("p1", "user", "@Bo", "editor", true)
        pages.removeShare("p1", "user", "bo")
        pages.shareSuggestions("p1", " B")
        pages.delete("p1")
        val notes = NotesApi(api)
        notes.create("T", "body", " 📝 ")
        notes.create("T", "body")
        notes.setEmoji("n1", 4, "")
        assertEq(listOf(
            "GET /api/notes/pages ",
            "POST /api/notes/pages {title=New}",
            "GET /api/notes/pages/p1?content=1 ",
            "GET /api/notes/pages/p1 ",
            "PATCH /api/notes/pages/p1 {baseVersion=3, title=T}",
            "PATCH /api/notes/pages/p2 {baseVersion=1, title=T}",
            "GET /api/notes/pages/p2 ",
            "POST /api/notes/pages/p1/shares {type=user, name=bo, role=editor, includeNotes=true}",
            "DELETE /api/notes/pages/p1/shares {type=user, name=bo}",
            "GET /api/notes/pages/p1/shares?query=b ",
            "DELETE /api/notes/pages/p1 ",
            "POST /api/notes {title=T, markdown=body, visibility=private, emoji=📝}",
            "POST /api/notes {title=T, markdown=body, visibility=private}",
            "PATCH /api/notes/n1 {baseVersion=4, emoji=}",
        ), calls)
    }
}
