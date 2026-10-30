import Foundation
import Combine
import IOKit
import Darwin.Mach

extension ASFWDriverConnector {
    // MARK: - Monitoring & Notifications

    func startMonitoring() {
        connectionQueue.sync {
            if monitoringActive { return }
            monitoringActive = true
            startMonitoringLocked()
        }
    }

    func stopMonitoringLocked() {
        if matchedIterator != 0 {
            IOObjectRelease(matchedIterator)
            matchedIterator = 0
        }
        if terminatedIterator != 0 {
            IOObjectRelease(terminatedIterator)
            terminatedIterator = 0
        }
        if let port = notificationPort {
            IONotificationPortDestroy(port)
            notificationPort = nil
        }
        monitoringActive = false
    }

    /// Whether `service` is the one this connector is bound to (always, unbound).
    func isBoundService(_ service: io_object_t) -> Bool {
        guard let bound = boundRegistryEntryID else { return true }
        var entryID: UInt64 = 0
        guard IORegistryEntryGetRegistryEntryID(service, &entryID) == KERN_SUCCESS else { return false }
        return entryID == bound
    }

    func handleMatched(iterator: io_iterator_t) {
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            guard isBoundService(service) else {
                IOObjectRelease(service)
                continue
            }
            log("Matched service 0x\(String(service, radix: 16))", level: .info)

            IOObjectRetain(service)
            if currentService != 0 {
                IOObjectRelease(currentService)
            }
            currentService = service
            openConnectionLocked(to: service, reason: "match notification")
            IOObjectRelease(service)
        }
    }

    func handleTerminated(iterator: io_iterator_t) {
        var terminationObserved = false
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            if isBoundService(service) {
                terminationObserved = true
                log("Service terminated 0x\(String(service, radix: 16))", level: .warning)
            }
            IOObjectRelease(service)
        }

        if terminationObserved {
            closeConnectionLocked(reason: "Service terminated")
        }
    }

    // MARK: - Connection management

    func manualConnectLocked(forceAttempt: Bool) {
        if connection != 0 && !forceAttempt {
            return
        }

        let matchingDict = boundRegistryEntryID.map { IORegistryEntryIDMatching($0) }
            ?? IOServiceNameMatching(serviceName)
        let service = IOServiceGetMatchingService(kIOMainPortDefault, matchingDict)
        guard service != 0 else {
            log("ASFWDriver service not found in IORegistry", level: .error)
            lastError = "ASFWDriver service not found"
            return
        }

        openConnectionLocked(to: service, reason: "manual connect")
        IOObjectRelease(service)
    }

    func openConnectionLocked(to service: io_service_t, reason: String) {
        if connection != 0 {
            return
        }

        log("Opening connection (\(reason))...", level: .info)
        var newConnection: io_connect_t = 0
        let kr = IOServiceOpen(service, mach_task_self_, 0, &newConnection)
        guard kr == KERN_SUCCESS else {
            let errorMsg = "Failed to open service: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return
        }

        connection = newConnection
        lastError = nil

        // The block appears when the runtime starts (a controller claimed with
        // asfw-start = manual has none until --bring-up, and loses it again
        // after a failed start); the connection stays open without it, with no
        // status to show, and the poll maps it once it exists.
        if !mapSharedStatusMemoryLocked() {
            log("Shared status block not available yet; connected without status", level: .warning)
        }

        if pollsStatus {
            startStatusPollingLocked()
        }

        DispatchQueue.main.async { [weak self] in
            self?.isConnected = true
        }
        log("Connection established", level: .success)
    }

    func closeConnectionLocked(reason: String) {
        statusPollTimer?.cancel()
        statusPollTimer = nil

        if sharedMemoryPointer != nil {
            IOConnectUnmapMemory64(connection,
                                   0,
                                   mach_task_self_,
                                   sharedMemoryAddress)
            sharedMemoryPointer = nil
            sharedMemoryAddress = 0
            sharedMemoryLength = 0
        }

        if connection != 0 {
            IOServiceClose(connection)
            connection = 0
        }

        if currentService != 0 {
            IOObjectRelease(currentService)
            currentService = 0
        }

        lastDeliveredSequence = 0
        lastDeliveredStatus = nil
        nextLiveStatusNanos = 0
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isConnected = false
            self.latestStatus = nil
        }
        log("Connection closed: \(reason)", level: .warning)
    }

    func mapSharedStatusMemoryLocked() -> Bool {
        var address: mach_vm_address_t = 0
        var length: mach_vm_size_t = 0
        let options: UInt32 = UInt32(kIOMapAnywhere | kIOMapDefaultCache)
        let kr = IOConnectMapMemory64(connection,
                                      0,
                                      mach_task_self_,
                                      &address,
                                      &length,
                                      options)
        guard kr == KERN_SUCCESS, let pointer = UnsafeMutableRawPointer(bitPattern: UInt(address)) else {
            if kr != kIOReturnNotReady || !statusMapFailureLogged {
                log("IOConnectMapMemory64 failed: \(interpretIOReturn(kr))", level: .error)
            }
            statusMapFailureLogged = true
            return false
        }
        statusMapFailureLogged = false

        sharedMemoryAddress = address
        sharedMemoryLength = length
        sharedMemoryPointer = pointer
        emitCurrentStatus()
        return true
    }

    /// The driver writes the shared status block without notifying anyone (a
    /// notification per write woke the app up to a thousand times a second); the
    /// app reads it four times a second and publishes what changed.
    func startStatusPollingLocked() {
        guard statusPollTimer == nil else { return }
        let timer = DispatchSource.makeTimerSource(queue: connectionQueue)
        timer.schedule(deadline: .now() + .milliseconds(250),
                       repeating: .milliseconds(250),
                       leeway: .milliseconds(50))
        timer.setEventHandler { [weak self] in
            self?.pollStatusLocked()
        }
        statusPollTimer = timer
        timer.resume()
    }

    /// A whole copy of the shared status block, or nil while the driver writes it.
    func readSharedStatusLocked() -> DriverStatus? {
        guard let pointer = sharedMemoryPointer, sharedMemoryLength >= 256 else { return nil }
        var copy = [UInt8](repeating: 0, count: 256)
        let sequence = copy.withUnsafeMutableBytes { buffer in
            ASFWCopySeqlockedBlock(UnsafeRawPointer(pointer), buffer.baseAddress, 256, 8)
        }
        guard sequence != 0 else { return nil }
        return copy.withUnsafeBytes { buffer in
            DriverStatus(rawPointer: buffer.baseAddress!, length: buffer.count)
        }
    }

    /// The driver rewrites the block every 250 ms, if only for its watchdog
    /// counters. Subscribers (the views' refreshes, each a set of driver calls)
    /// hear only of a changed controller or bus state; the live counters reach
    /// `latestStatus` once a second (b187: the app at 47–67 % CPU, refreshing
    /// every view four times a second).
    func pollStatusLocked() {
        if sharedMemoryPointer == nil, connection != 0 {
            guard mapSharedStatusMemoryLocked() else { return }
        }
        guard let status = readSharedStatusLocked() else { return }
        guard status.sequence != lastDeliveredSequence else { return }
        let changed = !(lastDeliveredStatus?.hasSameState(as: status) ?? false)
        guard changed || DispatchTime.now().uptimeNanoseconds >= nextLiveStatusNanos else { return }
        deliverStatusLocked(status, notifySubscribers: changed)
    }

    func emitCurrentStatus() {
        guard let status = readSharedStatusLocked() else { return }
        deliverStatusLocked(status, notifySubscribers: true)
    }

    private func deliverStatusLocked(_ status: DriverStatus, notifySubscribers: Bool) {
        lastDeliveredSequence = status.sequence
        lastDeliveredStatus = status
        nextLiveStatusNanos = DispatchTime.now().uptimeNanoseconds + 1_000_000_000
        DispatchQueue.main.async { [weak self] in
            self?.latestStatus = status
        }
        if notifySubscribers {
            statusSubject.send(status)
        }
    }

    private func startMonitoringLocked() {
        guard notificationPort == nil else { return }

        guard let port = IONotificationPortCreate(kIOMainPortDefault) else {
            log("Failed to create IONotificationPort", level: .error)
            return
        }
        notificationPort = port
        IONotificationPortSetDispatchQueue(port, connectionQueue)

        var matched: io_iterator_t = 0
        let matchDict = IOServiceNameMatching(serviceName)
        let matchResult = IOServiceAddMatchingNotification(
            port,
            kIOFirstMatchNotification,
            matchDict,
            { refCon, iterator in
                guard let refCon = refCon else { return }
                let connector = Unmanaged<ASFWDriverConnector>.fromOpaque(refCon).takeUnretainedValue()
                connector.handleMatched(iterator: iterator)
            },
            UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            &matched
        )

        if matchResult == KERN_SUCCESS {
            matchedIterator = matched
            handleMatched(iterator: matched)
        } else {
            log("IOServiceAddMatchingNotification (first match) failed: \(interpretIOReturn(matchResult))", level: .error)
        }

        var terminated: io_iterator_t = 0
        let termDict = IOServiceNameMatching(serviceName)
        let termResult = IOServiceAddMatchingNotification(
            port,
            kIOTerminatedNotification,
            termDict,
            { refCon, iterator in
                guard let refCon = refCon else { return }
                let connector = Unmanaged<ASFWDriverConnector>.fromOpaque(refCon).takeUnretainedValue()
                connector.handleTerminated(iterator: iterator)
            },
            UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            &terminated
        )

        if termResult == KERN_SUCCESS {
            terminatedIterator = terminated
            handleTerminated(iterator: terminated)
        } else {
            log("IOServiceAddMatchingNotification (terminated) failed: \(interpretIOReturn(termResult))", level: .error)
        }
    }
}
