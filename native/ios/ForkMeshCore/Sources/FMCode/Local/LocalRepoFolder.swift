import Foundation

public enum LocalRepoFolder {
    public static let bookmarkKey = "localRepos.folderBookmark"

    public static let insideApp =
        "That folder is inside ForkMesh's own folder, which iOS deletes with the app. Make or pick a folder outside it, for example in On My iPhone."
    public static let inICloud =
        "Pick a folder on this iPhone. Repositories in iCloud Drive can lose files Git needs."
    public static let unreachable =
        "The folder chosen for repositories can't be opened, so they are saved in ForkMesh's folder for now. Choose the folder again to get them back."
    public static let appFolderNote =
        "Saved in ForkMesh's folder in Files. iOS deletes it if the app is deleted."
    public static let backInAppFolder =
        "Repositories are saved in ForkMesh's folder again. iOS deletes it if the app is deleted."

    public static func chosenNote(_ folder: String) -> String {
        "Saved in “\(folder)” in Files. It stays if ForkMesh is deleted; after reinstalling, choose it again."
    }

    public static func problem(with folder: URL, appContainer: URL, isInICloud: Bool) -> String? {
        let picked = normalized(folder)
        let home = normalized(appContainer)
        if picked == home || picked.hasPrefix(home + "/") { return insideApp }
        if isInICloud { return inICloud }
        return nil
    }

    public static func movedMessage(_ result: LocalRepoRelocation, to folder: String) -> String {
        var message: String
        switch result.moved.count {
        case 0: message = "Repositories are now saved in “\(folder)”."
        case 1: message = "Moved 1 repository to “\(folder)”."
        default: message = "Moved \(result.moved.count) repositories to “\(folder)”."
        }
        if !result.leftBehind.isEmpty {
            message += " \(result.leftBehind.joined(separator: ", ")) stayed where "
                + (result.leftBehind.count == 1 ? "it was" : "they were")
                + ": “\(folder)” already has a folder with that name."
        }
        return message
    }

    @MainActor
    public static func failure(_ error: Error) -> String {
        "Repositories were not moved. " + LocalRepoScreenModel.message(error)
    }

    static func normalized(_ url: URL) -> String {
        let path = url.resolvingSymlinksInPath().standardizedFileURL.path
        if path.hasPrefix("/private/var/") { return String(path.dropFirst("/private".count)) }
        return path
    }
}
