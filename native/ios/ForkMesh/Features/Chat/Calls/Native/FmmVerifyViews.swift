#if canImport(WebRTC)
import FMChat
import SwiftUI

struct GroupCallVerifySheet: View {
    let call: NativeGroupCall
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: FmSpace.x4) {
            if call.verifyRejected {
                Image(systemName: "exclamationmark.shield.fill")
                    .font(.system(size: 44))
                    .foregroundStyle(.red)
                Text(GroupCallVerifyText.mismatch)
                    .font(.system(size: 15, weight: .medium))
                    .multilineTextAlignment(.center)
                Button("Leave the call", role: .destructive) {
                    dismiss()
                    call.leave()
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
            } else if let state = call.verify {
                Text(state.summary(name: call.displayName))
                    .font(.system(size: 15, weight: .semibold))
                    .multilineTextAlignment(.center)
                    .accessibilityIdentifier("chat-group-call-verify-line")
                if state.hasCode && !state.verified {
                    HStack(spacing: FmSpace.x4) {
                        ForEach(state.emoji.indices, id: \.self) { index in
                            VStack(spacing: 4) {
                                Text(state.emoji[index]).font(.system(size: 40))
                                Text(index < state.names.count ? state.names[index] : "").font(.system(size: 12)).foregroundStyle(.secondary)
                            }
                        }
                    }
                    .accessibilityElement(children: .combine)
                    .accessibilityIdentifier("chat-group-call-verify-emoji")
                    Text(GroupCallVerifyText.numbers(state.digits))
                        .font(.system(size: 14).monospacedDigit())
                        .foregroundStyle(.secondary)
                    HStack(spacing: FmSpace.x3) {
                        Button(GroupCallVerifyText.noMatch, role: .destructive) { call.rejectCode() }
                            .buttonStyle(.bordered)
                        Button(GroupCallVerifyText.match) { call.confirmCode() }
                            .buttonStyle(.borderedProminent)
                            .accessibilityIdentifier("chat-group-call-verify-match")
                    }
                } else if state.verified {
                    Image(systemName: "checkmark.shield.fill")
                        .font(.system(size: 44))
                        .foregroundStyle(.green)
                }
            } else {
                Text(GroupCallVerifyText.preparing)
                    .font(.system(size: 15))
                    .multilineTextAlignment(.center)
            }
        }
        .padding(FmSpace.x5)
        .presentationDetents([.medium])
    }
}

struct GroupCallAuthNote: View {
    let state: GroupCallAuthState
    let name: String

    var body: some View {
        if let text = GroupCallVerifyText.tileNote(state, name: name) {
            Text(text)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.white)
                .multilineTextAlignment(.leading)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 6).fill(state == .forged ? Color.red.opacity(0.85) : Color.orange.opacity(0.85)))
                .padding(6)
                .accessibilityIdentifier(state == .forged ? "chat-group-call-auth-forged" : "chat-group-call-auth-missing")
        }
    }
}
#endif
