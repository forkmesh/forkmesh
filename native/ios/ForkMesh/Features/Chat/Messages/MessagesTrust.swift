import FMBase
import FMChat
import FMCrypto
import SwiftUI
import UIKit

struct MessagesTrustBanners: View {
    let hub: ChatHub
    let conversation: String
    let colors: MessagesColors
    let onSafety: () -> Void

    var body: some View {
        let banners: [DirectTrustNotice] = hub.trustBanners(conversation)
        if !banners.isEmpty {
            VStack(spacing: FmSpace.x2) {
                ForEach(banners) { (notice: DirectTrustNotice) in
                    MessagesTrustBanner(hub: hub, notice: notice, colors: colors, onSafety: onSafety)
                }
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.bottom, FmSpace.x2)
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }
}

private struct MessagesTrustBanner: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let notice: DirectTrustNotice
    let colors: MessagesColors
    let onSafety: () -> Void
    @State private var working: Bool = false
    @State private var confirmBlock: Bool = false

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                Image(systemName: "exclamationmark.shield")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(colors.meta)
                    .accessibilityHidden(true)
                Text(notice.text)
                    .font(.system(size: 13))
                    .foregroundStyle(colors.dark ? Color.white : Color.black)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: FmSpace.x2) {
                Button(action: { decide(approve: true) }) {
                    Text(DirectTrustText.approve).font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
                .tint(fm.accent)
                .accessibilityIdentifier("chat-trust-approve")
                Button(role: .destructive, action: { confirmBlock = true }) {
                    Text(DirectTrustText.block).font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.bordered)
                .tint(fm.danger)
                .accessibilityIdentifier("chat-trust-block")
                Spacer(minLength: 0)
                Button(action: onSafety) {
                    Text(DirectTrustText.safetyNumber).font(.system(size: 13))
                }
                .buttonStyle(.borderless)
                .tint(fm.accent)
            }
            .controlSize(.small)
            .disabled(working)
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .chatGlass(RoundedRectangle(cornerRadius: 16, style: .continuous), border: fm.border)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("chat-trust-banner")
        .confirmationDialog(DirectTrustText.blockTitle(notice.kids.count), isPresented: $confirmBlock, titleVisibility: .visible) {
            Button(DirectTrustText.block, role: .destructive) { decide(approve: false) }
        } message: {
            Text(DirectTrustText.blockNote(notice.kids.count))
        }
    }

    private func decide(approve: Bool) {
        guard notice.asksApproval, !working else { return }
        working = true
        Task { @MainActor in
            if approve {
                await hub.approveDevices(notice.conversation, kids: notice.kids)
                FmHaptics.success()
            } else {
                await hub.blockDevices(notice.conversation, kids: notice.kids)
            }
            working = false
        }
    }
}

struct MessagesTrustLine: View {
    let notice: DirectTrustNotice
    let colors: MessagesColors
    let onSafety: () -> Void

    var body: some View {
        if notice.kind == .safetyNumberChanged {
            Button(action: onSafety) {
                MessagesSystemLine(text: notice.text, colors: colors, bold: true)
            }
            .buttonStyle(.plain)
            .accessibilityHint("Shows the safety number")
        } else {
            MessagesSystemLine(text: notice.text, colors: colors)
        }
    }
}

struct MessagesSafetyNumberScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let conversation: String
    @State private var number: ChatSafetyNumber?
    @State private var loaded: Bool = false
    @State private var sawChange: Bool = false
    @State private var scanning: Bool = false
    @State private var scan: DirectSafetyScan?

    private var name: String { hub.trustName(conversation) }

    var body: some View {
        List {
            if let number {
                Section {
                    VStack(spacing: FmSpace.x4) {
                        if sawChange {
                            Label(DirectTrustText.safetyNumberChanged(name), systemImage: "exclamationmark.shield")
                                .font(.system(size: 14, weight: .semibold))
                                .foregroundStyle(fm.warning)
                                .multilineTextAlignment(.center)
                        }
                        MessagesSafetyDigits(number: number)
                        QrCodeImage(text: number.qr, size: 180)
                        verifiedBadge(number.verified)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, FmSpace.x3)
                } footer: {
                    Text(DirectTrustText.safetyExplainer(name))
                }
                Section {
                    Button(action: {
                        scan = nil
                        scanning = true
                    }) {
                        Label(DirectTrustText.scanCode, systemImage: "qrcode.viewfinder")
                    }
                    .accessibilityIdentifier("chat-safety-scan")
                    if !number.verified {
                        Button(action: {
                            hub.markSafetyVerified(conversation)
                            FmHaptics.success()
                        }) {
                            Label(DirectTrustText.markVerified, systemImage: "checkmark.seal")
                        }
                        .accessibilityIdentifier("chat-safety-mark-verified")
                    }
                    Button(action: {
                        UIPasteboard.general.string = number.groups.joined(separator: " ")
                        hub.post("Copied.")
                    }) {
                        Label("Copy the number", systemImage: "doc.on.doc")
                    }
                }
                if let scan {
                    Section {
                        Text(scan.message)
                            .foregroundStyle(scan == .verified ? fm.success : fm.danger)
                            .fmMessageTools(scan.message)
                    }
                }
            } else if loaded {
                Section {
                    Text("The safety number appears once this conversation has connected and both of you have a device in it.")
                        .foregroundStyle(fm.textSecondary)
                }
            } else {
                Section { ProgressView().frame(maxWidth: .infinity) }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(DirectTrustText.safetyNumber)
        .navigationBarTitleDisplayMode(.inline)
        .task(id: hub.trustRevision) { reload() }
        .sheet(isPresented: $scanning) {
            QrScannerSheet(title: DirectTrustText.scanCode) { (value: String) in
                let result: DirectSafetyScan = hub.verifySafetyCode(conversation, scanned: value)
                scan = result
                if result == .verified { FmHaptics.success() }
            }
        }
    }

    private func reload() {
        let current: ChatSafetyNumber? = hub.safetyNumber(conversation)
        if current?.changed == true {
            sawChange = true
            hub.acknowledgeSafetyChange(conversation)
        }
        number = current
        loaded = true
    }

    @ViewBuilder private func verifiedBadge(_ verified: Bool) -> some View {
        if verified {
            Label(DirectTrustText.verified, systemImage: "checkmark.seal.fill")
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(fm.success)
                .accessibilityIdentifier("chat-safety-verified")
        } else {
            Label(DirectTrustText.notVerified, systemImage: "seal")
                .font(.system(size: 14))
                .foregroundStyle(fm.textSecondary)
        }
    }
}

private struct MessagesSafetyDigits: View {
    let number: ChatSafetyNumber

    var body: some View {
        let rows: [[String]] = number.rows
        VStack(spacing: FmSpace.x2) {
            ForEach(rows.indices, id: \.self) { (row: Int) in
                HStack(spacing: FmSpace.x4) {
                    ForEach(rows[row].indices, id: \.self) { (column: Int) in
                        Text(rows[row][column])
                    }
                }
            }
        }
        .font(.system(size: 20, weight: .medium, design: .monospaced))
        .monospacedDigit()
        .lineLimit(1)
        .minimumScaleFactor(0.6)
        .textSelection(.enabled)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(number.groups.joined(separator: ", "))
        .accessibilityIdentifier("chat-safety-digits")
    }
}
