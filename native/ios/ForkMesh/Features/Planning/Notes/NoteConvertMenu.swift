import FMApi
import FMBase
import FMPlanning
import FMTools
import SwiftUI

struct NoteConvertMenu: View {
    let model: NoteEditorModel
    let onNotice: (FmNotice) -> Void
    @State private var askingRepository: Bool = false
    @State private var repository: String = ""
    @State private var busy: Bool = false

    private var publicURL: String? {
        guard let note = model.note, note.str("visibility") == "public", let id = model.id, !id.isEmpty else {
            return nil
        }
        return PlanningServices.api?.webLink("/notes/\(id)")?.absoluteString
    }

    var body: some View {
        Menu {
            Button("Turn into a task") { toTask() }
            Button("Turn into an issue") {
                repository = ""
                askingRepository = true
            }
        } label: {
            Image(systemName: "arrow.turn.up.right")
        }
        .accessibilityLabel("Turn note into")
        .disabled(busy || model.id == nil)
        .alert("Issue in which repository?", isPresented: $askingRepository) {
            TextField("owner/repository", text: $repository)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) {}
            Button("File issue") { toIssue() }
        }
    }

    private func toTask() {
        guard let api = PlanningServices.api else {
            return
        }
        busy = true
        let title: String = model.title
        let markdown: String = model.markdown
        let link: String? = publicURL
        Task {
            let result: Result<JSONObject, TaskOperationError> = await NoteConversions.toTask(
                noteTitle: title, markdown: markdown, publicURL: link,
                call: { (method: String, suffix: String, body: JSONObject?) in try await api.tasksApi(method, suffix, body) })
            busy = false
            switch result {
            case .success:
                onNotice(FmNotice("Task created from this note"))
            case .failure(let error):
                onNotice(FmNotice.error(error.message))
            }
        }
    }

    private func toIssue() {
        guard let filer = PlanningServices.issueFiler else {
            onNotice(FmNotice.error("Sign in on this device to file issues."))
            return
        }
        busy = true
        let title: String = model.title
        let markdown: String = model.markdown
        let link: String? = publicURL
        let target: String = repository
        Task {
            let problem: String? = await NoteConversions.toIssue(
                noteTitle: title, markdown: markdown, publicURL: link, repository: target, file: filer)
            busy = false
            if let problem = problem {
                onNotice(FmNotice.error(problem))
            } else {
                onNotice(FmNotice("Issue filed from this note"))
            }
        }
    }
}
