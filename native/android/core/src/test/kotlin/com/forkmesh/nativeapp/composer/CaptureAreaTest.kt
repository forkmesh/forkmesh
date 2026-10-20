package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.PhonePackages
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CaptureAreaTest {
    fun testAreaIsTheDraggedBoxInAnyDirectionClampedToTheScreen() {
        assertEq(CaptureArea(100.0, 200.0, 200.0, 300.0), CaptureAreaRules.area(300.0, 500.0, 100.0, 200.0, 390.0, 844.0))
        assertEq(CaptureArea(0.0, 800.0, 390.0, 44.0), CaptureAreaRules.area(-20.0, 800.0, 420.0, 900.0, 390.0, 844.0))
        assertEq(CaptureArea.EMPTY, CaptureAreaRules.area(0.0, 0.0, 10.0, 10.0, 0.0, 844.0))
    }

    fun testTinyBoxesAreNotUsable() {
        expect(CaptureAreaRules.isUsable(CaptureArea(0.0, 0.0, 16.0, 40.0)))
        expect(!CaptureAreaRules.isUsable(CaptureArea(0.0, 0.0, 15.0, 400.0)))
        expect(!CaptureAreaRules.isUsable(CaptureArea.EMPTY))
    }

    fun testPixelsCoverTheWholeBoxInsideTheImage() {
        assertEq(CaptureArea(30.0, 61.0, 301.0, 151.0), CaptureAreaRules.pixels(CaptureArea(10.2, 20.5, 100.0, 50.0), 3.0, 1170, 2532))
        assertEq(CaptureArea(900.0, 2400.0, 270.0, 132.0), CaptureAreaRules.pixels(CaptureArea(300.0, 800.0, 200.0, 100.0), 3.0, 1170, 2532))
        assertEq(CaptureArea.EMPTY, CaptureAreaRules.pixels(CaptureArea(0.0, 0.0, 10.0, 10.0), 0.0, 10, 10))
    }

    fun testOnlyElementsSmallerThanHalfTheScreenCountAsContent() {
        expect(CaptureAreaRules.isContent(80.0, 44.0, 390.0, 844.0))
        expect(!CaptureAreaRules.isContent(390.0, 600.0, 390.0, 844.0))
        expect(!CaptureAreaRules.isContent(0.0, 44.0, 390.0, 844.0))
    }

    fun testABlankPointHasNoControlAndNoSmallElementUnderIt() {
        expect(CaptureAreaRules.isBlank(emptyList(), 390.0, 844.0))
        expect(CaptureAreaRules.isBlank(listOf(CaptureHit(390.0, 700.0, actionable = false)), 390.0, 844.0))
        expect(!CaptureAreaRules.isBlank(listOf(CaptureHit(390.0, 700.0, actionable = true)), 390.0, 844.0))
        expect(!CaptureAreaRules.isBlank(listOf(CaptureHit(390.0, 700.0, false), CaptureHit(120.0, 20.0, false)), 390.0, 844.0))
    }

    fun testTheCopyIsAtMost2048PixelsOnTheLongSide() {
        assertEq(1170 to 2532, CaptureAreaRules.fitted(1170, 2532, longSide = 4000))
        assertEq(946 to 2048, CaptureAreaRules.fitted(1170, 2532))
        assertEq(300 to 200, CaptureAreaRules.fitted(300, 200))
    }

    fun testTheCapturePackageIsOnByDefaultAndSwitchedPerPhone() {
        val prefs = MemoryPrefStore()
        assertEq(true, PhonePackages.isOn(PhonePackages.CAPTURE, prefs, legacy = false))
        PhonePackages.set(PhonePackages.CAPTURE, false, prefs)
        assertEq(false, PhonePackages.isOn(PhonePackages.CAPTURE, prefs, legacy = false))
        assertEq(false, prefs.getBool(PhonePackages.CAPTURE_KEY))
    }
}
