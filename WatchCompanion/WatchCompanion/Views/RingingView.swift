//
//  RingingView.swift
//  WatchCompanion
//
//  Full-screen alarm shown while the phone is ringing from a watch "Find My
//  Phone" ping. A big pulsing icon and a single large Stop button.
//

import SwiftUI

struct RingingView: View {
    @ObservedObject var manager: WatchManager
    @ObservedObject var settings: AppSettings
    @State private var pulse = false

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()

            VStack(spacing: 26) {
                Spacer()

                ZStack {
                    Circle()
                        .fill(settings.accent.opacity(0.16))
                        .frame(width: 200, height: 200)
                        .scaleEffect(pulse ? 1.15 : 0.85)
                    Image(systemName: "iphone.radiowaves.left.and.right")
                        .font(.system(size: 76))
                        .foregroundStyle(settings.accent)
                }

                VStack(spacing: 8) {
                    Text("Find My Phone")
                        .font(.system(size: 28, weight: .bold))
                        .foregroundStyle(Theme.textPrimary)
                    Text("Your watch is ringing this phone.")
                        .font(.system(size: 16))
                        .foregroundStyle(Theme.textSecondary)
                }

                Spacer()

                Button { manager.stopRinging() } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
                .buttonStyle(AccentButtonStyle(accent: settings.accent))
                .padding(.horizontal, 32)
                .padding(.bottom, 40)
            }
        }
        .onAppear {
            withAnimation(.easeInOut(duration: 0.65).repeatForever(autoreverses: true)) {
                pulse = true
            }
        }
    }
}
