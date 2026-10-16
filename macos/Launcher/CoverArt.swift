// SPDX-License-Identifier: GPL-3.0-only
//
// The game's box art, obtained as Dolphin does: downloaded from GameTDB on the user's Mac and kept
// in a local cache (Dolphin: GameFile::DownloadDefaultCover, art.gametdb.com/wii/cover/<region>/
// <id>.png into User/Cache/GameCovers). Nothing is bundled with the app or stored in the repo.
// The front of the full high-resolution cover is used when GameTDB has it, Dolphin's small cover
// otherwise. A picture of the user's own at Load/GameCovers/RMGE01.png takes precedence.

import AppKit
import Foundation
import SwiftUI

@MainActor
final class CoverArt: ObservableObject {
    static let downloadsKey = "downloadCovers"
    private static let gameID = "RMGE01"
    private static let region = "US"  // Dolphin's GameTDB region code for NTSC-U

    @Published private(set) var image: NSImage?
    private var loading = false

    static var customURL: URL {
        Paths.support.appendingPathComponent("Load/GameCovers/\(gameID).png")
    }
    static var cacheURL: URL {
        Paths.support.appendingPathComponent("Cache/GameCovers/\(gameID).png")
    }
    static var downloadsEnabled: Bool {
        UserDefaults.standard.object(forKey: downloadsKey) as? Bool ?? true
    }

    func load() {
        guard !loading else { return }
        loading = true
        Task {
            await loadNow()
            loading = false
        }
    }

    func loadNow() async {
        if let custom = NSImage(contentsOf: Self.customURL) {
            image = custom
            return
        }
        if let cached = NSImage(contentsOf: Self.cacheURL) {
            image = cached
            return
        }
        guard Self.downloadsEnabled else {
            image = nil
            return
        }
        if let data = await Self.download(), let downloaded = NSImage(data: data) {
            try? Paths.ensure(Self.cacheURL.deletingLastPathComponent())
            try? data.write(to: Self.cacheURL, options: .atomic)
            image = downloaded
        }
    }

    // After the preference changes: forget the cached picture when downloads are turned off.
    func refresh() {
        if !Self.downloadsEnabled { try? FileManager.default.removeItem(at: Self.cacheURL) }
        image = nil
        load()
    }

    // PNG data of the front cover.
    private static func download() async -> Data? {
        if let full = await fetch("https://art.gametdb.com/wii/coverfullHQ/\(region)/\(gameID).png"),
           let front = frontCover(of: full) {
            return front
        }
        return await fetch("https://art.gametdb.com/wii/cover/\(region)/\(gameID).png")
    }

    private static func fetch(_ address: String) async -> Data? {
        guard let url = URL(string: address) else { return nil }
        var request = URLRequest(url: url, timeoutInterval: 15)
        request.setValue("Orbit", forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200,
              NSBitmapImageRep(data: data) != nil else { return nil }
        return data
    }

    // A full cover is back, spine and front side by side; the front is the right-hand part with
    // the proportions of a Wii case front (160:224).
    private static func frontCover(of data: Data) -> Data? {
        guard let source = NSBitmapImageRep(data: data), let cgImage = source.cgImage else { return nil }
        let height = cgImage.height
        let width = Int((Double(height) * 160.0 / 224.0).rounded())
        guard width < cgImage.width,
              let cropped = cgImage.cropping(to: CGRect(x: cgImage.width - width, y: 0, width: width, height: height))
        else { return nil }
        return NSBitmapImageRep(cgImage: cropped).representation(using: .png, properties: [:])
    }
}

// The cover, or the app's icon while there is none.
struct CoverView: View {
    @ObservedObject var cover: CoverArt
    var height: CGFloat = 250

    var body: some View {
        Group {
            if let image = cover.image {
                Image(nsImage: image)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .frame(height: height)
                    .clipShape(RoundedRectangle(cornerRadius: 4))
                    .overlay(RoundedRectangle(cornerRadius: 4).strokeBorder(Color.primary.opacity(0.12)))
                    .shadow(color: .black.opacity(0.18), radius: 6, y: 2)
            } else {
                Image(nsImage: .appIcon)
                    .resizable()
                    .frame(width: 96, height: 96)
                    .frame(width: height * 160 / 224, height: height)
            }
        }
    }
}
