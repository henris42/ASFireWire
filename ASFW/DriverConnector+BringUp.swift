//
//  DriverConnector+BringUp.swift
//  ASFW
//
//  The runtime bring-up on request (user client selectors 74 and 75): a
//  controller claimed with the asfw-start = manual boot property waits for
//  `asfw-config-agent --bring-up`; `--bring-down` quiesces the runtime again
//  (as sleep does), so the bring-up can be watched step by step with the boot
//  trace, or run again.
//

import Foundation
import IOKit

extension ASFWDriverConnector {
    /// Asks the driver to run its bring-up now. The request is accepted
    /// (true) and runs on the driver's own queue; its steps show up as
    /// `[StartStep]` lines and in the boot trace.
    func requestBringUp() -> Bool {
        guard isConnected else { return false }
        return transport.callScalarStruct(selector: Method.bringUp.rawValue, scalars: [], initialCap: 16) != nil
    }

    /// Asks the driver to quiesce its runtime, as it does for sleep.
    func requestBringDown() -> Bool {
        guard isConnected else { return false }
        return transport.callScalarStruct(selector: Method.bringDown.rawValue, scalars: [], initialCap: 16) != nil
    }
}
