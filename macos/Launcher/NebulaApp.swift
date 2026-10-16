// SPDX-License-Identifier: GPL-3.0-only
//
// Nebula for Mac: a setup assistant on first run (the game image, the control method, the
// installation), then the main window with the Play button, plus a standard Settings window.

import AppKit
import GameController
import SwiftUI

struct NebulaApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        Window("Orbit", id: "main") {
            RootView()
                .environmentObject(model)
                .environmentObject(model.settings)
                .environmentObject(model.installer)
                .environmentObject(model.runner)
                .frame(width: 700, height: 470)
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
        SwiftUI.Settings {
            SettingsView()
                .environmentObject(model)
                .environmentObject(model.settings)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

enum Screen {
    case wizard, home
}

@MainActor
final class AppModel: ObservableObject {
    @Published var screen: Screen
    @Published var state: LauncherState
    @Published private(set) var controllers: [String] = []
    @Published private(set) var wiiRemotes: [String] = []

    let settings = Settings()
    let installer = Installer()
    let runner = GameRunner()
    let cover = CoverArt()
    private var wiiRemoteWatcher: WiiRemoteWatcher?

    init() {
        let state = LauncherState.load()
        self.state = state
        screen = state.setupComplete && Paths.isGameInstalled ? .home : .wizard
        runner.onExit = { [weak self] in self?.settings.load() }
        cover.load()
        watchControllers()
    }

    func finishSetup(controls: ControlStyle) {
        state.setupComplete = true
        state.controlStyle = controls
        state.save()
        screen = .home
    }

    func restartSetup() {
        installer.reset()
        screen = .wizard
    }

    // MARK: Controllers

    var preferredControllerKind: ControllerKind {
        guard settings.controlStyle != .keyboardMouse else { return .keyboard }
        for controller in GCController.controllers() {
            let category = controller.productCategory.lowercased()
            if category.contains("dualshock") || category.contains("dualsense") { return .playstation }
            if category.contains("switch") || category.contains("pro controller") { return .switchPro }
            if category.contains("xbox") { return .xbox }
        }
        return settings.controlStyle == .controller ? .xbox : .keyboard
    }

    private func watchControllers() {
        refreshControllers()
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.refreshControllers() }
            }
        }
        // Wii Remotes are not Game Controller devices: IOKit tells when they come and go.
        wiiRemoteWatcher = WiiRemoteWatcher { [weak self] names in self?.wiiRemotes = names }
        center.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.wiiRemoteWatcher?.refresh() }
        }
    }

    private func refreshControllers() {
        controllers = GCController.controllers().map { $0.vendorName ?? $0.productCategory }
    }
}

struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        switch model.screen {
        case .wizard: WizardView()
        case .home: HomeView()
        }
    }
}
