// SPDX-License-Identifier: GPL-3.0-only
//
// The settings the launcher edits are the runtime's own files, shared with the in-game menu (F1):
//   config.ini            display and graphics (runtime/src/gx/render_config.cpp)
//   runtime_settings.ini  input mode, bindings, vibration, audio (runtime/src/runtime_settings.cpp)
// Both are `key=value` lines; keys the launcher does not know are kept as they are.

import AppKit
import Foundation

// MARK: - key=value files

struct KeyValueFile {
    private(set) var lines: [String] = []
    let url: URL

    init(url: URL) {
        self.url = url
        if let text = try? String(contentsOf: url, encoding: .utf8) {
            lines = text.split(separator: "\n", omittingEmptySubsequences: false).map {
                String($0).trimmingCharacters(in: CharacterSet(charactersIn: "\r"))
            }
            while lines.last == "" { lines.removeLast() }
        }
    }

    subscript(key: String) -> String? {
        get {
            for line in lines {
                guard let equals = line.firstIndex(of: "=") else { continue }
                if line[..<equals].trimmingCharacters(in: .whitespaces) == key {
                    return line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
                }
            }
            return nil
        }
        set {
            let index = lines.firstIndex { line in
                guard let equals = line.firstIndex(of: "=") else { return false }
                return line[..<equals].trimmingCharacters(in: .whitespaces) == key
            }
            if let newValue {
                if let index { lines[index] = "\(key)=\(newValue)" } else { lines.append("\(key)=\(newValue)") }
            } else if let index {
                lines.remove(at: index)
            }
        }
    }

    func int(_ key: String, _ fallback: Int) -> Int { self[key].flatMap { Int($0) } ?? fallback }
    func bool(_ key: String, _ fallback: Bool) -> Bool {
        guard let value = self[key]?.lowercased() else { return fallback }
        return value == "1" || value == "true" || value == "yes" || value == "on"
    }
    func double(_ key: String, _ fallback: Double) -> Double { self[key].flatMap { Double($0) } ?? fallback }

    func save() throws {
        try Paths.ensure(url.deletingLastPathComponent())
        let text = lines.joined(separator: "\n") + "\n"
        let temporary = url.appendingPathExtension("tmp")
        try text.write(to: temporary, atomically: false, encoding: .utf8)
        _ = try FileManager.default.replaceItemAt(url, withItemAt: temporary)
    }
}

// MARK: - Choices

enum ControlStyle: String, CaseIterable, Identifiable, Codable {
    case auto, keyboardMouse = "keyboard_mouse", controller, wiiRemote = "real_wiimote"
    var id: String { rawValue }
    var title: String {
        switch self {
        case .auto: return "Teclado, ratón y mandos"
        case .keyboardMouse: return "Teclado y ratón"
        case .controller: return "Mando"
        case .wiiRemote: return "Wii Remote"
        }
    }
    // Wii Remotes paired over Bluetooth reach the game through the runtime's IOKit HID bridge.
    static var wiiRemoteAvailable: Bool { true }
}

enum DisplayMode: Int, CaseIterable, Identifiable {
    case window = 0, fullscreen = 1
    var id: Int { rawValue }
    var title: String { self == .fullscreen ? "Pantalla completa" : "Ventana" }
}

enum AspectChoice: Int, CaseIterable, Identifiable {
    case auto = 0, fourThree, sixteenTen, sixteenNine, twentyOneNine, thirtyTwoNine
    var id: Int { rawValue }
    var title: String {
        ["Automática", "4:3 (original)", "16:10", "16:9", "21:9", "32:9"][rawValue]
    }
}

enum AntiAliasing: Int, CaseIterable, Identifiable {
    case off = 0, smaa, auto, fxaa
    var id: Int { rawValue }
    var title: String { ["Desactivado", "SMAA", "Automático", "FXAA"][rawValue] }
}

// MARK: - Settings model

@MainActor
final class Settings: ObservableObject {
    // config.ini
    @Published var displayMode: DisplayMode = .fullscreen
    @Published var windowWidth = 1280
    @Published var windowHeight = 720
    @Published var internalAuto = true
    @Published var internalScale = 3
    @Published var aspect: AspectChoice = .auto
    @Published var vsync = false
    @Published var antiAliasing: AntiAliasing = .off
    @Published var anisotropy = 1
    @Published var perPixelLighting = false
    @Published var drawDistance = 0
    @Published var customTextures = false
    @Published var prefetchTextures = true
    // runtime_settings.ini
    @Published var controlStyle: ControlStyle = .auto
    @Published var vibration = true
    @Published var masterVolume = 1.0
    @Published var musicVolume = 1.0
    @Published var effectsVolume = 1.0
    @Published var voiceVolume = 1.0

    init() { load() }

    // Everything the Settings window edits, to save after a change.
    var snapshot: [String] {
        [displayMode.rawValue, windowWidth, windowHeight, internalAuto, internalScale, aspect.rawValue, vsync,
         antiAliasing.rawValue, anisotropy, perPixelLighting, drawDistance, customTextures, prefetchTextures,
         controlStyle.rawValue, vibration, masterVolume, musicVolume, effectsVolume, voiceVolume]
            .map { String(describing: $0) }
    }

    private var pendingSave: Task<Void, Never>?

    // Saves shortly after the last change (a slider sends many).
    func scheduleSave() {
        pendingSave?.cancel()
        pendingSave = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            self?.save()
        }
    }

    func load() {
        let config = KeyValueFile(url: Paths.renderConfig)
        displayMode = config.int("display_mode", 1) == 0 ? .window : .fullscreen
        windowWidth = config.int("window_width", 1280)
        windowHeight = config.int("window_height", 720)
        internalAuto = config.bool("internal_follow_window", true)
        internalScale = max(1, config.int("efb_scale", 3))
        aspect = AspectChoice(rawValue: config.int("aspect", 0)) ?? .auto
        vsync = config.int("vsync_interval", 0) != 0
        antiAliasing = AntiAliasing(rawValue: config.int("aa", 0)) ?? .off
        anisotropy = config.int("anisotropic_filtering", 1)
        perPixelLighting = config.bool("per_pixel_lighting", false)
        drawDistance = config.int("draw_distance", 0)
        customTextures = config.bool("custom_textures", false)
        prefetchTextures = config.bool("prefetch_custom_textures", true)

        let runtime = KeyValueFile(url: Paths.runtimeSettings)
        controlStyle = ControlStyle(rawValue: runtime["input_mode"] ?? "auto") ?? .auto
        if controlStyle == .wiiRemote && !ControlStyle.wiiRemoteAvailable { controlStyle = .auto }
        vibration = runtime.bool("tune.xbox.vibration", true)
        masterVolume = runtime.double("audio_master_gain", 1.0)
        musicVolume = runtime.double("audio_music_gain", 1.0)
        effectsVolume = runtime.double("audio_sfx_gain", 1.0)
        voiceVolume = runtime.double("audio_voice_gain", 1.0)
    }

    func save() {
        var config = KeyValueFile(url: Paths.renderConfig)
        config["display_mode"] = String(displayMode.rawValue)
        config["window_width"] = String(windowWidth)
        config["window_height"] = String(windowHeight)
        config["internal_follow_window"] = internalAuto ? "1" : "0"
        config["efb_scale"] = String(internalScale)
        config["aspect"] = String(aspect.rawValue)
        config["vsync_interval"] = vsync ? "1" : "0"
        config["aa"] = String(antiAliasing.rawValue)
        config["anisotropic_filtering"] = String(anisotropy)
        config["per_pixel_lighting"] = perPixelLighting ? "1" : "0"
        config["draw_distance"] = String(drawDistance)
        config["custom_textures"] = customTextures ? "1" : "0"
        config["prefetch_custom_textures"] = prefetchTextures ? "1" : "0"
        config["custom_textures_dir"] = Settings.textureDirectory
        try? config.save()

        var runtime = KeyValueFile(url: Paths.runtimeSettings)
        // A file without controls_version is read as version 1 and migrated (keyboard_mouse
        // becomes auto), so a new file says it is current. Older files keep their own version.
        if runtime.lines.isEmpty { runtime["controls_version"] = "5" }
        runtime["input_mode"] = controlStyle.rawValue
        for scheme in ["xbox", "switch_pro", "switch_pro2"] {
            runtime["tune.\(scheme).vibration"] = vibration ? "1" : "0"
        }
        runtime["audio_master_gain"] = String(format: "%.2f", masterVolume)
        runtime["audio_music_gain"] = String(format: "%.2f", musicVolume)
        runtime["audio_sfx_gain"] = String(format: "%.2f", effectsVolume)
        runtime["audio_voice_gain"] = String(format: "%.2f", voiceVolume)
        try? runtime.save()
    }

    // What the first-run wizard sets up: full screen at the Mac's resolution, the picture shaped to
    // the display (the 2D aspect repairs come with the runtime's defaults) and the bundled HD
    // textures preloaded.
    func applyRecommended(controls: ControlStyle) {
        displayMode = .fullscreen
        internalAuto = true
        aspect = .auto
        vsync = false
        antiAliasing = .off
        anisotropy = 1
        perPixelLighting = false
        drawDistance = 0
        customTextures = Settings.texturesAvailable
        prefetchTextures = true
        controlStyle = controls
        vibration = true
        save()
    }

    // The bundled pack, else the user's own folder (Dolphin's layout).
    static var textureDirectory: String {
        if Paths.hasBundledTextures { return Paths.bundledTextures.path }
        return Paths.support.appendingPathComponent("Load/Textures").path
    }

    static var texturesAvailable: Bool {
        let fm = FileManager.default
        let root = URL(fileURLWithPath: textureDirectory)
        return fm.fileExists(atPath: root.appendingPathComponent("RMG").path) ||
            fm.fileExists(atPath: root.appendingPathComponent("RMGE01").path)
    }

    // The largest internal scale worth offering: enough to cover the main display.
    static var maxInternalScale: Int {
        let screen = NSScreen.main?.frame.size ?? CGSize(width: 1920, height: 1080)
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        let width = screen.width * scale
        let height = screen.height * scale
        return min(8, max(1, Int(ceil(max(width / 640, height / 456)))))
    }

    static var windowSizes: [(Int, Int)] {
        let screen = NSScreen.main?.visibleFrame.size ?? CGSize(width: 1920, height: 1080)
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        let candidates = [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3200, 1800), (3840, 2160)]
        return candidates.filter { Double($0.0) <= screen.width * scale && Double($0.1) <= screen.height * scale }
    }

    // Before every launch: the bundled pack's location follows the app if it was moved.
    static func prepareForLaunch() throws {
        var config = KeyValueFile(url: Paths.renderConfig)
        config["custom_textures_dir"] = textureDirectory
        if config["custom_textures"] == "1" && !texturesAvailable { config["custom_textures"] = "0" }
        try config.save()
    }
}

extension Paths {
    static var runtimeSettings: URL { support.appendingPathComponent("runtime_settings.ini") }
}

// MARK: - Launcher state

struct LauncherState: Codable {
    var setupComplete = false
    var controlStyle: ControlStyle = .auto

    static func load() -> LauncherState {
        guard let data = try? Data(contentsOf: Paths.launcherState),
              let state = try? JSONDecoder().decode(LauncherState.self, from: data) else { return LauncherState() }
        return state
    }

    func save() {
        try? Paths.ensure(Paths.support)
        if let data = try? JSONEncoder().encode(self) { try? data.write(to: Paths.launcherState) }
    }
}
