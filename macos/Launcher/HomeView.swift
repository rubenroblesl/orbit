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
                    Text(runner.running ? L("Running") : L("Wii · USA (RMGE01)"))
                        .foregroundStyle(.secondary)
                    Button { runner.play() } label: {
                        Text("Play").frame(minWidth: 110)
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
                                StatusLine(kind: .warning, text: L("No controller is connected."))
                                Button("Use keyboard and mouse") {
                                    settings.controlStyle = .keyboardMouse
                                    settings.save()
                                }
                                .buttonStyle(.link)
                            }
                        } else if settings.controlStyle == .wiiRemote && model.wiiRemotes.isEmpty {
                            VStack(alignment: .leading, spacing: 4) {
                                StatusLine(kind: .warning, text: L("No Wii Remote is connected."))
                                Button("How to connect") { showingWiiHelp = true }
                                    .buttonStyle(.link)
                                    .popover(isPresented: $showingWiiHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
                            }
                        } else {
                            Text(verbatim: " ")
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
                Button("Controls…") { showingControls = true }
                SettingsLink { Text("Settings…") }
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
                     ? L("Window \(settings.windowWidth) × \(settings.windowHeight)") : DisplayMode.fullscreen.title)
        switch settings.controlStyle {
        case .keyboardMouse, .auto: parts.append(settings.controlStyle.title)
        case .controller: parts.append(model.controllers.first ?? ControlStyle.controller.title)
        case .wiiRemote: parts.append(model.wiiRemotes.isEmpty ? ControlStyle.wiiRemote.title : L("Wii Remote connected"))
        }
        if settings.customTextures { parts.append(L("HD textures")) }
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
                Text("Controls").font(.title2).fontWeight(.semibold)
                Spacer()
                Picker("Device", selection: $initial) {
                    ForEach(ControllerKind.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 420)
            }
            Table(ControlsReference.rows(for: initial)) {
                TableColumn("Action", value: \.action).width(min: 170, ideal: 200)
                TableColumn(initial.title, value: \.inputs).width(min: 200, ideal: 260)
                TableColumn("Wii", value: \.wii).width(min: 120, ideal: 165)
            }
            HStack(alignment: .firstTextBaseline) {
                Text(footnote)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 700, height: 520)
    }

    private var footnote: String {
        var text = L("To remap buttons, open the game menu (F1) › CONTROLS during play.")
        if initial == .playstation { text += " " + L("PlayStation controllers use the Xbox settings.") }
        return text
    }
}
