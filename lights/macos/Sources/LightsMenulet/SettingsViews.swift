import AppKit
import ServiceManagement
import SwiftUI
import UniformTypeIdentifiers

struct AccessForm: View {
    @ObservedObject var model: MenuletModel
    var compact = false
    @State private var credential = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Your room, one click away.").font(compact ? .headline : .title2).fontWeight(.semibold)
            Text("Use the room's guest password, or your private owner key. Your Mac stays on its current Internet connection.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            LabeledContent("Room", value: "203")
            DisclosureGroup("Advanced room invitation") {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Room invitation or address").font(.caption).foregroundStyle(.secondary)
                    TextField("Room invitation or address", text: $model.endpoint, prompt: Text("Provided by the owner"))
                        .textContentType(.URL).autocorrectionDisabled()
                    Text("Room 203's public identity is included. Another room requires its own owner-verified identity; changing this address does not replace that trust.")
                        .font(.caption).foregroundStyle(.secondary)
                }.padding(.top, 6)
            }
            TextField("Your Name (Optional)", text: $model.displayName)
            Picker("Access", selection: $model.accessRole) {
                ForEach(AccessRole.allCases) { Text($0.title).tag($0) }
            }.pickerStyle(.segmented)
            SecureField(model.accessRole == .owner ? "Owner Key" : "Guest Password", text: $credential)
            Toggle("Remember access on this Mac", isOn: $model.rememberAccess)
                .disabled(!model.canConnect || model.isPreview)
            Text(model.accessRole == .guest ? "After verification, saves the guest password and this Mac's individual grant in Keychain for secure reconnection. Nothing syncs through iCloud." : "After verification, saves the owner credential in this Mac's Keychain. Nothing syncs through iCloud.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                Button(model.requiresExplicitRejoin && model.accessRole == .guest ? "Rejoin with Current Password" : "Connect to Room") {
                    let secret = credential
                    credential = ""
                    let explicitRejoin = model.requiresExplicitRejoin && model.accessRole == .guest
                    Task { await model.connect(credential: secret, explicitlyRejoin: explicitRejoin) }
                }.buttonStyle(.borderedProminent).disabled(!model.canConnect || credential.isEmpty || model.endpoint.isEmpty || model.connection == .connecting)
                if !model.isPreview && !(model.requiresExplicitRejoin && model.accessRole == .guest) {
                    Button("Use Saved Access") {
                        credential = ""; model.rememberAccess = true
                        Task { await model.connect(credential: "") }
                    }.disabled(!model.canConnect || model.endpoint.isEmpty || model.connection == .connecting || model.needsCredentialCleanup)
                    .help("Use the access you previously saved in this Mac's Keychain. No new guest grant is created after revocation.")
                }
            }
            if model.requiresExplicitRejoin && model.accessRole == .guest {
                Text("Rejoin creates new guest access using the current password. It never restores the revoked grant automatically.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let reason = model.unavailableReason {
                Label(reason, systemImage: "info.circle").font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            FeedbackView(model: model)
            if model.needsCredentialCleanup {
                Button("Retry Removing Saved Access") { Task { await model.signOut() } }
            }
        }.textFieldStyle(.roundedBorder)
    }
}

public struct MenuletSettings: View {
    @ObservedObject private var model: MenuletModel
    @State private var loginEnabled = SMAppService.mainApp.status == .enabled
    @State private var loginProblem: String?
    @StateObject private var discovery = NearbyGatewayDiscovery()
    @State private var bluetoothProblem: String?
    public init(model: MenuletModel) { self.model = model }
    public var body: some View {
        TabView {
            Form {
                if model.isPreview { PreviewNotice() }
                Section("Appearance") {
                    Picker("Appearance", selection: $model.appearance) {
                        Text("System").tag("system"); Text("Light").tag("light"); Text("Dark").tag("dark")
                    }
                }
                Section("Startup") {
                    Toggle("Open at Login", isOn: Binding(get: { loginEnabled }, set: { enabled in
                        guard !model.isPreview else { return }
                        do {
                            if enabled { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
                            loginEnabled = SMAppService.mainApp.status == .enabled
                            loginProblem = SMAppService.mainApp.status == .requiresApproval ? "Enable 203 Lights in Login Items in System Settings." : nil
                        } catch { loginProblem = error.localizedDescription }
                    })).disabled(model.isPreview)
                    if model.isPreview { Text("Preview mode does not change Login Items.").font(.caption).foregroundStyle(.secondary) }
                    if let loginProblem { Text(loginProblem).font(.caption).foregroundStyle(.secondary) }
                }
                Section("About") {
                    LabeledContent("Version", value: "0.1.0 development")
                    Text("A native controller for room lighting. The entrance display and games are managed separately.")
                        .foregroundStyle(.secondary)
                }
            }.formStyle(.grouped).tabItem { Label("General", systemImage: "gearshape") }

            Form {
                Section("Connection") {
                    LabeledContent("Status", value: model.connection.title)
                    Picker("Preferred Connection", selection: $model.connectionPreference) {
                        ForEach(ConnectionPreference.allCases) { Text($0.title).tag($0) }
                    }
                    Text("Automatic uses an authenticated Internet or nearby Bluetooth path. It never switches your Mac's Wi-Fi network.")
                        .font(.callout).foregroundStyle(.secondary)
                    if let reason = model.unavailableReason { Text(reason).font(.caption).foregroundStyle(.secondary) }
                }
                Section("Privacy") {
                    Text(model.canConnect ? "Bluetooth permission is requested when you choose nearby discovery. A discovered name does not grant trust; the room's pinned identity is verified before access." : "Bluetooth permission will be requested when a configured nearby adapter needs it. This development build does not start connections.")
                        .font(.callout).foregroundStyle(.secondary)
                    Text("Guest credentials never provide access to the display's OTA controls.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Nearby Gateway") {
                    HStack {
                        Button(discovery.isScanning ? "Stop Looking" : "Find Nearby Gateway") {
                            if discovery.isScanning { discovery.stop() }
                            else {
                                do { try discovery.start(serviceUUID: BluetoothFrameCodec.serviceUUID); bluetoothProblem = nil }
                                catch { bluetoothProblem = error.localizedDescription }
                            }
                        }.disabled(model.isPreview || !model.canConnect)
                        if discovery.isScanning { ProgressView().controlSize(.small) }
                    }
                    ForEach(discovery.candidates) { candidate in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(candidate.advertisedName).lineLimit(1)
                                Text("Identity not yet verified · \(candidate.rssi) dBm").font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button(model.nearbyPeripheralID == candidate.id ? "Selected" : "Use") {
                                model.nearbyPeripheralID = candidate.id; discovery.stop()
                            }.disabled(model.nearbyPeripheralID == candidate.id)
                        }
                    }
                    if let problem = bluetoothProblem ?? discovery.problem { Text(problem).font(.caption).foregroundStyle(.secondary) }
                    if model.nearbyPeripheralID != nil { Text("Nearby selection is a connection hint. The room's identity will still be verified before control.").font(.caption).foregroundStyle(.secondary) }
                }
            }.formStyle(.grouped).tabItem { Label("Connection", systemImage: "network") }

            Form {
                if let snapshot = model.snapshot {
                    Section("Your Access") {
                        LabeledContent("Room", value: snapshot.name)
                        LabeledContent("Role", value: snapshot.role.title)
                        Button(model.isOwner ? "Manage Room…" : "Owner Access…") { NativeWindowPresenter.shared.showOwner(model: model) }
                        Button("Sign Out", role: .destructive) { Task { await model.signOut() } }
                    }
                    Text("Display names are self-asserted. Only the gateway's authenticated grant determines access.")
                        .font(.caption).foregroundStyle(.secondary)
                } else { AccessForm(model: model) }
            }.formStyle(.grouped).tabItem { Label("Access", systemImage: "person.crop.circle") }
        }.padding(16).frame(width: 520, height: 460).onDisappear { discovery.stop() }
    }
}

public enum OwnerSection: String, CaseIterable, Identifiable {
    case lights = "Lights", guests = "Guest Access", sessions = "Sessions", verification = "Local Verification", setup = "Add a Light"
    public var id: Self { self }
    var symbol: String {
        switch self { case .lights: "lightbulb.2"; case .guests: "lock.shield"; case .sessions: "person.2"; case .verification: "checkmark.shield"; case .setup: "plus.circle" }
    }
}

public struct OwnerWindow: View {
    @ObservedObject private var model: MenuletModel
    @State private var section: OwnerSection? = .lights
    @State private var pendingRemoval: String?
    public init(model: MenuletModel, section: OwnerSection = .lights) {
        self.model = model
        _section = State(initialValue: section)
    }
    public var body: some View {
        Group {
            if model.isOwner {
                NavigationSplitView {
                    List(OwnerSection.allCases, selection: $section) { item in Label(item.rawValue, systemImage: item.symbol).tag(item) }
                        .navigationSplitViewColumnWidth(min: 150, ideal: 175, max: 220)
                } detail: {
                    VStack(alignment: .leading, spacing: 12) {
                        if model.isPreview { PreviewNotice().padding([.horizontal, .top]) }
                        switch section ?? .lights {
                        case .lights: lights
                        case .guests: GuestPolicyView(model: model)
                        case .sessions: sessions
                        case .verification: LocalVerificationView(model: model)
                        case .setup: CommissioningView(model: model)
                        }
                        FeedbackView(model: model).padding([.horizontal, .bottom])
                    }.navigationTitle(section?.rawValue ?? "Manage Room")
                }
            } else {
                VStack(alignment: .leading, spacing: 16) {
                    Label("Owner Access", systemImage: "key").font(.title2)
                    Text("The owner key can manage lights and revoke guest access. It is separate from the guest password.")
                        .foregroundStyle(.secondary)
                    AccessForm(model: model)
                }.padding(28).onAppear { model.accessRole = .owner }
            }
        }.frame(minWidth: 660, idealWidth: 740, minHeight: 470, idealHeight: 560)
            .confirmationDialog("Remove this light from the room?", isPresented: Binding(get: { pendingRemoval != nil }, set: { if !$0 { pendingRemoval = nil } }), titleVisibility: .visible) {
                Button("Remove Light", role: .destructive) {
                    if let id = pendingRemoval { Task { await model.ownerAction(.removeLight(id)) } }
                    pendingRemoval = nil
                }
                Button("Cancel", role: .cancel) { pendingRemoval = nil }
            } message: { Text("This removes app control. It does not factory-reset the bulb.") }
    }
    private var lights: some View {
        List {
            ForEach(model.snapshot?.lights ?? []) { light in
                HStack {
                    Image(systemName: "lightbulb").font(.title3)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(light.name).font(.headline)
                        Text(capabilities(light)).font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text(light.reachable ? "Available" : "Not responding").font(.caption).foregroundStyle(.secondary)
                    Button("Remove…", role: .destructive) { pendingRemoval = light.id }.disabled(model.busy || !model.connection.usable || !model.supportsOwner(.remove))
                }.padding(.vertical, 5)
            }
            Text("Capabilities are supplied by the gateway after discovery. Unsupported controls are not sent to a bulb.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
    private var sessions: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Guest Sessions").font(.title2)
                Spacer()
                Button("Revoke All", role: .destructive) { Task { await model.ownerAction(.revokeAllGuests) } }
                    .disabled(model.busy || !model.connection.usable || !model.supportsOwner(.revoke) || model.snapshot?.sessions.isEmpty != false)
            }.padding(.horizontal)
            if model.snapshot?.sessions.isEmpty != false {
                ContentUnavailableView("No Guest Sessions", systemImage: "person.2", description: Text("Guests appear here after the gateway grants them access."))
            } else {
                List(model.snapshot?.sessions ?? []) { session in
                    HStack {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(session.name).font(.headline)
                            Text(session.device.map { "\($0) · Self-asserted name" } ?? "Self-asserted name").font(.caption).foregroundStyle(.secondary)
                            if let lastActive = session.lastActive {
                                Text("Active \(lastActive.formatted(.relative(presentation: .named)))").font(.caption).foregroundStyle(.secondary)
                            } else { Text(session.revoked ? "Revoked" : session.expired ? "Expired" : "Activity time not reported").font(.caption).foregroundStyle(.secondary) }
                        }
                        Spacer()
                        Button("Revoke", role: .destructive) { Task { await model.ownerAction(.revokeSession(session.id)) } }
                            .disabled(model.busy || !model.connection.usable || !model.supportsOwner(.revoke) || session.revoked || session.expired)
                    }.padding(.vertical, 5)
                }
            }
        }
    }
    private func capabilities(_ light: Light) -> String {
        var parts: [String] = []
        if light.capabilities.brightness { parts.append("Dimmable") }
        if let range = light.capabilities.kelvinRange { parts.append("\(range.lowerBound)–\(range.upperBound) K") }
        if light.capabilities.rgb { parts.append("Color") }
        if !light.capabilities.supportedSceneIDs.isEmpty { parts.append("\(light.capabilities.supportedSceneIDs.count) scenes") }
        return parts.isEmpty ? "Power control" : parts.joined(separator: " · ")
    }
}

struct GuestPolicyView: View {
    @ObservedObject var model: MenuletModel
    @State private var minutes = 15
    @State private var password = ""
    @State private var confirmation = ""
    var body: some View {
        Form {
            Section("Guest Controls") {
                Text(model.snapshot?.policy.guestRestriction(at: Date()) ?? "Guests can change light settings.").foregroundStyle(.secondary)
                Text("These controls apply to 203 Lights and the website. See Local Verification for the bulb Wi-Fi access boundary.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker("Duration", selection: $minutes) {
                    Text("15 minutes").tag(15); Text("30 minutes").tag(30); Text("1 hour").tag(60)
                }
                HStack {
                    Button("Pause Guests") { Task { await model.ownerAction(.freezeGuests(until: Date().addingTimeInterval(Double(minutes * 60)))) } }
                    Button("Resume Now") { Task { await model.ownerAction(.freezeGuests(until: nil)) } }
                }.disabled(model.busy || !model.connection.usable || !model.supportsOwner(.freeze))
            }
            Section("Owner Priority") {
                Text("Keep your current settings while guests remain connected. The hold expires automatically.").foregroundStyle(.secondary)
                HStack {
                    Button("Keep My Settings for \(minutes) Minutes") { Task { await model.ownerAction(.holdSettings(until: Date().addingTimeInterval(Double(minutes * 60)))) } }
                    Button("Release Hold") { Task { await model.ownerAction(.holdSettings(until: nil)) } }
                }.disabled(model.busy || !model.connection.usable || !model.supportsOwner(.holdSettings))
                if !model.supportsOwner(.holdSettings) {
                    Text("A separate timed hold is not supported by this gateway. Owner changes still take precedence.").font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Change Guest Password") {
                SecureField("New Password", text: $password)
                SecureField("Confirm Password", text: $confirmation)
                Text("Use at least 12 characters. The owner key is not changed.").font(.caption).foregroundStyle(.secondary)
                Text("Changing this password revokes every existing guest grant. Guests must explicitly rejoin using the new password.")
                    .font(.caption).foregroundStyle(.secondary)
                Button("Change Password") {
                    let value = password; password = ""; confirmation = ""
                    Task { await model.ownerAction(.rotateGuestPassword(value)) }
                }.disabled(model.busy || !model.connection.usable || !model.supportsOwner(.rotatePassword) || password.utf8.count < 12 || password.utf8.count > 128 || password != confirmation)
            }
        }.formStyle(.grouped)
    }
}

struct CommissioningView: View {
    @ObservedObject var model: MenuletModel
    @State private var selected: Int?
    @State private var name = ""
    @State private var confirmedPhysicalLight = false
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Label("Add a WiZ Light", systemImage: "lightbulb").font(.title2)
                Text("Discover a bulb already joined to the gateway's private Wi-Fi. Its capabilities are read from the bulb.").foregroundStyle(.secondary)
                GroupBox {
                    VStack(alignment: .leading, spacing: 12) {
                        Label("Temporary development setup", systemImage: "info.circle")
                        Text("Standalone bulb provisioning is still being developed. The current lab route uses WiZ-supported pairing and a local key export. Daily control does not use a WiZ account or WiZ servers.")
                            .font(.callout).foregroundStyle(.secondary)
                        Text("The gateway's private Wi-Fi does not forward Internet access. This can block official-app pairing; complete a supported setup before discovery. This app does not yet reset or transfer Wi-Fi settings to a bulb.")
                            .font(.callout).foregroundStyle(.secondary)
                        Text("Import the local-integration key in Local Verification. Gateway commands require that signing key; unsigned fallback stays off. This does not change which other clients the bulb accepts.")
                            .font(.caption).foregroundStyle(.secondary)
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(5)
                }
                HStack {
                    Button("Discover Lights") {
                        selected = nil; confirmedPhysicalLight = false
                        Task { await model.discoverLights() }
                    }.disabled(!model.canCommission || model.busy)
                    if model.discoveringLights {
                        ProgressView().controlSize(.small)
                        Button("Stop") { model.stopDiscoveringLights() }
                    }
                }
                if model.isPreview { Text("Preview mode does not discover or pair real bulbs.").font(.caption).foregroundStyle(.secondary) }
                else if !model.canCommission { Text("Connect with owner access to discover lights.").font(.caption).foregroundStyle(.secondary) }
                if let message = model.commissioningMessage { Text(message).font(.callout).foregroundStyle(.secondary) }
                ForEach(model.discoveredLights) { candidate in
                    Button {
                        selected = candidate.id; confirmedPhysicalLight = false
                        if name.isEmpty { name = "New Light" }
                    } label: {
                        HStack {
                            Image(systemName: selected == candidate.id ? "checkmark.circle.fill" : "circle")
                            VStack(alignment: .leading, spacing: 4) {
                                Text(candidate.module.isEmpty ? "WiZ Light" : candidate.module).fontWeight(.medium)
                                Text(candidate.mac).font(.caption.monospaced())
                                Text(candidate.identified ? "Discovered · local signing key required" : "Identification is incomplete")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                        }.padding(10).contentShape(Rectangle())
                    }.buttonStyle(.plain).disabled(model.busy || !candidate.identified)
                }
                if let selected, model.discoveredLights.contains(where: { $0.id == selected }) {
                    GroupBox("Name and confirm") {
                        VStack(alignment: .leading, spacing: 12) {
                            TextField("Light Name", text: $name)
                            Toggle("This is the physical light I intended to add", isOn: $confirmedPhysicalLight)
                            Button("Add with Signed Commands") {
                                Task { await model.adoptLight(candidateID: selected, name: name) }
                            }.disabled(model.busy || !confirmedPhysicalLight || name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                            Text("After adding, check that the actual light responds. Removing a room entry and factory-resetting a bulb are separate actions.")
                                .font(.caption).foregroundStyle(.secondary)
                        }.padding(6)
                    }
                }
            }.padding(20)
        }.onDisappear { model.stopDiscoveringLights() }
    }
}

struct LocalVerificationView: View {
    @ObservedObject var model: MenuletModel
    @State private var homeKey = ""
    @State private var showImporter = false
    @State private var importProblem: String?
    private var decodedKey: Data? {
        let value = homeKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.utf8.count == 32, value.utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }) else { return nil }
        var bytes = Data()
        var cursor = value.startIndex
        while cursor < value.endIndex {
            let next = value.index(cursor, offsetBy: 2)
            guard let byte = UInt8(value[cursor..<next], radix: 16) else { return nil }
            bytes.append(byte); cursor = next
        }
        return bytes
    }
    var body: some View {
        Form {
            Section("Signed Local Commands") {
                Text("The local-integration key lets the gateway sign commands to supported bulbs. It is different from the room's owner key and guest password. Bulb responses remain unverified LAN reports.")
                    .foregroundStyle(.secondary)
                Label {
                    Text("Bulb-side access is not verified. With WiZ set to “Allow all controls,” other clients on the bulb Wi-Fi may bypass guest pauses and revocations in this app or website. Importing a key does not change that setting.")
                        .fixedSize(horizontal: false, vertical: true)
                } icon: { Image(systemName: "info.circle") }
                .font(.callout)
                Text("Development bootstrap: importing an official WiZ export is temporary. Standalone provisioning is not yet available; daily control has no WiZ account dependency.")
                    .font(.caption).foregroundStyle(.secondary)
                Button(model.isPreview ? "Preview JSON Import…" : "Import WiZ Export JSON…", systemImage: "doc.badge.plus") { showImporter = true }
                    .disabled(model.busy || !model.connection.usable || !model.supportsOwner(.importHomeKey))
                Text("Choose the local JSON file exported by WiZ. Only its local-integration key is imported; device capabilities must still be discovered and verified. No URL is opened.")
                    .font(.caption).foregroundStyle(.secondary)
                if let importProblem { Text(importProblem).font(.caption).foregroundStyle(.secondary) }
                DisclosureGroup("Enter a key manually") {
                    SecureField("WiZ Local Key (32 Hex Characters)", text: $homeKey)
                    Button(model.isPreview ? "Preview Key Import" : "Import Key to Gateway") {
                        guard let value = decodedKey else { return }
                        homeKey = ""
                        Task { await model.ownerAction(.importHomeKey(value)) }
                    }.disabled(decodedKey == nil || model.busy || !model.connection.usable || !model.supportsOwner(.importHomeKey))
                }
                Text("The entry is cleared when submitted. The client does not remember this WiZ key; a real import is encrypted to the authenticated gateway.")
                    .font(.caption).foregroundStyle(.secondary)
                if !model.supportsOwner(.importHomeKey) {
                    Text("Key import is not available with this gateway connection.").font(.caption).foregroundStyle(.secondary)
                }
            }
        }.formStyle(.grouped)
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.json], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let files):
                guard let url = files.first else { return }
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                do {
                    let value = try WiZHomeImport.key(fromLocalFile: url)
                    importProblem = nil; homeKey = ""
                    Task { await model.ownerAction(.importHomeKey(value)) }
                } catch let error as WiZHomeImportError { importProblem = error.localizedDescription }
                catch { importProblem = "The export could not be read. Choose a local WiZ JSON file and try again." }
            case .failure(let error):
                if let error = error as? CocoaError, error.code == .userCancelled { return }
                importProblem = "The export could not be opened. Choose a local WiZ JSON file and try again."
            }
        }
    }
}
