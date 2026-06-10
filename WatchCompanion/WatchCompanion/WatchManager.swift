//
//  WatchManager.swift
//  WatchCompanion
//
//  CoreBluetooth client for the ESP32-S3-WatchFace.
//
//  The UUIDs and the write payload format below MUST stay in sync with the watch
//  firmware (ESP32-S3-WatchFace/ble_provision.h):
//    * Service  6b1f0001…  — advertised; we scan for ONLY this so other BLE
//                            devices never show up.
//    * Char     6b1f0002…  — WiFi provisioning. Write-only, requires an
//                            ENCRYPTED + AUTHENTICATED link. Writing to it makes
//                            iOS start pairing: a system dialog asks for the
//                            6-digit code the WATCH displays. Payload is the UTF-8
//                            string "SSID,password" (split on the FIRST comma, so
//                            the password may contain commas, the SSID may not).
//    * Char     6b1f0003…  — find-phone notify (future "Find My Phone" app). We
//                            discover it but don't use it yet.
//

import Foundation
import CoreBluetooth
import AVFoundation
import AudioToolbox
import UserNotifications

/// A watch found while scanning.
struct DiscoveredWatch: Identifiable {
    let id: UUID                 // peripheral.identifier (stable per iOS device)
    let peripheral: CBPeripheral?  // nil only for DEBUG demo seeding (CBPeripheral has no public init)
    var name: String
    var rssi: Int
}

final class WatchManager: NSObject, ObservableObject {

    // MARK: GATT contract — keep in sync with ble_provision.h
    static let serviceUUID    = CBUUID(string: "6b1f0001-9a3e-4c7a-9b2d-2f1a8c5e7d10")
    static let provUUID       = CBUUID(string: "6b1f0002-9a3e-4c7a-9b2d-2f1a8c5e7d10")
    static let findUUID       = CBUUID(string: "6b1f0003-9a3e-4c7a-9b2d-2f1a8c5e7d10")  // find-phone (notify, watch→phone)
    static let findWatchUUID  = CBUUID(string: "6b1f0004-9a3e-4c7a-9b2d-2f1a8c5e7d10")  // find-watch (write, phone→watch)

    enum Phase: Equatable {
        case bluetoothOff      // radio off / resetting
        case unauthorized      // user denied Bluetooth permission
        case idle              // ready, not scanning
        case scanning
        case connecting
        case connected         // services + provisioning characteristic ready
        case sending           // a credential write is in flight
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var watches: [DiscoveredWatch] = []
    @Published private(set) var connectedName: String?
    @Published var statusMessage: String?
    /// nil = no result yet, true/false = last sendWiFi outcome (drives the UI banner).
    @Published var lastSendSucceeded: Bool?
    /// Raised when a connect/encryption failure looks like a stale iOS bond: the
    /// phone still holds pairing keys the watch no longer has (watch reflashed /
    /// bonds cleared / 3-bond cap evicted it). iOS has no API to drop a bond, so
    /// the UI uses this to tell the user to "Forget This Device" in Settings.
    @Published var showPairingRecovery = false

    /// True while the phone is actively ringing from a watch "Find My Phone" ping.
    @Published private(set) var isRinging = false
    /// True once the connected watch exposes the find-watch characteristic (i.e. its
    /// firmware supports being pinged). Gates the "Find My Watch" button.
    @Published private(set) var canFindWatch = false

    private var central: CBCentralManager!
    private var connected: CBPeripheral?
    private var provChar: CBCharacteristic?
    private var findChar: CBCharacteristic?        // find-phone notify (6b1f0003…)
    private var findWatchChar: CBCharacteristic?   // find-watch write (6b1f0004…)
    private var ringPlayer: AVAudioPlayer?
    private var vibrateTimer: Timer?
    /// True only while a send/pairing attempt (sendWiFi) is in flight. A drop or
    /// write error during that window is a pairing failure (possible stale bond);
    /// a drop at any other time — e.g. the watch disconnecting itself to sleep — is
    /// just a normal goodbye and must NOT trigger the recovery message.
    private var pairingInFlight = false

    // MARK: Auto-connect
    private static let lastWatchKey = "lastWatchUUID"
    /// The watch we auto-connect to (held so a standing connect request can be re-armed).
    private var autoPeripheral: CBPeripheral?
    /// Set when the user explicitly disconnects, so auto-connect doesn't immediately
    /// fight them and reconnect; cleared on the next manual connect or toggle.
    private var userStopped = false

    /// Driven by AppSettings. When turned on, immediately tries to (re)connect; when
    /// off, cancels any standing/pending connection attempt.
    var autoConnect = false {
        didSet {
            guard autoConnect != oldValue else { return }
            if autoConnect { userStopped = false; tryAutoConnect() }
            else { cancelAutoConnect() }
        }
    }

    /// Identifier of the last watch we connected to (CoreBluetooth can re-resolve it
    /// without scanning, enabling a standing reconnect that fires when it reappears).
    private var lastWatchUUID: UUID? {
        get { UserDefaults.standard.string(forKey: Self.lastWatchKey).flatMap(UUID.init) }
        set { UserDefaults.standard.set(newValue?.uuidString, forKey: Self.lastWatchKey) }
    }

    override init() {
        super.init()
        #if DEBUG
        // Demo seam: `WC_DEMO=scan|provision` lets the UI be viewed in the
        // Simulator (which has no Bluetooth radio). Skips the real central so
        // its state updates don't override the seeded demo state.
        if let demo = ProcessInfo.processInfo.environment["WC_DEMO"] {
            applyDemo(demo)
            return
        }
        #endif
        central = CBCentralManager(delegate: self, queue: .main)
    }

    #if DEBUG
    private func applyDemo(_ mode: String) {
        switch mode {
        case "provision":
            connectedName = "ESP32-S3-WatchFace"
            phase = .connected
            canFindWatch = true
        default: // "scan"
            watches = [
                DiscoveredWatch(id: UUID(), peripheral: nil, name: "ESP32-S3-WatchFace", rssi: -52),
            ]
            phase = .idle
        }
    }
    #endif

    /// True once we're connected and the provisioning characteristic is ready.
    var isConnected: Bool { phase == .connected || phase == .sending }

    // MARK: Scanning

    func startScan() {
        guard central?.state == .poweredOn else { return }
        watches.removeAll()
        phase = .scanning
        // Filtering by service UUID means iOS only reports the watch, never other devices.
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    func stopScan() {
        central?.stopScan()
        if phase == .scanning { phase = .idle }
    }

    // MARK: Connection

    func connect(_ watch: DiscoveredWatch) {
        guard let peripheral = watch.peripheral else { return }  // nil only in DEBUG demo
        userStopped = false
        beginConnect(peripheral)
    }

    func disconnect() {
        pairingInFlight = false
        userStopped = true                 // don't let auto-connect immediately reconnect
        cancelAutoConnect()
        if let p = connected { central.cancelPeripheralConnection(p) }
    }

    private func beginConnect(_ peripheral: CBPeripheral) {
        stopScan()
        phase = .connecting
        statusMessage = nil
        showPairingRecovery = false
        connected = peripheral
        autoPeripheral = peripheral
        peripheral.delegate = self
        // No timeout: if the watch isn't in range yet this stays PENDING and fires
        // automatically when it appears — the basis of auto-connect, no timer needed.
        central.connect(peripheral, options: nil)
    }

    /// Try to (re)establish the auto-connect link. Prefers re-resolving the last
    /// known watch (a standing connect that waits for it to reappear); otherwise
    /// scans and connects to the first matching device it finds.
    private func tryAutoConnect() {
        guard autoConnect, !userStopped, let central, central.state == .poweredOn else { return }
        guard !isConnected, phase != .connecting else { return }
        if let id = lastWatchUUID, let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            beginConnect(p)
        } else {
            startScan()   // didDiscover will auto-connect to the watch
        }
    }

    private func cancelAutoConnect() {
        stopScan()
        if let p = autoPeripheral, !isConnected { central.cancelPeripheralConnection(p) }
        autoPeripheral = nil
    }

    // MARK: Provisioning

    /// Send "SSID,password" to the watch. The first write to the encrypted
    /// characteristic triggers iOS pairing — confirm the 6-digit code shown on
    /// the watch when the system prompt appears.
    func sendWiFi(ssid: String, password: String) {
        guard let p = connected, let ch = provChar else {
            statusMessage = "Not connected to the watch."
            return
        }
        let s = ssid.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { statusMessage = "SSID can't be empty."; return }
        guard !s.contains(",") else {
            statusMessage = "SSID can't contain a comma."   // firmware splits on the first comma
            return
        }
        guard let data = "\(s),\(password)".data(using: .utf8) else { return }

        phase = .sending
        lastSendSucceeded = nil
        pairingInFlight = true   // arms stale-bond detection for THIS attempt only
        statusMessage = "Sending… confirm the pairing code if asked."
        p.writeValue(data, for: ch, type: .withResponse)
    }

    // MARK: Find My Watch (phone → watch)

    /// Ask the watch to sound its own alarm. Writes "RING" to the find-watch
    /// characteristic; no-op (with a hint) if the firmware doesn't expose it.
    func findWatch() {
        guard let p = connected, let ch = findWatchChar else {
            statusMessage = "This watch's firmware doesn't support Find My Watch yet."
            return
        }
        guard let data = "RING".data(using: .utf8) else { return }
        p.writeValue(data, for: ch, type: .withResponse)
        statusMessage = "Ringing your watch…"
    }

    // MARK: Find My Phone (watch → phone)

    /// Start a continuous alarm: a looping ring that ignores the mute switch, plus
    /// repeating vibration, and a local notification (so a backgrounded/locked phone
    /// still alerts). Idempotent — repeated pings won't stack.
    func startRinging() {
        guard !isRinging else { return }
        isRinging = true

        // .playback so the ring sounds even with the silent switch on (it's an alarm).
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, options: [.duckOthers])
        try? session.setActive(true)

        if let url = Bundle.main.url(forResource: "Ring", withExtension: "wav"),
           let player = try? AVAudioPlayer(contentsOf: url) {
            player.numberOfLoops = -1          // loop until stopped
            player.volume = 1.0
            player.prepareToPlay()
            player.play()
            ringPlayer = player
        }

        AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
        vibrateTimer = Timer.scheduledTimer(withTimeInterval: 1.3, repeats: true) { _ in
            AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
        }

        let content = UNMutableNotificationContent()
        content.title = "Find My Phone"
        content.body = "Your watch is ringing this phone."
        content.sound = .default
        let req = UNNotificationRequest(identifier: "findMyPhone", content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req)
    }

    /// Stop the alarm and release the audio session.
    func stopRinging() {
        guard isRinging else { return }
        isRinging = false
        ringPlayer?.stop()
        ringPlayer = nil
        vibrateTimer?.invalidate()
        vibrateTimer = nil
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
}

// MARK: - CBCentralManagerDelegate

extension WatchManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if phase == .bluetoothOff { phase = .idle }
            tryAutoConnect()                 // resume the standing connection if enabled
        case .poweredOff:
            phase = .bluetoothOff
        case .unauthorized:
            phase = .unauthorized
        case .resetting, .unknown, .unsupported:
            phase = .bluetoothOff
        @unknown default:
            phase = .bluetoothOff
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name ?? "ESP32 Watch"
        if let i = watches.firstIndex(where: { $0.id == peripheral.identifier }) {
            watches[i].rssi = RSSI.intValue
            watches[i].name = name
        } else {
            watches.append(DiscoveredWatch(id: peripheral.identifier,
                                           peripheral: peripheral,
                                           name: name,
                                           rssi: RSSI.intValue))
        }
        // Auto-connect: grab the first matching watch we see while scanning.
        if autoConnect, !userStopped, phase == .scanning {
            beginConnect(peripheral)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedName = peripheral.name
        statusMessage = nil
        lastWatchUUID = peripheral.identifier   // remember it for future auto-connects
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        // A plain connection failure (not a pairing attempt) — don't suggest the
        // stale-bond fix here, just report it.
        phase = .idle
        connected = nil
        pairingInFlight = false
        statusMessage = "Couldn't connect to the watch."
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        // Only treat the drop as a stale-bond failure if it happened DURING a
        // pairing/send attempt. A disconnect at any other time (the watch going to
        // sleep, an idle teardown) is a normal goodbye and stays silent.
        let pairingFailed = pairingInFlight && lastSendSucceeded != true
        provChar = nil
        findChar = nil
        findWatchChar = nil
        canFindWatch = false
        connected = nil
        connectedName = nil
        pairingInFlight = false
        if phase != .bluetoothOff && phase != .unauthorized { phase = .idle }
        if pairingFailed { flagStaleBond() }
        // Re-arm a standing reconnect so the watch is picked back up when it returns
        // (e.g. after waking from sleep) — unless the user chose to disconnect.
        if autoConnect, !userStopped { tryAutoConnect() }
    }

    /// Surface the "your iPhone still holds an old pairing" recovery path.
    private func flagStaleBond() {
        showPairingRecovery = true
        statusMessage = "Couldn't pair. Your iPhone may still hold an old pairing for this watch."
    }
}

// MARK: - CBPeripheralDelegate

extension WatchManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for svc in services where svc.uuid == Self.serviceUUID {
            peripheral.discoverCharacteristics([Self.provUUID, Self.findUUID, Self.findWatchUUID], for: svc)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverCharacteristicsFor service: CBService,
                   error: Error?) {
        guard let chars = service.characteristics else { return }
        for ch in chars {
            switch ch.uuid {
            case Self.provUUID:
                provChar = ch
                phase = .connected
            case Self.findUUID:
                findChar = ch
                peripheral.setNotifyValue(true, for: ch)   // arm Find My Phone
            case Self.findWatchUUID:
                findWatchChar = ch
                canFindWatch = true                        // firmware supports Find My Watch
            default:
                break
            }
        }
    }

    /// Find My Phone: the watch notifies this characteristic ("RING") to make the
    /// phone ring. Any notification on it triggers the alarm.
    func peripheral(_ peripheral: CBPeripheral,
                   didUpdateValueFor characteristic: CBCharacteristic,
                   error: Error?) {
        guard characteristic.uuid == Self.findUUID, error == nil else { return }
        startRinging()
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didWriteValueFor characteristic: CBCharacteristic,
                   error: Error?) {
        // Find-watch ping: report only its own success/failure, not WiFi/pairing state.
        if characteristic.uuid == Self.findWatchUUID {
            statusMessage = (error == nil) ? "Your watch is ringing." : "Couldn't reach the watch."
            return
        }

        pairingInFlight = false   // the attempt concluded; a later goodbye stays silent
        if let error {
            lastSendSucceeded = false
            statusMessage = "Send failed: \(error.localizedDescription)"
            // Encryption/authentication failures on the write mean the secure link
            // couldn't be set up — the stale-bond signature.
            if let att = error as? CBATTError,
               att.code == .insufficientEncryption || att.code == .insufficientAuthentication {
                flagStaleBond()
            }
        } else {
            lastSendSucceeded = true
            statusMessage = "WiFi credentials sent to the watch."
        }
        phase = .connected
    }
}
