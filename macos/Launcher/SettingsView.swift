// SPDX-License-Identifier: GPL-3.0-only
//
// The Settings window (Orbit › Ajustes…, ⌘,): Pantalla, Gráficos, Sonido, Controles, Juego.
// Changes are written to the runtime's own files right away; the in-game menu (F1) edits the
// same files.

import AppKit
import SwiftUI

enum SettingsTab: String, CaseIterable, Identifiable {
    case display = "Pantalla", graphics = "Gráficos", sound = "Sonido", controls = "Controles", game = "Juego"
    var id: String { rawValue }
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
                    .tabItem { Label(item.rawValue, systemImage: item.symbol) }
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

// MARK: - Pantalla

private struct DisplaySettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                Picker("Modo", selection: $settings.displayMode) {
                    ForEach(DisplayMode.allCases) { Text($0.title).tag($0) }
                }
                if settings.displayMode == .window {
                    Picker("Tamaño de la ventana", selection: windowSize) {
                        ForEach(Settings.windowSizes, id: \.0) { Text("\($0.0) × \($0.1)").tag("\($0.0)x\($0.1)") }
                        if !Settings.windowSizes.contains(where: { "\($0.0)x\($0.1)" == windowSize.wrappedValue }) {
                            Text("\(settings.windowWidth) × \(settings.windowHeight)").tag(windowSize.wrappedValue)
                        }
                    }
                }
            }
            Section {
                Picker("Resolución interna", selection: resolution) {
                    Text("Automática").tag(0)
                    Divider()
                    ForEach(1...Settings.maxInternalScale, id: \.self) { Text(scaleTitle($0)).tag($0) }
                }
                Picker("Proporción", selection: $settings.aspect) {
                    ForEach(AspectChoice.allCases) { Text($0.title).tag($0) }
                }
            } footer: {
                Text("Con la resolución automática el juego se dibuja a la resolución de la ventana. La proporción automática se adapta a la forma de la pantalla.")
                    .foregroundStyle(.secondary)
            }
            Section {
                Toggle("Sincronización vertical", isOn: $settings.vsync)
            } footer: {
                Text("Evita el desgarro de imagen. Puede añadir algo de latencia.").foregroundStyle(.secondary)
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

// MARK: - Gráficos

private struct GraphicsSettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                Toggle("Texturas HD", isOn: $settings.customTextures)
                    .disabled(!Settings.texturesAvailable)
                Toggle("Precargar texturas", isOn: $settings.prefetchTextures)
                    .disabled(!settings.customTextures)
            } footer: {
                Text(Settings.texturesAvailable
                     ? "Texturas HD de CDAMJC y Razius y HUD en HD de Super Hackio y SY24, incluidos con su crédito y con agradecimiento. La precarga lee el paquete entero al iniciar el juego para evitar tirones y usa unos 2,5 GB de memoria."
                     : "No hay ningún paquete de texturas instalado.")
                    .foregroundStyle(.secondary)
            }
            Section {
                Picker("Suavizado de bordes", selection: $settings.antiAliasing) {
                    ForEach(AntiAliasing.allCases) { Text($0.title).tag($0) }
                }
                Picker("Filtrado anisótropo", selection: $settings.anisotropy) {
                    Text("Desactivado").tag(1)
                    ForEach([2, 4, 8, 16], id: \.self) { Text("\($0)×").tag($0) }
                }
                Toggle("Iluminación por píxel", isOn: $settings.perPixelLighting)
                Picker("Distancia de dibujado", selection: $settings.drawDistance) {
                    Text("Original").tag(0)
                    Text("2×").tag(1)
                    Text("4×").tag(2)
                    Text("8×").tag(3)
                }
            } footer: {
                Text("La iluminación por píxel y la distancia de dibujado alteran ligeramente el aspecto original.")
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Sonido

private struct SoundSettings: View {
    @EnvironmentObject private var settings: Settings

    var body: some View {
        Form {
            Section {
                volume("General", $settings.masterVolume)
                volume("Música", $settings.musicVolume)
                volume("Efectos", $settings.effectsVolume)
                volume("Voces", $settings.voiceVolume)
            } footer: {
                Text("También se puede ajustar durante la partida en el menú del juego (F1).").foregroundStyle(.secondary)
            }
        }
    }

    private func volume(_ title: String, _ value: Binding<Double>) -> some View {
        LabeledContent(title) {
            HStack {
                Slider(value: value, in: 0...2).frame(width: 220)
                Text("\(Int((value.wrappedValue * 100).rounded())) %")
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .frame(width: 46, alignment: .trailing)
            }
        }
    }
}

// MARK: - Controles

private struct ControlSettings: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @State private var showingControls = false
    @State private var showingWiiHelp = false

    var body: some View {
        Form {
            Section {
                Picker("Método de control", selection: $settings.controlStyle) {
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
                    LabeledContent("Wii Remote conectado",
                                   value: model.wiiRemotes.isEmpty ? "Ninguno" : model.wiiRemotes.joined(separator: ", "))
                    LabeledContent("Emparejar") {
                        Button("Cómo conectarlo…") { showingWiiHelp = true }
                            .popover(isPresented: $showingWiiHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
                    }
                } footer: {
                    Text(WiiRemoteHelp.sensorBar).foregroundStyle(.secondary)
                }
            }
            Section {
                LabeledContent("Mandos conectados",
                               value: model.controllers.isEmpty ? "Ninguno" : model.controllers.joined(separator: ", "))
                Toggle("Vibración", isOn: $settings.vibration)
                LabeledContent("Bluetooth") {
                    Button("Abrir ajustes de Bluetooth…") {
                        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")!)
                    }
                }
            }
            Section {
                LabeledContent("Asignación de botones") {
                    Button("Mostrar controles…") { showingControls = true }
                }
            }
        }
        .sheet(isPresented: $showingControls) {
            ControlsSheet(initial: model.preferredControllerKind)
        }
    }

    private var help: String {
        switch settings.controlStyle {
        case .keyboardMouse: return "Usa solo el teclado y el ratón. Los mandos se ignoran."
        case .controller: return "Usa solo el mando. El ratón y el trackpad no mueven el puntero."
        case .auto: return "Teclado, ratón y mandos funcionan a la vez."
        case .wiiRemote: return "Usa solo el Wii Remote y el Nunchuk, emparejados por Bluetooth. El teclado, el ratón y los mandos se ignoran."
        }
    }
}

// MARK: - Juego

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
                LabeledContent("Juego", value: "Super Mario Galaxy, EE. UU. (RMGE01)")
                LabeledContent("Origen", value: installedSource)
                LabeledContent("Ubicación") {
                    Button("Mostrar en el Finder") { reveal(Paths.game) }
                }
            }
            Section {
                Toggle("Descargar la portada de GameTDB", isOn: $downloadCovers)
                    .onChange(of: downloadCovers) { model.cover.refresh() }
            } footer: {
                Text("Como en Dolphin, la portada se descarga de GameTDB.com y se guarda solo en este Mac. Para usar otra imagen, colócala en Load/GameCovers/RMGE01.png dentro de la carpeta de Orbit.")
                    .foregroundStyle(.secondary)
            }
            Section {
                LabeledContent("Cambiar imagen del juego") {
                    Button("Cambiar…") {
                        NSApp.keyWindow?.close()
                        model.restartSetup()
                        openWindow(id: "main")
                    }
                }
                LabeledContent("Reparar instalación") {
                    Button("Reparar…") { repairing = true }
                }
            } footer: {
                Text("La reparación vuelve a compilar el juego con los datos ya instalados, sin pedir la imagen.")
                    .foregroundStyle(.secondary)
            }
            Section {
                LabeledContent("Partidas guardadas") {
                    Button("Mostrar en el Finder") { reveal(Paths.saves) }
                }
                LabeledContent("Registros") {
                    Button("Mostrar en el Finder") { reveal(Paths.logs) }
                }
                LabeledContent("Ajustes recomendados") {
                    Button("Restaurar…") { confirmRestore = true }
                }
            } footer: {
                Text("Orbit \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "")")
                    .foregroundStyle(.secondary)
            }
        }
        .confirmationDialog("¿Restaurar los ajustes recomendados?", isPresented: $confirmRestore) {
            Button("Restaurar") {
                settings.applyRecommended(controls: settings.controlStyle)
                settings.masterVolume = 1
                settings.musicVolume = 1
                settings.effectsVolume = 1
                settings.voiceVolume = 1
                settings.save()
            }
        } message: {
            Text("Se restablecen la pantalla, los gráficos y el sonido. Las partidas y la asignación de botones no cambian.")
        }
        .sheet(isPresented: $repairing) {
            RepairSheet().environmentObject(model.installer)
        }
    }

    private var installedSource: String {
        guard let data = try? Data(contentsOf: Paths.installManifest),
              let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let source = manifest["source"] as? String, !source.isEmpty else { return "Desconocido" }
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
            Text(installer.finished ? "Reparación completada"
                 : installer.failure != nil ? "No se pudo reparar la instalación" : "Reparando la instalación")
                .font(.headline)
            if let failure = installer.failure {
                StatusLine(kind: .failure, text: failure.errorDescription ?? "")
                    .textSelection(.enabled)
            } else if !installer.finished {
                ProgressView(value: installer.overallProgress)
                Text("Paso \(installer.step.rawValue + 1) de \(InstallStep.allCases.count): \(installer.step.title)")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                Text("El juego se ha vuelto a compilar correctamente.").foregroundStyle(.secondary)
            }
            HStack {
                Spacer()
                if installer.running {
                    Button("Cancelar") { installer.cancel() }
                } else {
                    Button("Cerrar") { installer.reset(); dismiss() }.keyboardShortcut(.defaultAction)
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
