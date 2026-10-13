import FMApi
import FMPackages
import SwiftUI

struct LedgerDestination: View {
    let section: String?

    var body: some View {
        if let client = PackagesFeature.client, let store = PackagesFeature.personalLedger {
            LedgerScreen(client: client, store: store, section: LedgerSection(route: section))
        } else {
            ContentUnavailableView("Ledger unavailable", systemImage: "book.closed")
        }
    }
}

struct LedgerScreen: View {
    @State private var model: LedgerModel
    @State private var section: LedgerSection
    @State private var adding = false

    init(client: PackagesClient, store: PersonalLedgerStore, section: LedgerSection) {
        _model = State(initialValue: LedgerModel(client: client, store: store))
        _section = State(initialValue: section)
    }

    var body: some View {
        List {
            Section {
                Picker("Section", selection: $section) {
                    ForEach(LedgerSection.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.menu)
                PackagesErrorBanner(text: model.errorText) { await refresh() }
            }
            switch section {
            case .records: records
            case .rewards: rewards
            case .mine: mine
            }
        }
        .navigationTitle("Ledger")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                if section == .mine {
                    Button { adding = true } label: { Label("Add record", systemImage: "plus") }
                }
            }
        }
        .overlay {
            if model.loading && model.entries.isEmpty && section == .records { ProgressView() }
        }
        .fmRefreshable { await refresh() }
        .task { await refresh() }
        .sheet(isPresented: $adding) {
            NavigationStack { PersonalRecordEditor { model.add($0) } }
        }
    }

    private func refresh() async {
        await model.refresh()
    }

    @ViewBuilder private var records: some View {
        Section {
            ForEach(model.entries) { entry in
                LedgerEntryRow(entry: entry)
            }
            if model.next != nil {
                Button("Load more") { Task { await model.loadMore() } }.disabled(model.loading)
            }
        } header: {
            Text(model.solUsd.isEmpty ? "Records" : "Records · SOL ≈ $\(model.solUsd)")
        }
    }

    @ViewBuilder private var rewards: some View {
        Section("Pending") {
            if model.pendingRewards.isEmpty { Text("No rewards pending.").foregroundStyle(.secondary) }
            ForEach(model.pendingRewards) { reward in
                RewardRow(reward: reward)
            }
        }
        Section {
            ForEach(model.paidRewards.prefix(50)) { reward in
                RewardRow(reward: reward)
            }
        } header: {
            Text("Paid")
        } footer: {
            Text("A public record of contributor rewards. Nothing is sent or received in this app.")
        }
    }

    @ViewBuilder private var mine: some View {
        Section {
            ForEach(model.totals, id: \.currency) { total in
                LabeledContent(total.currency) {
                    Text("+\(MoneyText.cents(total.income, "")) / −\(MoneyText.cents(total.expense, ""))").monospacedDigit()
                }
            }
        } footer: {
            Text("Your own income and expense records stay on this phone, encrypted in the keychain.")
        }
        Section {
            if model.records.isEmpty { Text("No records yet.").foregroundStyle(.secondary) }
            ForEach(model.records) { record in
                HStack {
                    Image(systemName: record.kind == .income ? "arrow.down.left" : "arrow.up.right")
                        .foregroundStyle(record.kind == .income ? .green : .red)
                    VStack(alignment: .leading) {
                        Text(record.title)
                        Text(PackagesFormat.date(record.occurredAt)).font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text(record.amountText).monospacedDigit().strikethrough(record.settled)
                }
                .accessibilityElement(children: .combine)
                .swipeActions(edge: .leading) {
                    Button(record.settled ? "Unsettle" : "Settled") { model.toggleSettled(record.id) }.tint(.green)
                }
                .swipeActions(edge: .trailing) {
                    Button("Delete", role: .destructive) { model.remove(record.id) }
                }
            }
        }
    }
}

struct LedgerEntryRow: View {
    let entry: LedgerEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(entry.title).lineLimit(2)
                Spacer()
                Text(entry.amountText).font(.subheadline.monospacedDigit())
            }
            HStack(spacing: 8) {
                Text(CrmStageLabel.title(entry.category))
                Text(CrmStageLabel.title(entry.status))
                Text(PackagesFormat.date(entry.occurredAt))
                if entry.verified { Image(systemName: "checkmark.seal").foregroundStyle(.green).accessibilityLabel("On chain") }
                if !entry.amountUsd.isEmpty && entry.currency == "SOL" { Text("≈ $\(entry.amountUsd)") }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
        .contextMenu {
            if let url = URL(string: entry.explorerUrl), !entry.explorerUrl.isEmpty {
                Link(destination: url) { Label("Open in explorer", systemImage: "safari") }
            }
        }
    }
}

struct RewardRow: View {
    let reward: RewardEstimate

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text("@" + reward.recipient)
                Spacer()
                Text(Lamports.sol(reward.amountLamports)).font(.subheadline.monospacedDigit())
            }
            Text(reward.subjectTitle.isEmpty ? reward.subjectLabel : "\(reward.subjectLabel): \(reward.subjectTitle)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
        .accessibilityElement(children: .combine)
    }
}

struct PersonalRecordEditor: View {
    let onSave: (PersonalRecord) -> Void
    @State private var kind: PersonalRecord.Kind = .expense
    @State private var title = ""
    @State private var amount = ""
    @State private var currency = "USD"
    @State private var date = Date()
    @State private var note = ""
    @Environment(\.dismiss) private var dismiss

    private var cents: Int64? {
        guard let value = Decimal(string: amount.replacingOccurrences(of: ",", with: ".")), value >= 0 else { return nil }
        return NSDecimalNumber(decimal: value * 100).int64Value
    }

    var body: some View {
        Form {
            Picker("Kind", selection: $kind) {
                Text("Expense").tag(PersonalRecord.Kind.expense)
                Text("Income").tag(PersonalRecord.Kind.income)
            }
            .pickerStyle(.segmented)
            TextField("What", text: $title)
            TextField("Amount", text: $amount).keyboardType(.decimalPad)
            TextField("Currency", text: $currency).textInputAutocapitalization(.characters)
            DatePicker("Date", selection: $date, displayedComponents: .date)
            TextField("Note", text: $note, axis: .vertical)
        }
        .navigationTitle("New record")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    guard let cents else { return }
                    onSave(PersonalRecord(
                        kind: kind, title: title, amountMinor: cents, currency: String(currency.uppercased().prefix(8)),
                        occurredAt: Int64(date.timeIntervalSince1970 * 1000), note: note))
                    dismiss()
                }
                .disabled(title.trimmingCharacters(in: .whitespaces).isEmpty || cents == nil)
            }
        }
    }
}
