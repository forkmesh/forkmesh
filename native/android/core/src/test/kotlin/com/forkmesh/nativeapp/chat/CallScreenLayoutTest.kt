package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq

class CallScreenLayoutTest {
    fun testCornersSitClearOfTheNameAndTheButtons() {
        assertEq(CallPoint(69f, 170f), CallPreviewLayout.center(CallPreviewCorner.TOP_LEADING, 390f, 844f))
        assertEq(CallPoint(321f, 170f), CallPreviewLayout.center(CallPreviewCorner.TOP_TRAILING, 390f, 844f))
        assertEq(CallPoint(69f, 584f), CallPreviewLayout.center(CallPreviewCorner.BOTTOM_LEADING, 390f, 844f))
        assertEq(CallPoint(321f, 504f), CallPreviewLayout.center(CallPreviewCorner.BOTTOM_TRAILING, 390f, 844f, bottomClearance = 260f))
    }

    fun testATinyScreenKeepsTheTileOnScreen() {
        val spot = CallPreviewLayout.center(CallPreviewCorner.BOTTOM_TRAILING, 100f, 200f)
        assertEq(CallPoint(69f, 170f), spot)
    }

    fun testReleaseSnapsToTheNearestCorner() {
        assertEq(CallPreviewCorner.BOTTOM_LEADING, CallPreviewLayout.nearest(CallPoint(120f, 500f), 390f, 844f))
        assertEq(CallPreviewCorner.TOP_TRAILING, CallPreviewLayout.nearest(CallPoint(250f, 300f), 390f, 844f))
    }

    fun testAFlickCarriesTheTileToTheFarCorner() {
        val still = CallPreviewLayout.settle(CallPreviewCorner.TOP_TRAILING, -60f, 40f, 0f, 0f, 390f, 844f)
        assertEq(CallPreviewCorner.TOP_TRAILING, still)
        val flicked = CallPreviewLayout.settle(CallPreviewCorner.TOP_TRAILING, -60f, 40f, -1500f, 2500f, 390f, 844f)
        assertEq(CallPreviewCorner.BOTTOM_LEADING, flicked)
    }

    fun testTheStoredCornerReadsBackAndUnknownValuesFallBack() {
        assertEq(CallPreviewCorner.BOTTOM_LEADING, CallPreviewCorner.from("bottomLeading"))
        assertEq(CallPreviewCorner.TOP_TRAILING, CallPreviewCorner.from("middle"))
        assertEq(CallPreviewCorner.TOP_TRAILING, CallPreviewCorner.from(null))
        assertEq("fm.call.previewCorner", CallPreviewCorner.KEY)
    }

    fun testButtonsTakeOneRowThenTighterGapsThenTwoRows() {
        val six = List(6) { 60f }
        assertEq(CallControlsArrangement.OneRow(16f), CallControlsLayout.arrange(440f, six))
        assertEq(CallControlsArrangement.OneRow(8f), CallControlsLayout.arrange(400f, six))
        assertEq(CallControlsArrangement.TwoRows, CallControlsLayout.arrange(358f, six))
        assertEq(CallControlsArrangement.OneRow(16f), CallControlsLayout.arrange(358f, List(3) { 60f }))
        assertEq(0f, CallControlsLayout.rowWidth(emptyList(), 16f))
    }
}
