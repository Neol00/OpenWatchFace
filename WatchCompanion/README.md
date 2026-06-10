# Watch Companion (iOS)

Companion iPhone app for the **ESP32-S3-WatchFace**. Finds the watch over
Bluetooth LE and sends it WiFi credentials. Built with SwiftUI + CoreBluetooth.

## What it does today

1. **Scan** — finds *only* the watch by filtering on its advertised service UUID,
   so no other BLE devices appear in the list.
2. **Connect** — taps a watch to connect and discovers its GATT service. With
   **auto-connect** (Settings) on, it keeps a standing connection and reconnects
   automatically whenever the watch is in range — no polling timer needed.
3. **Send WiFi** — enter an SSID + password and send it. The first send triggers
   iOS pairing: a system dialog asks for the **6-digit code shown on the watch**.
   On success the watch saves the network and connects automatically.
4. **Find My Phone** — the app subscribes to the find-phone characteristic. When
   the watch notifies it, the phone starts a **continuous alarm** (looping ring
   that ignores the silent switch, repeating vibration, a full-screen Stop screen,
   and a local notification). Requires the watch to be connected.
5. **Appearance** — pick an accent color from the watch's palette (Settings).

## BLE contract — must match `ESP32-S3-WatchFace/ble_provision.h`

| | UUID |
|---|---|
| Service | `6b1f0001-9a3e-4c7a-9b2d-2f1a8c5e7d10` |
| WiFi provision (write, encrypted+authenticated) | `6b1f0002-9a3e-4c7a-9b2d-2f1a8c5e7d10` |
| Find-phone (notify) | `6b1f0003-9a3e-4c7a-9b2d-2f1a8c5e7d10` |

Provisioning payload is the UTF-8 string **`SSID,password`**, split by the firmware
on the **first** comma (so the password may contain commas; the SSID may not).

Find My Phone: the watch sends a notify on `6b1f0003…` (firmware `ble_ping_phone()`
writes "RING"); the app rings on *any* notification from that characteristic.

## Find My Phone — background ringing

`Info.plist` declares the `audio` and `bluetooth-central` background modes, and the
ring uses an `AVAudioSession` `.playback` category so it sounds even when the phone
is on silent / locked. The looping tone is bundled as `Ring.wav` (a generated
two-tone "ring-ring"). `Info.plist` is merged on top of the generated one
(`GENERATE_INFOPLIST_FILE=YES` + `INFOPLIST_FILE=Info.plist`).

## Why manual SSID entry (not the phone's saved networks)

iOS does not expose the device's stored WiFi networks to apps — there is no public
API. So credentials are entered by hand. (Reading the *currently connected* SSID is
possible only with the special `com.apple.developer.networking.HotspotConfiguration`
/ location entitlements Apple rarely grants, so it's intentionally not used here.)

## Building & running

Open `WatchCompanion.xcodeproj` in Xcode and run.

- **Real testing needs a physical iPhone.** The iOS Simulator has no Bluetooth
  radio, so CoreBluetooth reports `poweredOff` and can't see the watch. The UI
  runs in the simulator, but pairing/sending only works on a device.
- Min iOS: 17.0. Bundle id: `com.noelejemyr.WatchCompanion` (change as needed).
- The Bluetooth usage string is set via the `INFOPLIST_KEY_NSBluetoothAlwaysUsageDescription`
  build setting (no separate Info.plist).
- **Signing:** set your own Team and a unique bundle id in Xcode's Signing &
  Capabilities to build/sideload onto your device.

> **Distribution:** this app is GPLv3 (same as the firmware) and is **not** on the App
> Store — GPL and the App Store's terms are incompatible. Build and sideload it yourself
> from source (above).

## Design

The UI mirrors the watch's own look (`Theme.swift`): true-black AMOLED
background, the watch's accent blue `#00B0FF` (and its full 6-color accent
palette), translucent rounded cards, and small UPPERCASE accent-colored section
headers — matching the watch's Appearance / Power / app-list screens.

## Previewing the UI in the Simulator (DEBUG only)

The Simulator has no Bluetooth radio, so the app normally shows "Bluetooth is
off" there. To view the Scan / Provision screens, launch with the `WC_DEMO`
environment variable, which seeds fake state:

```sh
SIMCTL_CHILD_WC_DEMO=scan      xcrun simctl launch <sim> com.noelejemyr.WatchCompanion
SIMCTL_CHILD_WC_DEMO=provision xcrun simctl launch <sim> com.noelejemyr.WatchCompanion
```

This seam is compiled out of release builds (`#if DEBUG`). Real pairing/sending
still requires a physical iPhone.

## Files

```
WatchCompanion/
  WatchCompanionApp.swift     @main entry point
  Theme.swift                 watch-matched colors + reusable Card/Header/Button
  WatchManager.swift          CoreBluetooth client (scan/connect/pair/send)
  Views/
    ContentView.swift         routes by Bluetooth phase
    ScanView.swift            device list + scan controls
    ProvisionView.swift       SSID/password form + send
  Assets.xcassets/            app icon + accent color
```
