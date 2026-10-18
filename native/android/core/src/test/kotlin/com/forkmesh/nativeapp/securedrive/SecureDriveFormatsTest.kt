package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

object SecureDriveFormatsTest {
    fun testSheets() {
        val sheet = Sheet.fromCsv("Item,Cost\r\nPaper,4\r\nInk,\"1,5\"\r\nTotal,=SUM(B2:B3)\r\n".toByteArray())
        assertEq(4, sheet.rows)
        assertEq(2, sheet.columns)
        assertEq("1,5", sheet.raw(2, 1), "quoted commas survive")
        assertEq("4", sheet.display(3, 1), "SUM skips text")
        sheet.setRaw(2, 1, "1.5")
        assertEq("5.5", sheet.display(3, 1), "SUM adds numbers")
        sheet.setRaw(0, 3, "=B2*2+B3^2")
        assertEq("10.25", sheet.display(0, 3), "arithmetic precedence")
        sheet.setRaw(1, 3, "=AVERAGE(B2:B3)")
        assertEq("2.75", sheet.display(1, 3), "AVERAGE")
        sheet.setRaw(2, 3, "=B2/0")
        assertEq("#DIV/0!", sheet.display(2, 3), "division by zero")
        sheet.setRaw(3, 3, "=D5")
        sheet.setRaw(4, 3, "=D4+1")
        assertEq("#CYCLE!", sheet.display(3, 3), "a cycle is caught")
        sheet.setRaw(5, 0, "=IF(B2>3,\"big\",\"small\")")
        assertEq("big", sheet.display(5, 0), "IF with text")
        sheet.setRaw(5, 1, "=ROUND(10/3,2)")
        assertEq("3.33", sheet.display(5, 1), "ROUND")
        sheet.setRaw(5, 2, "=NOPE(1)")
        assertEq("#NAME?", sheet.display(5, 2), "unknown functions")
        sheet.setRaw(6, 1, "=MIN(B2:B3)+MAX(B2:B3)*COUNT(B2:B3)-ABS(-1)")
        assertEq("8.5", sheet.display(6, 1), "MIN MAX COUNT ABS")
        sheet.setRaw(7, 1, "=1/3")
        assertEq("0.333333333333", sheet.display(7, 1), "twelve significant digits")
        sheet.setRaw(7, 2, "=10^20/3")
        assertEq("3.33333333333e+19", sheet.display(7, 2), "large numbers use an exponent")
        sheet.setRaw(7, 3, "=\"a\"&1")
        assertEq("#ERROR!", sheet.display(7, 3), "unknown operators")
        sheet.setRaw(6, 0, "line one\nline \"two\"")
        val again = Sheet.fromCsv(sheet.toCsv())
        assertEq(sheet.raw(6, 0), again.raw(6, 0), "CSV round-trips")
        assertEq(sheet.raw(3, 1), again.raw(3, 1))
        assertEq(Pair(11, 27), Sheet.parseCell("AB12"), "cell names")
        assertEq("AB", Sheet.columnName(27), "column names")
        expect(Sheet.parseCell("A0") == null && Sheet.parseCell("AAA1") == null, "bad cell names")
        assertEq(0, Sheet.fromCsv(ByteArray(0)).rows)
        assertEq("", String(Sheet().toCsv()))
    }

    fun testDrawings() {
        val drawing = Drawing(width = 800, height = 600, background = "#fafafa")
        drawing.strokes += Stroke("#0969da", 5.0, mutableListOf(Point(10.0, 20.0), Point(30.5, 40.0), Point(50.0, 60.0)))
        drawing.strokes += Stroke("#cf222e", 2.0, mutableListOf(Point(5.0, 5.0)))
        val parsed = Drawing.fromSvg(drawing.toSvg())
        expect(parsed != null, "a drawing parses back")
        parsed!!
        assertEq(800, parsed.width)
        assertEq("#fafafa", parsed.background)
        assertEq(2, parsed.strokes.size)
        assertEq(3, parsed.strokes[0].points.size)
        assertEq("#0969da", parsed.strokes[0].color)
        assertEq(5.0, parsed.strokes[0].width)
        assertEq(1, parsed.strokes[1].points.size)
        expect(Drawing.fromSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0 L1 1\"/></svg>".toByteArray()) == null, "a foreign SVG opens as a plain file")
        expect(Drawing.fromSvg("<svg data-forkmesh-drawing=\"1\"><path".toByteArray()) == null, "broken XML")
        val qt = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="1000" viewBox="0 0 1600 1000" data-forkmesh-drawing="1">
    <rect width="1600" height="1000" fill="#ffffff"/>
    <path d="M100.0 100.0 L500.0 100.0 L500.0 400.0" fill="none" stroke="#0969da" stroke-width="6.0" stroke-linecap="round" stroke-linejoin="round"/>
</svg>
"""
        val desktop = Drawing.fromSvg(qt.toByteArray())
        expect(desktop != null && desktop.strokes.size == 1 && desktop.strokes[0].points[2] == Point(500.0, 400.0), "reads the desktop's drawing")
    }
}
