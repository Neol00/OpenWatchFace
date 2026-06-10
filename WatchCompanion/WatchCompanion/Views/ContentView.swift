//
//  ContentView.swift
//  WatchCompanion
//
//  Top-level router: shows the right screen for the current Bluetooth phase,
//  styled to match the watch (true-black background, accent blue).
//

import SwiftUI
import UIKit
import UserNotifications

struct ContentView: View {
    @StateObject private var manager = WatchManager()
    @StateObject private var settings = AppSettings()
    @State private var showingSettings = false
    @Environment(\.openURL) private var openURL

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            content
            if manager.isRinging {
                RingingView(manager: manager, settings: settings)
                    .transition(.opacity)
                    .zIndex(1)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: manager.isRinging)
        .tint(settings.accent)
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showingSettings) {
            SettingsView(settings: settings)
        }
        .onAppear {
            manager.autoConnect = settings.autoConnect
            #if DEBUG
            let demo = ProcessInfo.processInfo.environment["WC_DEMO"] != nil
            if ProcessInfo.processInfo.environment["WC_OPENSETTINGS"] != nil { showingSettings = true }
            if ProcessInfo.processInfo.environment["WC_RINGING"] != nil { manager.startRinging() }
            #else
            let demo = false
            #endif
            if !demo {
                UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
            }
        }
        .onChange(of: settings.autoConnect) { on in manager.autoConnect = on }
        .alert("Pairing needs a reset", isPresented: $manager.showPairingRecovery) {
            Button("Open Settings") {
                if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
            }
            Button("OK", role: .cancel) { }
        } message: {
            Text("Your iPhone may still hold an old pairing for this watch (e.g. after the watch was reflashed or reset).\n\nOpen Settings → Bluetooth, tap the ⓘ next to the watch, choose \"Forget This Device,\" then scan and pair again.")
        }
    }

    private func openSettings() { showingSettings = true }

    @ViewBuilder
    private var content: some View {
        switch manager.phase {
        case .bluetoothOff:
            MessageState(systemImage: "antenna.radiowaves.left.and.right.slash",
                         title: "Bluetooth is off",
                         message: "Turn on Bluetooth to find your watch.",
                         onSettings: openSettings)
        case .unauthorized:
            MessageState(systemImage: "lock.slash",
                         title: "Bluetooth access needed",
                         message: "Allow Bluetooth for Watch Companion in Settings, then reopen the app.",
                         onSettings: openSettings)
        default:
            if manager.isConnected {
                ProvisionView(manager: manager, settings: settings, onSettings: openSettings)
            } else {
                ScanView(manager: manager, settings: settings, onSettings: openSettings)
            }
        }
    }
}

/// Centered icon + title + message placeholder, in the watch's muted style.
/// Optionally shows a Settings button so the app stays reachable even with
/// Bluetooth off / unauthorized.
struct MessageState: View {
    let systemImage: String
    let title: String
    let message: String
    var onSettings: (() -> Void)? = nil

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: systemImage)
                .font(.system(size: 46, weight: .regular))
                .foregroundStyle(Theme.textSecondary)
            Text(title)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(Theme.textPrimary)
            Text(message)
                .font(.system(size: 15))
                .foregroundStyle(Theme.textSecondary)
                .multilineTextAlignment(.center)
            if let onSettings {
                Button {
                    onSettings()
                } label: {
                    Label("Settings", systemImage: "gearshape")
                }
                .buttonStyle(.bordered)
                .padding(.top, 4)
            }
        }
        .padding(40)
    }
}

#Preview {
    ContentView()
}
