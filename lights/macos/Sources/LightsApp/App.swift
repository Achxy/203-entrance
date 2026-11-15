import LightsMenulet
import SwiftUI

@main
struct LightsApplication: App {
    @StateObject private var model = MenuletModel.configured(persistPreferences: true)
    var body: some Scene {
        MenuBarExtra("203 Lights", systemImage: "lightbulb.2") {
            QuickPanel(model: model)
        }.menuBarExtraStyle(.window)
        Settings { MenuletSettings(model: model) }
    }
}
