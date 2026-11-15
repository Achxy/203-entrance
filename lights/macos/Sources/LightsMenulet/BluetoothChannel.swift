import CoreBluetooth
import Foundation

/// A single bounded native CoreBluetooth byte channel. Constructing the object
/// does not request permission or start a scan. Call open only in response to
/// the user's nearby-control choice. An advertisement is only a reconnect hint;
/// Security2 and the pinned gateway identity must be verified above this layer.
@MainActor public final class BluetoothByteChannel: NSObject, @preconcurrency CBCentralManagerDelegate,
                                                    @preconcurrency CBPeripheralDelegate {
    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var target: UUID?
    private var receive: CBCharacteristic?
    private var transmit: CBCharacteristic?
    private var opening: CheckedContinuation<Void, any Error>?
    private var pending: CheckedContinuation<Data, any Error>?
    private var deadline: Task<Void, Never>?
    private var fragments: [Data] = []
    private var fragmentIndex = 0
    private var waitingWrite = false
    private var assembler: BluetoothResponseAssembler?
    private var completedReply: Data?
    private var nextID: UInt32 = 0
    private var generation: UInt64 = 0
    private var opened = false

    public override init() { super.init() }

    public func open(peripheralID: UUID) async throws {
        guard central == nil, opening == nil, pending == nil else { throw BluetoothChannelError.inFlight }
        if CBCentralManager.authorization == .denied || CBCentralManager.authorization == .restricted {
            throw BluetoothChannelError.permissionDenied
        }
        try Task.checkCancellation()
        let expectedGeneration = generation
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, any Error>) in
                opening = continuation; target = peripheralID
                central = CBCentralManager(delegate: self, queue: .main,
                                           options: [CBCentralManagerOptionShowPowerAlertKey: false])
                armDeadline(seconds: 12)
            }
        } onCancel: {
            Task { @MainActor [weak self] in
                guard let self, self.generation == expectedGeneration else { return }
                self.invalidate(CancellationError())
            }
        }
    }

    public func exchange(_ operation: BluetoothOperation, payload: Data) async throws -> Data {
        guard pending == nil, opening == nil else { invalidate(BluetoothChannelError.inFlight); throw BluetoothChannelError.inFlight }
        guard opened, let peripheral, receive != nil, transmit?.isNotifying == true else { throw BluetoothChannelError.notOpen }
        guard nextID < UInt32.max else { invalidate(BluetoothChannelError.invalidFrame); throw BluetoothChannelError.invalidFrame }
        try Task.checkCancellation()
        nextID += 1
        let outgoing: [Data]
        do {
            outgoing = try BluetoothFrameCodec.fragments(operation: operation, id: nextID, payload: payload,
                                                          maximumWriteBytes: peripheral.maximumWriteValueLength(for: .withResponse))
            assembler = try BluetoothResponseAssembler(operation: operation, id: nextID)
        } catch { invalidate(error); throw error }
        let expectedGeneration = generation
        let result: Data = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending = continuation; fragments = outgoing; fragmentIndex = 0
                waitingWrite = false; completedReply = nil
                armDeadline(seconds: 10)
                sendNextFragment()
            }
        } onCancel: {
            Task { @MainActor [weak self] in
                guard let self, self.generation == expectedGeneration else { return }
                self.invalidate(CancellationError())
            }
        }
        if operation == .close { invalidate(BluetoothChannelError.disconnected) }
        return result
    }

    public func close() { invalidate(BluetoothChannelError.disconnected) }

    private func armDeadline(seconds: UInt64) {
        deadline?.cancel()
        let expected = generation
        deadline = Task { [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            guard !Task.isCancelled, let self, self.generation == expected else { return }
            self.invalidate(BluetoothChannelError.timeout)
        }
    }

    private func invalidate(_ error: any Error) {
        generation &+= 1; opened = false
        deadline?.cancel(); deadline = nil
        let opening = self.opening, pending = self.pending
        self.opening = nil; self.pending = nil
        central?.stopScan()
        peripheral?.delegate = nil
        if let peripheral { central?.cancelPeripheralConnection(peripheral) }
        central?.delegate = nil
        central = nil; peripheral = nil; target = nil; receive = nil; transmit = nil
        fragments = []; assembler = nil; completedReply = nil; fragmentIndex = 0; waitingWrite = false; nextID = 0
        opening?.resume(throwing: error); pending?.resume(throwing: error)
    }

    private func connect(_ found: CBPeripheral) {
        guard opening != nil, peripheral == nil else { return }
        central?.stopScan(); peripheral = found; found.delegate = self
        central?.connect(found, options: [CBConnectPeripheralOptionNotifyOnDisconnectionKey: false])
    }

    public func centralManagerDidUpdateState(_ manager: CBCentralManager) {
        guard manager === central else { return }
        if manager.state == .poweredOn {
            guard opening != nil, let target else { return }
            if let found = manager.retrievePeripherals(withIdentifiers: [target]).first { connect(found) }
            else { manager.scanForPeripherals(withServices: [CBUUID(nsuuid: BluetoothFrameCodec.serviceUUID)],
                                               options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]) }
        } else if manager.state != .unknown && manager.state != .resetting {
            invalidate(manager.state == .unauthorized ? BluetoothChannelError.permissionDenied : BluetoothChannelError.unavailable)
        }
    }

    public func centralManager(_ manager: CBCentralManager, didDiscover found: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard manager === central, found.identifier == target else { return }
        connect(found)
    }
    public func centralManager(_ manager: CBCentralManager, didConnect found: CBPeripheral) {
        guard manager === central, found === peripheral, opening != nil else { return }
        found.discoverServices([CBUUID(nsuuid: BluetoothFrameCodec.serviceUUID)])
    }
    public func centralManager(_ manager: CBCentralManager, didFailToConnect found: CBPeripheral, error: (any Error)?) {
        guard manager === central, found === peripheral else { return }
        invalidate(error ?? BluetoothChannelError.disconnected)
    }
    public func centralManager(_ manager: CBCentralManager, didDisconnectPeripheral found: CBPeripheral, error: (any Error)?) {
        guard manager === central, found === peripheral else { return }
        invalidate(error ?? BluetoothChannelError.disconnected)
    }
    public func peripheral(_ found: CBPeripheral, didDiscoverServices error: (any Error)?) {
        guard found === peripheral, opening != nil else { return }
        guard error == nil, let service = found.services?.first(where: { $0.uuid == CBUUID(nsuuid: BluetoothFrameCodec.serviceUUID) }) else {
            invalidate(error ?? BluetoothChannelError.unsupported); return
        }
        found.discoverCharacteristics([CBUUID(nsuuid: BluetoothFrameCodec.receiveUUID), CBUUID(nsuuid: BluetoothFrameCodec.transmitUUID)], for: service)
    }
    public func peripheral(_ found: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: (any Error)?) {
        guard found === peripheral, opening != nil, service.uuid == CBUUID(nsuuid: BluetoothFrameCodec.serviceUUID) else { return }
        let rx = service.characteristics?.first { $0.uuid == CBUUID(nsuuid: BluetoothFrameCodec.receiveUUID) }
        let tx = service.characteristics?.first { $0.uuid == CBUUID(nsuuid: BluetoothFrameCodec.transmitUUID) }
        guard error == nil, let rx, let tx, rx.properties.contains(.write), tx.properties.contains(.indicate) else {
            invalidate(error ?? BluetoothChannelError.unsupported); return
        }
        receive = rx; transmit = tx
        found.setNotifyValue(true, for: tx)
    }
    public func peripheral(_ found: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: (any Error)?) {
        guard found === peripheral, characteristic === transmit else { return }
        guard error == nil, characteristic.isNotifying else { invalidate(error ?? BluetoothChannelError.disconnected); return }
        guard let continuation = opening else { return }
        opening = nil; opened = true; deadline?.cancel(); deadline = nil
        continuation.resume()
    }

    private func sendNextFragment() {
        guard pending != nil, !waitingWrite, let peripheral, let receive else { return }
        if fragmentIndex == fragments.count { finishIfComplete(); return }
        waitingWrite = true
        let fragment = fragments[fragmentIndex]; fragmentIndex += 1
        peripheral.writeValue(fragment, for: receive, type: .withResponse)
    }
    public func peripheral(_ found: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: (any Error)?) {
        guard found === peripheral, characteristic === receive, pending != nil, waitingWrite else { return }
        guard error == nil else { invalidate(error!); return }
        waitingWrite = false; sendNextFragment()
    }
    public func peripheral(_ found: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: (any Error)?) {
        guard found === peripheral, characteristic === transmit else { return }
        guard error == nil, pending != nil, fragmentIndex == fragments.count,
              let data = characteristic.value, var current = assembler else {
            invalidate(error ?? BluetoothChannelError.invalidFrame); return
        }
        do {
            completedReply = try current.consume(data)
            assembler = current
            finishIfComplete()
        } catch { invalidate(error) }
    }
    private func finishIfComplete() {
        guard !waitingWrite, fragmentIndex == fragments.count, let result = completedReply, let continuation = pending else { return }
        pending = nil; assembler = nil; completedReply = nil; fragments = []; fragmentIndex = 0
        deadline?.cancel(); deadline = nil
        continuation.resume(returning: result)
    }
}
