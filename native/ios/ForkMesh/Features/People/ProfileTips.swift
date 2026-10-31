import FMAccount
import FMApi
import SwiftUI
import UIKit

extension View {
    func opensProfile(_ name: String) -> some View {
        modifier(OpensProfile(account: ProfileLinks.account(name)))
    }
}

private struct OpensProfile: ViewModifier {
    let account: String?

    func body(content: Content) -> some View {
        if let account = account {
            content
                .contentShape(Rectangle())
                .onTapGesture { AppNavigator.shared?.open(.profile(name: account)) }
                .accessibilityAddTraits(.isButton)
                .accessibilityHint(Text("Opens @\(account)'s profile"))
        } else {
            content
        }
    }
}

struct ProfileTipContent: View {
    @Environment(\.fmPalette) var fm
    let name: String
    let address: String
    @State private var copied = false

    var body: some View {
        let valid = SolanaAddress.valid(address)
        if valid.isEmpty {
            Text("@\(name) has not entered a Solana address for tips.")
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        } else {
            VStack(spacing: FmSpace.x2) {
                QrCodeImage(text: SolanaAddress.qrText(valid), size: 180)
                    .accessibilityLabel(Text("Solana tip address QR code for @\(name)"))
                Text(valid)
                    .fmText(.bodyMedium)
                    .monospaced()
                    .multilineTextAlignment(.center)
                    .textSelection(.enabled)
                Text("Scan with a Solana wallet to tip @\(name).")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .multilineTextAlignment(.center)
                FmTextButton(copied ? "Address copied" : "Copy address", icon: "content_copy", onPressed: {
                    UIPasteboard.general.string = valid
                    copied = true
                })
            }
            .frame(maxWidth: .infinity)
        }
    }
}
