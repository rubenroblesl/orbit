// SPDX-License-Identifier: GPL-3.0-only
//
// Turns the user's disc image into an installed game, as the Windows Setup does:
//   identify -> extract -> package-content -> generate -> build the game modules -> install.
// The game modules are compiled on this Mac (nothing derived from the game ships in the app) with
// Apple's compiler from the Command Line Tools and the CMake and Ninja inside the app.

import Foundation

enum InstallStep: Int, CaseIterable, Identifiable {
    case tools, identify, extract, content, generate, compile, finish
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .tools: return "Comprobación de herramientas"
        case .identify: return "Verificación de la imagen"
        case .extract: return "Extracción del disco"
        case .content: return "Preparación del contenido"
        case .generate: return "Traducción del código"
        case .compile: return "Compilación"
        case .finish: return "Finalización"
        }
    }
    // Share of the whole installation, for the progress bar.
    var weight: Double {
        switch self {
        case .tools: return 0.01
        case .identify: return 0.01
        case .extract: return 0.12
        case .content: return 0.12
        case .generate: return 0.12
        case .compile: return 0.60
        case .finish: return 0.02
        }
    }
}

enum InstallError: LocalizedError {
    case toolsMissing
    case notSupported(String)
    case notEnoughSpace(needed: Int64, available: Int64)
    case commandFailed(step: InstallStep, detail: String)
    case cancelled

    var errorDescription: String? {
        switch self {
        case .toolsMissing:
            return "Las herramientas de línea de comandos de Apple no están instaladas."
        case .notSupported(let detail):
            return "El archivo no corresponde a Super Mario Galaxy (EE. UU., revisión 0).\n\(detail)"
        case .notEnoughSpace(let needed, let available):
            let formatter = ByteCountFormatter()
            return "Espacio insuficiente: se necesitan \(formatter.string(fromByteCount: needed)) y hay \(formatter.string(fromByteCount: available)) disponibles."
        case .commandFailed(let step, let detail):
            return "Error en el paso «\(step.title)».\n\(detail)"
        case .cancelled:
            return "Instalación cancelada."
        }
    }
}

@MainActor
final class Installer: ObservableObject {
    @Published private(set) var step: InstallStep = .tools
    @Published private(set) var stepProgress: Double = 0
    @Published private(set) var detail: String = ""
    @Published private(set) var running = false
    @Published private(set) var failure: InstallError?
    @Published private(set) var finished = false

    private var current: Process?
    private var cancelled = false

    var overallProgress: Double {
        var done = 0.0
        for item in InstallStep.allCases where item.rawValue < step.rawValue { done += item.weight }
        return min(1.0, done + step.weight * stepProgress)
    }

    // MARK: Static checks (used by the wizard before starting)

    nonisolated static func developerToolsAvailable() -> Bool {
        let result = run(URL(fileURLWithPath: "/usr/bin/xcrun"), ["--find", "clang++"])
        return result.status == 0 && !result.output.isEmpty
    }

    nonisolated static func installDeveloperTools() {
        _ = run(URL(fileURLWithPath: "/usr/bin/xcode-select"), ["--install"])
    }

    struct Identification {
        let supported: Bool
        let summary: String
    }

    nonisolated static func identify(_ image: URL) -> Identification {
        let result = run(Paths.recompiler, ["identify", image.path])
        let lines = result.output.split(separator: "\n").map(String.init)
        let first = lines.first ?? "No se pudo leer el archivo."
        return Identification(supported: result.status == 0 && first.contains("RMGE01") && first.contains("revision 0"),
                              summary: first)
    }

    // MARK: Installation

    // A new installation from a disc image, or (image nil) a repair that rebuilds the modules from
    // the game inputs kept by the last installation.
    func start(image: URL?) {
        guard !running else { return }
        running = true
        cancelled = false
        failure = nil
        finished = false
        Task {
            do {
                try await install(image: image)
                finished = true
            } catch let error as InstallError {
                failure = cancelled ? .cancelled : error
            } catch {
                failure = .commandFailed(step: step, detail: error.localizedDescription)
            }
            running = false
        }
    }

    func cancel() {
        cancelled = true
        current?.terminate()
    }

    // A frozen state for the screenshots of `Orbit --snapshots`.
    func preview(step: InstallStep, progress: Double, detail: String) {
        self.step = step
        stepProgress = progress
        self.detail = detail
        running = true
    }

    func reset() {
        failure = nil
        finished = false
        step = .tools
        stepProgress = 0
        detail = ""
    }

    private func begin(_ next: InstallStep, _ text: String = "") {
        step = next
        stepProgress = 0
        detail = text
    }

    // The files `generate` needs, kept so the game can be rebuilt without the disc (as Setup does).
    private static let gameInputs = ["sys/main.dol", "sys/boot.bin", "sys/bi2.bin", "sys/apploader.img", "sys/fst.bin",
                                     "files/ModuleData/HomeButtonMenuWrapperRSO.rso", "files/ModuleData/product.sel"]

    private func install(image: URL?) async throws {
        let fm = FileManager.default
        try Paths.ensure(Paths.support)
        try Paths.ensure(Paths.logs)
        let log = Paths.logs.appendingPathComponent("install.log")
        fm.createFile(atPath: log.path, contents: nil)

        begin(.tools)
        guard Installer.developerToolsAvailable() else { throw InstallError.toolsMissing }

        let work = Paths.installWork
        try? fm.removeItem(at: work)
        try Paths.ensure(work)
        let staging = work.appendingPathComponent("Game")
        let inputs = staging.appendingPathComponent("game-inputs")
        let generated = work.appendingPathComponent("generated")
        let build = work.appendingPathComponent("build")
        try Paths.ensure(staging)
        var summary = ""
        var source = ""

        if let image {
            begin(.identify)
            let identification = Installer.identify(image)
            guard identification.supported else { throw InstallError.notSupported(identification.summary) }
            summary = identification.summary
            source = image.lastPathComponent
            detail = summary

            // Extracted disc + content + generated sources + objects, before the disc is deleted.
            let needed: Int64 = 12 * 1_000_000_000
            let values = try? Paths.support.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            if let available = values?.volumeAvailableCapacityForImportantUsage, available < needed {
                throw InstallError.notEnoughSpace(needed: needed, available: available)
            }

            var data = image
            var isDirectory: ObjCBool = false
            if fm.fileExists(atPath: image.path, isDirectory: &isDirectory), isDirectory.boolValue {
                // An extracted folder: X/DATA/sys/main.dol or X/sys/main.dol.
                if fm.fileExists(atPath: image.appendingPathComponent("DATA/sys/main.dol").path) {
                    data = image.appendingPathComponent("DATA")
                }
                begin(.extract)
                stepProgress = 1
            } else {
                let disc = work.appendingPathComponent("disc")
                begin(.extract)
                try await execute(.extract, Paths.recompiler, ["extract", image.path, "--output", disc.path], log: log,
                                  progress: OutputTail.extractProgress)
                data = disc.appendingPathComponent("DATA")
            }
            for relative in Installer.gameInputs {
                let target = inputs.appendingPathComponent(relative)
                try Paths.ensure(target.deletingLastPathComponent())
                try fm.copyItem(at: data.appendingPathComponent(relative), to: target)
            }

            begin(.content)
            try await execute(.content, Paths.recompiler,
                              ["package-content", data.path, "--output", staging.appendingPathComponent("content").path],
                              log: log)
            try? fm.removeItem(at: work.appendingPathComponent("disc"))  // ~3 GB no longer needed
        } else {
            // Repair: keep the installed content and inputs, rebuild everything else.
            begin(.identify, "Se reutilizan los datos de la instalación anterior.")
            guard fm.fileExists(atPath: Paths.game.appendingPathComponent("game-inputs/sys/main.dol").path),
                  fm.fileExists(atPath: Paths.gameContent.path) else {
                throw InstallError.notSupported("No hay datos de una instalación anterior. Selecciona la imagen del juego.")
            }
            try fm.copyItem(at: Paths.game.appendingPathComponent("game-inputs"), to: inputs)
            if let manifest = try? JSONSerialization.jsonObject(with: Data(contentsOf: Paths.installManifest)) as? [String: Any] {
                summary = manifest["sourceSummary"] as? String ?? ""
                source = manifest["source"] as? String ?? ""
            }
            stepProgress = 1
            begin(.extract)
            stepProgress = 1
            begin(.content)
            stepProgress = 1
        }

        begin(.generate)
        try await execute(.generate, Paths.recompiler, ["generate", inputs.path, "--output", generated.path], log: log,
                          progress: OutputTail.generateProgress)

        begin(.compile)
        let clang = Installer.run(URL(fileURLWithPath: "/usr/bin/xcrun"), ["--find", "clang++"]).output
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let jobs = Installer.compileJobs()
        try await execute(.compile, Paths.cmake, [
            "-S", generated.path, "-B", build.path, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_MAKE_PROGRAM=\(Paths.ninja.path)",
            "-DCMAKE_CXX_COMPILER=\(clang)",
            "-DGALAXY_RUNTIME_INCLUDE=\(Paths.sdkInclude.path)",
            "-DGALAXY_PPC_FLOAT_LIBRARY=\(Paths.sdkLibrary("libgalaxy_ppc_float.a").path)",
            "-DGALAXY_SOFTFLOAT_LIBRARY=\(Paths.sdkLibrary("libgalaxy_softfloat.a").path)",
            "-DGALAXY_DSP_ALU_LIBRARY=\(Paths.sdkLibrary("libgalaxy_dsp_alu.a").path)",
            "-DGALAXY_MODULE_COMPILE_JOBS=\(jobs)",
        ], log: log, finishes: false)
        try await execute(.compile, Paths.cmake, ["--build", build.path, "--parallel", "\(jobs)"], log: log,
                          progress: OutputTail.ninjaProgress)

        begin(.finish)
        for (from, name) in [(build.appendingPathComponent("game/RMGE01_game.dylib"), "RMGE01_game.dylib"),
                             (build.appendingPathComponent("home/RMGE01_home_button.dylib"), "RMGE01_home_button.dylib"),
                             (build.appendingPathComponent("dsp/RMGE01_dsp.dylib"), "RMGE01_dsp.dylib"),
                             (generated.appendingPathComponent("RMGE01_boot_image.bin"), "RMGE01_boot_image.bin")] {
            try fm.moveItem(at: from, to: staging.appendingPathComponent(name))
        }
        if image == nil {
            try fm.moveItem(at: Paths.gameContent, to: staging.appendingPathComponent("content"))
        }
        let manifest: [String: Any] = [
            "gameId": "RMGE01",
            "revision": 0,
            "source": source,
            "sourceSummary": summary,
            "installed": ISO8601DateFormatter().string(from: Date()),
            "launcherVersion": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "",
        ]
        let data = try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
        try data.write(to: staging.appendingPathComponent("install.json"))
        // Swap the new installation in; an earlier one is replaced only now.
        try? fm.removeItem(at: Paths.game)
        try fm.moveItem(at: staging, to: Paths.game)
        try? fm.removeItem(at: work)
        stepProgress = 1
    }

    nonisolated private static func compileJobs() -> Int {
        let cores = ProcessInfo.processInfo.activeProcessorCount
        // The largest translation units take 1.5-3.5 GB each.
        let memoryJobs = Int(ProcessInfo.processInfo.physicalMemory / (1_610_612_736))
        return max(1, min(cores, memoryJobs))
    }

    private func execute(_ step: InstallStep, _ tool: URL, _ arguments: [String], log: URL,
                         progress: ((String) -> Double?)? = nil, finishes: Bool = true) async throws {
        if cancelled { throw InstallError.cancelled }
        let process = Process()
        process.executableURL = tool
        process.arguments = arguments
        var environment = ProcessInfo.processInfo.environment
        environment["PATH"] = "\(Paths.toolchain.appendingPathComponent("bin").path):/usr/bin:/bin:/usr/sbin:/sbin"
        process.environment = environment
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        let handle = try? FileHandle(forWritingTo: log)
        handle?.seekToEndOfFile()
        handle?.write("$ \(tool.lastPathComponent) \(arguments.joined(separator: " "))\n".data(using: .utf8)!)
        let tail = OutputTail()
        pipe.fileHandleForReading.readabilityHandler = { [weak self] reader in
            let data = reader.availableData
            guard !data.isEmpty else { return }
            handle?.write(data)
            guard let text = String(data: data, encoding: .utf8) else { return }
            tail.append(text)
            if let progress, let fraction = progress(text) {
                Task { @MainActor in self?.stepProgress = fraction }
            }
        }
        current = process
        try process.run()
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            process.terminationHandler = { _ in continuation.resume() }
        }
        pipe.fileHandleForReading.readabilityHandler = nil
        handle?.closeFile()
        current = nil
        if cancelled { throw InstallError.cancelled }
        if process.terminationStatus != 0 {
            throw InstallError.commandFailed(step: step, detail: tail.text)
        }
        if finishes { stepProgress = 1 }
    }

    // MARK: Helpers

    struct RunResult {
        let status: Int32
        let output: String
    }

    nonisolated static func run(_ tool: URL, _ arguments: [String]) -> RunResult {
        let process = Process()
        process.executableURL = tool
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
        } catch {
            return RunResult(status: -1, output: error.localizedDescription)
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return RunResult(status: process.terminationStatus, output: String(data: data, encoding: .utf8) ?? "")
    }
}

// The last lines a tool printed (for error messages), and Ninja's [done/total] progress.
final class OutputTail: @unchecked Sendable {
    private var lines: [String] = []
    private let lock = NSLock()

    func append(_ text: String) {
        lock.lock()
        defer { lock.unlock() }
        lines.append(contentsOf: text.split(separator: "\n").map(String.init))
        if lines.count > 12 { lines.removeFirst(lines.count - 12) }
    }

    var text: String {
        lock.lock()
        defer { lock.unlock() }
        return lines.joined(separator: "\n")
    }

    // nebula-recomp extract: "PROGRESS <done> <total>".
    static func extractProgress(_ text: String) -> Double? {
        var result: Double?
        for line in text.split(separator: "\n") where line.hasPrefix("PROGRESS ") {
            let parts = line.split(separator: " ")
            if parts.count == 3, let done = Double(parts[1]), let total = Double(parts[2]), total > 0 {
                result = done / total
            }
        }
        return result
    }

    // nebula-recomp generate: "STEP <n>/5 ...".
    static func generateProgress(_ text: String) -> Double? {
        var result: Double?
        for line in text.split(separator: "\n") where line.hasPrefix("STEP ") {
            let fraction = line.dropFirst(5).split(separator: " ").first?.split(separator: "/") ?? []
            if fraction.count == 2, let step = Double(fraction[0]), let total = Double(fraction[1]), total > 0 {
                result = (step - 1) / total
            }
        }
        return result
    }

    static func ninjaProgress(_ text: String) -> Double? {
        var result: Double?
        for line in text.split(separator: "\n") {
            guard line.hasPrefix("["), let close = line.firstIndex(of: "]") else { continue }
            let parts = line[line.index(after: line.startIndex)..<close].split(separator: "/")
            if parts.count == 2, let done = Double(parts[0]), let total = Double(parts[1]), total > 0 {
                result = done / total
            }
        }
        return result
    }
}
