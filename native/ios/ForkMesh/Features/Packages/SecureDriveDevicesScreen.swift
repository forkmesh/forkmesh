import FMPackages
import SwiftUI
import UIKit

struct SecureDriveInviteCode: Identifiable {
    let code: String
    var id: String { code }
}

struct SecureDriveDevicesView: View {
    let model: SecureDriveModel
    @State private var approving: SecureDriveJoinRequest?
    @State private var denying: SecureDriveJoinRequest?
    @State private var removing: SecureDriveDeviceRow?
    @State private var invite: SecureDriveInviteCode?
    @State private var cancelling = false

    private var canWrite: Bool { !model.snapshot.removed && !model.busy }

    var body: some View {
        List {
            if !model.snapshot.requests.isEmpty {
                Section {
                    ForEach(model.snapshot.requests) { (request: SecureDriveJoinRequest) in
                        VStack(alignment: .leading, spacing: 6) {
                            Text(request.name.isEmpty ? "A device" : request.name).font(.headline)
                            Text(request.safety)
                                .font(.title3.monospacedDigit().weight(.semibold))
                                .textSelection(.enabled)
                            Text("Asked \(Date(timeIntervalSince1970: Double(request.time) / 1000).formatted(.relative(presentation: .named)))")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                            HStack {
                                Button("Approve\u{2026}") { approving = request }
                                    .buttonStyle(.borderedProminent)
                                Button("Turn down", role: .destructive) { denying = request }
                                    .buttonStyle(.bordered)
                            }
                            .disabled(!canWrite)
                        }
                        .padding(.vertical, 4)
                    }
                } header: {
                    Text("Asking to join")
                } footer: {
                    Text("Approve a device only if its screen shows the same safety number. A different number means someone else used the code.")
                }
            }
            Section {
                ForEach(model.snapshot.devices) { (device: SecureDriveDeviceRow) in
                    deviceRow(device)
                }
            } header: {
                Text("Approved devices")
            }
            Section {
                Button {
                    Task {
                        if let code = await model.invite() { invite = SecureDriveInviteCode(code: code) }
                    }
                } label: {
                    Label("Invite a device\u{2026}", systemImage: "person.badge.plus")
                }
                .disabled(!canWrite)
                .accessibilityIdentifier("secure-drive-invite")
                if !model.snapshot.invites.isEmpty {
                    Button(role: .destructive) {
                        cancelling = true
                    } label: {
                        Label("Cancel open invitations", systemImage: "xmark.circle")
                    }
                    .disabled(!canWrite)
                }
            } footer: {
                Text(model.snapshot.invites.isEmpty
                    ? "Codes work once and expire 24 hours after they are made."
                    : "\(SecureDriveEditorText.plural(model.snapshot.invites.count, "invitation code")) still open. Codes work once and expire 24 hours after they are made.")
            }
            Section {
                Text(model.snapshot.thisDeviceSafety)
                    .font(.title3.monospacedDigit())
                    .textSelection(.enabled)
            } header: {
                Text("This iPhone's safety number")
            }
        }
        .listStyle(.insetGrouped)
        .refreshable { await model.refresh(force: true) }
        .alert("Approve \(approving?.name ?? "")?", isPresented: approveShown) {
            Button("Cancel", role: .cancel) {}
            Button("Approve") {
                if let request = approving { Task { await model.approve(request) } }
            }
        } message: {
            Text("Approve only if \(approving?.name ?? "it") shows this safety number:\n\n\(approving?.safety ?? "")\n\nOnce approved it can open, change and delete every file on the drive.")
        }
        .confirmationDialog("Turn down \(denying?.name ?? "")?", isPresented: denyShown, titleVisibility: .visible) {
            Button("Turn Down", role: .destructive) {
                if let request = denying { Task { await model.deny(request) } }
            }
        } message: {
            Text("Its invitation code stops working.")
        }
        .confirmationDialog("Remove \(removing?.name ?? "")?", isPresented: removeShown, titleVisibility: .visible) {
            Button("Remove", role: .destructive) {
                if let device = removing { Task { await model.remove(device) } }
            }
        } message: {
            Text("\(removing?.name ?? "It") loses access to everything on the drive from now on. Secure Drive makes a new drive key, seals it to the other approved devices, and re-encrypts every file under it, which can take a while for large drives.\n\nCopies it already opened or saved stay with it. Remove it before handing the device on or when it is lost.")
        }
        .confirmationDialog("Cancel open invitations?", isPresented: $cancelling, titleVisibility: .visible) {
            Button("Cancel Invitations", role: .destructive) { Task { await model.cancelInvites() } }
            Button("Keep Them", role: .cancel) {}
        } message: {
            Text("Those codes stop working. Requests already waiting are turned down too.")
        }
        .sheet(item: $invite) { (item: SecureDriveInviteCode) in
            SecureDriveInviteSheet(code: item.code)
        }
    }

    private func deviceRow(_ device: SecureDriveDeviceRow) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(device.name).font(.body.weight(.medium))
                if device.removed {
                    Text("Removed").font(.caption).foregroundStyle(.red)
                }
            }
            Text(device.safety)
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            Text(device.root ? "\(device.name), created the drive" : "Approved by \(device.addedBy) \(Date(timeIntervalSince1970: Double(device.addedAt) / 1000).formatted(date: .abbreviated, time: .omitted))")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
        .swipeActions(edge: .trailing) {
            if !device.removed && !device.isThisDevice && canWrite {
                Button(role: .destructive) {
                    removing = device
                } label: {
                    Label("Remove", systemImage: "person.badge.minus")
                }
            }
        }
        .contextMenu {
            if !device.removed && !device.isThisDevice && canWrite {
                Button(role: .destructive) {
                    removing = device
                } label: {
                    Label("Remove from the drive\u{2026}", systemImage: "person.badge.minus")
                }
            }
            Button {
                UIPasteboard.general.string = device.safety
            } label: {
                Label("Copy safety number", systemImage: "doc.on.doc")
            }
        }
    }

    private var approveShown: Binding<Bool> {
        Binding(get: { approving != nil }, set: { if !$0 { approving = nil } })
    }

    private var denyShown: Binding<Bool> {
        Binding(get: { denying != nil }, set: { if !$0 { denying = nil } })
    }

    private var removeShown: Binding<Bool> {
        Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })
    }
}

struct SecureDriveInviteSheet: View {
    let code: String
    @Environment(\.dismiss) private var dismiss
    @State private var copied = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    Text("On the other device, open Secure Drive, choose Join a drive, pick the same drive file on the router and type this code. Then approve it here under Devices, after checking that both screens show the same safety number.")
                    Text(code)
                        .font(.title2.monospaced().weight(.semibold))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.vertical, 12)
                        .accessibilityIdentifier("secure-drive-invite-code")
                    Button {
                        UIPasteboard.general.setItems([[UIPasteboard.typeAutomatic: code]], options: [.expirationDate: Date().addingTimeInterval(600), .localOnly: true])
                        copied = true
                    } label: {
                        Label(copied ? "Copied" : "Copy code", systemImage: copied ? "checkmark" : "doc.on.doc")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    Text("Tell the code in person or over a call rather than by email. It works once and expires in 24 hours. On its own it opens nothing: the drive key is only handed over when you approve.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                .padding(24)
            }
            .navigationTitle("Invite a device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .onDisappear {
            if copied, UIPasteboard.general.string == code { UIPasteboard.general.string = "" }
        }
    }
}
