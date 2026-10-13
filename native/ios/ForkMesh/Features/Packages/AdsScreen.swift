import FMApi
import FMPackages
import SwiftUI

struct AdsDestination: View {
    var body: some View {
        PackagesGate(feature: "Ad review") { client in
            AdsScreen(client: client)
        }
    }
}

struct AdsScreen: View {
    @State private var model: AdsModel
    @State private var reviewing: OwnedAd?

    init(client: PackagesClient) {
        _model = State(initialValue: AdsModel(client: client))
    }

    var body: some View {
        List {
            PackagesErrorBanner(text: model.errorText) { await model.refresh() }
            if model.isReviewer { reviewQueue }
        }
        .navigationTitle("Ad review")
        .overlay {
            if model.loading && !model.checked {
                ProgressView()
            } else if model.checked && !model.isReviewer {
                ContentUnavailableView(
                    "Nothing to review", systemImage: "checkmark.shield",
                    description: Text("Sponsored messages are checked here by the people who review them."))
            }
        }
        .fmRefreshable { await model.refresh() }
        .task { await model.refresh() }
        .sheet(item: $reviewing) { ad in
            NavigationStack { AdReviewSheet(model: model, ad: ad) }
        }
    }

    @ViewBuilder private var reviewQueue: some View {
        Section {
            Picker("Status", selection: $model.reviewFilter) {
                ForEach(AdReviewQueue.filters, id: \.self) { filter in
                    Text("\(CrmStageLabel.title(filter)) (\(model.queue?.counts[filter] ?? 0))").tag(filter)
                }
            }
            .onChange(of: model.reviewFilter) { _, _ in Task { await model.loadQueue() } }
        }
        Section {
            if model.queue?.ads.isEmpty ?? true { Text("Nothing to review.").foregroundStyle(.secondary) }
            ForEach(model.queue?.ads ?? []) { ad in
                Button {
                    reviewing = ad
                } label: {
                    AdRow(ad: ad, busy: model.working.contains(ad.id))
                }
                .buttonStyle(.plain)
            }
        }
    }
}

struct AdRow: View {
    let ad: OwnedAd
    let busy: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(ad.headline.isEmpty ? "Untitled ad" : ad.headline).font(.body.weight(.medium))
                Spacer()
                if busy { ProgressView() }
            }
            if !ad.body.isEmpty { Text(ad.body).font(.subheadline).foregroundStyle(.secondary).lineLimit(3) }
            HStack(spacing: 10) {
                Text(ad.deliveryText)
                if ad.clicks > 0 { Text("\(ad.clicks) clicks in \(ad.clickWindowDays) days") }
                if !ad.owner.isEmpty { Text("@" + ad.owner) }
                if ad.openReports > 0 { Label("\(ad.openReports)", systemImage: "flag").foregroundStyle(.red) }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            if !ad.reviewReason.isEmpty {
                Text(ad.reviewReason).font(.caption).foregroundStyle(.orange)
                    .fmMessageTools(ad.reviewReason)
            }
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}

struct AdReviewSheet: View {
    let model: AdsModel
    let ad: OwnedAd
    @State private var reason = ""
    @State private var codes: Set<String> = []
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section("Creative") {
                Text(ad.headline).font(.headline)
                Text(ad.body)
                LabeledContent("Button", value: ad.cta)
                LabeledContent("Sponsor", value: ad.sponsor)
                LabeledContent("Link", value: ad.url).textSelection(.enabled)
                LabeledContent("Category", value: ad.category)
                if !ad.screenFlags.isEmpty {
                    Label(ad.screenFlags.joined(separator: ", "), systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                        .fmMessageTools(ad.screenFlags.joined(separator: ", "))
                }
            }
            if ad.reviewActions.contains("reject") || ad.reviewActions.contains("suspend") {
                Section("Reason (needed to reject or suspend)") {
                    TextField("What rule does it break?", text: $reason, axis: .vertical)
                    ForEach(model.queue?.prohibited ?? []) { rule in
                        Toggle(rule.label, isOn: Binding(
                            get: { codes.contains(rule.id) },
                            set: { (on: Bool) -> Void in
                                if on { _ = codes.insert(rule.id) } else { _ = codes.remove(rule.id) }
                            }))
                    }
                }
            }
            Section {
                ForEach(ad.reviewActions, id: \.self) { action in
                    let decision = AdReviewDecision(action: action, reason: reason, codes: Array(codes))
                    Button(CrmStageLabel.title(action), role: action == "approve" || action == "reinstate" ? nil : .destructive) {
                        Task {
                            if await model.review(ad, decision: decision) {
                                FmHaptics.commit()
                                dismiss()
                            }
                        }
                    }
                    .disabled(!decision.isComplete || model.working.contains(ad.id))
                }
            } footer: {
                Text("Approving pins this exact text. An edit sends it back to review.")
            }
        }
        .navigationTitle("Review ad")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
        }
    }
}
