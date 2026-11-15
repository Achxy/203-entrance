import Foundation

/// Only nonsecret appearance/connection choices live in UserDefaults. Access
/// credentials and gateway grants belong in the separate Keychain store.
@MainActor final class MenuletPreferences {
    private let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }
    var appearance: String {
        get { let value = defaults.string(forKey: "lights.appearance") ?? "system"; return ["system", "light", "dark"].contains(value) ? value : "system" }
        set { defaults.set(newValue, forKey: "lights.appearance") }
    }
    var connection: ConnectionPreference {
        get { ConnectionPreference(rawValue: defaults.string(forKey: "lights.connection") ?? "") ?? .automatic }
        set { defaults.set(newValue.rawValue, forKey: "lights.connection") }
    }
}
