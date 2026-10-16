// SPDX-License-Identifier: GPL-3.0-only
//
// The main window once the game is installed: the Play button, the current setup in one line,
// and the way to the button map and to Settings.

import AppKit
import SwiftUI

struct HomeView: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var runner: GameRunner

    @State private var showingControls = false
    @State private var showingWiiHelp = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .center, spacing: 36) {
                CoverView(cover: model.cover, height: 270)
                VStack(alignment: .leading, spacing: 8) {
                    Text("Super Mario Galaxy")
                        .font(.title)
                        .fontWeight(.semibold)
                    Text(runner.running ? "En ejecución" : "Wii · EE. UU. (RMGE01)")
                        .foregroundStyle(.secondary)
                    Button { runner.play() } label: {
                        Text("Jugar").frame(minWidth: 110)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    .keyboardShortcut(.defaultAction)
                    .disabled(runner.running)
                    .padding(.top, 14)
                    Group {
                        if let message = runner.lastExitMessage {
                            StatusLine(kind: .failure, text: message)
                        } else if settings.controlStyle == .controller && model.controllers.isEmpty {
                            VStack(alignment: .leading, spacing: 4) {
                                StatusLine(kind: .warning, text: "No hay ningún mando conectado.")
                                Button("Usar teclado y ratón") {
                                    settings.controlStyle = .keyboardMouse
                                    settings.save()
                                }
                                .buttonStyle(.link)
                            }
                        } else if settings.controlStyle == .wiiRemote && model.wiiRemotes.isEmpty {
                            VStack(alignment: .leading, spacing: 4) {
                                StatusLine(kind: .warning, text: "No hay ningún Wii Remote conectado.")
                                Button("Cómo conectarlo") { showingWiiHelp = true }
                                    .buttonStyle(.link)
                                    .popover(isPresented: $showingWiiHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
                            }
                        } else {
                            Text(" ")
                        }
                    }
                    .font(.callout)
                    .padding(.top, 6)
                }
                .frame(width: 300, alignment: .leading)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            Divider()
            HStack(spacing: 10) {
                Text(summary)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer()
                Button("Controles…") { showingControls = true }
                SettingsLink { Text("Ajustes…") }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
        }
        .sheet(isPresented: $showingControls) {
            ControlsSheet(initial: model.preferredControllerKind)
        }
    }

    private var summary: String {
        var parts: [String] = []
        parts.append(settings.displayMode == .window
                     ? "Ventana \(settings.windowWidth) × \(settings.windowHeight)" : "Pantalla completa")
        switch settings.controlStyle {
        case .keyboardMouse: parts.append("Teclado y ratón")
        case .controller: parts.append(model.controllers.first ?? "Mando")
        case .auto: parts.append("Teclado, ratón y mandos")
        case .wiiRemote: parts.append(model.wiiRemotes.isEmpty ? "Wii Remote" : "Wii Remote conectado")
        }
        if settings.customTextures { parts.append("Texturas HD") }
        return parts.joined(separator: " · ")
    }
}

// MARK: - Button map

struct ControlsSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State var initial: ControllerKind

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Controles").font(.title2).fontWeight(.semibold)
                Spacer()
                Picker("Dispositivo", selection: $initial) {
                    ForEach(ControllerKind.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 420)
            }
            Table(ControlsReference.rows(for: initial)) {
                TableColumn("Acción", value: \.action).width(min: 170, ideal: 200)
                TableColumn(initial.title, value: \.inputs).width(min: 200, ideal: 290)
                TableColumn("Wii", value: \.wii).width(min: 100, ideal: 130)
            }
            HStack(alignment: .firstTextBaseline) {
                Text(footnote)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()
                Button("Cerrar") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 700, height: 520)
    }

    private var footnote: String {
        var text = "Para reasignar botones, abre el menú del juego (F1) › CONTROLS durante la partida."
        if initial == .playstation { text += " Los mandos de PlayStation usan la configuración de Xbox." }
        return text
    }
}
