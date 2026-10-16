// SPDX-License-Identifier: GPL-3.0-only
//
// The setup assistant, laid out like the macOS Installer: the steps on the left, the current page
// on the right and the navigation buttons along the bottom.
//   Introduction -> Requirements -> Game -> Controls -> Summary -> Installation

import AppKit
import GameController
import SwiftUI

enum WizardPage: Int, CaseIterable {
    case welcome, tools, game, controls, ready, installing, done
}

struct WizardView: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var installer: Installer

    @State private var page: WizardPage
    @State private var checkingTools = false
    @State private var toolsReady = false
    @State private var waitingForTools = false
    @State private var image: URL?
    @State private var identification: Installer.Identification?
    @State private var identifying = false
    @State private var dropTargeted = false
    @State private var controls: ControlStyle = GCController.controllers().isEmpty ? .keyboardMouse : .controller
    @State private var alreadyInstalled = Paths.isGameInstalled

    init(page: WizardPage = .welcome, image: URL? = nil, identification: Installer.Identification? = nil,
         controls: ControlStyle? = nil, toolsReady: Bool = false) {
        _page = State(initialValue: page)
        _image = State(initialValue: image)
        _identification = State(initialValue: identification)
        _toolsReady = State(initialValue: toolsReady)
        if let controls { _controls = State(initialValue: controls) }
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Divider()
            VStack(spacing: 0) {
                content
                    .padding(.horizontal, 32)
                    .padding(.top, 30)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                Divider()
                footer
                    .padding(.horizontal, 20)
                    .padding(.vertical, 14)
            }
        }
        .onAppear {
            // Back from Settings › Change game image: straight to the game page.
            if model.state.setupComplete && page == .welcome {
                page = .game
                controls = settings.controlStyle == .auto ? .controller : settings.controlStyle
            }
        }
        .onChange(of: installer.finished) { _, finished in
            if finished { page = .done }
        }
    }

    // MARK: Steps

    private static let steps: [(String, [WizardPage])] = [
        (L("Introduction"), [.welcome]),
        (L("Requirements"), [.tools]),
        (L("Game"), [.game]),
        (L("Controls"), [.controls]),
        (L("Summary"), [.ready]),
        (L("Installation"), [.installing, .done]),
    ]

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                Image(nsImage: .appIcon).resizable().frame(width: 36, height: 36)
                Text(verbatim: "Orbit").font(.headline)
            }
            .padding(.bottom, 26)
            ForEach(Array(Self.steps.enumerated()), id: \.offset) { index, step in
                let state = stepState(index, step.1)
                HStack(spacing: 9) {
                    Group {
                        switch state {
                        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.secondary)
                        case .current: Image(systemName: "circle.fill").foregroundStyle(Color.accentColor)
                        case .upcoming: Image(systemName: "circle").foregroundStyle(.tertiary)
                        }
                    }
                    .font(.system(size: 11))
                    .frame(width: 14)
                    Text(step.0)
                        .fontWeight(state == .current ? .semibold : .regular)
                        .foregroundStyle(state == .upcoming ? .secondary : .primary)
                }
                .padding(.vertical, 6)
            }
            Spacer()
        }
        .padding(.horizontal, 20)
        .padding(.top, 26)
        .frame(width: 190, alignment: .leading)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Color.primary.opacity(0.035))
    }

    private enum StepState { case done, current, upcoming }

    private func stepState(_ index: Int, _ pages: [WizardPage]) -> StepState {
        if pages.contains(page) { return page == .done ? .done : .current }
        let currentIndex = Self.steps.firstIndex { $0.1.contains(page) } ?? 0
        return index < currentIndex ? .done : .upcoming
    }

    // MARK: Pages

    @ViewBuilder
    private var content: some View {
        switch page {
        case .welcome: welcome
        case .tools: tools
        case .game: game
        case .controls: controlsPage
        case .ready: ready
        case .installing: installing
        case .done: done
        }
    }

    @ViewBuilder
    private var footer: some View {
        HStack(spacing: 10) {
            switch page {
            case .welcome:
                Spacer()
                Button("Continue") { begin() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(checkingTools)
            case .tools:
                Button("Back") { page = .welcome }
                Spacer()
                if toolsReady {
                    Button("Continue") { page = .game }.keyboardShortcut(.defaultAction)
                } else {
                    Button("Install tools…") { installTools() }.keyboardShortcut(.defaultAction)
                }
            case .game:
                if model.state.setupComplete {
                    Button("Cancel") { model.screen = .home }
                } else {
                    Button("Back") { page = .welcome }
                }
                Spacer()
                Button("Continue") { page = .controls }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!(identification?.supported ?? false) && !(alreadyInstalled && image == nil))
            case .controls:
                Button("Back") { page = .game }
                Spacer()
                Button("Continue") { page = .ready }.keyboardShortcut(.defaultAction)
            case .ready:
                Button("Back") { page = .controls }
                Spacer()
                Button(needsInstall ? L("Install") : L("Save")) { confirm() }.keyboardShortcut(.defaultAction)
            case .installing:
                if installer.failure != nil {
                    Button("Back") { installer.reset(); page = .game }
                    Button("Show log") {
                        NSWorkspace.shared.open(Paths.logs.appendingPathComponent("install.log"))
                    }
                    Spacer()
                    Button("Retry") { installer.reset(); installer.start(image: image) }
                        .keyboardShortcut(.defaultAction)
                } else {
                    Spacer()
                    Button("Cancel") { installer.cancel() }
                }
            case .done:
                Spacer()
                Button("Continue") { model.finishSetup(controls: controls) }.keyboardShortcut(.defaultAction)
            }
        }
    }

    // MARK: Introduction

    private var welcome: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Install Super Mario Galaxy"),
                       detail: L("Orbit runs Super Mario Galaxy natively on this Mac. To install it, you need a copy of the US version of the game (RMGE01). You only do this once."))
            GroupBox {
                SummaryGrid(rows: [
                    (L("Location"), "~/Library/Application Support/Orbit"),
                    (L("Disk space"), L("12 GB during installation, 3.5 GB afterwards")),
                    (L("Time"), L("3 to 15 minutes, depending on the Mac")),
                ])
                .padding(6)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if runningFromDiskImage {
                StatusLine(kind: .info, text: L("Orbit is running from the disk image. Copy it to the Applications folder before regular use."))
                    .foregroundStyle(.secondary)
            }
            if checkingTools {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Checking requirements…").foregroundStyle(.secondary)
                }
            }
        }
    }

    private var runningFromDiskImage: Bool {
        let path = Bundle.main.bundlePath
        return path.hasPrefix("/Volumes/") || path.contains("AppTranslocation")
    }

    private func begin() {
        checkingTools = true
        Task {
            let available = await Task.detached { Installer.developerToolsAvailable() }.value
            checkingTools = false
            toolsReady = available
            page = available ? .game : .tools
        }
    }

    // MARK: Requirements

    private var tools: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Command line tools"),
                       detail: L("Orbit compiles the game on this Mac with Apple’s command line tools. They are free and macOS installs them."))
            if toolsReady {
                StatusLine(kind: .success, text: L("The tools are installed."))
            } else {
                StatusLine(kind: .warning, text: L("The tools are not installed."))
                Text("Select “Install tools…” and confirm the installation in Apple’s dialog. This page detects when it finishes.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if waitingForTools {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("Waiting for the installation to finish…").foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private func installTools() {
        Installer.installDeveloperTools()
        guard !waitingForTools else { return }
        waitingForTools = true
        Task {
            while !toolsReady && page == .tools {
                try? await Task.sleep(for: .seconds(4))
                toolsReady = await Task.detached { Installer.developerToolsAvailable() }.value
            }
            waitingForTools = false
        }
    }

    // MARK: Game

    private var game: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Game image"),
                       detail: L("Select the Super Mario Galaxy disc image. Orbit only reads it during installation, so you can delete it afterwards."))
            dropArea
            if image != nil, let identification, !identifying {
                if identification.supported {
                    StatusLine(kind: .success, text: L("Super Mario Galaxy, USA (RMGE01), revision 0."))
                } else {
                    StatusLine(kind: .failure, text: unsupportedHelp(identification))
                }
            } else if alreadyInstalled && image == nil {
                StatusLine(kind: .info, text: L("The game is already installed. Continue without selecting a file, or choose another one to reinstall the game."))
            }
            Text("Supported formats: WBFS, ISO, RVZ, WIA, CISO, GCZ or an extracted folder.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    private var dropArea: some View {
        VStack(spacing: 10) {
            if identifying {
                ProgressView().controlSize(.small)
                Text("Checking the file…").foregroundStyle(.secondary)
            } else {
                Image(systemName: "opticaldisc")
                    .font(.system(size: 26, weight: .light))
                    .foregroundStyle(.secondary)
                Text(image?.lastPathComponent ?? L("Drag the disc image here"))
                Button(image == nil ? L("Choose…") : L("Choose another…")) { chooseFile() }
            }
        }
        .frame(maxWidth: .infinity, minHeight: 150)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(dropTargeted ? Color.accentColor.opacity(0.08) : Color.primary.opacity(0.025))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(dropTargeted ? Color.accentColor : Color.secondary.opacity(0.4),
                              style: StrokeStyle(lineWidth: 1, dash: [5, 4]))
        )
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            check(url)
            return true
        } isTargeted: { dropTargeted = $0 }
    }

    private func unsupportedHelp(_ identification: Installer.Identification) -> String {
        let summary = identification.summary
        if summary.contains("RMGP01") { return L("This is the European version (RMGP01). Orbit needs the US version (RMGE01).") }
        if summary.contains("RMGJ01") { return L("This is the Japanese version (RMGJ01). Orbit needs the US version (RMGE01).") }
        if summary.contains("RMGK01") { return L("This is the Korean version (RMGK01). Orbit needs the US version (RMGE01).") }
        if summary.contains("RMGE01") { return L("This is another revision of the disc. Orbit needs revision 0 of the US version.") }
        return L("The file is not recognized as Super Mario Galaxy. (\(summary))")
    }

    private func chooseFile() {
        let panel = NSOpenPanel()
        panel.message = L("Select the Super Mario Galaxy disc image")
        panel.prompt = L("Choose")
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { check(url) }
    }

    private func check(_ url: URL) {
        image = url
        identification = nil
        identifying = true
        Task {
            let result = await Task.detached { Installer.identify(url) }.value
            identification = result
            identifying = false
        }
    }

    // MARK: Controls

    private var controlsPage: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Control method"),
                       detail: L("You can change it at any time in Settings."))
            VStack(spacing: 0) {
                choice(.keyboardMouse, symbol: "keyboard", title: ControlStyle.keyboardMouse.title,
                       detail: L("Move Mario with WASD and point with the mouse."))
                Divider().padding(.leading, 54)
                choice(.controller, symbol: "gamecontroller", title: ControlStyle.controller.title,
                       detail: model.controllers.isEmpty
                           ? L("Xbox, PlayStation or Switch Pro, over Bluetooth or USB. No controller is connected.")
                           : L("Xbox, PlayStation or Switch Pro. Connected: \(model.controllers.joined(separator: ", "))."))
                Divider().padding(.leading, 54)
                choice(.wiiRemote, symbol: "av.remote", title: ControlStyle.wiiRemote.title,
                       detail: model.wiiRemotes.isEmpty
                           ? L("Original Wii Remote and Nunchuk, over Bluetooth. No Wii Remote is connected.")
                           : L("Original Wii Remote and Nunchuk. Connected: \(model.wiiRemotes.joined(separator: ", "))."),
                       enabled: ControlStyle.wiiRemoteAvailable)
            }
            .background(RoundedRectangle(cornerRadius: 8).fill(Color.primary.opacity(0.03)))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.25)))
            if controls == .wiiRemote {
                WiiRemoteStatus(connected: !model.wiiRemotes.isEmpty)
            }
        }
    }

    private func choice(_ style: ControlStyle, symbol: String, title: String, detail: String,
                        enabled: Bool = true) -> some View {
        Button { controls = style } label: {
            HStack(spacing: 14) {
                Image(systemName: symbol)
                    .font(.system(size: 20, weight: .light))
                    .frame(width: 26)
                    .foregroundStyle(.secondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                    Text(detail).font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                Image(systemName: "checkmark")
                    .foregroundStyle(Color.accentColor)
                    .opacity(controls == style ? 1 : 0)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 11)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }

    // MARK: Summary

    private var needsInstall: Bool { image != nil && (identification?.supported ?? false) }

    private var ready: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Summary"),
                       detail: L("Orbit applies the settings below. You can change all of them later in Settings."))
            GroupBox {
                SummaryGrid(rows: summaryRows)
                    .padding(6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if needsInstall {
                Text("Installation takes 3 to 15 minutes. The Mac may run slower while the game compiles.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var summaryRows: [(String, String)] {
        var rows: [(String, String)] = []
        if let image, needsInstall { rows.append((L("Game"), image.lastPathComponent)) }
        rows.append((L("Display"), L("Full screen, \(screenResolution)")))
        rows.append((L("Aspect ratio"), L("Automatic (fits the display)")))
        rows.append((L("HD textures"), Settings.texturesAvailable ? L("On, preloaded") : L("Not available")))
        rows.append((L("Controls"), controls.title))
        return rows
    }

    private var screenResolution: String {
        guard let screen = NSScreen.main else { return L("display resolution") }
        return "\(Int(screen.frame.width * screen.backingScaleFactor)) × \(Int(screen.frame.height * screen.backingScaleFactor))"
    }

    private func confirm() {
        settings.applyRecommended(controls: controls)
        if needsInstall {
            installer.reset()
            installer.start(image: image)
            page = .installing
        } else {
            page = .done
        }
    }

    // MARK: Installation

    @ViewBuilder
    private var installing: some View {
        if let failure = installer.failure {
            VStack(alignment: .leading, spacing: 16) {
                PageHeader(title: L("The installation could not be completed"))
                StatusLine(kind: .failure, text: failure.errorDescription ?? L("Unknown error."))
                    .textSelection(.enabled)
                Text(failureAdvice(failure))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else {
            VStack(alignment: .leading, spacing: 16) {
                PageHeader(title: L("Installing Super Mario Galaxy"))
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: installer.overallProgress)
                    HStack {
                        Text("Step \(installer.step.rawValue + 1) of \(InstallStep.allCases.count): \(installer.step.title)")
                        Spacer()
                        Text(installer.overallProgress, format: .percent.precision(.fractionLength(0)))
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                    .font(.callout)
                }
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(InstallStep.allCases) { step in
                        HStack(spacing: 8) {
                            stepIcon(step).frame(width: 14)
                            Text(step.title)
                                .foregroundStyle(step.rawValue > installer.step.rawValue ? .secondary : .primary)
                        }
                        .font(.callout)
                    }
                }
                Text(installer.detail)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
    }

    @ViewBuilder
    private func stepIcon(_ step: InstallStep) -> some View {
        if step.rawValue < installer.step.rawValue || installer.finished {
            Image(systemName: "checkmark").foregroundStyle(.secondary).font(.caption)
        } else if step == installer.step {
            ProgressView().controlSize(.mini)
        } else {
            Color.clear.frame(width: 1, height: 1)
        }
    }

    private func failureAdvice(_ failure: InstallError) -> String {
        switch failure {
        case .toolsMissing:
            return L("Install Apple’s command line tools and try again.")
        case .notEnoughSpace:
            return L("Free up disk space and select Retry.")
        case .notSupported:
            return L("Select another game image.")
        case .cancelled:
            return L("Select Retry to resume it.")
        case .commandFailed:
            return L("Select Retry. If the error happens again, the log has the details.")
        }
    }

    // MARK: Done

    private var done: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: L("Installation complete"),
                       detail: L("Super Mario Galaxy is ready. These are the main controls. The full list is in Controls."))
            GroupBox {
                SummaryGrid(rows: quickControls)
                    .padding(6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Text("During play, F1 opens the game menu (fn + F1 on laptops) and ⌘Q quits the game.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var quickControls: [(String, String)] {
        let preferred = model.preferredControllerKind
        let kind: ControllerKind = controls == .keyboardMouse ? .keyboard : preferred == .keyboard ? .xbox : preferred
        let wanted = ["stick_up", "a", "shake", "b", "pointer_up", "plus"]
        let rows = ControlsReference.rows(for: kind)
        return wanted.compactMap { id in
            rows.first { $0.id == id }.map { ($0.action, controls == .wiiRemote ? $0.wii : $0.inputs) }
        }
    }
}
