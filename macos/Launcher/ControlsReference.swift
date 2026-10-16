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
        case .keyboard: return L("Keyboard and mouse")
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
        Entry(targets: ["stick_up", "stick_down", "stick_left", "stick_right"], action: L("Move"), wii: L("Nunchuk stick")),
        Entry(targets: ["a"], action: L("Jump / Confirm"), wii: "A"),
        Entry(targets: ["jump_back"], action: L("Jump / Back in menus"), wii: "A / B"),
        Entry(targets: ["shake"], action: L("Spin"), wii: L("Shake the Wii Remote")),
        Entry(targets: ["z"], action: L("Crouch"), wii: "Z"),
        Entry(targets: ["b"], action: L("Shoot Star Bits"), wii: "B"),
        Entry(targets: ["c"], action: L("Center the camera"), wii: "C"),
        Entry(targets: ["pointer_up", "pointer_down", "pointer_left", "pointer_right"], action: L("Pointer"), wii: L("Point at the screen")),
        Entry(targets: ["pointer_recenter"], action: L("Recenter the pointer"), wii: L("No equivalent")),
        Entry(targets: ["dpad_up", "dpad_down", "dpad_left", "dpad_right"], action: L("Camera / First person"), wii: L("D-pad")),
        Entry(targets: ["plus"], action: L("Pause"), wii: "+"),
        Entry(targets: ["minus"], action: L("− Button"), wii: "−"),
        Entry(targets: ["home"], action: L("HOME Menu"), wii: "HOME"),
        Entry(targets: ["one"], action: L("1 Button"), wii: "1"),
        Entry(targets: ["two"], action: L("2 Button"), wii: "2"),
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
                case .keyboard: text = text.isEmpty ? L("Mouse") : L("Mouse") + " · " + text
                case .switchPro: text = L("Gyro") + (text.isEmpty ? "" : " · " + text)
                default: break
                }
            }
            if entry.targets[0] == "shake" && kind == .switchPro { text += " · " + L("shake the controller") }
            if text.isEmpty { continue }
            rows.append(ControlRow(id: entry.targets[0], action: entry.action, wii: entry.wii, inputs: text))
        }
        return rows
    }

    // Four directions: a whole stick or pad when every direction uses it, else the keys in
    // up-left-down-right order (W A S D, ↑ ← ↓ →).
    private static func directions(_ lists: [[String]], kind: ControllerKind) -> String {
        let groups = lists.map { Set($0.map(group)) }
        if let first = groups.first, !first.contains(.none), groups.allSatisfy({ $0 == first }) {
            return first.sorted { $0.rawValue < $1.rawValue }.map(\.title).joined(separator: " · ")
        }
        guard lists.count == 4 else { return "" }
        let ordered = [lists[0], lists[2], lists[1], lists[3]]
        let firsts = ordered.compactMap { $0.first.map { name($0, kind: kind) } }
        return firsts.joined(separator: " ")
    }

    // The stick or pad a binding belongs to, in display order.
    private enum InputGroup: Int {
        case leftStick, dpad, rightStick, none
        var title: String {
            switch self {
            case .leftStick: return L("Left stick")
            case .dpad: return L("D-pad")
            case .rightStick: return L("Right stick")
            case .none: return ""
            }
        }
    }

    private static func group(_ token: String) -> InputGroup {
        if token.hasPrefix("LX") || token.hasPrefix("LY") { return .leftStick }
        if token.hasPrefix("RX") || token.hasPrefix("RY") { return .rightStick }
        if token.hasPrefix("Dpad") { return .dpad }
        return .none
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
            "Space": L("Space"), "Enter": "Return", "Tab": "Tab", "Backspace": L("Delete"), "Escape": "Esc",
            "MouseLeft": L("Left click"), "MouseRight": L("Right click"), "MouseMiddle": L("Middle click"),
            "Mouse4": L("Mouse button 4"), "Mouse5": L("Mouse button 5"),
            "LShift": L("⇧ Shift"), "RShift": L("⇧ Right Shift"), "Shift": L("⇧ Shift"),
            "LCtrl": L("⌃ Control"), "RCtrl": L("⌃ Right Control"), "Ctrl": L("⌃ Control"),
            "LAlt": L("⌥ Option"), "RAlt": L("⌥ Right Option"), "Alt": L("⌥ Option"),
            "Up": "↑", "Down": "↓", "Left": "←", "Right": "→",
            "Equals": "=", "Minus": "-", "LBracket": "[", "RBracket": "]", "Backslash": "\\",
            "Semicolon": ";", "Quote": "'", "Comma": ",", "Period": ".", "Slash": "/", "Grave": "`",
            "CapsLock": L("Caps Lock"),
        ]
        if let name = names[token] { return name }
        if token.hasPrefix("Num"), token.count == 4 { return L("\(String(token.suffix(1))) (keypad)") }
        return token
    }

    private static func dpad(_ arrow: String) -> String { InputGroup.dpad.title + " " + arrow }

    private static func axisName(_ token: String) -> String? {
        let group = group(token)
        return group == .leftStick || group == .rightStick ? group.title : nil
    }

    private static func xboxName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "LS": L("Left stick click (LS)"), "RS": L("Right stick click (RS)"),
            "Menu": L("Menu ☰"), "View": L("View ⧉"), "Guide": "Xbox",
            "DpadUp": dpad("↑"), "DpadDown": dpad("↓"), "DpadLeft": dpad("←"), "DpadRight": dpad("→"),
        ]
        return names[token] ?? token
    }

    private static func playstationName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "A": "✕", "B": "○", "X": "□", "Y": "△",
            "LB": "L1", "RB": "R1", "LT": "L2", "RT": "R2",
            "LS": L("Left stick click (L3)"), "RS": L("Right stick click (R3)"),
            "Menu": "Options", "View": "Create", "Guide": "PS", "Share": L("Touchpad"),
            "DpadUp": dpad("↑"), "DpadDown": dpad("↓"), "DpadLeft": dpad("←"), "DpadRight": dpad("→"),
        ]
        return names[token] ?? token
    }

    private static func switchName(_ token: String) -> String {
        if let axis = axisName(token) { return axis }
        let names: [String: String] = [
            "LS": L("Left stick click"), "RS": L("Right stick click"),
            "Plus": "+", "Minus": "−", "Home": "HOME", "Capture": L("Capture"),
            "DpadUp": dpad("↑"), "DpadDown": dpad("↓"), "DpadLeft": dpad("←"), "DpadRight": dpad("→"),
        ]
        return names[token] ?? token
    }
}
