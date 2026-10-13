// WS7: a new Vultr host, created by the desktop with its own Vultr key. The plan and region
// pickers list the desktop's own choices, with its automatic pick preselected.

import FMControl
import FMHosts
import SwiftUI

struct AddHostSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var store: HostsStore

    @State private var form: AddHostForm?
    @State private var error = ""
    @State private var confirmation: HostConfirmation?
    @State private var preparing = false

    var body: some View {
        Form {
            if let binding = Binding($form) {
                Section {
                    TextField(binding.wrappedValue.options.suggestedName.isEmpty ? "Automatic" : binding.wrappedValue.options.suggestedName,
                              text: binding.name)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } header: {
                    Text("Name")
                } footer: {
                    Text("Leave empty to use the next free mirror name.")
                }
                Section {
                    Picker("Plan", selection: Binding(
                        get: { binding.wrappedValue.planId },
                        set: { binding.wrappedValue.choosePlan($0) }
                    )) {
                        ForEach(binding.wrappedValue.options.plans) { plan in
                            Text(planTitle(plan, defaultId: binding.wrappedValue.options.defaultPlan)).tag(plan.id)
                        }
                    }
                    .pickerStyle(.navigationLink)
                    Picker("Region", selection: binding.region) {
                        ForEach(binding.wrappedValue.regions, id: \.self) { region in
                            Text(regionTitle(region)).tag(region)
                        }
                    }
                } header: {
                    Text("Server")
                } footer: {
                    Text("The desktop's own choices: every plan with at least 1 GB of memory and IPv4 in a US region, cheapest first. The preselected plan and region are what the desktop picks by itself.")
                }
                if let problem = binding.wrappedValue.problem {
                    Section {
                        Label(problem, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                            .fmMessageTools(problem)
                    }
                }
                Section {
                    Button {
                        Task { await prepare() }
                    } label: {
                        HStack {
                            Text("Create \(binding.wrappedValue.effectiveName.isEmpty ? "host" : binding.wrappedValue.effectiveName)")
                            Spacer()
                            if preparing { ProgressView() }
                        }
                    }
                    .disabled(preparing || binding.wrappedValue.problem != nil)
                }
            } else if error.isEmpty {
                HStack {
                    ProgressView()
                    Text("Asking Vultr through the desktop…").foregroundStyle(.secondary)
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                        .fmMessageTools(error)
                }
            }
        }
        .navigationTitle("Add host")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
        }
        .task { await load() }
        .sheet(item: $confirmation, onDismiss: {
            if !store.running.filter({ $0.hasPrefix("install:") }).isEmpty { dismiss() }
        }) { confirmation in
            NavigationStack {
                HostActionSheet(
                    store: store, confirmation: confirmation, plan: form?.requestedPlan ?? "",
                    region: form?.requestedRegion ?? "")
            }
            .presentationDetents([.medium])
        }
    }

    private func planTitle(_ plan: HostPlanChoice, defaultId: String) -> String {
        plan.id == defaultId ? "\(plan.label) (default)" : plan.label
    }

    private func regionTitle(_ code: String) -> String {
        let names = [
            "ewr": "New Jersey", "atl": "Atlanta", "ord": "Chicago", "dfw": "Dallas", "mia": "Miami",
            "lax": "Los Angeles", "sea": "Seattle", "sjc": "Silicon Valley", "hon": "Honolulu",
        ]
        if let name = names[code] { return "\(name) (\(code))" }
        return code
    }

    private func load() async {
        do {
            let options = try await store.createOptions()
            form = AddHostForm(options: options)
            error = options.vultr ? "" : (AddHostForm(options: options).problem ?? "")
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }

    private func prepare() async {
        guard let form else { return }
        preparing = true
        defer { preparing = false }
        do {
            let typed = form.name.trimmingCharacters(in: .whitespacesAndNewlines)
            confirmation = try await store.prepare(.install, host: typed)
            error = ""
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }
}
