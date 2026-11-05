import QtQuick

// Central design tokens for the desktop UI. The palette follows Apple's
// system colours: soft neutral surfaces, a single blue accent, and a clear
// three-level text hierarchy. Everything binds to `dark`, so switching the
// theme is one property change.
//
// The secondary/tertiary pair is deliberately darker than the iOS label
// colours: captions here are 11-12px CJK glyphs, and #8e8e93 on a card is
// only ~2.9:1.
QtObject {
    id: theme

    property bool dark: true

    readonly property string fontFamily: "Noto Sans CJK SC"

    readonly property int toolbarHeight: 52
    readonly property int radiusSmall: 6
    readonly property int radiusMedium: 10
    readonly property int radiusLarge: 14

    // Surfaces
    readonly property color windowBg: dark ? "#1c1c1e" : "#f5f5f7"
    readonly property color sidebarBg: dark ? "#232325" : "#f0f0f2"
    readonly property color contentBg: dark ? "#1c1c1e" : "#ffffff"
    readonly property color cardBg: dark ? "#2c2c2e" : "#f2f2f7"
    readonly property color controlBg: dark ? "#2c2c2e" : "#e9e9ed"
    readonly property color hoverBg: dark ? "#3a3a3c" : "#e2e2e7"
    readonly property color separator: dark ? "#38383a" : "#dcdce1"

    // Text
    readonly property color textPrimary: dark ? "#f5f5f7" : "#1d1d1f"
    readonly property color textSecondary: dark ? "#c7c7cc" : "#5c5c61"
    readonly property color textTertiary: dark ? "#98989d" : "#6e6e73"

    // Accent and status
    readonly property color accent: dark ? "#0a84ff" : "#007aff"
    readonly property color accentSoft: dark
        ? Qt.rgba(10 / 255, 132 / 255, 255 / 255, 0.18)
        : Qt.rgba(0 / 255, 122 / 255, 255 / 255, 0.12)
    readonly property color accentText: "#ffffff"
    readonly property color danger: dark ? "#ff453a" : "#ff3b30"
    readonly property color success: dark ? "#30d158" : "#34c759"
    readonly property color warning: dark ? "#ffd60a" : "#ff9f0a"
}
