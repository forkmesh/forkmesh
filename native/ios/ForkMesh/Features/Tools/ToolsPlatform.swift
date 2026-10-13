// The iOS halves the Tools area needs: the vault's clipboard with expiry (a 30-second clearing
// timer) and the frame probe that feeds the telemetry strip's FPS and FRAME metrics.

import FMTools
import FMVault
import Foundation
import QuartzCore
import UIKit

/// FMVault's VaultClipboard over UIPasteboard. With an expiry the item is local-only (it never
/// reaches Universal Clipboard) and the system removes it at that time, even with the app gone.
@MainActor
final class PasteboardVaultClipboard: VaultClipboard {
    func copy(_ text: String, expiresAfter: TimeInterval?) {
        let pasteboard: UIPasteboard = UIPasteboard.general
        guard let seconds = expiresAfter else {
            pasteboard.string = text
            return
        }
        let item: [String: Any] = ["public.utf8-plain-text": text]
        let options: [UIPasteboard.OptionsKey: Any] = [
            UIPasteboard.OptionsKey.localOnly: true,
            UIPasteboard.OptionsKey.expirationDate: Date().addingTimeInterval(seconds),
        ]
        pasteboard.setItems([item], options: options)
    }
}

/// Counts the frames the display delivered each second and the longest gap between two of them.
/// A blocked main thread shows as a low count and a long frame. Runs only while the app is
/// visible.
@MainActor
final class TelemetryFrameProbe: NSObject {
    private let telemetry: AppTelemetry
    private var link: CADisplayLink?
    private var lastTimestamp: CFTimeInterval = 0

    init(telemetry: AppTelemetry) {
        self.telemetry = telemetry
        super.init()
    }

    func start() {
        if link != nil {
            return
        }
        lastTimestamp = 0
        let displayLink: CADisplayLink = CADisplayLink(target: self, selector: #selector(tick(_:)))
        displayLink.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 60, preferred: 60)
        displayLink.add(to: RunLoop.main, forMode: RunLoop.Mode.common)
        link = displayLink
    }

    func stop() {
        link?.invalidate()
        link = nil
        lastTimestamp = 0
    }

    @objc private func tick(_ displayLink: CADisplayLink) {
        let now: CFTimeInterval = displayLink.timestamp
        defer { lastTimestamp = now }
        if lastTimestamp <= 0 {
            return
        }
        let span: CFTimeInterval = now - lastTimestamp
        telemetry.recordFrame(Duration.microseconds(Int64(span * 1_000_000)))
    }
}
