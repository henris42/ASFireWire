//
//  CommandsView.swift
//  ASFW
//
//  Created by ASFireWire Project on 11.12.2025.
//

import SwiftUI

struct CommandsView: View {
    @ObservedObject var viewModel: DebugViewModel

    var body: some View {
        TabView {
            // Read/Write Tab
            ReadWriteView(viewModel: viewModel)
                .tabItem {
                    Label("Read/Write", systemImage: "arrow.left.arrow.right")
                }

            // Compare & Swap Tab
            CompareSwapView(connector: viewModel.connector)
                .tabItem {
                    Label("Compare & Swap", systemImage: "lock.rectangle")
                }
        }
        .navigationTitle("Async Commands")
    }
}

#if false
#Preview {
    CommandsView(viewModel: DebugViewModel())
}
#endif
