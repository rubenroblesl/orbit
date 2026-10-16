// SPDX-License-Identifier: GPL-3.0-only
//
// Wii Remotes paired with this Mac, and how to pair one. The game reads a paired remote through
// the runtime's IOKit HID bridge (runtime/platform/macos/src/wiimote_hid_macos.cpp); the launcher
// only watches which ones are connected and never opens them.

import AppKit
import IOKit.hid
import SwiftUI

@MainActor
final class WiiRemoteWatcher {
    // Nintendo RVL-CNT-01 (Wii Remote) and RVL-CNT-01-TR (Wii Remote Plus).
    static let vendorID = 0x057e
    static let productIDs = [0x0306, 0x0330]

    private let manager: IOHIDManager
    private let onChange: ([String]) -> Void

    init(onChange: @escaping ([String]) -> Void) {
        self.onChange = onChange
        manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let matching = Self.productIDs.map { [kIOHIDVendorIDKey: Self.vendorID, kIOHIDProductIDKey: $0] as [String: Any] }
        IOHIDManagerSetDeviceMatchingMultiple(manager, matching as CFArray)
        let context = Unmanaged.passUnretained(self).toOpaque()
        let changed: IOHIDDeviceCallback = { context, _, _, _ in
            guard let context else { return }
            let watcher = Unmanaged<WiiRemoteWatcher>.fromOpaque(context).takeUnretainedValue()
            Task { @MainActor in watcher.refresh() }
        }
        IOHIDManagerRegisterDeviceMatchingCallback(manager, changed, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, changed, context)
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        refresh()
    }

    // The connected remotes' names, as System Settings shows them.
    func refresh() {
        var names: [String] = []
        if let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> {
            for device in devices {
                let product = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String
                let id = IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? Int
                names.append(product ?? (id == 0x0330 ? "Nintendo RVL-CNT-01-TR" : "Nintendo RVL-CNT-01"))
            }
        }
        onChange(names.sorted())
    }
}

enum WiiRemoteHelp {
    static let steps = [
        "Abre Ajustes del Sistema › Bluetooth.",
        "Pulsa a la vez 1 y 2 en el Wii Remote (o el botón rojo SYNC, bajo la tapa de las pilas) hasta que parpadeen los LED.",
        "Cuando aparezca «Nintendo RVL-CNT-01» (o «RVL-CNT-01-TR»), pulsa Conectar.",
    ]
    static let sensorBar = "Para apuntar hace falta una barra de sensores encendida (USB o a pilas) junto a la pantalla. " +
        "Conecta el Nunchuk antes de jugar. Si el Wii Remote se desconecta, pulsa uno de sus botones (o 1 y 2) " +
        "para volver a conectarlo y el juego lo recupera."
    static let bluetoothSettings = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")!
}

// Under the control choices: whether a remote is connected, and the pairing steps on demand.
struct WiiRemoteStatus: View {
    let connected: Bool
    @State private var showingHelp = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            StatusLine(kind: connected ? .success : .info,
                       text: connected ? "Wii Remote conectado. Enchufa el Nunchuk antes de jugar."
                                       : "Empareja el Wii Remote en Ajustes del Sistema › Bluetooth antes de jugar.")
            Spacer()
            Button("Cómo conectarlo…") { showingHelp = true }
                .popover(isPresented: $showingHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
        }
        .font(.callout)
    }
}

struct WiiRemotePairingHelp: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Conectar un Wii Remote").font(.headline)
            ForEach(Array(WiiRemoteHelp.steps.enumerated()), id: \.offset) { index, step in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text("\(index + 1).").monospacedDigit()
                    Text(step).fixedSize(horizontal: false, vertical: true)
                }
            }
            Text(WiiRemoteHelp.sensorBar)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button("Abrir ajustes de Bluetooth…") { NSWorkspace.shared.open(WiiRemoteHelp.bluetoothSettings) }
        }
        .font(.callout)
        .padding(16)
        .frame(width: 380)
    }
}
