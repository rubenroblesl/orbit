// SPDX-License-Identifier: GPL-3.0-only
//
// The setup assistant, laid out like the macOS Installer: the steps on the left, the current page
// on the right and the navigation buttons along the bottom.
//   Introducción -> Requisitos -> Juego -> Controles -> Resumen -> Instalación

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
            // Back from Settings › Cambiar juego: straight to the game page.
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
        ("Introducción", [.welcome]),
        ("Requisitos", [.tools]),
        ("Juego", [.game]),
        ("Controles", [.controls]),
        ("Resumen", [.ready]),
        ("Instalación", [.installing, .done]),
    ]

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                Image(nsImage: .appIcon).resizable().frame(width: 36, height: 36)
                Text("Orbit").font(.headline)
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
                Button("Continuar") { begin() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(checkingTools)
            case .tools:
                Button("Atrás") { page = .welcome }
                Spacer()
                if toolsReady {
                    Button("Continuar") { page = .game }.keyboardShortcut(.defaultAction)
                } else {
                    Button("Instalar herramientas…") { installTools() }.keyboardShortcut(.defaultAction)
                }
            case .game:
                if model.state.setupComplete {
                    Button("Cancelar") { model.screen = .home }
                } else {
                    Button("Atrás") { page = .welcome }
                }
                Spacer()
                Button("Continuar") { page = .controls }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!(identification?.supported ?? false) && !(alreadyInstalled && image == nil))
            case .controls:
                Button("Atrás") { page = .game }
                Spacer()
                Button("Continuar") { page = .ready }.keyboardShortcut(.defaultAction)
            case .ready:
                Button("Atrás") { page = .controls }
                Spacer()
                Button(needsInstall ? "Instalar" : "Guardar") { confirm() }.keyboardShortcut(.defaultAction)
            case .installing:
                if installer.failure != nil {
                    Button("Atrás") { installer.reset(); page = .game }
                    Button("Mostrar registro") {
                        NSWorkspace.shared.open(Paths.logs.appendingPathComponent("install.log"))
                    }
                    Spacer()
                    Button("Reintentar") { installer.reset(); installer.start(image: image) }
                        .keyboardShortcut(.defaultAction)
                } else {
                    Spacer()
                    Button("Cancelar") { installer.cancel() }
                }
            case .done:
                Spacer()
                Button("Continuar") { model.finishSetup(controls: controls) }.keyboardShortcut(.defaultAction)
            }
        }
    }

    // MARK: Introducción

    private var welcome: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Instalación de Super Mario Galaxy",
                       detail: "Orbit ejecuta Super Mario Galaxy de forma nativa en este Mac. Para instalarlo necesitas una copia del juego en su versión de EE. UU. (RMGE01). El proceso se realiza una sola vez.")
            GroupBox {
                SummaryGrid(rows: [
                    ("Ubicación", "~/Library/Application Support/Orbit"),
                    ("Espacio", "12 GB durante la instalación, 3,5 GB después"),
                    ("Duración", "Entre 3 y 15 minutos, según el equipo"),
                ])
                .padding(6)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if runningFromDiskImage {
                StatusLine(kind: .info, text: "Orbit se está ejecutando desde la imagen de disco. Para el uso habitual, cópiala antes a la carpeta Aplicaciones.")
                    .foregroundStyle(.secondary)
            }
            if checkingTools {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Comprobando requisitos…").foregroundStyle(.secondary)
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

    // MARK: Requisitos

    private var tools: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Herramientas de línea de comandos",
                       detail: "Orbit compila el juego en este Mac con las herramientas de línea de comandos de Apple. Son gratuitas y se instalan desde macOS.")
            if toolsReady {
                StatusLine(kind: .success, text: "Las herramientas están instaladas.")
            } else {
                StatusLine(kind: .warning, text: "Las herramientas no están instaladas.")
                Text("Selecciona «Instalar herramientas…» y confirma la instalación en el cuadro de diálogo de Apple. Esta página detecta automáticamente cuándo termina.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if waitingForTools {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("Esperando a que finalice la instalación…").foregroundStyle(.secondary)
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

    // MARK: Juego

    private var game: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Imagen del juego",
                       detail: "Selecciona la imagen de disco de Super Mario Galaxy. Solo se lee durante la instalación. Después puedes borrarla.")
            dropArea
            if image != nil, let identification, !identifying {
                if identification.supported {
                    StatusLine(kind: .success, text: "Super Mario Galaxy, EE. UU. (RMGE01), revisión 0.")
                } else {
                    StatusLine(kind: .failure, text: unsupportedHelp(identification))
                }
            } else if alreadyInstalled && image == nil {
                StatusLine(kind: .info, text: "El juego ya está instalado. Puedes continuar sin seleccionar un archivo o elegir otro para reinstalarlo.")
            }
            Text("Formatos admitidos: WBFS, ISO, RVZ, WIA, CISO, GCZ o una carpeta extraída.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    private var dropArea: some View {
        VStack(spacing: 10) {
            if identifying {
                ProgressView().controlSize(.small)
                Text("Verificando el archivo…").foregroundStyle(.secondary)
            } else {
                Image(systemName: "opticaldisc")
                    .font(.system(size: 26, weight: .light))
                    .foregroundStyle(.secondary)
                Text(image?.lastPathComponent ?? "Arrastra aquí la imagen del disco")
                Button(image == nil ? "Elegir…" : "Elegir otro…") { chooseFile() }
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
        if summary.contains("RMGP01") { return "Es la versión europea (RMGP01). Se necesita la versión de EE. UU. (RMGE01)." }
        if summary.contains("RMGJ01") { return "Es la versión japonesa (RMGJ01). Se necesita la versión de EE. UU. (RMGE01)." }
        if summary.contains("RMGK01") { return "Es la versión coreana (RMGK01). Se necesita la versión de EE. UU. (RMGE01)." }
        if summary.contains("RMGE01") { return "Es otra revisión del disco. Se necesita la revisión 0 de EE. UU." }
        return "No se reconoce el archivo como Super Mario Galaxy. (\(summary))"
    }

    private func chooseFile() {
        let panel = NSOpenPanel()
        panel.message = "Selecciona la imagen de disco de Super Mario Galaxy"
        panel.prompt = "Elegir"
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

    // MARK: Controles

    private var controlsPage: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Método de control",
                       detail: "Se puede cambiar en cualquier momento en Ajustes.")
            VStack(spacing: 0) {
                choice(.keyboardMouse, symbol: "keyboard", title: "Teclado y ratón",
                       detail: "Mueve a Mario con WASD y apunta con el ratón.")
                Divider().padding(.leading, 54)
                choice(.controller, symbol: "gamecontroller", title: "Mando",
                       detail: model.controllers.isEmpty
                           ? "Xbox, PlayStation o Switch Pro, por Bluetooth o USB. No hay ningún mando conectado."
                           : "Xbox, PlayStation o Switch Pro. Conectado: \(model.controllers.joined(separator: ", ")).")
                Divider().padding(.leading, 54)
                choice(.wiiRemote, symbol: "av.remote", title: "Wii Remote",
                       detail: model.wiiRemotes.isEmpty
                           ? "Wii Remote y Nunchuk originales, por Bluetooth. No hay ningún Wii Remote conectado."
                           : "Wii Remote y Nunchuk originales. Conectado: \(model.wiiRemotes.joined(separator: ", ")).",
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

    // MARK: Resumen

    private var needsInstall: Bool { image != nil && (identification?.supported ?? false) }

    private var ready: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Resumen",
                       detail: "Se aplicará la siguiente configuración. Todo puede modificarse después en Ajustes.")
            GroupBox {
                SummaryGrid(rows: summaryRows)
                    .padding(6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if needsInstall {
                Text("La instalación tarda entre 3 y 15 minutos. Durante la compilación el equipo puede ir más lento.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var summaryRows: [(String, String)] {
        var rows: [(String, String)] = []
        if let image, needsInstall { rows.append(("Juego", image.lastPathComponent)) }
        rows.append(("Pantalla", "Pantalla completa, \(screenResolution)"))
        rows.append(("Proporción", "Automática (adaptada a la pantalla)"))
        rows.append(("Texturas HD", Settings.texturesAvailable ? "Activadas, con precarga" : "No disponibles"))
        rows.append(("Controles", controls.title))
        return rows
    }

    private var screenResolution: String {
        guard let screen = NSScreen.main else { return "resolución de la pantalla" }
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

    // MARK: Instalación

    @ViewBuilder
    private var installing: some View {
        if let failure = installer.failure {
            VStack(alignment: .leading, spacing: 16) {
                PageHeader(title: "No se pudo completar la instalación")
                StatusLine(kind: .failure, text: failure.errorDescription ?? "Error desconocido.")
                    .textSelection(.enabled)
                Text(failureAdvice(failure))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else {
            VStack(alignment: .leading, spacing: 16) {
                PageHeader(title: "Instalando Super Mario Galaxy")
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: installer.overallProgress)
                    HStack {
                        Text("Paso \(installer.step.rawValue + 1) de \(InstallStep.allCases.count): \(installer.step.title)")
                        Spacer()
                        Text("\(Int((installer.overallProgress * 100).rounded())) %")
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
            return "Instala las herramientas de línea de comandos de Apple y vuelve a intentarlo."
        case .notEnoughSpace:
            return "Libera espacio en el disco y selecciona Reintentar."
        case .notSupported:
            return "Selecciona otra imagen del juego."
        case .cancelled:
            return "Puedes reanudarla con Reintentar."
        case .commandFailed:
            return "Selecciona Reintentar. Si el error se repite, el registro contiene los detalles."
        }
    }

    // MARK: Terminado

    private var done: some View {
        VStack(alignment: .leading, spacing: 18) {
            PageHeader(title: "Instalación completada",
                       detail: "Super Mario Galaxy está listo. Estos son los controles principales. La lista completa está en Controles.")
            GroupBox {
                SummaryGrid(rows: quickControls)
                    .padding(6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Text("Durante la partida, F1 abre el menú del juego (fn + F1 en portátiles) y ⌘Q cierra el juego.")
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
