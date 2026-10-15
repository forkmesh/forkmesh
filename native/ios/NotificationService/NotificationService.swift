// The notification service extension, with the per-project session lookup
// (the push's `origin` picks the keychain item, Shared/PushCredentialStore.swift).
//
// Topic behaviour: Apple delivers to the containing app's apns-topic
// (com.forkmesh.forkmesh, the App Store app id; the relay stores it per device
// token) and iOS runs this extension; the extension's own bundle id is never a
// topic. Only `chat` and `direct-messages` are
// sealed (a direct message gets only its sender's portrait); every other push,
// `code-push` included, names its row (`alertId`) and is enriched with that
// row's portrait and the unread badge.

import FMBase
import FMCrypto
import FMPush
import UserNotifications

/// Adds the actor's portrait and the unread badge to a ForkMesh push.
///
/// The Worker's alert already names its event in full — headline, repository
/// and byline, body text (see `app/src/apns.py`) — so the banner reads right
/// even when nothing here succeeds. Because every alert sets
/// `mutable-content`, iOS runs this extension before the banner is drawn, and
/// it asks the relay, over the account's own session out of the shared
/// keychain, for the same notification rendered for a lock screen
/// (`GET /api/push/alert`) to pick up what a 4 KB payload cannot carry: the
/// actor's portrait as an attachment and the unread count as the app badge.
///
/// The push names its row (`alertId`), so the extension enriches exactly that
/// one. A push from a relay that predates that falls back to the newest unread
/// row, as before. Chat and direct messages are sealed end to end and their
/// alert already says who or where; a direct message only picks up its
/// sender's portrait from the public profile (`GET /api/accounts/<sender>`).
///
/// Every failure path ends at the same place — `deliver` with the alert the
/// push arrived with, which is the event itself.
final class NotificationService: UNNotificationServiceExtension {
    /// iOS allows around 30 seconds; well before that it revokes the extension
    /// and posts the original content. Give up early enough to still deliver a
    /// deliberate fallback instead of being cut off mid-flight.
    private static let fetchTimeout: TimeInterval = 8
    private static let receiptTimeout: TimeInterval = 5

    /// Alert ids already rendered by an earlier invocation, newest last. Two
    /// pings landing seconds apart otherwise both read "the newest unread row"
    /// and draw the same banner twice. `UserDefaults.standard` here is the
    /// extension's own container — it persists between invocations and needs no
    /// entitlement, which is why the dedupe lives here and not in the keychain.
    private static let renderedKey = "forkmesh.rendered-alert-ids"
    private static let renderedMemory = 20
    static let roomKeys = PushRoomKeys(
        store: KeychainSharedItemStore(service: PushCredentialStore.service, accessGroup: PushCredentialStore.accessGroup))

    private var contentHandler: ((UNNotificationContent) -> Void)?
    private var fallback: UNNotificationContent?
    private var task: URLSessionDataTask?
    private var receipt: URLSessionDataTask?
    private let receiptSent = DispatchGroup()

    override func didReceive(
        _ request: UNNotificationRequest,
        withContentHandler contentHandler: @escaping (UNNotificationContent) -> Void
    ) {
        self.contentHandler = contentHandler
        self.fallback = request.content
        guard
            let content = request.content.mutableCopy() as? UNMutableNotificationContent
        else {
            deliver(request.content)
            return
        }

        // Chat and direct messages are sealed end to end: the relay stores
        // ciphertext, so there is no text to go and fetch. The relay's alert
        // already names the sender or the channel; only an alert that somehow
        // arrived without a title gets the kind named here.
        if let sealed = Self.sealedTopic(request.content.userInfo["topic"]) {
            if content.title.isEmpty {
                content.title = sealed
                content.body = "End-to-end encrypted \u{2014} open Chat in ForkMesh to read it."
            }
            let pushedOrigin = (request.content.userInfo["origin"] as? String) ?? ""
            let credentials = PushCredentialStore.load(origin: pushedOrigin)
            let origin = credentials?.origin ?? pushedOrigin
            let sender = request.content.userInfo["sender"] as? String
            let mode = PushPreviewMode.parse(credentials?.preview)
            let chat = PushChatRequest.parse(Self.payload(request.content.userInfo))
            if let chat, !chat.threadIdentifier.isEmpty { content.threadIdentifier = chat.threadIdentifier }
            sendDeliveryReceipt(chat, credentials: credentials)
            if mode == .nameAndMessage, PushMeshWake.isWake(request.content.userInfo) {
                content.threadIdentifier = PushMeshWake.identifier
                UNUserNotificationCenter.current().getDeliveredNotifications { delivered in
                    if let local = delivered.first(where: {
                        PushMeshWake.isLocalAlert(identifier: $0.request.identifier, info: $0.request.content.userInfo)
                    }) {
                        content.title = local.request.content.title
                        content.subtitle = ""
                        content.body = local.request.content.body
                    }
                    self.deliver(content)
                }
                return
            }
            switch mode {
            case .hidden:
                content.title = PushPreviewText.hiddenTitle
                content.subtitle = ""
                content.body = PushPreviewText.genericBody
                deliver(content)
            case .nameOnly:
                if let chat, let preview = PushPreviewText.preview(nil, request: chat, mode: .nameOnly) {
                    content.title = preview.title
                }
                content.body = PushPreviewText.genericBody
                attachPortrait(to: content, origin: origin, sender: sender)
            case .nameAndMessage:
                guard let chat, let credentials,
                    let url = Self.pendingURL(origin: credentials.origin, path: chat.pendingPath)
                else {
                    attachPortrait(to: content, origin: origin, sender: sender)
                    return
                }
                var fetch = URLRequest(url: url)
                fetch.httpMethod = "GET"
                fetch.timeoutInterval = Self.fetchTimeout
                fetch.setValue("Bearer " + credentials.session, forHTTPHeaderField: "Authorization")
                fetch.setValue("application/json", forHTTPHeaderField: "Accept")
                fetch.cachePolicy = .reloadIgnoringLocalCacheData
                let deviceKeyName = credentials.deviceKeyName
                task = URLSession.shared.dataTask(with: fetch) { data, response, _ in
                    if let data, (response as? HTTPURLResponse)?.statusCode == 200,
                        let message = PushChatOpener.open(
                            JSON.parseObject(data), request: chat,
                            privateKey: PushCredentialStore.deviceKey(named: deviceKeyName), keys: Self.roomKeys),
                        let preview = PushPreviewText.preview(message, request: chat, mode: .nameAndMessage)
                    {
                        content.title = preview.title
                        content.subtitle = ""
                        content.body = preview.body
                        var info = content.userInfo
                        info["messageId"] = message.id
                        content.userInfo = info
                    }
                    self.attachPortrait(to: content, origin: origin, sender: sender)
                }
                task?.resume()
            }
            return
        }

        // The instance that sent the push picks the session: a ForkMesh Next
        // push is expanded with the Next session, never with production's.
        let origin =
            (request.content.userInfo["origin"] as? String)
            ?? (request.content.userInfo["host"] as? String) ?? ""
        guard let credentials = PushCredentialStore.load(origin: origin),
            let url = Self.alertURL(origin: credentials.origin, account: credentials.account)
        else {
            // Signed out, or a build that has not registered yet. The alert the
            // relay sent already says what happened.
            deliver(request.content)
            return
        }

        var fetch = URLRequest(url: url)
        fetch.httpMethod = "GET"
        fetch.timeoutInterval = Self.fetchTimeout
        fetch.setValue("Bearer " + credentials.session, forHTTPHeaderField: "Authorization")
        fetch.setValue("application/json", forHTTPHeaderField: "Accept")
        fetch.cachePolicy = .reloadIgnoringLocalCacheData

        // Captured strongly on purpose. A weak capture that lost its object would
        // return without calling the handler at all, and iOS treats an invocation
        // that never posts as a crash; the cycle self -> task -> closure -> self
        // breaks the moment the task completes, which is always, timeout
        // included.
        let pushed = (request.content.userInfo["alertId"] as? String) ?? ""
        task = URLSession.shared.dataTask(with: fetch) { data, _, _ in
            guard let data = data,
                let alert = Self.alertToRender(in: data, pushed: pushed)
            else {
                if let data = data, let unread = Self.unreadCount(in: data), unread > 0 {
                    content.badge = NSNumber(value: unread)
                }
                self.deliver(content)
                return
            }
            Self.apply(alert.fields, unread: alert.unread, to: content)
            Self.remember(alert.id)
            self.deliver(content)
        }
        task?.resume()
    }

    private func sendDeliveryReceipt(_ chat: PushChatRequest?, credentials: PushCredentials?) {
        guard let chat, let credentials, let path = chat.deliveredPath,
            let url = Self.pendingURL(origin: credentials.origin, path: path)
        else { return }
        var post = URLRequest(url: url)
        post.httpMethod = "POST"
        post.timeoutInterval = Self.receiptTimeout
        post.setValue("Bearer " + credentials.session, forHTTPHeaderField: "Authorization")
        post.setValue("application/json", forHTTPHeaderField: "Accept")
        post.cachePolicy = .reloadIgnoringLocalCacheData
        let sent = receiptSent
        sent.enter()
        receipt = URLSession.shared.dataTask(with: post) { _, _, _ in sent.leave() }
        receipt?.resume()
    }

    private func attachPortrait(to content: UNMutableNotificationContent, origin: String, sender: String?) {
        guard let url = Self.senderURL(origin: origin, sender: sender) else {
            deliver(content)
            return
        }
        var fetch = URLRequest(url: url)
        fetch.httpMethod = "GET"
        fetch.timeoutInterval = Self.fetchTimeout
        fetch.setValue("application/json", forHTTPHeaderField: "Accept")
        task = URLSession.shared.dataTask(with: fetch) { data, _, _ in
            if let portrait = Self.attachment(base64Png: Self.avatarPng(in: data)) {
                content.attachments = [portrait]
            }
            self.deliver(content)
        }
        task?.resume()
    }

    static func payload(_ info: [AnyHashable: Any]) -> JSONObject {
        var fields: [String: Any] = [:]
        for (key, value) in info {
            if let key = key as? String { fields[key] = value }
        }
        guard JSONSerialization.isValidJSONObject(fields), let data = try? JSONSerialization.data(withJSONObject: fields)
        else { return [:] }
        return JSON.parseObject(data)
    }

    static func pendingURL(origin: String, path: String) -> URL? {
        guard var components = URLComponents(string: origin),
            components.scheme == "https" || components.scheme == "http", components.host?.isEmpty == false
        else { return nil }
        let parts = path.split(separator: "?", maxSplits: 1)
        guard let route = parts.first, route.hasPrefix("/api/chat/") else { return nil }
        components.path = String(route)
        components.percentEncodedQuery = parts.count > 1 ? String(parts[1]) : nil
        components.fragment = nil
        return components.url
    }

    /// iOS is about to take the extension away. Deliver whatever has been
    /// assembled so far rather than letting the system post nothing.
    override func serviceExtensionTimeWillExpire() {
        task?.cancel()
        receipt?.cancel()
        if let content = fallback { deliver(content) }
    }

    /// The single exit. Every path — an early return, the fetch completing, the
    /// system taking the extension away — ends here, and the nil-out makes the
    /// second caller a no-op: posting twice from one invocation is treated as a
    /// crash and costs the extension its next few launches.
    private func deliver(_ content: UNNotificationContent) {
        guard let handler = contentHandler else { return }
        contentHandler = nil
        receiptSent.notify(queue: .global()) { handler(content) }
    }

    /// The banner text for a topic the relay cannot read, or nil for the inbox
    /// topics, whose content this extension goes and fetches.
    static func sealedTopic(_ topic: Any?) -> String? {
        switch topic as? String {
        case "chat": return "New message"
        case "direct-messages": return "New direct message"
        default: return nil
        }
    }

    static func alertURL(origin: String, account: String) -> URL? {
        guard var components = URLComponents(string: origin) else { return nil }
        // Only ever an https origin the app itself is already talking to; a
        // plain-http relay would put the session on the wire in clear.
        guard components.scheme == "https" || components.scheme == "http" else { return nil }
        components.path = "/api/push/alert"
        components.queryItems = [
            URLQueryItem(name: "node", value: account),
            URLQueryItem(name: "limit", value: "3"),
        ]
        return components.url
    }

    static func senderURL(origin: String, sender: String?) -> URL? {
        let name = (sender ?? "").trimmingCharacters(in: .whitespaces).lowercased()
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyz0123456789-_.")
        guard !name.isEmpty, name.count <= 64, name != ".", name != "..",
            name.unicodeScalars.allSatisfy({ allowed.contains($0) }),
            var components = URLComponents(string: origin),
            components.scheme == "https" || components.scheme == "http",
            components.host?.isEmpty == false
        else {
            return nil
        }
        components.path = "/api/accounts/" + name
        components.query = nil
        components.fragment = nil
        return components.url
    }

    static func avatarPng(in data: Data?) -> String? {
        guard let data = data,
            let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            return nil
        }
        return root["avatarPng"] as? String
    }

    // MARK: - Rendering

    /// The alert the push named, or — for a push from an older relay that names
    /// none — the newest one no earlier invocation has drawn. Nil when the
    /// named row is not in the answer: the push's own text then stands.
    static func alertToRender(
        in data: Data, pushed: String
    ) -> (id: String, unread: Int, fields: [String: Any])? {
        guard !pushed.isEmpty else { return firstUnrendered(in: data) }
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let alerts = root["alerts"] as? [[String: Any]],
            let match = alerts.first(where: { ($0["id"] as? String) == pushed })
        else {
            return nil
        }
        return (id: pushed, unread: root["unread"] as? Int ?? 0, fields: match)
    }

    static func unreadCount(in data: Data) -> Int? {
        let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        return root?["unread"] as? Int
    }

    /// The newest alert in the relay's answer that no earlier invocation has
    /// already drawn, with the account's unread count alongside it.
    static func firstUnrendered(in data: Data) -> (id: String, unread: Int, fields: [String: Any])? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let alerts = root["alerts"] as? [[String: Any]], !alerts.isEmpty
        else {
            return nil
        }
        let unread = root["unread"] as? Int ?? 0
        let seen = Set(UserDefaults.standard.stringArray(forKey: renderedKey) ?? [])
        // Falling back to the newest rather than to the relay's own alert is
        // deliberate: two pushes can describe one row, and the same event said
        // twice reads better than two banners that disagree.
        let chosen = alerts.first { !seen.contains(($0["id"] as? String) ?? "") } ?? alerts[0]
        return (id: (chosen["id"] as? String) ?? "", unread: unread, fields: chosen)
    }

    static func apply(
        _ alert: [String: Any], unread: Int, to content: UNMutableNotificationContent
    ) {
        let title = (alert["title"] as? String) ?? ""
        if !title.isEmpty { content.title = title }
        content.subtitle = (alert["subtitle"] as? String) ?? ""
        // An event with no text of its own (a release, a host going offline) has
        // said everything in the title and subtitle; leaving the placeholder body
        // under it would contradict them.
        content.body = (alert["body"] as? String) ?? ""
        if let thread = alert["threadId"] as? String, !thread.isEmpty {
            content.threadIdentifier = thread
        }
        if unread > 0 { content.badge = NSNumber(value: unread) }
        // Where a tap on this banner should land (issue #724). The extension is
        // the only party that knows which row it drew, so it leaves that on the
        // delivered notification for PushNotificationBridge to hand to the app.
        var info = content.userInfo
        if let id = alert["id"] as? String, !id.isEmpty { info["alertId"] = id }
        if let href = alert["href"] as? String, href.hasPrefix("/") { info["href"] = href }
        if let kind = alert["kind"] as? String {
            info["kind"] = kind
            if content.categoryIdentifier.isEmpty {
                let categories: [String: String] = [
                    "agent_approval": "agent-approval", "pull_submitted": "pr-review", "mention": "mention",
                ]
                content.categoryIdentifier = categories[kind] ?? "ping"
            }
        }
        if let repo = alert["repo"] as? String { info["repo"] = repo }
        content.userInfo = info
        if let portrait = attachment(base64Png: alert["avatarPng"] as? String) {
            content.attachments = [portrait]
        }
    }

    /// The actor's portrait, written to the extension's temporary directory
    /// because `UNNotificationAttachment` only takes a file URL. iOS moves the
    /// file into its own store, so there is nothing to clean up afterwards.
    static func attachment(base64Png: String?) -> UNNotificationAttachment? {
        guard let base64Png = base64Png, !base64Png.isEmpty,
            let bytes = Data(base64Encoded: base64Png)
        else {
            return nil
        }
        let url = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("forkmesh-actor-" + UUID().uuidString + ".png")
        guard (try? bytes.write(to: url)) != nil else { return nil }
        return try? UNNotificationAttachment(identifier: "actor", url: url, options: nil)
    }

    static func remember(_ id: String) {
        guard !id.isEmpty else { return }
        var seen = UserDefaults.standard.stringArray(forKey: renderedKey) ?? []
        seen.removeAll { $0 == id }
        seen.append(id)
        if seen.count > renderedMemory { seen.removeFirst(seen.count - renderedMemory) }
        UserDefaults.standard.set(seen, forKey: renderedKey)
    }
}
