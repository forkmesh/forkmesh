// In debug builds, a draggable pill with the latest slow event's duration, opening a popup that
// lists every recorded event (Clear, Copy, Close). Laid over the app by the shell: `.overlay {
// DevPerformanceOverlay() }`. Draws nothing in release builds or without a
// PerformanceMonitorService. Text: FMTools' PerformanceLogText.

import FMAccount
import FMBase
import FMTools
import SwiftUI
import UIKit

struct DevPerformanceOverlay: View {
    @State private var monitor: PerformanceMonitorService?
    @State private var offset: CGPoint?
    @State private var dragStart: CGPoint?
    @State private var showLog: Bool = false

    init() {
        #if DEBUG
        let found: PerformanceMonitorService? = Services.get(PerformanceMonitorService.self)
        _monitor = State(initialValue: found?.enabled == true ? found : nil)
        #else
        _monitor = State(initialValue: nil)
        #endif
    }

    var body: some View {
        if let monitor = monitor {
            GeometryReader { (proxy: GeometryProxy) in
                layer(monitor, proxy)
            }
        }
    }

    @ViewBuilder
    private func layer(_ monitor: PerformanceMonitorService, _ proxy: GeometryProxy) -> some View {
        ZStack(alignment: .topLeading) {
            Color.clear
                .allowsHitTesting(false)
            if let event = monitor.latestEvent {
                indicator(event, proxy)
            }
            if showLog {
                popup(monitor)
            }
        }
    }

    private func clamped(_ point: CGPoint, _ proxy: GeometryProxy) -> CGPoint {
        let size: CGSize = proxy.size
        let result: (x: Double, y: Double) = PerformanceLogText.clampIndicator(
            x: Double(point.x), y: Double(point.y), boundsWidth: Double(size.width), boundsHeight: Double(size.height),
            width: 90, height: 32, topInset: Double(proxy.safeAreaInsets.top), bottomInset: Double(proxy.safeAreaInsets.bottom))
        return CGPoint(x: result.x, y: result.y)
    }

    private func indicator(_ event: PerformanceEvent, _ proxy: GeometryProxy) -> some View {
        let resting: CGPoint = CGPoint(x: proxy.size.width - 90 - FmSpace.x3, y: proxy.safeAreaInsets.top + FmSpace.x2)
        let at: CGPoint = clamped(offset ?? resting, proxy)
        return DevPerformancePill(event: event)
            .offset(x: at.x, y: at.y)
            .onTapGesture { showLog = true }
            .gesture(
                DragGesture()
                    .onChanged { (value: DragGesture.Value) in
                        let start: CGPoint = dragStart ?? at
                        if dragStart == nil {
                            dragStart = at
                        }
                        offset = clamped(CGPoint(x: start.x + value.translation.width, y: start.y + value.translation.height), proxy)
                    }
                    .onEnded { (_: DragGesture.Value) in
                        dragStart = nil
                    }
            )
    }

    private func popup(_ monitor: PerformanceMonitorService) -> some View {
        ZStack {
            Color.black.opacity(0.38)
                .ignoresSafeArea()
                .onTapGesture { showLog = false }
            DevPerformanceDialog(monitor: monitor, onClose: { showLog = false })
                .padding(FmSpace.x4)
        }
    }
}

private struct DevPerformancePill: View {
    @Environment(\.fmPalette) private var fm
    let event: PerformanceEvent

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon("speed_rounded", size: 16, color: Color.white)
            Text("\(event.durationMs)ms")
                .font(Font.system(size: 12, weight: .heavy))
                .foregroundStyle(Color.white)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.warning)
        .clipShape(Capsule())
        .shadow(color: Color.black.opacity(0.25), radius: 6, x: 0, y: 3)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Open performance monitor"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("dev-performance-indicator")
    }
}

private struct DevPerformanceDialog: View {
    @Environment(\.fmPalette) private var fm
    let monitor: PerformanceMonitorService
    let onClose: () -> Void

    private var header: some View {
        HStack {
            Text("Performance monitor")
                .font(Font.system(size: 20, weight: .bold))
                .foregroundStyle(fm.textPrimary)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmTextButton("Clear", icon: "delete_outline", onPressed: { monitor.clear() })
        }
        .padding(.leading, FmSpace.x4)
        .padding(.trailing, FmSpace.x3)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
    }

    @ViewBuilder private var events: some View {
        let list: [PerformanceEvent] = monitor.events
        if list.isEmpty {
            FmEmptyState(icon: "speed_rounded", title: "No slow events", message: PerformanceLogText.emptyMessage(monitor))
                .frame(maxHeight: 280)
        } else {
            ScrollView {
                LazyVStack(spacing: FmSpace.x2) {
                    ForEach(list) { (event: PerformanceEvent) in
                        DevPerformanceEventCard(event: event)
                    }
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x1)
                .padding(.bottom, FmSpace.x5)
            }
        }
    }

    private var footer: some View {
        HStack {
            FmTextButton("Copy", icon: "copy_rounded", onPressed: {
                UIPasteboard.general.string = PerformanceLogText.log(monitor.events)
            })
            Spacer()
            FmFilledButton("Close", icon: "close_rounded", onPressed: onClose)
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x4)
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            events
            if let path = monitor.stallLogPath {
                Text("Logged to \(path)")
                    .font(Font.system(size: 11))
                    .foregroundStyle(fm.textSecondary)
                    .textSelection(.enabled)
                    .padding(.horizontal, FmSpace.x4)
            }
            footer
        }
        .frame(maxWidth: 640)
        .frame(maxHeight: 600)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay {
            FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.border, lineWidth: 1)
        }
        .shadow(color: Color.black.opacity(0.22), radius: 28, x: 0, y: 18)
        .accessibilityIdentifier("dev-performance-log-popup")
    }
}

private struct DevPerformanceEventCard: View {
    @Environment(\.fmPalette) private var fm
    let event: PerformanceEvent

    var body: some View {
        FmCard(color: fm.bgBase, padding: EdgeInsets.fmAll(FmSpace.x3), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon(PerformanceLogText.icon(event), size: 18, color: fm.warning)
                    Text(event.name)
                        .font(Font.system(size: 15, weight: .heavy))
                        .foregroundStyle(fm.textPrimary)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    FmStatusBadge(label: "\(event.durationMs)ms", color: fm.warning)
                }
                Text(PerformanceLogText.kindLine(event))
                    .font(Font.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
                let details: String = PerformanceLogText.details(event)
                if !details.isEmpty {
                    Text(details)
                        .font(Font.system(size: 12))
                        .foregroundStyle(fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(details)
                }
                if !event.stackTrace.isEmpty {
                    Text(event.stackTrace)
                        .fmMono(size: 10, color: fm.textTertiary, height: 1.25)
                        .textSelection(.enabled)
                }
            }
        }
    }
}
