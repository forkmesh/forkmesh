import FMPackages
import SwiftUI

enum CrmComposeMode: String, CaseIterable, Identifiable {
    case crm
    case outreach
    case mailbox

    var id: String { rawValue }

    var title: String {
        switch self {
        case .crm: return "CRM send"
        case .outreach: return "Outreach"
        case .mailbox: return "Mailbox"
        }
    }
}

struct CrmComposeSheet: View {
    @Bindable var model: CrmModel
    @State private var draft: CrmEmailDraft
    @State private var mode: CrmComposeMode
    @State private var logIt = true
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    init(model: CrmModel, draft: CrmEmailDraft, outreach: Bool) {
        self.model = model
        _draft = State(initialValue: draft)
        let settings = model.data?.settings
        let canSend = !(settings?.fromEmail.isEmpty ?? true) && (settings?.mailtrapConfigured ?? false)
        let allowed = model.data?.outreach.allowed ?? false
        _mode = State(initialValue: outreach && allowed ? .outreach : (canSend ? .crm : (allowed ? .outreach : .mailbox)))
    }

    private var contact: CrmContact? { model.contact(draft.contactId) ?? model.data?.contact(email: draft.to) }

    private var outreach: CrmOutreach? { model.data?.outreach }

    private var settings: CrmSettings? { model.data?.settings }

    private var modes: [CrmComposeMode] {
        var modes: [CrmComposeMode] = []
        if let settings, settings.mailtrapConfigured { modes.append(.crm) }
        if outreach?.allowed == true { modes.append(.outreach) }
        modes.append(.mailbox)
        return modes
    }

    var body: some View {
        Form {
            if modes.count > 1 {
                Section {
                    Picker("Send with", selection: $mode) {
                        ForEach(modes) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                } footer: {
                    Text(footer)
                }
            }
            Section {
                HStack {
                    if let contact { CrmAvatar(name: contact.name, email: contact.email, size: 28) }
                    TextField("To", text: $draft.to)
                        .keyboardType(.emailAddress)
                        .textContentType(.emailAddress)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                if mode == .outreach, let templates = outreach?.templates, !templates.isEmpty {
                    Picker("Template", selection: $draft.template) {
                        Text("Blank").tag("blank")
                        ForEach(templates) { Text($0.label).tag($0.key) }
                    }
                    .onChange(of: draft.template) { _, key in apply(key) }
                }
                TextField("Subject", text: $draft.subject)
            }
            Section {
                TextField("Message", text: $draft.body, axis: .vertical).lineLimit(8...24)
            }
            if mode == .mailbox {
                Section {
                    Toggle("Log it on the contact", isOn: $logIt)
                }
            }
            if mode == .outreach, let outreach {
                Section {
                    Gauge(value: Double(outreach.sentToday), in: 0...Double(max(1, outreach.dailyLimit))) {
                        Text("Sent today")
                    } currentValueLabel: {
                        Text("\(outreach.sentToday) of \(outreach.dailyLimit)")
                    }
                    .tint(outreach.remaining > 5 ? .green : .orange)
                    LabeledContent("From", value: outreach.fromName.isEmpty ? outreach.fromEmail : "\(outreach.fromName) <\(outreach.fromEmail)>")
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle(contact.map { "Email \($0.displayName)" } ?? "New email")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button(mode == .mailbox ? "Open" : "Send") { Task { await send() } }
                    .disabled(!canSend || model.saving)
            }
        }
        .onAppear { model.clearError() }
    }

    private var canSend: Bool {
        if mode == .mailbox { return draft.to.contains("@") }
        if mode == .outreach && (outreach?.remaining ?? 0) == 0 { return false }
        return draft.isValid
    }

    private var footer: String {
        switch mode {
        case .crm:
            let from = settings?.fromEmail ?? ""
            return from.isEmpty ? "Set a sender address in CRM settings first." : "Sent from \(from) and tracked on the contact."
        case .outreach:
            return "Sent from the organization's outreach address. Up to \(outreach?.dailyLimit ?? 50) a day per sender."
        case .mailbox:
            return settings?.mailProvider == "gmail" ? "Opens Gmail with the message filled in." : "Opens Zoho Mail with the message filled in."
        }
    }

    private func apply(_ key: String) {
        guard let template = outreach?.templates.first(where: { $0.key == key }) else { return }
        let filled = template.filled(for: contact)
        draft.subject = filled.subject
        draft.body = filled.body
    }

    private func send() async {
        switch mode {
        case .crm:
            if await model.sendEmail(draft) != nil {
                FmHaptics.send()
                dismiss()
            } else {
                FmHaptics.failure()
            }
        case .outreach:
            if await model.sendOutreach(draft) {
                FmHaptics.send()
                dismiss()
            } else {
                FmHaptics.failure()
            }
        case .mailbox:
            guard let url = settings?.composeURL(to: draft.to, subject: draft.subject, body: draft.body)
                ?? CrmSettings.mailtoURL(to: draft.to, subject: draft.subject, body: draft.body)
            else { return }
            openURL(url)
            if logIt, !draft.subject.isEmpty {
                _ = await model.logEmail(to: draft.to, subject: draft.subject, contactId: draft.contactId)
            }
            dismiss()
        }
    }
}
