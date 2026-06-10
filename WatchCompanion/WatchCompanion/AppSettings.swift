//
//  AppSettings.swift
//  WatchCompanion
//
//  App-wide preferences. Currently just the accent color, chosen from the same
//  palette the watch offers in Appearance (Theme.accents) and persisted across
//  launches. Injected once at the top and read by every screen for its tint.
//

import SwiftUI

final class AppSettings: ObservableObject {
    private static let accentKey = "accentIndex"
    private static let autoConnectKey = "autoConnect"

    /// Index into `Theme.accents`. Persisted to UserDefaults on every change.
    @Published var accentIndex: Int {
        didSet { UserDefaults.standard.set(accentIndex, forKey: Self.accentKey) }
    }

    /// When on, the app keeps a standing request to connect to the watch and
    /// reconnects automatically whenever it comes into range (no manual tapping).
    @Published var autoConnect: Bool {
        didSet { UserDefaults.standard.set(autoConnect, forKey: Self.autoConnectKey) }
    }

    init() {
        let saved = UserDefaults.standard.integer(forKey: Self.accentKey)   // 0 (blue) if unset
        accentIndex = Theme.accents.indices.contains(saved) ? saved : 0
        autoConnect = UserDefaults.standard.bool(forKey: Self.autoConnectKey) // false if unset
    }

    /// The currently selected accent color.
    var accent: Color { Theme.accents[accentIndex] }
}
