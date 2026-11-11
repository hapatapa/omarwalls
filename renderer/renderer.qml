import QtQuick
import QtWebEngine
import org.kde.layershell 1.0 as LayerShell

Window {
    id: root
    visible: true
    color: "#101216"
    title: "hapatapa-webwall"

    property var shownUrl: wallpaperUrl
    // Set by the C++ host to trigger a page reload after script re-injection
    // (theme change). Consumed in the onChanged handler below.
    property bool reloadRequested: false

    LayerShell.Window.layer: LayerShell.Window.LayerBackground
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    LayerShell.Window.exclusionZone: -1
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityNone
    LayerShell.Window.scope: "hapatapa-webwall"

    WebEngineView {
        id: view
        anchors.fill: parent
        url: shownUrl
        backgroundColor: "#101216"
        settings.javascriptEnabled: true
        settings.webGLEnabled: true
    }

    onShownUrlChanged: {
        view.url = shownUrl
    }

    onReloadRequestedChanged: {
        if (root.reloadRequested) {
            root.reloadRequested = false
            view.reload()
        }
    }
}