//
//  Theme.swift
//  WatchCompanion
//
//  Visual language mirrored from the ESP32-S3-WatchFace UI: true-black AMOLED
//  background, a user-selectable accent (default the watch's blue #00B0FF),
//  translucent rounded cards, and small UPPERCASE accent-colored section headers.
//  The accent palette is the exact set the watch offers in Appearance.
//

import SwiftUI

enum Theme {
    /// True black, matching the watch's AMOLED face.
    static let background = Color.black
    /// Translucent card fill sitting on the black background (the watch's tiles).
    static let card = Color(white: 0.10)
    static let cardElevated = Color(white: 0.14)

    static let textPrimary = Color.white
    static let textSecondary = Color(white: 0.55)   // ~#888
    static let hairline = Color(white: 0.22)

    static let cornerRadius: CGFloat = 18

    /// The watch's accent palette (APPR_COLORS in app_appearance.h), in order.
    static let accents: [Color] = [
        Color(hex: 0x00B0FF),   // Blue (default)
        Color(hex: 0x00C2A8),   // Teal
        Color(hex: 0x32D74B),   // Green
        Color(hex: 0xFF9F0A),   // Amber
        Color(hex: 0xDF73FF),   // Heliotrope
        Color(hex: 0xFF453A),   // Red
    ]
    static let defaultAccent = accents[0]

    // Status colors used by the watch's toasts.
    static let success = Color(hex: 0x32D74B)
    static let warning = Color(hex: 0xFF9F0A)
    static let danger  = Color(hex: 0xFF453A)
}

extension Color {
    /// Build a Color from a 0xRRGGBB literal, matching the firmware's lv_color_hex use.
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red:   Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue:  Double(hex & 0xFF) / 255,
            opacity: 1
        )
    }
}

// MARK: - Reusable building blocks that echo the watch screens

/// Small UPPERCASE header in the accent color, like "ACCENT COLOR" / "BATTERY".
struct SectionHeader: View {
    let title: String
    var accent: Color = Theme.defaultAccent

    var body: some View {
        Text(title.uppercased())
            .font(.system(size: 13, weight: .semibold))
            .tracking(0.8)
            .foregroundStyle(accent)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A translucent rounded card, like the watch's app tiles / panels.
struct Card<Content: View>: View {
    var fill: Color = Theme.card
    @ViewBuilder var content: () -> Content

    var body: some View {
        content()
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fill, in: RoundedRectangle(cornerRadius: Theme.cornerRadius, style: .continuous))
    }
}

/// Full-width prominent action button in the accent color.
struct AccentButtonStyle: ButtonStyle {
    var accent: Color = Theme.defaultAccent
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .semibold))
            .foregroundStyle(.black)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(accent.opacity(configuration.isPressed ? 0.7 : 1),
                        in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .opacity(configuration.isPressed ? 0.9 : 1)
    }
}
