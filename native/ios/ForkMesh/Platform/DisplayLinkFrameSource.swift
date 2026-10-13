// The frame source for FMAccount's PerformanceMonitorService: frames that
// arrived late, measured with a CADisplayLink.

import FMAccount
import Foundation
import QuartzCore
import UIKit

/// Reports a `FrameTiming` whenever the main run loop delivered a display
/// callback much later than the display asked for it, which is what a
/// blocked main thread looks like from the compositor's side.
///
/// The link runs at a low rate on purpose. A 120 Hz callback would wake the
/// CPU on every vsync for as long as the app is open; ten a second still
/// resolves the 200 ms stalls the monitor records, and the monitor's own
/// main-thread heartbeat covers the rest. The link is paused while the app is
/// in the background (`stopReporting` on uninstall, and iOS stops display
/// callbacks for a backgrounded app anyway).
final class DisplayLinkFrameSource: NSObject, FrameTimingSource {
    private var link: CADisplayLink?
    private var handler: (([FrameTiming]) -> Void)?
    private var lastTimestamp: CFTimeInterval = 0
    private var observers: [NSObjectProtocol] = []
    static let pauses: [Notification.Name] = [
        UIApplication.willResignActiveNotification,
        UIApplication.didEnterBackgroundNotification,
        UIApplication.willEnterForegroundNotification,
        UIApplication.didBecomeActiveNotification,
    ]

    func startReporting(_ handler: @escaping ([FrameTiming]) -> Void) {
        let start: () -> Void = { [weak self] in
            self?.begin(handler)
        }
        if Thread.isMainThread {
            start()
        } else {
            DispatchQueue.main.async(execute: start)
        }
    }

    func stopReporting() {
        let stop: () -> Void = { [weak self] in
            self?.link?.invalidate()
            self?.link = nil
            self?.handler = nil
            self?.lastTimestamp = 0
            self?.observers.forEach(NotificationCenter.default.removeObserver)
            self?.observers = []
        }
        if Thread.isMainThread {
            stop()
        } else {
            DispatchQueue.main.async(execute: stop)
        }
    }

    private func begin(_ handler: @escaping ([FrameTiming]) -> Void) {
        link?.invalidate()
        self.handler = handler
        lastTimestamp = 0
        let displayLink: CADisplayLink = CADisplayLink(target: self, selector: #selector(tick(_:)))
        displayLink.preferredFrameRateRange = CAFrameRateRange(minimum: 8, maximum: 15, preferred: 10)
        displayLink.add(to: RunLoop.main, forMode: RunLoop.Mode.common)
        link = displayLink
        observers.forEach(NotificationCenter.default.removeObserver)
        observers = Self.pauses.map { (name: Notification.Name) -> NSObjectProtocol in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] (_: Notification) in
                self?.lastTimestamp = 0
            }
        }
    }

    @objc private func tick(_ displayLink: CADisplayLink) {
        let now: CFTimeInterval = displayLink.timestamp
        defer { lastTimestamp = now }
        guard lastTimestamp > 0 else { return }
        let interval: CFTimeInterval = now - lastTimestamp
        let expected: CFTimeInterval = max(displayLink.targetTimestamp - displayLink.timestamp, 1.0 / 60.0)
        // Only a callback that came at least two periods late is worth a
        // report; the monitor applies its own stall threshold on top.
        guard interval > expected * 2 else { return }
        let late: CFTimeInterval = interval - expected
        let timing: FrameTiming = FrameTiming(
            buildDuration: Self.duration(late),
            rasterDuration: Duration.zero,
            vsyncOverhead: Duration.zero,
            totalSpan: Self.duration(interval)
        )
        handler?([timing])
    }

    private static func duration(_ seconds: CFTimeInterval) -> Duration {
        return Duration.microseconds(Int64(seconds * 1_000_000))
    }
}
