import QtQuick
import QtMultimedia
import org.kde.layershell 1.0 as LayerShell

// Video wallpaper renderer for hapatapa.omarwalls. `wallpaperUrl` is the
// file:// URL of the video (from the manifest entry). Loops it, muted, on the
// Background layer at full-bleed.
Window {
    id: root
    visible: true
    color: "#000000"
    title: "hapatapa-webwall-video"

    property var shownSource: wallpaperUrl
    // Set by the C++ host to trigger a restart after a theme change.
    property bool reloadRequested: false

    LayerShell.Window.layer: LayerShell.Window.LayerBackground
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    LayerShell.Window.exclusionZone: -1
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityNone
    LayerShell.Window.scope: "hapatapa-webwall"

    Video {
        id: player
        anchors.fill: parent
        source: root.shownSource
        fillMode: VideoOutput.PreserveAspectCrop
        muted: true
        loops: MediaPlayer.Infinite
        autoPlay: false
        onSourceChanged: { restart(); }
        onErrorOccurred: console.log("WEBWALL: media error", error, errorString)
    }

    function restart() {
        player.stop()
        player.play()
    }

    Component.onCompleted: restart()

    onReloadRequestedChanged: {
        if (root.reloadRequested) { root.reloadRequested = false; root.restart() }
    }
}