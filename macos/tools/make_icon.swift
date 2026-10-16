import AppKit
// usage: make_icon <nebula.png> <out.png>
let args = CommandLine.arguments
guard args.count == 3, let nebula = NSImage(contentsOfFile: args[1]) else { exit(2) }
let size = 1024.0
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let inset = 100.0
let tile = NSRect(x: inset, y: inset, width: size - 2 * inset, height: size - 2 * inset)
let shape = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
NSGraphicsContext.current?.saveGraphicsState()
let shadow = NSShadow(); shadow.shadowBlurRadius = 28; shadow.shadowOffset = NSSize(width: 0, height: -12)
shadow.shadowColor = NSColor.black.withAlphaComponent(0.45); shadow.set()
NSColor.black.setFill(); shape.fill()
NSGraphicsContext.current?.restoreGraphicsState()
shape.addClip()
let gradient = NSGradient(colors: [NSColor(calibratedRed: 0.20, green: 0.10, blue: 0.42, alpha: 1),
                                   NSColor(calibratedRed: 0.05, green: 0.06, blue: 0.20, alpha: 1),
                                   NSColor(calibratedRed: 0.01, green: 0.01, blue: 0.06, alpha: 1)],
                          atLocations: [0, 0.55, 1], colorSpace: .deviceRGB)!
gradient.draw(in: shape, relativeCenterPosition: NSPoint(x: 0, y: 0.1))
var generator = SystemRandomNumberGenerator()
for _ in 0..<160 {
    let x = Double.random(in: tile.minX...tile.maxX, using: &generator)
    let y = Double.random(in: tile.minY...tile.maxY, using: &generator)
    let r = Double.random(in: 1.0...3.4, using: &generator)
    NSColor.white.withAlphaComponent(Double.random(in: 0.25...0.9, using: &generator)).setFill()
    NSBezierPath(ovalIn: NSRect(x: x, y: y, width: r, height: r)).fill()
}
let art = NSRect(x: tile.minX - 40, y: tile.minY - 40, width: tile.width + 80, height: tile.height + 80)
nebula.draw(in: art, from: .zero, operation: .screen, fraction: 1.0)
nebula.draw(in: art, from: .zero, operation: .sourceOver, fraction: 0.55)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
