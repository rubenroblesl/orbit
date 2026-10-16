// SPDX-License-Identifier: GPL-3.0-only
//
// Starts the game (the runtime inside Contents/Helpers/Orbit Game.app) with the installed
// content and modules, hides the launcher while it runs, and brings it back when the game ends.

import AppKit
import Foundation

@MainActor
final class GameRunner: ObservableObject {
    @Published private(set) var running = false
    @Published private(set) var lastExitMessage: String?

    // Runs when the game closes (the launcher reloads what the in-game menu may have changed).
    var onExit: (() -> Void)?

    private var process: Process?
    private var logURL: URL?

    func play() {
        guard !running else { return }
        lastExitMessage = nil
        do {
            try Settings.prepareForLaunch()
            try Paths.ensure(Paths.logs)
        } catch {
            lastExitMessage = L("The settings could not be saved: \(error.localizedDescription)")
            return
        }
        let log = Paths.logs.appendingPathComponent("game.log")
        rotate(log)
        FileManager.default.createFile(atPath: log.path, contents: nil)
        let output = try? FileHandle(forWritingTo: log)

        let process = Process()
        process.executableURL = Paths.runtimeExecutable
        process.arguments = [Paths.gameContent.path, Paths.gameModule.path]
        var environment = ProcessInfo.processInfo.environment
        environment["GALAXY_DSP_MODULE_PATH"] = Paths.dspModule.path
        environment["GALAXY_SKIP_LAUNCHER"] = "1"
        // The files the launcher edits, whatever the runtime would pick by itself (the in-game menu
        // saves to them too).
        environment["GALAXY_RENDER_CONFIG_PATH"] = Paths.renderConfig.path
        environment["GALAXY_RUNTIME_SETTINGS_PATH"] = Paths.runtimeSettings.path
        process.environment = environment
        process.currentDirectoryURL = Paths.game
        process.standardOutput = output
        process.standardError = output
        process.terminationHandler = { [weak self] finished in
            let status = finished.terminationStatus
            let reason = finished.terminationReason
            Task { @MainActor in self?.ended(status: status, reason: reason) }
        }
        do {
            try process.run()
        } catch {
            lastExitMessage = L("The game could not be opened: \(error.localizedDescription)")
            return
        }
        self.process = process
        logURL = log
        running = true
        // Out of the way while playing: no window, no Dock icon.
        if !NSApp.windows.isEmpty {
            NSApp.windows.forEach { $0.orderOut(nil) }
            NSApp.setActivationPolicy(.accessory)
        }
    }

    private func ended(status: Int32, reason: Process.TerminationReason) {
        running = false
        process = nil
        if !NSApp.windows.isEmpty {
            NSApp.setActivationPolicy(.regular)
            (NSApp.windows.first { $0.canBecomeMain } ?? NSApp.windows.first)?.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
        onExit?()
        if reason == .uncaughtSignal || status != 0 {
            lastExitMessage = wiiRemoteExitMessage() ??
                L("The game quit unexpectedly (code \(Int(status))). The log is in Settings › Game.")
        }
    }

    // The runtime's Wii Remote failures (runtime/platform/macos/src/wiimote_hid_macos.cpp), from
    // the end of the game's log.
    private func wiiRemoteExitMessage() -> String? {
        guard let logURL, let handle = try? FileHandle(forReadingFrom: logURL) else { return nil }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        try? handle.seek(toOffset: size > 65536 ? size - 65536 : 0)
        guard let data = try? handle.readToEnd(), let tail = String(data: data, encoding: .utf8) else { return nil }
        if tail.contains("no Wii Remote is connected") {
            return L("The game quit because no Wii Remote is connected. Pair the Wii Remote in System Settings › Bluetooth and select Play again.")
        }
        if tail.contains("real Wii Remote HID") || tail.contains("real Wiimote HID") {
            return L("The game quit because of a problem with the Wii Remote. The log is in Settings › Game.")
        }
        return nil
    }

    private func rotate(_ log: URL) {
        let fm = FileManager.default
        let previous = log.deletingPathExtension().appendingPathExtension("previous.log")
        try? fm.removeItem(at: previous)
        try? fm.moveItem(at: log, to: previous)
    }
}
