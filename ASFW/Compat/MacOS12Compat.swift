//
//  MacOS12Compat.swift
//  ASFW (lite builds for macOS 12 and 13)
//
//  Stand-ins for SwiftUI API the app uses that macOS 12 does not have. Compiled
//  only with ASFW_LITE (tools/build-for-macos.sh sets it for monterey and
//  ventura); the full app for macOS 14+ uses the real ones. Declarations in this
//  module win over the unavailable SwiftUI ones with the same name, so the
//  views stay unchanged. Layout fidelity is deliberately modest: no column
//  alignment in Grid, no flow layout, no copy support on lists.
//

#if ASFW_LITE
import SwiftUI

// MARK: - ContentUnavailableView (macOS 14)

struct ContentUnavailableView: View {
    private let title: String
    private let systemImage: String
    private let description: Text?

    init(_ title: String, systemImage: String, description: Text? = nil) {
        self.title = title
        self.systemImage = systemImage
        self.description = description
    }

    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: systemImage)
                .font(.system(size: 40))
                .foregroundColor(.secondary)
            Text(title)
                .font(.title3)
            if let description {
                description
                    .font(.callout)
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Grid / GridRow (macOS 13)

private struct GridHorizontalSpacingKey: EnvironmentKey {
    static let defaultValue: CGFloat? = nil
}

private extension EnvironmentValues {
    var gridHorizontalSpacing: CGFloat? {
        get { self[GridHorizontalSpacingKey.self] }
        set { self[GridHorizontalSpacingKey.self] = newValue }
    }
}

/// Rows stack vertically; columns are not aligned across rows.
struct Grid<Content: View>: View {
    private let alignment: Alignment
    private let horizontalSpacing: CGFloat?
    private let verticalSpacing: CGFloat?
    private let content: Content

    init(alignment: Alignment = .center,
         horizontalSpacing: CGFloat? = nil,
         verticalSpacing: CGFloat? = nil,
         @ViewBuilder content: () -> Content) {
        self.alignment = alignment
        self.horizontalSpacing = horizontalSpacing
        self.verticalSpacing = verticalSpacing
        self.content = content()
    }

    var body: some View {
        VStack(alignment: alignment.horizontal, spacing: verticalSpacing) {
            content
        }
        .environment(\.gridHorizontalSpacing, horizontalSpacing)
    }
}

struct GridRow<Content: View>: View {
    private let alignment: VerticalAlignment?
    private let content: Content
    @Environment(\.gridHorizontalSpacing) private var spacing

    init(alignment: VerticalAlignment? = nil, @ViewBuilder content: () -> Content) {
        self.alignment = alignment
        self.content = content()
    }

    var body: some View {
        HStack(alignment: alignment ?? .center, spacing: spacing) {
            content
        }
    }
}

// MARK: - NavigationSplitView (macOS 13)

struct NavigationSplitView<Sidebar: View, Detail: View>: View {
    private let sidebar: Sidebar
    private let detail: Detail

    init(@ViewBuilder sidebar: () -> Sidebar, @ViewBuilder detail: () -> Detail) {
        self.sidebar = sidebar()
        self.detail = detail()
    }

    var body: some View {
        NavigationView {
            sidebar
            detail
        }
    }
}

// MARK: - Text and font modifiers (macOS 13 / 13.3)

extension Text {
    func monospaced() -> Text {
        font(.system(.body, design: .monospaced))
    }
}

extension View {
    func monospaced() -> some View {
        font(.system(.body, design: .monospaced))
    }

    func fontWeight(_ weight: Font.Weight?) -> some View {
        transformEnvironment(\.font) { font in
            if let weight {
                font = (font ?? .body).weight(weight)
            }
        }
    }

    func fontDesign(_ design: Font.Design?) -> some View {
        font(.system(.body, design: design ?? .default))
    }

    /// Copy support for list selections needs Transferable (macOS 13); not offered here.
    func copyable<T>(_ payload: @autoclosure @escaping () -> [T]) -> some View {
        self
    }
}

extension Color {
    /// `.gradient` (macOS 13) is a flat fill here.
    var gradient: Color { self }
}

// MARK: - onChange with old/new values (macOS 14)

private struct ChangeObserver<Value: Equatable>: ViewModifier {
    let value: Value
    let initial: Bool
    let action: (Value, Value) -> Void
    @State private var previous: Value?

    func body(content: Content) -> some View {
        content
            .onChange(of: value) { newValue in
                action(previous ?? newValue, newValue)
                previous = newValue
            }
            .onAppear {
                previous = value
                if initial {
                    action(value, value)
                }
            }
    }
}

extension View {
    func onChange<Value: Equatable>(of value: Value,
                                    initial: Bool = false,
                                    _ action: @escaping (Value, Value) -> Void) -> some View {
        modifier(ChangeObserver(value: value, initial: initial, action: action))
    }

    func onChange<Value: Equatable>(of value: Value,
                                    initial: Bool = false,
                                    _ action: @escaping () -> Void) -> some View {
        modifier(ChangeObserver(value: value, initial: initial, action: { _, _ in action() }))
    }
}
#endif
