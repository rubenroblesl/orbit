// SPDX-License-Identifier: GPL-3.0-only
//
// The button map shown by the launcher: the runtime's default bindings (runtime/src/
// input_bindings.cpp, controls version 5) with the player's changes from runtime_settings.ini
// (`bind.<scheme>.<target>=Source, Source`), written for people rather than for the ini file.

import Foundation

enum ControllerKind: String, CaseIterable, Identifiable {
    case keyboard, xbox, playstation, switchPro
    var id: String { rawValue }
    var title: String {
        switch self {
        case .keyboard: return "Teclado y ratón"
        case .xbox: return "Xbox"
        case .playstation: return "PlayStation"
        case .switchPro: return "Switch Pro"
        }
    }
    var symbol: String {
        switch self {
        case .keyboard: return "keyboard"
        case .xbox, .playstation: return "gamecontroller"
        case .switchPro: return "gamecontroller.fill"
        }
    }
    // PlayStation pads arrive through GameController with the Xbox layout.
    var scheme: String {
        switch self {
        case .keyboard: return "keyboard"
        case .xbox, .playstation: return "xbox"
        case .switchPro: return "switch_pro"
        }
    }
}

struct ControlRow: Identifiable {
    let id: String
    let action: String
    let wii: String
    let inputs: String
}

enum ControlsReference {
    private struct Entry {
        let targets: [String]  // one target, or the four directions up/down/left/right
        let action: String
        let wii: String
    }

    private static let entries: [Entry] = [
        Entry(targets: ["stick_up", "stick_down", "stick_left", "stick_right"], action: "Movimiento", wii: "Stick del Nunchuk"),
        Entry(targets: ["a"], action: "Saltar / Aceptar", wii: "A"),
        Entry(targets: ["jump_back"], action: "Saltar / Atrás en menús", wii: "A / B"),
        Entry(targets: ["shake"], action: "Giro", wii: "Agitar Wii Remote"),
        Entry(targets: ["z"], action: "Agacharse", wii: "Z"),
        Entry(targets: ["b"], action: "Disparar fragmentos de estrella", wii: "B"),
        Entry(targets: ["c"], action: "Centrar la cámara", wii: "C"),
        Entry(targets: ["pointer_up", "pointer_down", "pointer_left", "pointer_right"], action: "Puntero", wii: "Apuntar"),
        Entry(targets: ["pointer_recenter"], action: "Centrar el puntero", wii: "Sin equivalente"),
        Entry(targets: ["dpad_up", "dpad_down", "dpad_left", "dpad_right"], action: "Cámara / Primera persona", wii: "Cruceta"),
        Entry(targets: ["plus"], action: "Pausa", wii: "+"),
        Entry(targets: ["minus"], action: "Botón −", wii: "−"),
        Entry(targets: ["home"], action: "Menú HOME", wii: "HOME"),
        Entry(targets: ["one"], action: "Botón 1", wii: "1"),
        Entry(targets: ["two"], action: "Botón 2", wii: "2"),
    ]

    private static let defaults: [String: [String: String]] = [
        "keyboard": [
            "a": "Space, Enter, MouseLeft", "b": "LShift, RShift, MouseRight", "one": "1, Num1", "two": "2, Num2",
            "plus": "Equals, P", "minus": "Minus, M", "home": "H",
            "dpad_up": "Up", "dpad_down": "Down", "dpad_left": "Left", "dpad_right": "Right",
            "stick_up": "W", "stick_down": "S", "stick_left": "A", "stick_right": "D",
            "c": "C", "z": "Z, LCtrl, RCtrl", "shake": "Q, E",
            "pointer_up": "I", "pointer_down": "K", "pointer_left": "J", "pointer_right": "L",
            "pointer_recenter": "R",
        ],
        "xbox": [
            "a": "A", "jump_back": "B", "b": "LB", "plus": "Menu", "minus": "View",
            "dpad_up": "DpadUp", "dpad_down": "DpadDown", "dpad_left": "DpadLeft", "dpad_right": "DpadRight",
            "stick_up": "LY+", "stick_down": "LY-", "stick_left": "LX-", "stick_right": "LX+",
            "c": "LS", "z": "LT", "shake": "X, Y, RB, RT",
            "pointer_up": "RY+", "pointer_down": "RY-", "pointer_left": "RX-", "pointer_right": "RX+",
            "pointer_recenter": "RS",
        ],
        "switch_pro": [
            "a": "A", "jump_back": "B", "b": "L", "plus": "Plus", "minus": "Minus", "home": "Home",
            "dpad_up": "DpadUp, RY+", "dpad_down": "DpadDown, RY-", "dpad_left": "DpadLeft, RX-", "dpad_right": "DpadRight, RX+",
            "stick_up": "LY+", "stick_down": "LY-", "stick_left": "LX-", "stick_right": "LX+",
            "c": "LS", "z": "ZL", "shake": "Y, X, ZR",
            "pointer_up": "RY+", "pointer_down": "RY-", "pointer_left": "RX-", "pointer_right": "RX+",
            "pointer_recenter": "R",
        ],
    ]

    static func rows(for kind: ControllerKind) -> [ControlRow] {
        let file = KeyValueFile(url: Paths.runtimeSettings)
        let scheme = kind.scheme
        func sources(_ target: String) -> [String] {
            let text = file["bind.\(scheme).\(target)"] ?? defaults[scheme]?[target] ?? "None"
            return text.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty && $0.lowercased() != "none" }
        }
        var rows: [ControlRow] = []
        for entry in entries {
            var text: String
            if entry.targets.count == 4 {
                text = directions(entry.targets.map(sources), kind: kind)
            } else {
                text = unique(sources(entry.targets[0]).map { name($0, kind: kind) }).joined(separator: " · ")
            }
            if entry.targets[0] == "pointer_up" {
                switch kind {
                case .keyboard: text = text.isEmpty ? "Ratón" : "Ratón · " + text
                case .switchPro: text = "Giroscopio" + (text.isEmpty ? "" : " · " + text)
                default: break
                }
            }
            if entry.targets[0] == "shake" && kind == .switchPro { text += " · agitar el mando" }
            if text.isEmpty { continue }
            rows.append(ControlRow(id: entry.targets[0], action: entry.action, wii: entry.wii, inputs: text))
        }
        return rows
    }

    // Four directions: a whole stick or pad when every direction uses it, else the keys in
    // up-left-down-right order (W A S D, ↑ ← ↓ →).
    private static func directions(_ lists: [[String]], kind: ControllerKind) -> String {
        let groups = lists.map { Set($0.map(group)) }
        if let first = groups.first, !first.contains(""), groups.allSatisfy({ $0 == first }) {
            let order = ["Stick izquierdo", "Cruceta", "Stick derecho"]
            return first.sorted { (order.firstIndex(of: $0) ?? 9) < (order.firstIndex(of: $1) ?? 9) }
                .joined(separator: " · ")
        }
        guard lists.count == 4 else { return "" }
        let ordered = [lists[0], lists[2], lists[1], lists[3]]
        let firsts = ordered.compactMap { $0.first.map { name($0, kind: kind) } }
        return firsts.joined(separator: " ")
    }

    private static func group(_ token: String) -> String {
        if token.hasPrefix("LX") || token.hasPrefix("LY") { return "Stick izquierdo" }
        if token.hasPrefix("RX") || token.hasPrefix("RY") { return "Stick derecho" }
        if token.hasPrefix("Dpad") { return "Cruceta" }
        return ""
    }

    private static func unique(_ names: [String]) -> [String] {
        var seen = Set<String>()
        return names.filter { seen.insert($0).inserted }
    }

    static func name(_ token: String, kind: ControllerKind) -> String {
        switch kind {
        case .keyboard: return keyName(token)
        case .xbox: return xboxName(token)
        case .playstation: return playstationName(token)
        case .switchPro: return switchName(token)
        }
    }

    private static func keyName(_ token: String) -> String {
        let names: [String: String] = [
            "Space": "Espacio", "Enter": "Return", "Tab": "Tab", "Backspace": "Borrar", "Escape": "Esc",
            "MouseLeft": "Clic izquierdo", "MouseRight": "Clic derecho", "MouseMiddle": "Clic central",
            "Mouse4": "Botón 4 del ratón", "Mouse5": "Botón 5 del ratón",
            "LShift": "⇧ Mayús", "RShift": "⇧ Mayús derecha", "Shift": "⇧ Mayús",
            "LCtrl": "⌃ Control", "RCtrl": "⌃ Control derecha", "Ctrl": "⌃ Control",
            "LAlt": "⌥ Opción", "RAlt": "⌥ Opción derecha", "Alt": "⌥ Opción",
            "Up": "↑", "Down": "↓", "Left": "←", "Right": "→",
            "Equals": "=", "Minus": "-", "LBracket": "[", "RBracket": "]", "Backslash": "\\",
            "Semicolon": ";", "Quote": "'", "Comma": ",", "Period": ".", "Slash": "/", "Grave": "`",
            "CapsLock": "Bloq Mayús",
        ]
        if let name = names[token] { return name }
        if token.hasPrefix("Num"), token.count == 4 { return "\(token.suffix(1)) (teclado numérico)" }
        return token
    }

    private static func axisName(_ token: String) -> String? {
        let group = group(token)
        return group.isEmpty || group == "Cruceta" ? nil : group
    }

    private static func xboxName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "LS": "Clic del stick izquierdo (LS)", "RS": "Clic del stick derecho (RS)",
            "Menu": "Menú ☰", "View": "Vista ⧉", "Guide": "Xbox",
            "DpadUp": "Cruceta ↑", "DpadDown": "Cruceta ↓", "DpadLeft": "Cruceta ←", "DpadRight": "Cruceta →",
        ]
        return names[token] ?? token
    }

    private static func playstationName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "A": "✕", "B": "○", "X": "□", "Y": "△",
            "LB": "L1", "RB": "R1", "LT": "L2", "RT": "R2",
            "LS": "Clic del stick izquierdo (L3)", "RS": "Clic del stick derecho (R3)",
            "Menu": "Options", "View": "Create", "Guide": "PS", "Share": "Panel táctil",
            "DpadUp": "Cruceta ↑", "DpadDown": "Cruceta ↓", "DpadLeft": "Cruceta ←", "DpadRight": "Cruceta →",
        ]
        return names[token] ?? token
    }

    private static func switchName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "LS": "Clic del stick izquierdo", "RS": "Clic del stick derecho",
            "Plus": "+", "Minus": "−", "Home": "HOME", "Capture": "Captura",
            "DpadUp": "Cruceta ↑", "DpadDown": "Cruceta ↓", "DpadLeft": "Cruceta ←", "DpadRight": "Cruceta →",
        ]
        return names[token] ?? token
    }
}
