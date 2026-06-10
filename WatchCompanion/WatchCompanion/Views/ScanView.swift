//
//  ScanView.swift
//  WatchCompanion
//
//  Scans for the watch (filtered to its service UUID, so nothing else appears)
//  and lets you tap one to connect. Styled like the watch's app list.
//

import SwiftUI

struct ScanView: View {
    @ObservedObject var manager: WatchManager
    @ObservedObject var settings: AppSettings
    var onSettings: () -> Void
    private var accent: Color { settings.accent }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            header

            SectionHeader(title: "Devices", accent: accent)

            if manager.watches.isEmpty {
                emptyState
            } else {
                ForEach(manager.watches) { watch in
                    watchRow(watch)
                }
            }

            Spacer(minLength: 0)

            if let message = manager.statusMessage {
                Text(message)
                    .font(.footnote)
                    .foregroundStyle(Theme.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .center)
            }

            scanButton
        }
        .padding(20)
    }

    private var header: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Watch Companion")
                    .font(.system(size: 30, weight: .bold))
                    .foregroundStyle(Theme.textPrimary)
                Text("Find your watch over Bluetooth")
                    .font(.system(size: 15))
                    .foregroundStyle(Theme.textSecondary)
            }
            Spacer()
            Button(action: onSettings) {
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 20))
                    .foregroundStyle(accent)
            }
            .accessibilityLabel("Settings")
        }
        .padding(.top, 8)
    }

    private var emptyState: some View {
        Card {
            HStack(spacing: 14) {
                Group {
                    if manager.phase == .scanning || manager.phase == .connecting {
                        ProgressView().tint(accent)
                    } else {
                        Image(systemName: "applewatch.radiowaves.left.and.right")
                            .font(.title2)
                            .foregroundStyle(Theme.textSecondary)
                    }
                }
                .frame(width: 28)

                Text(statusLine)
                    .font(.system(size: 15))
                    .foregroundStyle(Theme.textSecondary)
            }
        }
    }

    private var statusLine: String {
        switch manager.phase {
        case .scanning:   return "Searching for your watch…"
        case .connecting: return "Connecting…"
        default:          return "Make sure the watch is on with WiFi & BLE enabled, then scan."
        }
    }

    private func watchRow(_ watch: DiscoveredWatch) -> some View {
        Button {
            manager.connect(watch)
        } label: {
            Card(fill: Theme.cardElevated) {
                HStack(spacing: 14) {
                    Image(systemName: "applewatch")
                        .font(.title2)
                        .foregroundStyle(accent)
                        .frame(width: 28)

                    VStack(alignment: .leading, spacing: 2) {
                        Text(watch.name)
                            .font(.system(size: 17, weight: .semibold))
                            .foregroundStyle(Theme.textPrimary)
                        Text(signalLabel(watch.rssi))
                            .font(.caption)
                            .foregroundStyle(Theme.textSecondary)
                    }

                    Spacer()

                    if manager.phase == .connecting {
                        ProgressView().tint(accent)
                    } else {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundStyle(Theme.textSecondary)
                    }
                }
            }
        }
        .buttonStyle(.plain)
        .disabled(manager.phase == .connecting)
    }

    private func signalLabel(_ rssi: Int) -> String {
        let strength: String
        switch rssi {
        case (-60)...:     strength = "Strong"
        case (-75)..<(-60): strength = "Good"
        default:           strength = "Weak"
        }
        return "\(strength) signal · \(rssi) dBm"
    }

    private var scanButton: some View {
        Button {
            if manager.phase == .scanning { manager.stopScan() }
            else { manager.startScan() }
        } label: {
            Label(manager.phase == .scanning ? "Stop scanning" : "Scan for watch",
                  systemImage: manager.phase == .scanning ? "stop.fill" : "magnifyingglass")
        }
        .buttonStyle(AccentButtonStyle(accent: accent))
        .disabled(manager.phase == .connecting)
    }
}
