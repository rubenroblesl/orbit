// SPDX-License-Identifier: GPL-3.0-only
//
// Where the launcher finds its bundled tools and keeps the user's installation. The app bundle
// stays read-only (and signed): everything made from the user's game lives in
// ~/Library/Application Support/Orbit, next to the settings the runtime already uses.

import Foundation

enum Paths {
    static let fileManager = FileManager.default

    // MARK: Bundle

    static var resources: URL { Bundle.main.resourceURL! }
    static var toolchain: URL { resources.appendingPathComponent("Toolchain", isDirectory: true) }
    static var recompiler: URL { toolchain.appendingPathComponent("bin/nebula-recomp") }
    static var cmake: URL { toolchain.appendingPathComponent("bin/cmake") }
    static var ninja: URL { toolchain.appendingPathComponent("bin/ninja") }
    static var sdk: URL { resources.appendingPathComponent("SDK", isDirectory: true) }
    static var sdkInclude: URL { sdk.appendingPathComponent("include", isDirectory: true) }
    static func sdkLibrary(_ name: String) -> URL { sdk.appendingPathComponent("lib/\(name)") }
    static var bundledTextures: URL { resources.appendingPathComponent("Textures", isDirectory: true) }
    static var hasBundledTextures: Bool {
        fileManager.fileExists(atPath: bundledTextures.appendingPathComponent("RMG").path) ||
            fileManager.fileExists(atPath: bundledTextures.appendingPathComponent("RMGE01").path)
    }
    static var gameApp: URL {
        Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/Orbit Game.app", isDirectory: true)
    }
    static var runtimeExecutable: URL { gameApp.appendingPathComponent("Contents/MacOS/Orbit") }

    // MARK: User data

    // The runtime's LOCALAPPDATA: Application Support unless the environment names another folder
    // (the game started by the launcher inherits it, so both agree).
    static var appData: URL {
        if let value = ProcessInfo.processInfo.environment["LOCALAPPDATA"], !value.isEmpty {
            return URL(fileURLWithPath: value, isDirectory: true)
        }
        return fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
    }
    static var support: URL { appData.appendingPathComponent("Orbit", isDirectory: true) }

    // Builds before the rename kept everything in a "Nebula" folder; it becomes the Orbit folder.
    static func migrateLegacyFolder() {
        let legacy = appData.appendingPathComponent("Nebula", isDirectory: true)
        guard !fileManager.fileExists(atPath: support.path), fileManager.fileExists(atPath: legacy.path) else { return }
        try? fileManager.moveItem(at: legacy, to: support)
    }
    static var game: URL { support.appendingPathComponent("Game", isDirectory: true) }
    static var gameContent: URL { game.appendingPathComponent("content", isDirectory: true) }
    static var gameModule: URL { game.appendingPathComponent("RMGE01_game.dylib") }
    static var dspModule: URL { game.appendingPathComponent("RMGE01_dsp.dylib") }
    static var installManifest: URL { game.appendingPathComponent("install.json") }
    static var installWork: URL { support.appendingPathComponent("Install", isDirectory: true) }
    static var logs: URL { support.appendingPathComponent("Logs", isDirectory: true) }
    static var renderConfig: URL { support.appendingPathComponent("config.ini") }
    static var launcherState: URL { support.appendingPathComponent("launcher.json") }
    // The runtime's save location.
    static var saves: URL { appData.appendingPathComponent("SuperMarioGalaxy/nand", isDirectory: true) }

    static func ensure(_ directory: URL) throws {
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    static var isGameInstalled: Bool {
        let required = [gameModule, dspModule, game.appendingPathComponent("RMGE01_home_button.dylib"),
                        game.appendingPathComponent("RMGE01_boot_image.bin"), gameContent, installManifest]
        return required.allSatisfy { fileManager.fileExists(atPath: $0.path) }
    }
}
