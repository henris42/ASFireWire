import Foundation
import Combine
import os.log
import IOKit
import SystemExtensions
import Darwin.Mach

final class ASFWDriverConnector: ObservableObject {
    // MARK: - Types

    enum Method: UInt32 {
        case getBusResetCount = 0
        case getBusResetHistory = 1
        case getControllerStatus = 2
        // 3 retired (FW-171): the driver only ever answered kIOReturnUnsupported.
        case clearHistory = 4
        case getSelfIDCapture = 5
        case getTopologySnapshot = 6
        // Selector 7 retired (GUI health check).
        case asyncRead = 8
        case asyncWrite = 9
        // 10 retired (status listener): the shared status block is polled.
        case getTransactionResult = 12
        case exportConfigROM = 14
        case triggerROMRead = 15
        case getDiscoveredDevices = 16
        case asyncCompareSwap = 17
        case getDriverVersion = 18
        case getAVCUnits = 22
        case getSubunitCapabilities = 23
        case getSubunitDescriptor = 24
        case reScanAVCUnits = 25
        // IRM test methods (temporary for Phase 0.5)
        case testIRMAllocation = 26
        case testIRMRelease = 27
        // CMP test methods (temporary for Phase 0.5)
        case testCMPConnectOPCR = 28
        case testCMPDisconnectOPCR = 29
        case testCMPConnectIPCR = 30
        case testCMPDisconnectIPCR = 31
        // 34/35 retired (FW-171): zeroed metrics / no-op reset. Use
        // getAudioTelemetry (1013) for audio receive health.
        // IT DMA Allocation (no CMP)
        case startIsochTransmit = 36
        case stopIsochTransmit = 37
        // AV/C raw FCP command (request/response)
        case sendRawFCPCommand = 38
        case getRawFCPCommandResult = 39
        case asyncBlockRead = 44
        case asyncBlockWrite = 45
        // DV capture (raw DIF stream via shared ring, memory type 1)
        case startDVCapture = 50
        case stopDVCapture = 51
        // Developer-only local software bus reset, generation-pinned.
        case requestUserBusReset = 61
        case startAudioStreaming = 62
        case stopAudioStreaming = 63
        // One unit's FCP exchange log, paged (UserClient/WireFormats/AVCExchangeLogWire.hpp).
        case getFCPExchangeLog = 65
        case getAVCDiscoveryDocument = 68
        // Device settings (clock source, optical modes): request/result pair.
        case requestDeviceSettings = 69
        case getDeviceSettingsResult = 70
        // The driver log level (DriverConnector+LogLevel.swift): set, and read back.
        case setLogLevel = 72
        case getLogLevel = 73
        // The runtime bring-up and its quiesce on request (DriverConnector+BringUp.swift).
        case bringUp = 74
        case bringDown = 75
        // Read-only audio telemetry diagnostics.
        case getAudioTelemetry = 1013
        // Driver-owned LogRing category names and named filter presets.
        case getLogCatalog = 1014
    }

    // MARK: - Re-exported Models

    typealias SharedStatusReason = DriverConnectorSharedStatusReason
    typealias SharedStatusFlags = DriverConnectorSharedStatusFlags
    typealias DriverStatus = DriverConnectorStatus
    typealias DriverVersionInfo = DriverConnectorVersionInfo
    typealias AVCSubunitInfo = DriverConnectorAVCSubunitInfo
    typealias AVCUnitInfo = DriverConnectorAVCUnitInfo
    typealias AVCMusicCapabilities = DriverConnectorAVCMusicCapabilities
    typealias FWDeviceState = DriverConnectorFWDeviceState
    typealias FWUnitState = DriverConnectorFWUnitState
    typealias FWDeviceInfo = DriverConnectorFWDeviceInfo
    typealias FWUnitInfo = DriverConnectorFWUnitInfo
    typealias LogMessage = DriverConnectorLogMessage

    // MARK: - Published Properties

    @Published var isConnected: Bool = false
    @Published var lastError: String?
    @Published var logMessages: [LogMessage] = []
    @Published var latestStatus: DriverStatus?

    // MARK: - Public Publishers

    let statusSubject = PassthroughSubject<DriverStatus, Never>()
    var statusPublisher: AnyPublisher<DriverStatus, Never> {
        statusSubject.eraseToAnyPublisher()
    }

    // MARK: - Connection State

    var connection: io_connect_t = 0
    let connectionQueue = DispatchQueue(label: "net.asfw.app.DriverConnector.connection")

    /// Marks `connectionQueue` so `deinit` can tell whether it is already running on
    /// it. The status poll and IOKit notification handlers take a temporary strong `self`
    /// for the duration of the callback (`[weak self]` cannot prevent that), so when the
    /// last other reference is dropped while one is in flight, `deinit` runs on
    /// `connectionQueue` itself. A `sync` there is a self-deadlock and traps with
    /// "dispatch_sync called on queue already owned by current thread".
    private static let connectionQueueKey = DispatchSpecificKey<UInt8>()
    let serviceName = "ASFWDriver"
    /// With several FireWire controllers each has its own ASFWDriver service.
    /// A bound connector talks to exactly that one (by IORegistry entry ID) and
    /// ignores the others' match and termination notifications; unbound, it
    /// follows whichever service matches, as before.
    let boundRegistryEntryID: UInt64?
    let pollsStatus: Bool

    var notificationPort: IONotificationPortRef?
    var matchedIterator: io_iterator_t = 0
    var terminatedIterator: io_iterator_t = 0
    var currentService: io_object_t = 0
    var monitoringActive = false

    /// Reads the driver's shared status block four times a second; the driver
    /// writes it without notifying anyone.
    var statusPollTimer: DispatchSourceTimer?

    var sharedMemoryAddress: mach_vm_address_t = 0
    var sharedMemoryLength: mach_vm_size_t = 0
    var sharedMemoryPointer: UnsafeMutableRawPointer?
    /// The poll retries the status mapping while it is missing; only the first
    /// refusal is logged.
    var statusMapFailureLogged = false
    var lastDeliveredSequence: UInt64 = 0
    /// The last status handed on, and when the live counters may next reach
    /// `latestStatus` without a state change (DriverConnector+Lifecycle).
    var lastDeliveredStatus: DriverStatus?
    var nextLiveStatusNanos: UInt64 = 0
    private let logStore = DriverConnectorLogStore(maxEntries: 100)

    lazy var transport = DriverConnectorTransport(
        connectionProvider: { [weak self] in self?.connection ?? 0 },
        interpretIOReturn: { [weak self] kr in
            self?.interpretIOReturn(kr) ?? String(format: "Unknown error 0x%x (%d)", UInt32(bitPattern: kr), kr)
        },
        errorHandler: { [weak self] message in
            DispatchQueue.main.async { [weak self] in
                self?.lastError = message
            }
        }
    )

    // MARK: - Initialisation

    /// `pollsStatus: false` for connectors that never show the controller status
    /// (the config agent, settings appliers): they do not read the shared block.
    init(registryEntryID: UInt64? = nil, pollsStatus: Bool = true) {
        boundRegistryEntryID = registryEntryID
        self.pollsStatus = pollsStatus
        connectionQueue.setSpecific(key: Self.connectionQueueKey, value: 1)
        startMonitoring()
    }

    deinit {
        // By deinit no other reference exists, so running the teardown inline when we
        // are already on `connectionQueue` cannot race with anything — and it is the
        // only safe option, since `sync` onto the current queue traps.
        if DispatchQueue.getSpecific(key: Self.connectionQueueKey) != nil {
            closeConnectionLocked(reason: "Connector deinit (on connection queue)")
            stopMonitoringLocked()
        } else {
            connectionQueue.sync {
                self.closeConnectionLocked(reason: "Connector deinit")
                self.stopMonitoringLocked()
            }
        }
    }

    // MARK: - Public API

    func connect(forceAttempt: Bool = false) -> Bool {
        log("Manual connect requested", level: .info)
        connectionQueue.async {
            self.manualConnectLocked(forceAttempt: forceAttempt)
        }
        return true
    }

    func disconnect() {
        connectionQueue.async {
            self.closeConnectionLocked(reason: "Manual disconnect")
        }
    }

    // MARK: - Struct Method Helper (handles variable-size returns and kIOReturnNoSpace retry)

    func callStruct(_ selector: Method, input: Data? = nil, initialCap: Int = 64 * 1024) -> Data? {
        transport.callStruct(selector: selector.rawValue, input: input, initialCap: initialCap)
    }

    /// Scalar input, structure output (the driver reads `scalars` as scalarInput).
    func callScalarStruct(_ selector: Method, scalars: [UInt64], initialCap: Int = 4096) -> Data? {
        transport.callScalarStruct(selector: selector.rawValue, scalars: scalars, initialCap: initialCap)
    }

    func callStructWithScalar(_ selector: Method, input: Data? = nil, initialCap: Int = 64 * 1024, scalarOutput: inout UInt64) -> Data? {
        transport.callStructWithScalar(selector: selector.rawValue, input: input, initialCap: initialCap, scalarOutput: &scalarOutput)
    }

    // MARK: - Logging helpers

    /// Connector events also go to os_log (subsystem net.asfw.app, category
    /// connector): `log show --predicate 'subsystem == "net.asfw.app"'` on a
    /// target shows why a connection opened, closed or failed.
    private static let osLog = OSLog(subsystem: "net.asfw.app", category: "connector")

    func log(_ message: String, level: LogMessage.Level = .info) {
        let tag = boundRegistryEntryID.map { "[service \($0)] " } ?? ""
        let type: OSLogType = level == .error ? .error : level == .warning ? .default : .info
        os_log("%{public}@%{public}@", log: Self.osLog, type: type, tag, message)
        let logEntry = LogMessage(timestamp: Date(), level: level, message: message)
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.logMessages = self.logStore.append(logEntry)
        }
    }


    func interpretIOReturn(_ kr: kern_return_t) -> String {
        let KERN_SUCCESS: kern_return_t = 0
        let KERN_PROTECTION_FAILURE: kern_return_t = -308
        let kIOReturnNotPrivileged: kern_return_t = -536870207
        let kIOReturnNoDevice: kern_return_t = -536870208
        let kIOReturnBadArgument: kern_return_t = -536870206
        let kIOReturnUnsupported: kern_return_t = -536870201
        let kIOReturnNotOpen: kern_return_t = -536870195
        let kIOReturnBusy: kern_return_t = -536870187
        let kIOReturnTimeout: kern_return_t = -536870186
        let kIOReturnNotFound: kern_return_t = -536870160

        switch kr {
        case KERN_SUCCESS:
            return "Success"
        case KERN_PROTECTION_FAILURE:
            return "KERN_PROTECTION_FAILURE (open denied; check Info.plist UserClient and entitlements)"
        case kIOReturnNotPrivileged:
            return "Not Privileged (app sandbox must be disabled)"
        case kIOReturnNoDevice:
            return "No Device (driver not loaded)"
        case kIOReturnBadArgument:
            return "Bad Argument"
        case kIOReturnUnsupported:
            return "Unsupported Operation"
        case kIOReturnNotOpen:
            return "Not Open"
        case kIOReturnBusy:
            return "Device Busy"
        case kIOReturnTimeout:
            return "Timeout"
        case kIOReturnNotFound:
            return "Not Found"
        default:
            return String(format: "Unknown error 0x%x (%d)", UInt32(bitPattern: kr), kr)
        }
    }

    func kernResultString(_ kr: kern_return_t) -> String {
        if let cString = mach_error_string(kr) {
            return String(cString: cString)
        }
        return "kern_result = \(kr)"
    }

}
