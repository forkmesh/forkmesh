#if canImport(AuthenticationServices) && os(iOS)
import AuthenticationServices
import Foundation

public enum VaultAutofillServices {
    public static func encode(_ identifier: ASCredentialServiceIdentifier) -> String {
        #if compiler(>=6.3)
        if #available(iOS 26.2, *), identifier.type == .app {
            return VaultAutofillCache.appPrefix + identifier.identifier
        }
        #endif
        return identifier.identifier
    }

    public static func label(_ identifiers: [ASCredentialServiceIdentifier]) -> String {
        for identifier in identifiers {
            #if compiler(>=6.3)
            if #available(iOS 26.2, *), identifier.type == .app {
                if let name = identifier.displayName, !name.isEmpty { return name }
                continue
            }
            #endif
            let host = VaultAutofillCache.host(of: identifier.identifier)
            if !host.isEmpty { return host }
        }
        return ""
    }

    public static func serviceIdentifier(for host: String) -> ASCredentialServiceIdentifier? {
        if host.hasPrefix(VaultAutofillCache.appPrefix) {
            #if compiler(>=6.3)
            if #available(iOS 26.2, *) {
                return ASCredentialServiceIdentifier(identifier: String(host.dropFirst(VaultAutofillCache.appPrefix.count)), type: .app)
            }
            #endif
            return nil
        }
        return ASCredentialServiceIdentifier(identifier: host, type: .domain)
    }

    public static func identities(_ identities: [VaultAutofillIdentity]) -> [ASPasswordCredentialIdentity] {
        identities.compactMap { identity in
            serviceIdentifier(for: identity.host).map {
                ASPasswordCredentialIdentity(serviceIdentifier: $0, user: identity.user, recordIdentifier: identity.recordIdentifier)
            }
        }
    }
}
#endif
