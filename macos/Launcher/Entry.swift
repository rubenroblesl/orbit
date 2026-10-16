// SPDX-License-Identifier: GPL-3.0-only
//
// Starts the launcher window, or, from Terminal, one of the windowless modes used to test it:
//   Orbit --install <disc image or folder>   install (or repair, without a path) and print progress
//   Orbit --setup <keyboard|controller|wiimote>  apply the recommended settings and finish the wizard
//   Orbit --play                             start the game as the Play button does and wait for it
//   Orbit --snapshots <folder>               render every screen to PNG files without showing them
// LOCALAPPDATA selects another data folder, as it does for the game.

import AppKit
import SwiftUI

@main
enum Entry {
    static func main() {
        Paths.migrateLegacyFolder()
        let arguments = Array(CommandLine.arguments.dropFirst())
        switch arguments.first {
        case "--install":
            MainActor.assumeIsolated { exit(Headless.install(arguments.count > 1 ? URL(fileURLWithPath: arguments[1]) : nil)) }
        case "--setup":
            MainActor.assumeIsolated {
                let choice = arguments.count > 1 ? arguments[1] : "keyboard"
                let controls: ControlStyle = choice == "controller" ? .controller : choice == "wiimote" ? .wiiRemote : .keyboardMouse
                Settings().applyRecommended(controls: controls)
                var state = LauncherState.load()
                state.setupComplete = true
                state.controlStyle = controls
                state.save()
                print("settings: \(Paths.renderConfig.path)\n          \(Paths.runtimeSettings.path)")
                exit(0)
            }
        case "--play":
            MainActor.assumeIsolated {
                _ = NSApplication.shared
                let runner = GameRunner()
                runner.play()
                guard runner.running else { fputs("error: \(runner.lastExitMessage ?? "")\n", stderr); exit(1) }
                print("game started; log: \(Paths.logs.appendingPathComponent("game.log").path)")
                while runner.running { RunLoop.main.run(until: Date().addingTimeInterval(0.5)) }
                print(runner.lastExitMessage ?? "game closed normally")
                exit(0)
            }
        case "--snapshots":
            guard arguments.count > 1 else { fputs("usage: Orbit --snapshots <folder>\n", stderr); exit(2) }
            MainActor.assumeIsolated { exit(Snapshots.render(to: URL(fileURLWithPath: arguments[1]))) }
        default:
            NebulaApp.main()
        }
    }
}

@MainActor
enum Headless {
    static func install(_ image: URL?) -> Int32 {
        let installer = Installer()
        installer.start(image: image)
        var last = ""
        while installer.running {
            RunLoop.main.run(until: Date().addingTimeInterval(0.5))
            let line = String(format: "%3.0f%%  %@", installer.overallProgress * 100, installer.step.title)
            if line != last {
                print(line)
                fflush(stdout)
                last = line
            }
        }
        if let failure = installer.failure {
            fputs("error: \(failure.errorDescription ?? "")\n", stderr)
            return 1
        }
        print("installed: \(Paths.game.path)")
        return 0
    }
}

@MainActor
enum Snapshots {
    static func render(to folder: URL) -> Int32 {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        try? Paths.ensure(folder)
        let model = AppModel()
        if CommandLine.arguments.contains("--recommended") { model.settings.applyRecommended(controls: .keyboardMouse) }
        // The cover arrives from GameTDB (or the cache) on its own; wait for it a few seconds.
        let coverDeadline = Date().addingTimeInterval(15)
        while model.cover.image == nil && Date() < coverDeadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }
        let image = URL(fileURLWithPath: "/Users/me/Games/Super Mario Galaxy.wbfs")
        let good = Installer.Identification(supported: true, summary: "RMGE01 SUPER MARIO GALAXY revision 0 (WBFS)")
        let bad = Installer.Identification(supported: false, summary: "RMGP01 SUPER MARIO GALAXY revision 0 (ISO)")

        func shot<V: View>(_ name: String, _ view: V, size: CGSize = CGSize(width: 700, height: 470),
                           dark: Bool = false) {
            let root = view
                .environmentObject(model)
                .environmentObject(model.settings)
                .environmentObject(model.installer)
                .environmentObject(model.runner)
                .frame(width: size.width, height: size.height)
                .background(Color(nsColor: .windowBackgroundColor))
            let host = NSHostingView(rootView: root)
            host.frame = CGRect(origin: .zero, size: size)
            let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
            window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            window.contentView = host
            host.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.6))
            host.layoutSubtreeIfNeeded()
            guard let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return }
            host.cacheDisplay(in: host.bounds, to: bitmap)
            let url = folder.appendingPathComponent("\(name)\(dark ? "-oscuro" : "").png")
            try? bitmap.representation(using: .png, properties: [:])?.write(to: url)
            print(url.path)
            window.close()
        }

        for dark in [false, true] {
            shot("01-introduccion", WizardView(page: .welcome), dark: dark)
            shot("02-requisitos", WizardView(page: .tools), dark: dark)
            shot("03-juego", WizardView(page: .game), dark: dark)
            shot("04-juego-correcto", WizardView(page: .game, image: image, identification: good), dark: dark)
            shot("05-juego-incorrecto", WizardView(page: .game, image: image, identification: bad), dark: dark)
            shot("06-controles", WizardView(page: .controls, controls: .keyboardMouse), dark: dark)
            shot("06-controles-wii", WizardView(page: .controls, controls: .wiiRemote), dark: dark)
            shot("07-resumen", WizardView(page: .ready, image: image, identification: good, controls: .keyboardMouse), dark: dark)
            model.installer.preview(step: .compile, progress: 0.42, detail: "[331/786] Building CXX object game/CMakeFiles/RMGE01_game.dir/functions_0212.cpp.o")
            shot("08-instalando", WizardView(page: .installing, image: image, identification: good), dark: dark)
            model.installer.reset()
            shot("09-completada", WizardView(page: .done, controls: .keyboardMouse), dark: dark)
            shot("10-principal", HomeView(), dark: dark)
            for tab in SettingsTab.allCases {
                shot("11-ajustes-\(tab.rawValue.lowercased())", SettingsTabContent(tab: tab),
                     size: CGSize(width: 540, height: 400), dark: dark)
            }
            // The Wii Remote's settings and help, shown without saving the choice.
            let style = model.settings.controlStyle
            model.settings.controlStyle = .wiiRemote
            shot("11-ajustes-controles-wii", SettingsTabContent(tab: .controls), size: CGSize(width: 540, height: 400),
                 dark: dark)
            shot("10-principal-wii", HomeView(), dark: dark)
            model.settings.controlStyle = style
            shot("13-ayuda-wii", WiiRemotePairingHelp(), size: CGSize(width: 380, height: 290), dark: dark)
            for kind in ControllerKind.allCases {
                shot("12-controles-\(kind.rawValue)", ControlsSheet(initial: kind), size: CGSize(width: 700, height: 520), dark: dark)
            }
        }
        return 0
    }
}
