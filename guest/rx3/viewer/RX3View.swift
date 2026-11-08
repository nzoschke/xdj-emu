// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// RX3View: a bring-up window on the XDJ-RX3 guest's LCD, before the app can
// show it. Maps the rx3fb shared file (guest/modules/rx3_board/rx3fb.c) and
// redraws it about 30 times a second. View only.
//
//   xcrun swiftc -O -o build/rx3/RX3View guest/rx3/viewer/RX3View.swift
//   build/rx3/RX3View build/rx3/run/rx3fb.shm
import AppKit

let path = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "build/rx3/run/rx3fb.shm"

final class LCD {
    let base: UnsafeMutableRawPointer
    let size: Int
    var rgba = [UInt32](repeating: 0, count: 1280 * 800)

    init?(path: String) {
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else { return nil }
        var st = stat()
        fstat(fd, &st)
        size = Int(st.st_size)
        guard let p = mmap(nil, size, PROT_READ, MAP_SHARED, fd, 0), p != MAP_FAILED else { return nil }
        base = p
        close(fd)
    }

    func u32(_ off: Int) -> UInt32 { base.load(fromByteOffset: off, as: UInt32.self) }
    func u8(_ off: Int) -> UInt8 { base.load(fromByteOffset: off, as: UInt8.self) }

    /// The visible frame as an image, or nil before the guest has set a mode.
    func frame() -> CGImage? {
        guard u32(0) == 0x4633_5852 else { return nil }  // "RX3F"
        let gen = u32(8)
        guard gen & 1 == 0 else { return nil }
        let w = Int(u32(12)), h = Int(u32(16)), bpp = Int(u32(20))
        let stride = Int(u32(24)), yoff = Int(u32(28)), pixels = Int(u32(32))
        guard w == 1280, h == 800, bpp == 16 || bpp == 32,
              pixels + (yoff + h) * stride <= size else { return nil }
        let row0 = base + pixels + yoff * stride
        rgba.withUnsafeMutableBufferPointer { out in
            for y in 0..<h {
                let src = row0 + y * stride
                let dst = out.baseAddress! + y * w
                if bpp == 16 {
                    let s = src.assumingMemoryBound(to: UInt16.self)
                    for x in 0..<w {
                        let v = UInt32(s[x])
                        let r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f
                        // RGBA8888 in memory order R, G, B, A (little-endian word).
                        dst[x] = ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8)
                            | (((b << 3) | (b >> 2)) << 16) | 0xff00_0000
                    }
                } else {
                    let s = src.assumingMemoryBound(to: UInt32.self)
                    for x in 0..<w {
                        let v = s[x]  // XRGB8888
                        dst[x] = ((v >> 16) & 0xff) | (v & 0xff00) | ((v & 0xff) << 16) | 0xff00_0000
                    }
                }
            }
        }
        let data = rgba.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true,
                       intent: .defaultIntent)
    }
}

final class View: NSView {
    var lcd: LCD?
    var image: CGImage?

    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()
        guard let image else {
            let s = "Waiting for the XDJ-RX3 guest's framebuffer…" as NSString
            s.draw(at: NSPoint(x: 20, y: 20), withAttributes: [.foregroundColor: NSColor.gray,
                                                             .font: NSFont.systemFont(ofSize: 14)])
            return
        }
        NSGraphicsContext.current?.cgContext.draw(image, in: bounds)
    }

    @objc func tick() {
        if lcd == nil { lcd = LCD(path: path) }
        if let f = lcd?.frame() { image = f }
        needsDisplay = true
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 800),
                      styleMask: [.titled, .closable, .miniaturizable, .resizable],
                      backing: .buffered, defer: false)
window.title = "XDJ-RX3 (dj-emu bring-up viewer)"
window.contentAspectRatio = NSSize(width: 1280, height: 800)
let view = View(frame: window.contentView!.bounds)
view.autoresizingMask = [.width, .height]
window.contentView = view
window.center()
window.makeKeyAndOrderFront(nil)
Timer.scheduledTimer(timeInterval: 1.0 / 30, target: view, selector: #selector(View.tick),
                     userInfo: nil, repeats: true)
final class Delegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
let delegate = Delegate()
app.delegate = delegate
app.activate(ignoringOtherApps: true)
app.run()
