//
//  AppearanceView.swift
//  WatchCompanion
//
//  Accent-color picker, mirroring the watch's Appearance screen: a grid of
//  circular swatches (the same palette as the watch), the selected one ringed in
//  white, over a true-black background. Picking one updates the whole app live.
//

import SwiftUI

struct AppearanceView: View {
    @ObservedObject var settings: AppSettings
    @Environment(\.dismiss) private var dismiss

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 18), count: 3)
    private let names = ["Blue", "Teal", "Green", "Amber", "Heliotrope", "Red"]

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()

            VStack(alignment: .leading, spacing: 22) {
                header

                SectionHeader(title: "Accent color", accent: settings.accent)

                LazyVGrid(columns: columns, spacing: 22) {
                    ForEach(Theme.accents.indices, id: \.self) { i in
                        swatch(i)
                    }
                }

                Text("Tip: the accent colors buttons, headers and highlights throughout the app.")
                    .font(.footnote)
                    .foregroundStyle(Theme.textSecondary)

                Spacer()
            }
            .padding(24)
        }
        .preferredColorScheme(.dark)
    }

    private var header: some View {
        HStack {
            Text("Appearance")
                .font(.system(size: 30, weight: .bold))
                .foregroundStyle(Theme.textPrimary)
            Spacer()
            Button("Done") { dismiss() }
                .font(.system(size: 17, weight: .semibold))
                .tint(settings.accent)
        }
        .padding(.top, 4)
    }

    private func swatch(_ i: Int) -> some View {
        let selected = settings.accentIndex == i
        return Button {
            withAnimation(.easeOut(duration: 0.15)) { settings.accentIndex = i }
        } label: {
            Circle()
                .fill(Theme.accents[i])
                .frame(height: 84)
                .overlay(
                    // Selected: thick white ring. Unselected: faint outline (matches the watch).
                    Circle().strokeBorder(selected ? Color.white : Color(white: 0.23),
                                          lineWidth: selected ? 4 : 2)
                )
                .shadow(color: Theme.accents[i].opacity(selected ? 0.6 : 0),
                        radius: 10)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(names[i])
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

#Preview {
    AppearanceView(settings: AppSettings())
}
