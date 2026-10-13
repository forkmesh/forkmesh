package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.math.hypot

class MarkupTest {
    private fun near(a: Float, b: Float) = abs(a - b) < 0.001f

    fun testFitCentersTheImageInsideTheBox() {
        assertEq(MarkupFit(0f, 50f, 400f, 200f), Markup.fit(2000, 1000, 400f, 300f))
        assertEq(MarkupFit(100f, 0f, 200f, 400f), Markup.fit(500, 1000, 400f, 400f))
        assertEq(MarkupFit(0f, 0f, 0f, 0f), Markup.fit(0, 1000, 400f, 400f))
        assertEq(MarkupFit(0f, 0f, 0f, 0f), Markup.fit(100, 100, 0f, 400f))
    }

    fun testNormalizeMapsIntoTheImageAndClampsOutside() {
        val fit = Markup.fit(2000, 1000, 400f, 300f)
        assertEq(MarkupPoint(0f, 0f), Markup.normalize(0f, 50f, fit))
        assertEq(MarkupPoint(0.5f, 0.5f), Markup.normalize(200f, 150f, fit))
        assertEq(MarkupPoint(1f, 0f), Markup.normalize(900f, -40f, fit))
        assertEq(MarkupPoint(0f, 1f), Markup.normalize(-5f, 290f, fit))
        assertEq(MarkupPoint(0f, 0f), Markup.normalize(10f, 10f, MarkupFit(0f, 0f, 0f, 0f)))
    }

    fun testPenGrowsAndShapesKeepTheirTwoEnds() {
        val start = MarkupPoint(0.1f, 0.1f)
        var pen = MarkupStroke(MarkupTool.Pen, Markup.DEFAULT_COLOR, listOf(start))
        pen = Markup.extend(pen, MarkupPoint(0.2f, 0.2f))
        pen = Markup.extend(pen, MarkupPoint(0.2f, 0.2f))
        pen = Markup.extend(pen, MarkupPoint(0.3f, 0.2f))
        assertEq(3, pen.points.size)
        var box = MarkupStroke(MarkupTool.Rect, Markup.DEFAULT_COLOR, listOf(start))
        box = Markup.extend(box, MarkupPoint(0.2f, 0.2f))
        box = Markup.extend(box, MarkupPoint(0.6f, 0.4f))
        assertEq(listOf(start, MarkupPoint(0.6f, 0.4f)), box.points)
    }

    fun testTapsDrawDotsButNotEmptyShapes() {
        val dot = MarkupStroke(MarkupTool.Pen, Markup.DEFAULT_COLOR, listOf(MarkupPoint(0.5f, 0.5f)))
        expect(Markup.isVisible(dot))
        val flat = MarkupStroke(MarkupTool.Arrow, Markup.DEFAULT_COLOR, listOf(MarkupPoint(0.5f, 0.5f), MarkupPoint(0.501f, 0.5f)))
        expect(!Markup.isVisible(flat))
        val arrow = MarkupStroke(MarkupTool.Arrow, Markup.DEFAULT_COLOR, listOf(MarkupPoint(0.1f, 0.5f), MarkupPoint(0.4f, 0.5f)))
        expect(Markup.isVisible(arrow))
        expect(!Markup.isVisible(MarkupStroke(MarkupTool.Pen, Markup.DEFAULT_COLOR, emptyList())))
    }

    fun testArrowHeadPointsBackFromTheTip() {
        val head = Markup.arrowHead(0f, 0f, 100f, 0f, 4f)
        assertEq(2, head.size)
        for ((x, y) in head) {
            expect(x < 100f)
            expect(near(hypot(100f - x, y), 20f))
        }
        expect(near(head[0].second, -head[1].second))
        val short = Markup.arrowHead(0f, 0f, 10f, 0f, 4f)
        expect(near(hypot(10f - short[0].first, short[0].second), 5f))
    }

    fun testStrokeWidthFollowsTheShorterSide() {
        expect(near(Markup.strokeWidth(2000f, 1000f), 6f))
        expect(near(Markup.strokeWidth(100f, 100f), 3f))
    }

    fun testCapturesAreNamedScreenshot() {
        assertEq("screenshot.jpg", PhotoPrep.attachmentName(Markup.CAPTURE_NAME))
        assertEq("shared-1.jpg", PhotoPrep.attachmentName("shared-1.jpg"))
    }

    fun testEightDesktopSwatchesStartingWithRed() {
        assertEq(8, Markup.COLORS.size)
        assertEq(0xFFE5484D, Markup.DEFAULT_COLOR)
    }
}
