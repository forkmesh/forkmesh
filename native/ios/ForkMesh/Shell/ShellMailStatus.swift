import FMAssistant
import Foundation
import Observation
import SwiftUI
import UIKit

struct ShellMailNotice: Equatable {
    let id: Int
    let line: String
    let isError: Bool
}

@MainActor
@Observable
final class ShellMailStatus {
    static let shared: ShellMailStatus = ShellMailStatus()
    nonisolated static let sentSeconds: Double = 3
    nonisolated static let noticeSeconds: Double = 8

    private(set) var stage: GenieAutoSendStage?
    private(set) var notice: ShellMailNotice?

    @ObservationIgnored private var count: Int = 0
    @ObservationIgnored private var settle: Task<Void, Never>?
    @ObservationIgnored private var fade: Task<Void, Never>?

    var busy: Bool {
        guard let stage else { return false }
        return stage != .sent
    }

    func set(_ next: GenieAutoSendStage) {
        settle?.cancel()
        withAnimation(.easeOut(duration: 0.2)) { stage = next }
        guard next == .sent else { return }
        settle = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(ShellMailStatus.sentSeconds))
            if !Task.isCancelled { self?.clear() }
        }
    }

    func finish() {
        guard busy else { return }
        clear()
    }

    func say(_ line: String, isError: Bool = false) {
        count += 1
        let id: Int = count
        withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) {
            notice = ShellMailNotice(id: id, line: line, isError: isError)
        }
        UIAccessibility.post(notification: .announcement, argument: line)
        fade?.cancel()
        fade = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(ShellMailStatus.noticeSeconds + Double(line.count) / 30))
            guard !Task.isCancelled, self?.notice?.id == id else { return }
            self?.dismiss()
        }
    }

    func dismiss() {
        fade?.cancel()
        withAnimation(.easeOut(duration: 0.25)) { notice = nil }
    }

    private func clear() {
        settle?.cancel()
        withAnimation(.easeOut(duration: 0.25)) { stage = nil }
    }
}
