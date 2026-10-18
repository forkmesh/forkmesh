import FMCodeInsight
import SwiftUI

struct EmberFilterSheet: View {
    @Bindable var model: EmberPageModel
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss

    private var commits: Binding<Double> {
        Binding(get: { Double(model.filters.minCommits) },
                set: { (value: Double) in
                    model.filters.minCommits = Int(value.rounded())
                    model.filtersChanged(debounce: true)
                })
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Search by filename") {
                    TextField("Search by filename", text: $model.filters.search)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.search)
                        .onSubmit {
                            model.selectBestMatch()
                            dismiss()
                        }
                        .onChange(of: model.filters.search) { (_: String, _: String) in model.filtersChanged(debounce: true) }
                }
                Section {
                    Picker("Author", selection: $model.filters.author) {
                        ForEach(model.authorOptions) { (option: EmberAuthorOption) in
                            Text(option.label).tag(option.author)
                        }
                    }
                    .disabled(!model.history || model.authorOptions.count <= 1)
                    .onChange(of: model.filters.author) { (_: String, _: String) in model.filtersChanged(debounce: false) }
                } header: {
                    Text("Filter by authors")
                } footer: {
                    if !model.history { Text("Author and commit filters need git history.") }
                }
                Section {
                    EmberRangeSlider(low: $model.filters.minHealth, high: $model.filters.maxHealth, bounds: 1...10, step: 0.5) {
                        model.filtersChanged(debounce: true)
                    }
                    .padding(.vertical, FmSpace.x2)
                    HStack {
                        Text("\(Ember.decimal(model.filters.minHealth)) to \(Ember.decimal(model.filters.maxHealth))")
                            .fmText(.bodyMedium, tabular: true)
                        Spacer()
                        ForEach(HealthBand.allCases, id: \.self) { (band: HealthBand) in
                            let range = Ember.bandRange(band)
                            let active = model.filters.minHealth == range.0 && model.filters.maxHealth == range.1
                            Button { model.toggleBand(band) } label: {
                                Circle()
                                    .fill(Ember.bandColor(band).color)
                                    .frame(width: 22, height: 22)
                                    .overlay(Circle().stroke(fm.textPrimary, lineWidth: active ? 2 : 0))
                                    .frame(width: 36, height: 36)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel("Only \(band.name.lowercased()) files")
                            .accessibilityAddTraits(active ? [.isButton, .isSelected] : .isButton)
                        }
                    }
                } header: {
                    Text("Code health range")
                }
                Section {
                    Slider(value: commits, in: 0...Double(max(1, model.commitMax)), step: 1) {
                        Text("Minimum commits")
                    }
                    .disabled(!model.history)
                    .accessibilityValue(Ember.commitThresholdText(model.filters.minCommits))
                } header: {
                    HStack {
                        Text("Commit threshold")
                        Spacer()
                        Text(Ember.commitThresholdText(model.filters.minCommits))
                    }
                }
                Section {
                    Button("Reset filters") { model.resetFilters() }
                        .disabled(!model.filters.active)
                    Text("\(Ember.number(model.coloring.matching)) of \(Ember.number(model.snapshot.files.count)) files match")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .navigationTitle("Filters")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
    }
}
