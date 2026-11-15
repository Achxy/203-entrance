import Foundation

@main struct BluetoothFrameChecks {
    static func main() throws {
        var assertions = 0
        func expect(_ value: Bool) {
            precondition(value); assertions += 1
        }
        func rejects(_ operation: () throws -> Void) {
            do { try operation(); preconditionFailure("Expected rejection") } catch { assertions += 1 }
        }
        let golden = try BluetoothFrameCodec.fragments(operation: .session, id: 1, payload: Data([1,2,3]), maximumWriteBytes: 128)
        expect(golden == [Data([1,1,3,0,0,0,0,1,0,3,0,0,1,2,3])])
        rejects { _ = try BluetoothFrameCodec.fragments(operation: .data, id: 0, payload: Data([1]), maximumWriteBytes: 128) }
        rejects { _ = try BluetoothFrameCodec.fragments(operation: .data, id: 1, payload: Data(count: 1025), maximumWriteBytes: 128) }
        rejects { _ = try BluetoothFrameCodec.fragments(operation: .data, id: 1, payload: Data(), maximumWriteBytes: 128) }
        rejects { _ = try BluetoothFrameCodec.fragments(operation: .data, id: 1, payload: Data([1]), maximumWriteBytes: 19) }
        rejects { _ = try BluetoothFrameCodec.fragments(operation: .close, id: 1, payload: Data([1]), maximumWriteBytes: 128) }
        expect(try BluetoothFrameCodec.fragments(operation: .close, id: 1, payload: Data(), maximumWriteBytes: 128).count == 1)
        let payload = Data((0..<1024).map { UInt8(truncatingIfNeeded: $0) })
        for capacity in stride(from: 20, through: 512, by: 7) {
            let frames = try BluetoothFrameCodec.fragments(operation: .data, id: 0x01020304, payload: payload, maximumWriteBytes: capacity)
            var joined = Data()
            for (index, frame) in frames.enumerated() {
                expect(frame.count <= capacity && frame.count > 12)
                expect(Array(frame.prefix(8)) == [1,2,UInt8((index == 0 ? 1 : 0) | (index == frames.count-1 ? 2 : 0)),0,1,2,3,4])
                joined.append(frame.dropFirst(12))
            }
            expect(joined == payload)
        }
        func response(body: Data, id: UInt32 = 5, operation: UInt8 = 0x82, capacity: Int = 125) -> [Data] {
            let bytes = [UInt8](body)
            var output: [Data] = [], offset = 0
            while offset < bytes.count {
                let n = min(capacity-12, bytes.count-offset)
                let flags: UInt8 = (offset == 0 ? 1 : 0) | (offset+n == bytes.count ? 2 : 0)
                var value: [UInt8] = [1,operation,flags,0,UInt8(truncatingIfNeeded:id>>24),UInt8(truncatingIfNeeded:id>>16),UInt8(truncatingIfNeeded:id>>8),UInt8(truncatingIfNeeded:id),UInt8(bytes.count>>8),UInt8(truncatingIfNeeded:bytes.count),UInt8(offset>>8),UInt8(truncatingIfNeeded:offset)]
                value.append(contentsOf: bytes[offset..<(offset+n)]);output.append(Data(value));offset += n
            }
            return output
        }
        for capacity in stride(from: 20, through: 512, by: 7) {
            var assembler = try BluetoothResponseAssembler(operation: .data, id: 5)
            let frames = response(body: Data([0,200])+payload, capacity: capacity)
            for (index, frame) in frames.enumerated() {
                let result = try assembler.consume(frame)
                expect(index == frames.count-1 ? result == payload : result == nil)
            }
            rejects { _ = try assembler.consume(frames.last!) }
        }
        for status: UInt16 in [0,199,201,400,429,500,599,600] {
            var assembler = try BluetoothResponseAssembler(operation: .data, id: 5)
            let packet = response(body: Data([UInt8(status>>8),UInt8(truncatingIfNeeded:status),1]))[0]
            rejects { _ = try assembler.consume(packet) }
        }
        let valid = response(body: Data([0,200,42]))[0]
        for field in [0,1,2,3,4,8,9,10,11] {
            var changed = valid; changed[field] ^= 0x40
            var assembler = try BluetoothResponseAssembler(operation: .data, id: 5)
            rejects { _ = try assembler.consume(changed) }
        }
        for size in 0..<valid.count {
            var assembler = try BluetoothResponseAssembler(operation: .data, id: 5)
            rejects { _ = try assembler.consume(valid.prefix(size)) }
        }
        let split = response(body: Data([0,200])+payload, capacity: 125)
        var outOfOrder = try BluetoothResponseAssembler(operation: .data, id: 5)
        rejects { _ = try outOfOrder.consume(split[1]) }
        var replay = try BluetoothResponseAssembler(operation: .data, id: 5)
        expect(try replay.consume(split[0]) == nil)
        rejects { _ = try replay.consume(split[0]) }
        var close = try BluetoothResponseAssembler(operation: .close, id: 5)
        expect(try close.consume(response(body: Data([0,200]), operation: 0x83)[0]) == Data())
        print("PASS \(assertions) native Bluetooth framing assertions; no radio used")
    }
}
