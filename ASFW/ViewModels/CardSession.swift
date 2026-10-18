//
//  CardSession.swift
//  ASFW
//
//  The connector and view models for one FireWire controller, and the
//  workspace that keeps one session per card the driver currently claims.
//

import Combine
import Foundation

@MainActor
final class CardSession: ObservableObject, Identifiable {
    let card: ControllerCard
    let debugVM: DebugViewModel
    let topologyVM: TopologyViewModel
    let romExplorerVM: RomExplorerViewModel
    let diagnosticsStore: DiagnosticsStore
    let diceReportStore: DiceReportStore
    let avcReportStore: AvcReportStore
    let avcUnitsStore: AvcUnitsStore
    let deviceSettingsVM: DeviceSettingsViewModel
#if !ASFW_LITE
    let mcpVM: ASFWMCPControlViewModel
#endif

    private(set) var isStarted = false
    private var cancellables = Set<AnyCancellable>()

    var id: UInt64 { card.id }
    var connector: ASFWDriverConnector { debugVM.connector }

    init(card: ControllerCard) {
        self.card = card
        let connector = ASFWDriverConnector(registryEntryID: card.registryEntryID)
        debugVM = DebugViewModel(connector: connector)
        topologyVM = TopologyViewModel(connector: connector)
        romExplorerVM = RomExplorerViewModel(connector: connector, topologyViewModel: topologyVM)
        diagnosticsStore = DiagnosticsStore(connector: connector)
        diceReportStore = DiceReportStore(connector: connector)
        avcReportStore = AvcReportStore(connector: connector)
        avcUnitsStore = AvcUnitsStore(connector: connector)
        deviceSettingsVM = DeviceSettingsViewModel(connector: connector, debugVM: debugVM)
#if !ASFW_LITE
        mcpVM = ASFWMCPControlViewModel(connector: connector)
#endif

        // Views observe the session; surface what the sidebar depends on.
        debugVM.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
        // A new bus generation: refresh the topology, then the ROM explorer's nodes.
        debugVM.$topologyCache
            .map { $0?.generation }
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] _ in self?.topologyVM.refresh() }
            .store(in: &cancellables)
        topologyVM.$topology
            .map { $0?.generation }
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] _ in self?.romExplorerVM.refreshAvailableNodes() }
            .store(in: &cancellables)
    }

    func start(driverVM: DriverViewModel) {
        guard !isStarted else { return }
        isStarted = true
        debugVM.setDriverViewModel(driverVM)
        debugVM.connect()
        topologyVM.startAutoRefresh()
        romExplorerVM.setConnector(connector, topologyViewModel: topologyVM)
#if !ASFW_LITE
        if mcpVM.isEnabled {
            Task { await mcpVM.start() }
        }
#endif
    }

    func stop() {
        guard isStarted else { return }
        isStarted = false
#if !ASFW_LITE
        Task { await mcpVM.stop() }
#endif
        debugVM.disconnect()
        topologyVM.stopAutoRefresh()
    }
}

/// One session per claimed controller; the selected one drives the window.
@MainActor
final class CardWorkspace: ObservableObject {
    @Published private(set) var cards: [ControllerCard] = []
    @Published var selectedCardID: UInt64? {
        didSet { forwardSelectedSession() }
    }
    private(set) var sessions: [UInt64: CardSession] = [:]

    let registry = CardRegistry()
    private var driverVM: DriverViewModel?
    private var cancellables = Set<AnyCancellable>()
    private var selectedForward: AnyCancellable?

    var selectedSession: CardSession? {
        selectedCardID.flatMap { sessions[$0] }
    }

    init() {
        registry.$cards
            .receive(on: DispatchQueue.main)
            .sink { [weak self] cards in self?.reconcile(cards) }
            .store(in: &cancellables)
    }

    /// Start every session; sessions created later start on creation.
    func attach(driverVM: DriverViewModel) {
        self.driverVM = driverVM
        for session in sessions.values {
            session.start(driverVM: driverVM)
        }
    }

    func stopAll() {
        for session in sessions.values {
            session.stop()
        }
        driverVM = nil
    }

    private func reconcile(_ newCards: [ControllerCard]) {
        let newIDs = Set(newCards.map(\.id))
        for (id, session) in sessions where !newIDs.contains(id) {
            session.stop()
            sessions.removeValue(forKey: id)
        }
        for card in newCards where sessions[card.id] == nil {
            let session = CardSession(card: card)
            sessions[card.id] = session
            if let driverVM {
                session.start(driverVM: driverVM)
            }
        }
        cards = newCards
        if let selected = selectedCardID, newIDs.contains(selected) {
            forwardSelectedSession()
        } else {
            selectedCardID = newCards.first?.id
        }
    }

    private func forwardSelectedSession() {
        selectedForward = selectedSession?.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
    }
}
