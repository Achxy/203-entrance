import CoreBluetooth
import Foundation

public struct NearbyGateway: Identifiable, Equatable, Sendable {
    public let id: UUID
    public let advertisedName: String
    public let rssi: Int
}

public enum NearbyDiscoveryError: LocalizedError {
    case permissionDenied, unavailable, invalidDuration
    public var errorDescription: String? {
        switch self {
        case .permissionDenied: "Allow Bluetooth for 203 Lights in System Settings to look for the nearby gateway."
        case .unavailable: "Bluetooth is unavailable. Internet control can still be used when the gateway is online."
        case .invalidDuration: "Nearby discovery must have a bounded scan duration."
        }
    }
}

/// Service UUIDs come from the verified firmware contract. Advertisements are
/// untrusted hints. Discovery never supplies credentials or grants access.
/// Constructing this object does not create CBCentralManager or request access.
@MainActor public final class NearbyGatewayDiscovery: NSObject, ObservableObject, @preconcurrency CBCentralManagerDelegate {
    @Published public private(set) var candidates: [NearbyGateway] = []
    @Published public private(set) var isScanning = false
    @Published public private(set) var problem: String?
    private var central: CBCentralManager?
    private var service: CBUUID?
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var deadline: Task<Void, Never>?

    public override init() { super.init() }
    public func start(serviceUUID: UUID, duration: TimeInterval = 10) throws {
        guard (1...15).contains(duration) else { throw NearbyDiscoveryError.invalidDuration }
        if CBCentralManager.authorization == .denied || CBCentralManager.authorization == .restricted {
            throw NearbyDiscoveryError.permissionDenied
        }
        stop()
        candidates = []; peripherals = [:]; problem = nil
        service = CBUUID(nsuuid: serviceUUID)
        if let central { beginIfReady(central) }
        else { central = CBCentralManager(delegate: self, queue: .main, options: [CBCentralManagerOptionShowPowerAlertKey: false]) }
        deadline = Task { [weak self] in
            try? await Task.sleep(for: .seconds(duration))
            guard !Task.isCancelled else { return }
            self?.stop()
        }
    }
    public func stop() {
        deadline?.cancel(); deadline = nil
        central?.stopScan(); isScanning = false; service = nil
    }
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { beginIfReady(central) }
        else if central.state != .unknown && central.state != .resetting {
            problem = central.state == .unauthorized ? NearbyDiscoveryError.permissionDenied.localizedDescription : NearbyDiscoveryError.unavailable.localizedDescription
            stop()
        }
    }
    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard isScanning, candidates.count < 8 || peripherals[peripheral.identifier] != nil else { return }
        let advertised = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? "Nearby gateway"
        let name = String(advertised.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) }.prefix(80))
        let candidate = NearbyGateway(id: peripheral.identifier, advertisedName: name, rssi: RSSI.intValue)
        peripherals[peripheral.identifier] = peripheral
        if let index = candidates.firstIndex(where: { $0.id == candidate.id }) { candidates[index] = candidate }
        else { candidates.append(candidate) }
    }
    private func beginIfReady(_ central: CBCentralManager) {
        guard let service, central.state == .poweredOn else { return }
        central.scanForPeripherals(withServices: [service], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        isScanning = true
    }
}
