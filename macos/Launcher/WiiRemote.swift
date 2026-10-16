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
        L("Open System Settings › Bluetooth."),
        L("Press 1 and 2 on the Wii Remote at the same time, or the red SYNC button under the battery cover, until the LEDs blink."),
        L("When “Nintendo RVL-CNT-01” or “RVL-CNT-01-TR” appears, select Connect."),
    ]
    static let sensorBar = L("Pointing needs a powered sensor bar (USB or battery) next to the display. Connect the Nunchuk before playing. If the Wii Remote disconnects, press one of its buttons, or 1 and 2, to connect it again and the game continues with it.")
    static let bluetoothSettings = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")!
}

// Under the control choices: whether a remote is connected, and the pairing steps on demand.
struct WiiRemoteStatus: View {
    let connected: Bool
    @State private var showingHelp = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            StatusLine(kind: connected ? .success : .info,
                       text: connected ? L("Wii Remote connected. Plug in the Nunchuk before playing.")
                                       : L("Pair the Wii Remote in System Settings › Bluetooth before playing."))
            Spacer()
            Button("How to connect…") { showingHelp = true }
                .popover(isPresented: $showingHelp, arrowEdge: .bottom) { WiiRemotePairingHelp() }
        }
        .font(.callout)
    }
}

struct WiiRemotePairingHelp: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Connect a Wii Remote").font(.headline)
            ForEach(Array(WiiRemoteHelp.steps.enumerated()), id: \.offset) { index, step in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(verbatim: "\(index + 1).").monospacedDigit()
                    Text(step).fixedSize(horizontal: false, vertical: true)
                }
            }
            Text(WiiRemoteHelp.sensorBar)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button("Open Bluetooth settings…") { NSWorkspace.shared.open(WiiRemoteHelp.bluetoothSettings) }
        }
        .font(.callout)
        .padding(16)
        .frame(width: 380)
    }
}
