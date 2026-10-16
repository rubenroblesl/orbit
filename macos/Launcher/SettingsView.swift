// SPDX-License-Identifier: GPL-3.0-only
//
// The Settings window (Orbit › Settings…, ⌘,): Display, Graphics, Sound, Controls, Game.
// Changes are written to the runtime's own files right away; the in-game menu (F1) edits the
// same files.

import AppKit
import SwiftUI

enum SettingsTab: String, CaseIterable, Identifiable {
    case display, graphics, sound, controls, game
    var id: String { rawValue }
    var title: String {
        switch self {
        case .display: return L("Display")
        case .graphics: return L("Graphics")
        case .sound: return L("Sound")
        case .controls: return L("Controls")
        case .game: return L("Game")
        }
    }
    var symbol: String {
        switch self {
        case .display: return "display"
        case .graphics: return "photo"
        case .sound: return "speaker.wave.2"
        case .controls: return "gamecontroller"
        case .game: return "internaldrive"
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject private var settings: Settings
    @State private var tab: SettingsTab = .display

    var body: some View {
        TabView(selection: $tab) {
            ForEach(SettingsTab.allCases) { item in
                SettingsTabContent(tab: item)
                    .tabItem { Label(item.title, systemImage: item.symbol) }
                    .tag(item)
            }
        }
        .frame(width: 540)
        .onChange(of: settings.snapshot) { settings.scheduleSave() }
    }
}

struct SettingsTabContent: View {
    let tab: SettingsTab

    var body: some View {
        Group {
            switch tab {
            case .display: DisplaySettings()
            case .graphics: GraphicsSettings()
            case .sound: SoundSettings()
            case .controls: ControlSettings()
            case .game: GameSettings()
            }
        }
        .formStyle(.grouped)
        .frame(width: 540, height: 400)
    }
}

// MARK: - Display

private struct DisplaySettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                Picker("Mode", selection: $settings.displayMode) {
                    ForEach(DisplayMode.allCases) { Text($0.title).tag($0) }
                }
                if settings.displayMode == .window {
                    Picker("Window size", selection: windowSize) {
                        ForEach(Settings.windowSizes, id: \.0) { Text(verbatim: "\($0.0) × \($0.1)").tag("\($0.0)x\($0.1)") }
                        if !Settings.windowSizes.contains(where: { "\($0.0)x\($0.1)" == windowSize.wrappedValue }) {
                            Text(verbatim: "\(settings.windowWidth) × \(settings.windowHeight)").tag(windowSize.wrappedValue)
                        }
                    }
                }
            }
            Section {
                Picker("Internal resolution", selection: resolution) {
                    Text("Automatic").tag(0)
                    Divider()
                    ForEach(1...Settings.maxInternalScale, id: \.self) { Text(scaleTitle($0)).tag($0) }
                }
                Picker("Aspect ratio", selection: $settings.aspect) {
                    ForEach(AspectChoice.allCases) { Text($0.title).tag($0) }
                }
            } footer: {
                Text("With automatic resolution, the game renders at the resolution of the window. Automatic aspect ratio follows the shape of the display.")
                    .foregroundStyle(.secondary)
            }
            Section {
                Toggle("Vertical sync", isOn: $settings.vsync)
            } footer: {
                Text("Prevents screen tearing. It can add some latency.").foregroundStyle(.secondary)
            }
        }
    }

    private var windowSize: Binding<String> {
        Binding {
            "\(settings.windowWidth)x\(settings.windowHeight)"
        } set: { value in
            let parts = value.split(separator: "x").compactMap { Int($0) }
            if parts.count == 2 { settings.windowWidth = parts[0]; settings.windowHeight = parts[1] }
        }
    }

    private var resolution: Binding<Int> {
        Binding {
            settings.internalAuto ? 0 : min(settings.internalScale, Settings.maxInternalScale)
        } set: { value in
            settings.internalAuto = value == 0
            if value != 0 { settings.internalScale = value }
        }
    }

    private func scaleTitle(_ scale: Int) -> String {
        let lines = 528 * scale
        return scale == 1 ? "1× (original, 640 × 528)" : "\(scale)× (\(640 * scale) × \(lines))"
    }
}

// MARK: - Graphics

private struct GraphicsSettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                Toggle("HD textures", isOn: $settings.customTextures)
                    .disabled(!Settings.texturesAvailable)
                Toggle("Preload textures", isOn: $settings.prefetchTextures)
                    .disabled(!settings.customTextures)
            } footer: {
                Text(Settings.texturesAvailable
                     ? L("HD textures by CDAMJC and Razius and HD HUD by Super Hackio and SY24, included with credit and with thanks. Preloading reads the whole pack when the game starts to avoid stutter and uses about 2.5 GB of memory.")
                     : L("No texture pack is installed."))
                    .foregroundStyle(.secondary)
            }
            Section {
                Picker("Anti-aliasing", selection: $settings.antiAliasing) {
                    ForEach(AntiAliasing.allCases) { Text($0.title).tag($0) }
                }
                Picker("Anisotropic filtering", selection: $settings.anisotropy) {
                    Text("Off").tag(1)
                    ForEach([2, 4, 8, 16], id: \.self) { Text(verbatim: "\($0)×").tag($0) }
                }
                Toggle("Per-pixel lighting", isOn: $settings.perPixelLighting)
                Picker("Draw distance", selection: $settings.drawDistance) {
                    Text("Original").tag(0)
                    Text(verbatim: "2×").tag(1)
                    Text(verbatim: "4×").tag(2)
                    Text(verbatim: "8×").tag(3)
                }
            } footer: {
                Text("Per-pixel lighting and a longer draw distance change the original look slightly.")
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Sound

private struct SoundSettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                volume(L("Master volume"), $settings.masterVolume)
                volume(L("Music"), $settings.musicVolume)
                volume(L("Effects"), $settings.effectsVolume)
                volume(L("Voices"), $settings.voiceVolume)
            } footer: {
                Text("You can also adjust the volume during play in the game menu (F1).").foregroundStyle(.secondary)
            }
        }
    }

    private func volume(_ title: String, _ value: Binding<Double>) -> some View {
        LabeledContent(title) {
            HStack {
                Slider(value: value, in: 0...2).frame(width: 220)
                Text(value.wrappedValue, format: .percent.precision(.fractionLength(0)))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .frame(width: 46, alignment: .trailing)
            }
        }
    }
}

// MARK: - Controls

private struct ControlSettings: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @State private var showingControls = false
    @State private var showingWiiHelp = false

    var body: some View {
        Form {
            Section {
                Picker("Control method", selection: $settings.controlStyle) {
                    Text(ControlStyle.keyboardMouse.title).tag(ControlStyle.keyboardMouse)
                    Text(ControlStyle.controller.title).tag(ControlStyle.controller)
                    Text(ControlStyle.auto.title).tag(ControlStyle.auto)
                    Text(ControlStyle.wiiRemote.title).tag(ControlStyle.wiiRemote)
                        .selectionDisabled(!ControlStyle.wiiRemoteAvailable)
                }
            } footer: {
                Text(help).foregroundStyle(.secondary)
            }
            if settings.controlStyle == .wiiRemote {
                Section {
                    LabeledContent("Connected Wii Remote",
                                   value: model.wiiRemotes.isEmpty ? L("None") : model.wiiRemotes.joined(separator: ", "))
                    LabeledContent("Pairing") {
                        Button("How to connect…") { showingWiiHelp = true }
                            .popover(isPresented: $showingWiiHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
                    }
                } footer: {
                    Text(WiiRemoteHelp.sensorBar).foregroundStyle(.secondary)
                }
            }
            Section {
                LabeledContent("Connected controllers",
                               value: model.controllers.isEmpty ? L("None") : model.controllers.joined(separator: ", "))
                Toggle("Vibration", isOn: $settings.vibration)
                LabeledContent("Bluetooth") {
                    Button("Open Bluetooth settings…") {
                        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")!)
                    }
                }
            }
            Section {
                LabeledContent("Button mapping") {
                    Button("Show controls…") { showingControls = true }
                }
            }
        }
        .sheet(isPresented: $showingControls) {
            ControlsSheet(initial: model.preferredControllerKind)
        }
    }

    private var help: String {
        switch settings.controlStyle {
        case .keyboardMouse: return L("Only the keyboard and mouse are used. Controllers are ignored.")
        case .controller: return L("Only the controller is used. The mouse and trackpad do not move the pointer.")
        case .auto: return L("The keyboard, mouse and controllers work at the same time.")
        case .wiiRemote: return L("Only the Wii Remote and Nunchuk, paired over Bluetooth, are used. The keyboard, mouse and controllers are ignored.")
        }
    }
}

// MARK: - Game

private struct GameSettings: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @Environment(\.openWindow) private var openWindow
    @State private var repairing = false
    @State private var confirmRestore = false
    @AppStorage(CoverArt.downloadsKey) private var downloadCovers = true

    var body: some View {
        Form {
            Section {
                LabeledContent("Game", value: L("Super Mario Galaxy, USA (RMGE01)"))
                LabeledContent("Source", value: installedSource)
                LabeledContent("Location") {
                    Button("Show in Finder") { reveal(Paths.game) }
                }
            }
            Section {
                Toggle("Download box art from GameTDB", isOn: $downloadCovers)
                    .onChange(of: downloadCovers) { model.cover.refresh() }
            } footer: {
                Text("As in Dolphin, the box art is downloaded from GameTDB.com and kept only on this Mac. To use another picture, place it at Load/GameCovers/RMGE01.png in the Orbit folder.")
                    .foregroundStyle(.secondary)
            }
            Section {
                LabeledContent("Change game image") {
                    Button("Change…") {
                        NSApp.keyWindow?.close()
                        model.restartSetup()
                        openWindow(id: "main")
                    }
                }
                LabeledContent("Repair installation") {
                    Button("Repair…") { repairing = true }
                }
            } footer: {
                Text("Repairing compiles the game again from the installed data. It does not need the disc image.")
                    .foregroundStyle(.secondary)
            }
            Section {
                LabeledContent("Saved games") {
                    Button("Show in Finder") { reveal(Paths.saves) }
                }
                LabeledContent("Logs") {
                    Button("Show in Finder") { reveal(Paths.logs) }
                }
                LabeledContent("Recommended settings") {
                    Button("Restore…") { confirmRestore = true }
                }
            } footer: {
                Text(verbatim: "Orbit \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "")")
                    .foregroundStyle(.secondary)
            }
        }
        .confirmationDialog("Restore the recommended settings?", isPresented: $confirmRestore) {
            Button("Restore") {
                settings.applyRecommended(controls: settings.controlStyle)
                settings.masterVolume = 1
                settings.musicVolume = 1
                settings.effectsVolume = 1
                settings.voiceVolume = 1
                settings.save()
            }
        } message: {
            Text("Display, graphics and sound settings are reset. Saved games and button mapping do not change.")
        }
        .sheet(isPresented: $repairing) {
            RepairSheet().environmentObject(model.installer)
        }
    }

    private var installedSource: String {
        guard let data = try? Data(contentsOf: Paths.installManifest),
              let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let source = manifest["source"] as? String, !source.isEmpty else { return L("Unknown") }
        return URL(fileURLWithPath: source).lastPathComponent
    }

    private func reveal(_ url: URL) {
        try? Paths.ensure(url)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}

// Rebuilding the game modules from the installed game files.
struct RepairSheet: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(installer.finished ? L("Repair complete")
                 : installer.failure != nil ? L("The installation could not be repaired") : L("Repairing the installation"))
                .font(.headline)
            if let failure = installer.failure {
                StatusLine(kind: .failure, text: failure.errorDescription ?? "")
                    .textSelection(.enabled)
            } else if !installer.finished {
                ProgressView(value: installer.overallProgress)
                Text("Step \(installer.step.rawValue + 1) of \(InstallStep.allCases.count): \(installer.step.title)")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                Text("The game was compiled again successfully.").foregroundStyle(.secondary)
            }
            HStack {
                Spacer()
                if installer.running {
                    Button("Cancel") { installer.cancel() }
                } else {
                    Button("Close") { installer.reset(); dismiss() }.keyboardShortcut(.defaultAction)
                }
            }
        }
        .padding(20)
        .frame(width: 420)
        .onAppear {
            installer.reset()
            installer.start(image: nil)
        }
    }
}
