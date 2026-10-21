// Import external repository metadata from GitHub, GitLab, Codeberg or Gitea. The
// logic is FMCode's MirrorImportModel. `onDone` receives the notice to show
// (nil on Cancel) and the presenter dismisses.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct MirrorImportSheet: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: MirrorImportModel

    private let onDone: (String?) -> Void

    init(onDone: @escaping (String?) -> Void) {
        _model = State(initialValue: MirrorImportModel(client: RepoServices.client, session: { RepoServices.session }))
        self.onDone = onDone
    }

    private var urlBinding: Binding<String> {
        Binding<String>(
            get: { model.url },
            set: { (value: String) in
                model.url = value
                model.urlChanged()
            })
    }

    private func submit() {
        Task {
            if let notice = await model.submit() {
                onDone(notice)
            }
        }
    }

    private var urlField: some View {
        FmField(label: "Repository URL") {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                FmTextField(text: urlBinding, hint: "https://github.com/owner/repository", autofocus: true, onSubmit: submit)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                if let problem = model.problem {
                    Text(problem)
                        .fmText(.bodySmall, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(problem)
                }
            }
        }
    }

    private var tokenField: some View {
        FmField(label: "Provider token (private repositories only)") {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                FmTextField(text: $model.token, secure: true)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Text(MirrorImportModel.tokenHelper)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(MirrorImportModel.title)
                .fmText(.titleLarge)
                .padding(.bottom, FmSpace.x4)
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    urlField
                    tokenField
                    Text(MirrorImportModel.metadataNote)
                        .fmText(.bodySmall, color: fm.textTertiary, height: 1.4)
                    if !model.status.isEmpty {
                        Text(model.status)
                            .fmText(.bodySmall, color: model.busy ? fm.textSecondary : fm.danger, height: 1.4)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(model.busy ? nil : model.status)
                    }
                }
            }
            .scrollBounceBehavior(.basedOnSize)
            FmDialogActions(
                onCancel: { onDone(nil) },
                onConfirm: submit,
                confirmLabel: "Import",
                busy: model.busy)
                .padding(.top, FmSpace.x3)
        }
        .padding(FmSpace.x5)
        .fmPreGlassBackground(fm.bgOverlay)
        .fmSheetStyle(fitsContent: true, background: fm.bgOverlay)
    }
}
