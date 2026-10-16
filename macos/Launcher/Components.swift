// SPDX-License-Identifier: GPL-3.0-only
//
// Small views shared by the setup assistant, the main window and Settings. Everything uses the
// system's colors and controls, so the launcher follows light and dark mode like any Mac app.

import AppKit
import SwiftUI

// Text for the current language from the app's Localizable.strings. English is the source
// language (the key); es.lproj has the Spanish text. SwiftUI's Text, Button and Label look up
// string literals themselves, so this is for text kept in a String first.
func L(_ text: String.LocalizationValue) -> String { String(localized: text) }

// A page title with its explanation underneath.
struct PageHeader: View {
    let title: String
    var detail: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.title2)
                .fontWeight(.semibold)
            if let detail {
                Text(detail)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// An icon and a line of text for a result or a warning.
struct StatusLine: View {
    enum Kind { case success, failure, warning, info }
    let kind: Kind
    let text: String

    var body: some View {
        Label {
            Text(text).fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: symbol).foregroundStyle(color)
        }
    }

    private var symbol: String {
        switch kind {
        case .success: return "checkmark.circle.fill"
        case .failure: return "xmark.circle.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .info: return "info.circle"
        }
    }

    private var color: Color {
        switch kind {
        case .success: return .green
        case .failure: return .red
        case .warning: return .orange
        case .info: return .secondary
        }
    }
}

// The rows of a key/value summary.
struct SummaryGrid: View {
    let rows: [(String, String)]

    var body: some View {
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 18, verticalSpacing: 7) {
            ForEach(rows, id: \.0) { row in
                GridRow {
                    Text(row.0).foregroundStyle(.secondary).gridColumnAlignment(.trailing)
                    Text(row.1)
                }
            }
        }
    }
}

extension NSImage {
    // The app's own icon (the generic one when run outside the bundle).
    static var appIcon: NSImage { NSApplication.shared.applicationIconImage }
}
