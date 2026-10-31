import FMAccount
import FMApi
import FMBase
import FMPlanning
import FMTools
import SwiftUI

@MainActor
struct PlanningInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        PlanningTimersRuntime.shared()?.appBecameActive()
        services.routes.register(.notes, by: "WS15") { _ in AnyView(NotesScreen()) }
        services.routes.register(.note, by: "WS15") { route in
            guard case let .note(id, comment) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(NoteEditorScreen(noteId: id.isEmpty ? nil : id, openComment: comment))
        }
        services.routes.register(.notePage, by: "WS15") { route in
            guard case let .notePage(id, note) = route, !id.isEmpty else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(NotePageScreen(pageId: id, noteId: note))
        }
        services.routes.register(.tasks, by: "WS15") { route in
            guard case let .tasks(filter) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(TasksScreen(filter: filter))
        }
        services.routes.register(.task, by: "WS15") { route in
            guard case let .task(id) = route, !id.isEmpty else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(PlanningTaskDestination(taskId: id))
        }
        services.routes.register(.calendar, by: "WS15") { route in
            guard case let .calendar(date, mode) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(CalendarScreen(date: date, mode: mode))
        }
        services.routes.register(.event, by: "WS15") { route in
            guard case let .event(id) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(CalendarEventDestination(eventId: id))
        }
        services.routes.register(.timers, by: "WS15") { _ in AnyView(TimersScreen()) }
        services.routes.register(.project, by: "WS15") { route in
            guard case let .project(repo, id) = route, let number = Int(id), number > 0 else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(RepoPlanningDestination(repo: repo, projectNumber: number))
        }
        services.routes.register(.milestones, by: "WS15") { route in
            guard case let .milestones(repo) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(RepoPlanningDestination(repo: repo, projectNumber: nil))
        }
    }
}

struct PlanningTaskDestination: View {
    let taskId: String
    @State private var model: TaskDetailModel?

    init(taskId: String) {
        self.taskId = taskId
        if let api = Services.get(ApiService.self) {
            let actor: String = Services.get(AuthService.self)?.session?.nodeName.lowercased() ?? ""
            _model = State(initialValue: TaskDetailModel(taskId: taskId, actor: actor, canManage: false, api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            TaskDetailScreen(model: model)
        } else {
            FmEmptyState(icon: "task_alt", title: "Task unavailable")
        }
    }
}
