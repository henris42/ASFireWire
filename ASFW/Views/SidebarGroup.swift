enum SidebarGroup: String, CaseIterable, Identifiable {
    case general = "General"
    case devices = "Devices & Audio"
    case video = "Video"
    case reports = "Reports"
    case advanced = "Advanced Tools"

    var id: Self { self }
}
