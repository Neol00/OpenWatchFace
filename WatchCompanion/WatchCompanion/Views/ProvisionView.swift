//
//  ProvisionView.swift
//  WatchCompanion
//
//  Shown once connected. Enter a WiFi SSID + password and send it to the watch,
//  styled like the watch's settings screens (accent headers, dark cards).
//
//  Note: iOS does not let apps read the phone's saved WiFi networks (no public
//  API), so credentials are entered manually here. The first send triggers iOS
//  pairing — confirm the 6-digit code shown on the watch.
//

import SwiftUI

struct ProvisionView: View {
    @ObservedObject var manager: WatchManager
    @ObservedObject var settings: AppSettings
    var onSettings: () -> Void
    private var accent: Color { settings.accent }

    @State private var ssid: String = ""
    @State private var password: String = ""
    @State private var showPassword = false
    @FocusState private var focused: Field?

    private enum Field { case ssid, password }

    private var canSend: Bool {
        !ssid.trimmingCharacters(in: .whitespaces).isEmpty && manager.phase != .sending
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack {
                    Spacer()
                    Button(action: onSettings) {
                        Image(systemName: "gearshape.fill")
                            .font(.system(size: 20))
                            .foregroundStyle(accent)
                    }
                    .accessibilityLabel("Settings")
                }

                connectedCard

                VStack(alignment: .leading, spacing: 10) {
                    SectionHeader(title: "WiFi network", accent: accent)
                    credentialFields
                    Text("The watch saves this network and connects to it automatically.")
                        .font(.footnote)
                        .foregroundStyle(Theme.textSecondary)
                        .padding(.horizontal, 4)
                }

                sendButton

                if let message = manager.statusMessage {
                    resultBanner(message)
                }

                VStack(alignment: .leading, spacing: 10) {
                    SectionHeader(title: "Find My Watch", accent: accent)
                    Button {
                        manager.findWatch()
                    } label: {
                        Label("Ring my watch", systemImage: "applewatch.radiowaves.left.and.right")
                    }
                    .buttonStyle(AccentButtonStyle(accent: accent))
                    .disabled(!manager.canFindWatch)
                    .opacity(manager.canFindWatch ? 1 : 0.5)

                    Text(manager.canFindWatch
                         ? "Sound an alarm on your watch to help you find it."
                         : "Update the watch firmware to enable Find My Watch.")
                        .font(.footnote)
                        .foregroundStyle(Theme.textSecondary)
                        .padding(.horizontal, 4)
                }

                Spacer(minLength: 12)

                Button(role: .destructive) {
                    manager.disconnect()
                } label: {
                    Label("Disconnect", systemImage: "xmark.circle")
                        .font(.system(size: 16, weight: .medium))
                        .frame(maxWidth: .infinity)
                }
                .tint(Theme.danger)
            }
            .padding(20)
        }
        .scrollDismissesKeyboard(.interactively)
    }

    private var connectedCard: some View {
        Card {
            HStack(spacing: 14) {
                ZStack {
                    Circle().fill(accent.opacity(0.18)).frame(width: 44, height: 44)
                    Image(systemName: "applewatch.radiowaves.left.and.right")
                        .font(.system(size: 20))
                        .foregroundStyle(accent)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("Connected")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(Theme.success)
                    Text(manager.connectedName ?? "Watch")
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(Theme.textPrimary)
                }
                Spacer()
            }
        }
        .padding(.top, 8)
    }

    private var credentialFields: some View {
        VStack(spacing: 0) {
            HStack {
                Image(systemName: "wifi").foregroundStyle(Theme.textSecondary).frame(width: 24)
                TextField("Network name (SSID)", text: $ssid)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focused, equals: .ssid)
                    .submitLabel(.next)
                    .onSubmit { focused = .password }
            }
            .padding(.vertical, 14)

            Divider().overlay(Theme.hairline)

            HStack {
                Image(systemName: "lock").foregroundStyle(Theme.textSecondary).frame(width: 24)
                Group {
                    if showPassword {
                        TextField("Password", text: $password)
                    } else {
                        SecureField("Password", text: $password)
                    }
                }
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($focused, equals: .password)
                .submitLabel(.done)

                Button {
                    showPassword.toggle()
                } label: {
                    Image(systemName: showPassword ? "eye.slash" : "eye")
                        .foregroundStyle(Theme.textSecondary)
                }
                .buttonStyle(.plain)
            }
            .padding(.vertical, 14)
        }
        .padding(.horizontal, 16)
        .foregroundStyle(Theme.textPrimary)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: Theme.cornerRadius, style: .continuous))
    }

    private var sendButton: some View {
        Button {
            focused = nil
            manager.sendWiFi(ssid: ssid, password: password)
        } label: {
            HStack(spacing: 8) {
                if manager.phase == .sending {
                    ProgressView().tint(.black)
                } else {
                    Image(systemName: "paperplane.fill")
                }
                Text(manager.phase == .sending ? "Sending…" : "Send to watch")
            }
        }
        .buttonStyle(AccentButtonStyle(accent: accent))
        .disabled(!canSend)
        .opacity(canSend ? 1 : 0.5)
    }

    @ViewBuilder
    private func resultBanner(_ message: String) -> some View {
        let ok = manager.lastSendSucceeded
        let color: Color = ok == true ? Theme.success : (ok == false ? Theme.danger : Theme.textSecondary)
        let icon = ok == true ? "checkmark.circle.fill"
            : (ok == false ? "exclamationmark.triangle.fill" : "info.circle")

        Card {
            HStack(spacing: 12) {
                Image(systemName: icon).foregroundStyle(color)
                Text(message)
                    .font(.system(size: 15))
                    .foregroundStyle(Theme.textPrimary)
                Spacer(minLength: 0)
            }
        }
    }
}
