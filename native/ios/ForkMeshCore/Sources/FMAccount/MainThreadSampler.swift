import Foundation
#if canImport(Darwin)
import Darwin
#endif

public enum MainThreadSampler {
    public static let maxFrames = 128

    public static func forMainThread() -> (@Sendable () -> [UInt])? {
        #if canImport(Darwin) && (arch(arm64) || arch(x86_64))
        guard Thread.isMainThread else { return nil }
        let main: pthread_t = pthread_self()
        let port: thread_t = pthread_mach_thread_np(main)
        let top = UInt(bitPattern: pthread_get_stackaddr_np(main))
        return { sample(port, stackTop: top) }
        #else
        return nil
        #endif
    }

    #if canImport(Darwin) && (arch(arm64) || arch(x86_64))
    #if arch(arm64)
    static let addressMask: UInt = 0x0000_000F_FFFF_FFFF
    #else
    static let addressMask: UInt = 0x0000_7FFF_FFFF_FFFF
    #endif

    static func sample(_ thread: thread_t, stackTop: UInt) -> [UInt] {
        if pthread_mach_thread_np(pthread_self()) == thread { return [] }
        let frames = UnsafeMutablePointer<UInt>.allocate(capacity: maxFrames)
        defer { frames.deallocate() }
        guard thread_suspend(thread) == KERN_SUCCESS else { return [] }
        let count = walk(thread, stackTop: stackTop, into: frames)
        thread_resume(thread)
        return Array(UnsafeBufferPointer(start: frames, count: count))
    }

    private static func walk(_ thread: thread_t, stackTop: UInt, into frames: UnsafeMutablePointer<UInt>) -> Int {
        #if arch(arm64)
        var state = arm_thread_state64_t()
        var stateCount = mach_msg_type_number_t(MemoryLayout<arm_thread_state64_t>.size / MemoryLayout<natural_t>.size)
        let result: kern_return_t = withUnsafeMutablePointer(to: &state) { pointer in
            pointer.withMemoryRebound(to: natural_t.self, capacity: Int(stateCount)) { raw in
                thread_get_state(thread, thread_state_flavor_t(ARM_THREAD_STATE64), raw, &stateCount)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let pc = UInt(state.__pc) & addressMask
        let link = UInt(state.__lr) & addressMask
        var frame = UInt(state.__fp) & addressMask
        let stackBottom = UInt(state.__sp) & addressMask
        #else
        var state = x86_thread_state64_t()
        var stateCount = mach_msg_type_number_t(MemoryLayout<x86_thread_state64_t>.size / MemoryLayout<natural_t>.size)
        let result: kern_return_t = withUnsafeMutablePointer(to: &state) { pointer in
            pointer.withMemoryRebound(to: natural_t.self, capacity: Int(stateCount)) { raw in
                thread_get_state(thread, thread_state_flavor_t(x86_THREAD_STATE64), raw, &stateCount)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let pc = UInt(state.__rip) & addressMask
        let link: UInt = 0
        var frame = UInt(state.__rbp) & addressMask
        let stackBottom = UInt(state.__rsp) & addressMask
        #endif
        var count = 0
        if pc != 0 {
            frames[count] = pc
            count += 1
        }
        if link != 0 && link != pc {
            frames[count] = link
            count += 1
        }
        let recordSize = UInt(2 * MemoryLayout<UInt>.size)
        while count < maxFrames, frame >= stackBottom, frame & 7 == 0, frame <= stackTop - recordSize {
            guard let record = UnsafePointer<UInt>(bitPattern: frame) else { break }
            let next = record[0] & addressMask
            let returnAddress = record[1] & addressMask
            if returnAddress == 0 { break }
            if count == 0 || frames[count - 1] != returnAddress {
                frames[count] = returnAddress
                count += 1
            }
            if next <= frame { break }
            frame = next
        }
        return count
    }
    #endif

    public struct Frame: Sendable, Equatable {
        public let image: String
        public let offset: UInt
        public let inApp: Bool

        public init(image: String, offset: UInt, inApp: Bool) {
            self.image = image
            self.offset = offset
            self.inApp = inApp
        }

        public var text: String { inApp ? "\(image) +0x\(String(offset, radix: 16))" : image }
    }

    public static let signatureImages = 3
    public static let signatureAppFrames = 2

    public static func signature(_ frames: [Frame]) -> String {
        var images: [String] = []
        var app: [String] = []
        for frame in frames {
            if frame.inApp {
                app.append(frame.text)
                if app.count == signatureAppFrames { break }
            } else if app.isEmpty, images.count < signatureImages, images.last != frame.image, !images.contains(frame.image) {
                images.append(frame.image)
            }
        }
        if app.isEmpty { app = ["no app frame"] }
        return (images + app).joined(separator: " < ")
    }

    public static func resolve(_ addresses: [UInt], appPath: String = Bundle.main.bundlePath) -> [Frame] {
        addresses.map { (address: UInt) -> Frame in
            #if canImport(Darwin)
            var info = Dl_info()
            guard let pointer = UnsafeRawPointer(bitPattern: address), dladdr(pointer, &info) != 0,
                let file = info.dli_fname
            else { return Frame(image: "???", offset: address, inApp: false) }
            let path = String(cString: file)
            let base = info.dli_fbase.map { UInt(bitPattern: $0) } ?? 0
            return Frame(
                image: URL(fileURLWithPath: path).lastPathComponent, offset: address &- base,
                inApp: !appPath.isEmpty && path.hasPrefix(appPath + "/"))
            #else
            return Frame(image: "???", offset: address, inApp: false)
            #endif
        }
    }
}
