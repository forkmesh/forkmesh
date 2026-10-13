// Port of native/android core/core/Services.kt: the app-wide service registry.
// Each feature registers
// its long-lived services at startup and view models look them up by type:
//
//     Services.put(ApiService(settings))
//     let api: ApiService = Services.require()

import Foundation

public enum Services {
    private static let lock = NSLock()
    private static var registry: [ObjectIdentifier: Any] = [:]

    /// Registers `instance` under its static type `T`; pass the protocol type
    /// explicitly (`Services.put(store, as: KeyValueStore.self)`) to register
    /// under an existential.
    @discardableResult
    public static func put<T>(_ instance: T, as type: T.Type = T.self) -> T {
        lock.lock()
        registry[ObjectIdentifier(type)] = instance
        lock.unlock()
        return instance
    }

    public static func remove<T>(_ type: T.Type) {
        lock.lock()
        registry.removeValue(forKey: ObjectIdentifier(type))
        lock.unlock()
    }

    /// The registered instance, or nil when the feature providing it is absent.
    public static func get<T>(_ type: T.Type = T.self) -> T? {
        lock.lock()
        defer { lock.unlock() }
        return registry[ObjectIdentifier(type)] as? T
    }

    /// The registered instance; traps when nothing registered it, which is a wiring bug.
    public static func require<T>(_ type: T.Type = T.self) -> T {
        guard let instance = get(type) else { fatalError("\(type) is not registered") }
        return instance
    }

    /// Test hook.
    public static func clear() {
        lock.lock()
        registry.removeAll()
        lock.unlock()
    }
}
