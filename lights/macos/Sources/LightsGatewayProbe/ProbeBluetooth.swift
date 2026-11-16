import AppKit
import CoreBluetooth
import CryptoKit
import Foundation
import Security
import LightsMenulet

enum ProbeBluetoothAuthorization: String, Sendable {
    case undetermined = "not_determined", allowed, denied, restricted
}
enum ProbePermissionAction: Equatable { case proceed, requestApproval }
enum ProbePermissionPolicy {
    static func action(operation: ProbeOperation, authorization: ProbeBluetoothAuthorization) throws -> ProbePermissionAction {
        switch authorization {
        case .allowed: return .proceed
        case .undetermined:
            if operation == .authorization { return .requestApproval }
            throw ProbeFailure.bluetoothApprovalRequired
        case .denied: throw ProbeFailure.bluetoothDenied
        case .restricted: throw ProbeFailure.bluetoothRestricted
        }
    }
}

struct ProbeApplicationIdentity: Encodable, Sendable {
    static let expectedIdentifier = "dev.achyuth.203lights.probe"
    static let expectedExecutable = "LightsGatewayProbe"
    let bundle_id: String
    let signing_identifier: String
    let executable_sha256: String
    let signature_valid: Bool

    static func validate(bundleExtension: String?, bundleID: String?, executable: String?, packageType: String?,
                         usage: String?, signingIdentifier: String?, signatureValid: Bool) throws {
        guard bundleExtension == "app", bundleID == expectedIdentifier, executable == expectedExecutable,
              packageType == "APPL", let usage, (1...512).contains(usage.utf8.count),
              signingIdentifier == expectedIdentifier, signatureValid else { throw ProbeFailure.diagnosticBundleRequired }
    }

    @MainActor static func current() throws -> Self {
        let bundle = Bundle.main
        guard bundle.bundleURL.pathExtension == "app", bundle.bundleIdentifier == expectedIdentifier,
              let executable = bundle.executableURL else { throw ProbeFailure.diagnosticBundleRequired }
        var running: SecCode?, code: SecStaticCode?, information: CFDictionary?
        guard SecCodeCopySelf([], &running) == errSecSuccess, let running,
              SecCodeCheckValidity(running, [], nil) == errSecSuccess,
              SecCodeCopyStaticCode(running, [], &code) == errSecSuccess, let code,
              SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSStrictValidate), nil) == errSecSuccess,
              SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &information) == errSecSuccess,
              let information else { throw ProbeFailure.diagnosticBundleRequired }
        let signing = (information as NSDictionary)[kSecCodeInfoIdentifier] as? String
        try validate(bundleExtension: bundle.bundleURL.pathExtension, bundleID: bundle.bundleIdentifier,
                     executable: bundle.object(forInfoDictionaryKey: "CFBundleExecutable") as? String,
                     packageType: bundle.object(forInfoDictionaryKey: "CFBundlePackageType") as? String,
                     usage: bundle.object(forInfoDictionaryKey: "NSBluetoothAlwaysUsageDescription") as? String,
                     signingIdentifier: signing, signatureValid: true)
        let attributes = try FileManager.default.attributesOfItem(atPath: executable.path)
        guard let size = attributes[.size] as? NSNumber, (1...67_108_864).contains(size.intValue) else { throw ProbeFailure.diagnosticBundleRequired }
        let bytes = try Data(contentsOf: executable, options: [.mappedIfSafe])
        guard bytes.count == size.intValue else { throw ProbeFailure.diagnosticBundleRequired }
        let fingerprint = Data(SHA256.hash(data: bytes)).map { String(format: "%02x", $0) }.joined()
        // Explicit Bluetooth diagnostic invocations run as this application in
        // the GUI session; ordinary invocation and checks never reach this path.
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate()
        return Self(bundle_id: expectedIdentifier, signing_identifier: signing!, executable_sha256: fingerprint, signature_valid: true)
    }
}

@MainActor enum ProbeReadPreparation {
    static func load(_ options: ProbeOptions, expectedGateway: String, evidence: inout ProbeEvidence,
                     application: () throws -> ProbeApplicationIdentity = { try ProbeApplicationIdentity.current() },
                     authorization: () -> ProbeBluetoothAuthorization = { ProbeBluetoothController.currentAuthorization },
                     loader: (String, String) throws -> ProbeOwnerBundle = { try ProbeOwnerBundle.load($0, expectedGateway: $1) }) throws -> ProbeOwnerBundle {
        guard options.operation == .room, let path = options.bundle else { throw ProbeFailure.arguments }
        if options.transport == .bluetooth {
            evidence.stage = "bluetooth_preflight"
            evidence.diagnostic_app = try application()
            let status = authorization()
            evidence.bluetooth_authorization_before = status.rawValue
            _ = try ProbePermissionPolicy.action(operation: .room, authorization: status)
            evidence.bluetooth_authorization_after = status.rawValue
            evidence.peripheral_id = options.peripheralID?.uuidString.lowercased()
            guard options.peripheralID != nil else { throw ProbeFailure.arguments }
        }
        // This is deliberately after identity and existing-permission checks.
        // No credential is loaded while a Bluetooth permission dialog is pending.
        return try loader(path, expectedGateway)
    }
}

struct ProbeDiscoveredPeripheral: Encodable, Equatable, Sendable {
    let peripheral_id: String
    let rssi: Int?
    let identity_verified = false
}
struct ProbeDiscoveryCollector {
    private(set) var items: [UUID: ProbeDiscoveredPeripheral] = [:]
    mutating func observe(id: UUID, advertisedServices: [UUID], rssi: Int) {
        guard advertisedServices.contains(BluetoothFrameCodec.serviceUUID), items[id] != nil || items.count < 8 else { return }
        items[id] = ProbeDiscoveredPeripheral(peripheral_id: id.uuidString.lowercased(), rssi: (-127...20).contains(rssi) ? rssi : nil)
    }
    var sorted: [ProbeDiscoveredPeripheral] { items.values.sorted { $0.peripheral_id < $1.peripheral_id } }
}

/// Constructing this type is inert. Only the separately authorized permission
/// mode may instantiate a manager while authorization is undetermined.
@MainActor final class ProbeBluetoothController: NSObject, @preconcurrency CBCentralManagerDelegate {
    static var currentAuthorization: ProbeBluetoothAuthorization {
        switch CBManager.authorization {
        case .notDetermined: .undetermined
        case .allowedAlways: .allowed
        case .denied: .denied
        case .restricted: .restricted
        @unknown default: .restricted
        }
    }
    private var manager: CBCentralManager?
    private var permission: CheckedContinuation<ProbeBluetoothAuthorization, Error>?
    private var discovery: CheckedContinuation<[ProbeDiscoveredPeripheral], Error>?
    private var deadline: Task<Void, Never>?, scanEnd: Task<Void, Never>?
    private var generation: UInt64 = 0
    private var scanSeconds: UInt64 = 0
    private var scanning = false
    private var collector = ProbeDiscoveryCollector()

    func authorize(timeoutSeconds: UInt64) async throws -> ProbeBluetoothAuthorization {
        guard (5...180).contains(timeoutSeconds), manager == nil else { throw ProbeFailure.arguments }
        let status = Self.currentAuthorization
        if try ProbePermissionPolicy.action(operation: .authorization, authorization: status) == .proceed { return status }
        let epoch = generation
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                permission = continuation
                manager = CBCentralManager(delegate: self, queue: .main, options: [CBCentralManagerOptionShowPowerAlertKey: false])
                armDeadline(timeoutSeconds)
            }
        } onCancel: { Task { @MainActor [weak self] in if self?.generation == epoch { self?.cancel(CancellationError()) } } }
    }

    func discover(scanSeconds: UInt64, timeoutSeconds: UInt64) async throws -> [ProbeDiscoveredPeripheral] {
        guard (1...15).contains(scanSeconds), (5...30).contains(timeoutSeconds), timeoutSeconds > scanSeconds,
              manager == nil else { throw ProbeFailure.arguments }
        _ = try ProbePermissionPolicy.action(operation: .discovery, authorization: Self.currentAuthorization)
        self.scanSeconds = scanSeconds
        collector = ProbeDiscoveryCollector()
        let epoch = generation
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                discovery = continuation
                manager = CBCentralManager(delegate: self, queue: .main, options: [CBCentralManagerOptionShowPowerAlertKey: false])
                armDeadline(timeoutSeconds)
            }
        } onCancel: { Task { @MainActor [weak self] in if self?.generation == epoch { self?.cancel(CancellationError()) } } }
    }

    func close() { cancel(CancellationError()) }
    private func armDeadline(_ seconds: UInt64) {
        let epoch = generation
        deadline = Task { [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            guard !Task.isCancelled, let self, self.generation == epoch else { return }
            self.cancel(ProbeFailure.deadline)
        }
    }
    private func cleanup() {
        generation &+= 1
        deadline?.cancel(); deadline = nil; scanEnd?.cancel(); scanEnd = nil
        if scanning { manager?.stopScan() }
        scanning = false; manager?.delegate = nil; manager = nil
    }
    private func cancel(_ error: Error) {
        let oldPermission = permission, oldDiscovery = discovery
        permission = nil; discovery = nil; cleanup()
        oldPermission?.resume(throwing: error); oldDiscovery?.resume(throwing: error)
    }
    private func finishDiscovery() {
        let pending = discovery; discovery = nil
        let result = collector.sorted
        cleanup(); pending?.resume(returning: result)
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central === manager else { return }
        let status = Self.currentAuthorization
        if let pending = permission, status != .undetermined {
            permission = nil; cleanup()
            do { _ = try ProbePermissionPolicy.action(operation: .authorization, authorization: status); pending.resume(returning: status) }
            catch { pending.resume(throwing: error) }
            return
        }
        guard discovery != nil else { return }
        do { _ = try ProbePermissionPolicy.action(operation: .discovery, authorization: status) }
        catch { cancel(error); return }
        if central.state == .poweredOn, !scanning {
            scanning = true
            central.scanForPeripherals(withServices: [CBUUID(nsuuid: BluetoothFrameCodec.serviceUUID)], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            let epoch = generation
            scanEnd = Task { [weak self] in
                guard let self else { return }
                try? await Task.sleep(for: .seconds(self.scanSeconds))
                guard !Task.isCancelled, self.generation == epoch else { return }
                self.finishDiscovery()
            }
        } else if ![.unknown, .resetting, .poweredOn].contains(central.state) { cancel(ProbeFailure.bluetoothUnavailable) }
    }
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard central === manager, discovery != nil, scanning else { return }
        let advertised = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []) +
                         (advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] ?? [])
        collector.observe(id: peripheral.identifier, advertisedServices: advertised.compactMap { UUID(uuidString: $0.uuidString) }, rssi: RSSI.intValue)
    }
}

@MainActor enum ProbeBluetoothModes {
    static func perform(_ options: ProbeOptions, evidence: inout ProbeEvidence) async throws {
        guard options.bundle == nil, options.transport == .bluetooth, options.operation != .room else { throw ProbeFailure.arguments }
        evidence.stage = "bluetooth_preflight"
        evidence.diagnostic_app = try ProbeApplicationIdentity.current()
        let before = ProbeBluetoothController.currentAuthorization
        evidence.bluetooth_authorization_before = before.rawValue
        _ = try ProbePermissionPolicy.action(operation: options.operation, authorization: before)
        let controller = ProbeBluetoothController()
        defer { controller.close() }
        do {
            if options.operation == .authorization {
                evidence.stage = "waiting_for_bluetooth_permission"
                let after = try await controller.authorize(timeoutSeconds: options.timeoutSeconds)
                evidence.bluetooth_authorization_after = after.rawValue
                evidence.stage = "bluetooth_permission_available"
            } else {
                evidence.stage = "discovering_lighting_service"
                evidence.discovered_peripherals = try await controller.discover(scanSeconds: options.scanSeconds, timeoutSeconds: options.timeoutSeconds)
                evidence.bluetooth_authorization_after = ProbeBluetoothController.currentAuthorization.rawValue
                evidence.stage = "discovery_complete"
            }
            evidence.outcome = "success"
        } catch {
            evidence.bluetooth_authorization_after = ProbeBluetoothController.currentAuthorization.rawValue
            throw error
        }
    }
}
