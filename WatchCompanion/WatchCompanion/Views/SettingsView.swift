//
//  SettingsView.swift
//  WatchCompanion
//
//  App settings: the accent-color picker (mirroring the watch's Appearance screen)
//  plus a Connection section with the auto-connect toggle. Reachable from every
//  screen, connected or not.
//

import SwiftUI

struct SettingsView: View {
    @ObservedObject var settings: AppSettings
    @Environment(\.dismiss) private var dismiss

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 18), count: 3)
    private let names = ["Blue", "Teal", "Green", "Amber", "Heliotrope", "Red"]

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()

            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    header

                    SectionHeader(title: "Accent color", accent: settings.accent)
                    LazyVGrid(columns: columns, spacing: 22) {
                        ForEach(Theme.accents.indices, id: \.self) { i in
                            swatch(i)
                        }
                    }

                    SectionHeader(title: "Connection", accent: settings.accent)
                    autoConnectCard

                    Text("Tip: the accent colors buttons, headers and highlights throughout the app.")
                        .font(.footnote)
                        .foregroundStyle(Theme.textSecondary)

                    Spacer(minLength: 0)
                }
                .padding(24)
            }
        }
        .preferredColorScheme(.dark)
    }

    private var header: some View {
        HStack {
            Text("Settings")
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
                    Circle().strokeBorder(selected ? Color.white : Color(white: 0.23),
                                          lineWidth: selected ? 4 : 2)
                )
                .shadow(color: Theme.accents[i].opacity(selected ? 0.6 : 0), radius: 10)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(names[i])
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }

    private var autoConnectCard: some View {
        Card {
            Toggle(isOn: $settings.autoConnect) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Auto-connect to watch")
                        .font(.system(size: 16, weight: .medium))
                        .foregroundStyle(Theme.textPrimary)
                    Text("Reconnect automatically whenever the watch is in range.")
                        .font(.footnote)
                        .foregroundStyle(Theme.textSecondary)
                }
            }
            .tint(settings.accent)
        }
    }
}

#Preview {
    SettingsView(settings: AppSettings())
}
