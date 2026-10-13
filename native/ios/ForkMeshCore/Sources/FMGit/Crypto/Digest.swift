public protocol GitDigest: AnyObject {
    func add(_ bytes: [UInt8]) throws
    func close() throws -> [UInt8]
}

@inline(__always)
public func rotateLeft32(_ value: UInt32, _ bits: UInt32) -> UInt32 {
    (value << bits) | (value >> (32 - bits))
}

@inline(__always)
public func rotateRight32(_ value: UInt32, _ bits: UInt32) -> UInt32 {
    (value >> bits) | (value << (32 - bits))
}
