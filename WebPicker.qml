// WebPicker — omarwalls' wallpaper picker, styled as an Omarchy panel.
// An `overlay` plugin: summon it, type to filter, arrow through the grid,
// Enter applies by writing the state file. The last card stops the live
// wallpaper. Theme colors come from the native qs.Commons palette (which reads
// the Omarchy colors.toml) — no hooks.
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import QtQuick
import qs.Commons
import qs.Ui

Item {
  id: root

  // Injected by omarchy-shell when the overlay is summoned.
  property string omarchyPath: Quickshell.env("OMARCHY_PATH")
  property var shell: null
  property var manifest: null

  property bool opened: false
  property string filterText: ""
  property int selectedIndex: 0
  property bool cursorActive: false
  property var items: []
  property var filteredItems: []

  // Shares the [menu] surface tokens — themes that style the menu also
  // style this picker. Composed in the singleton so they drop straight into
  // Rectangle bindings.
  property color background: Color.menu.background
  property color foreground: Color.menu.text
  property color border: Color.menu.border
  property var borderSpec: Border.surfaceSpec("menu", "border", border, Math.max(1, Style.space(2)))
  property color scrim: Color.menu.scrim
  property color selectedBackground: Color.menu.selectedBackground
  property color selectedText: Color.menu.selectedText
  readonly property int cornerRadius: Style.cornerRadius
  property string fontFamily: Style.font.menuFamily
  property int contentMargin: Style.spacing.panelPadding
  property int headerHeight: Math.max(Style.space(34), Style.font.title + Style.spacing.controlPaddingY * 2)
  property int contentSpacing: Style.spacing.md
  property int cardWidth: Math.min(Style.space(720), panel.width - Style.gapsOut * 2)
  property int cardHeight: Math.min(Style.space(560), panel.height - Style.gapsOut * 2)

  property int cellWidth: Math.max(Style.space(184), Math.min(Style.space(216),
                            Math.floor((root.cardWidth - root.contentMargin * 2
                                       - root.contentSpacing * 2) / 3)))
  property int cellHeight: cellWidth * 9 / 16 + Style.space(26) + contentSpacing
  property int columns: Math.max(1, Math.floor((cardWidth - contentMargin * 2 + contentSpacing) / (cellWidth + contentSpacing)))
  property int previewHeight: cellWidth * 9 / 16

  // The picker is self-contained: no CLI, no IPC. It (re)enumerates the
  // wallpaper store from disk and writes the manifest path into the state file
  // — the Service side watches that file and dispatches to the renderer.
  readonly property string storeRoot: (Quickshell.env("HOME") || "") + "/.local/share/omarchy-webwall/wallpapers"
  readonly property string stateFile: (Quickshell.env("HOME") || "") + "/.local/state/omarchy-webwall/current"

  function stateDir() { return root.stateFile.replace(/\/[^\/]+$/, "") }

  function writeState(manifestPath) {
    var cmd = ["bash", "-c",
      "mkdir -p " + JSON.stringify(root.stateDir()) + " && printf '%s' " + JSON.stringify(manifestPath) + " > " + JSON.stringify(root.stateFile)]
    Quickshell.execDetached(cmd)
  }

  function applyItem(it) {
    if (!it) return
    if (String(it.type) === "stop") root.writeState("")
    else root.writeState(String(it.manifest))
    root.dismiss()
  }

  function open(payloadJson) {
    root.opened = true
    root.filterText = ""
    root.selectedIndex = 0
    root.cursorActive = true
    root.rebuildDisplay()
    loader.running = true    // re-enumerate the store every summon
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }
  function close() { root.opened = false }
  function dismiss() {
    root.opened = false
    if (root.shell && typeof root.shell.hide === "function")
      root.shell.hide((root.manifest && root.manifest.id) || "hapatapa.omarwalls")
  }
  function toggle() { if (root.opened) dismiss(); else open("{}") }

  function rebuildDisplay() {
    var out = []
    var needle = root.filterText.trim().toLowerCase()
    for (var i = 0; i < root.items.length; i++) {
      var it = root.items[i]
      if (needle && String(it.title + " " + it.kind).toLowerCase().indexOf(needle) < 0) continue
      out.push(it)
    }
    root.filteredItems = out

    if (out.length === 0) selectedIndex = 0
    else if (selectedIndex >= out.length) selectedIndex = out.length - 1
    else if (selectedIndex < 0) selectedIndex = 0
    cursorActive = out.length > 0

    Qt.callLater(function() {
      if (out.length > 0) resultGrid.positionViewAtIndex(root.selectedIndex, GridView.Contain)
    })
  }

  function select(delta) {
    if (root.filteredItems.length === 0) return
    if (!cursorActive) {
      cursorActive = true
      selectedIndex = delta < 0 ? root.filteredItems.length - 1 : 0
    } else {
      selectedIndex = (selectedIndex + delta + root.filteredItems.length) % root.filteredItems.length
    }
    resultGrid.positionViewAtIndex(selectedIndex, GridView.Contain)
  }

  function selectRow(delta) {
    if (root.filteredItems.length === 0) return
    if (!cursorActive) {
      cursorActive = true
      selectedIndex = delta < 0 ? root.filteredItems.length - 1 : 0
      resultGrid.positionViewAtIndex(selectedIndex, GridView.Contain)
      return
    }
    var newIndex = selectedIndex + delta * columns
    if (newIndex < 0) newIndex = 0
    if (newIndex >= root.filteredItems.length) newIndex = root.filteredItems.length - 1
    selectedIndex = newIndex
    resultGrid.positionViewAtIndex(selectedIndex, GridView.Contain)
  }

  function selectPage(delta) {
    if (root.filteredItems.length === 0) return
    if (!cursorActive) {
      cursorActive = true
      selectedIndex = delta < 0 ? root.filteredItems.length - 1 : 0
      resultGrid.positionViewAtIndex(selectedIndex, GridView.Contain)
      return
    }
    var visibleRows = Math.max(1, Math.floor(resultGrid.height / root.cellHeight))
    var newIndex = selectedIndex + delta * columns * visibleRows
    if (newIndex < 0) newIndex = 0
    if (newIndex >= root.filteredItems.length) newIndex = root.filteredItems.length - 1
    selectedIndex = newIndex
    resultGrid.positionViewAtIndex(selectedIndex, GridView.Contain)
  }

  function setFilter(nextFilter) {
    root.filterText = nextFilter
    root.selectedIndex = 0
    root.cursorActive = true
    root.rebuildDisplay()
  }

  function activateIndex(index) {
    if (index < 0 || index >= root.filteredItems.length) return
    root.applyItem(root.filteredItems[index])
  }

  // Build the catalog from the wallpaper store: every <id>/wallpaper.json in
  // ~/.local/share/omarchy-webwall/wallpapers, reading kind/title from the
  // manifest and pointing at its local preview.png.
  Process {
    id: loader
    command: ["bash", "-c",
      "d='" + root.storeRoot + "'; for j in \"$d\"/*/wallpaper.json; do \
       [ -f \"$j\" ] || continue; \
       id=$(basename \"$(dirname \"$j\")\"); \
       kind=$(grep -o '\"kind\"[[:space:]]*:[[:space:]]*\"[^\"]*\"' \"$j\" | head -1 | cut -d'\"' -f4); \
       title=$(grep -o '\"title\"[[:space:]]*:[[:space:]]*\"[^\"]*\"' \"$j\" | head -1 | cut -d'\"' -f4); \
       prov=\"$(dirname \"$j\")/preview.png\"; [ -f \"$prov\" ] || prov=\"\"; \
       printf '{\"id\":\"%s\",\"title\":\"%s\",\"kind\":\"%s\",\"manifest\":\"%s\",\"preview\":\"%s\",\"type\":\"wall\"}\n' \"$id\" \"${title:-$id}\" \"${kind:-unknown}\" \"$j\" \"$prov\"; done"]
    running: false
    stdout: StdioCollector {
      onStreamFinished: {
        var arr = []
        try {
          var lines = String(text).trim().split("\n")
          for (var i = 0; i < lines.length; i++) {
            if (!lines[i]) continue
            try { arr.push(JSON.parse(lines[i])) } catch (ee) {}
          }
        } catch (e) { arr = [] }
        arr.push({ id: "__STOP__", title: "Stop live wallpaper", type: "stop", kind: "stop", preview: "" })
        root.items = arr
        root.rebuildDisplay()
      }
    }
  }

  PanelWindow {
    id: panel
    visible: root.opened
    anchors { top: true; bottom: true; left: true; right: true }
    color: "transparent"
    WlrLayershell.namespace: "hapatapa-omarwalls"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive
    exclusionMode: ExclusionMode.Ignore

    Rectangle {
      anchors.fill: parent
      color: root.scrim
    }

    MouseArea {
      anchors.fill: parent
      onClicked: root.dismiss()
    }

    BorderSurface {
      id: card
      width: root.cardWidth
      height: root.cardHeight
      radius: root.cornerRadius
      anchors.centerIn: parent
      color: root.background
      borderSpec: root.borderSpec
      padding: root.contentMargin

      MouseArea { anchors.fill: parent; onClicked: {} }

      Item {
        id: keyCatcher
        anchors.fill: parent
        focus: true

        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) {
          if (event.key === Qt.Key_Escape) {
            if (root.filterText) root.setFilter("")
            else root.dismiss()
            event.accepted = true
          } else if (Util.editsFilter(event, root.filterText)) {
            root.setFilter(Util.editedFilter(event, root.filterText))
            event.accepted = true
          } else if (event.key === Qt.Key_Left) {
            root.select(-1)
            event.accepted = true
          } else if (event.key === Qt.Key_Right) {
            root.select(1)
            event.accepted = true
          } else if (event.key === Qt.Key_Up) {
            root.selectRow(-1)
            event.accepted = true
          } else if (event.key === Qt.Key_Down) {
            root.selectRow(1)
            event.accepted = true
          } else if (event.key === Qt.Key_PageUp) {
            root.selectPage(-1)
            event.accepted = true
          } else if (event.key === Qt.Key_PageDown) {
            root.selectPage(1)
            event.accepted = true
          } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (root.cursorActive) root.activateIndex(root.selectedIndex)
            else if (root.filteredItems.length > 0) root.cursorActive = true
            event.accepted = true
          } else if (event.text && event.text.length === 1 && event.text.charCodeAt(0) >= 32 && event.text.charCodeAt(0) !== 127) {
            root.setFilter(root.filterText + event.text)
            event.accepted = true
          }
        }
      }

      Column {
        anchors.fill: parent
        anchors.topMargin: card.contentTopInset
        anchors.rightMargin: card.contentRightInset
        anchors.bottomMargin: card.contentBottomInset
        anchors.leftMargin: card.contentLeftInset
        spacing: root.contentSpacing

        Rectangle {
          width: parent.width
          height: root.headerHeight
          radius: root.cornerRadius
          color: "transparent"

          Text {
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.filterText || "Choose a wallpaper…"
            color: root.foreground
            opacity: root.filterText ? 1 : 0.58
            font.family: root.fontFamily
            font.pixelSize: Style.font.heading
            elide: Text.ElideRight
          }
        }

        Text {
          textFormat: Text.PlainText
          width: parent.width
          text: "Type to filter  ·  Arrow keys to move  ·  Enter apply  ·  Esc close"
          color: root.foreground
          opacity: 0.46
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
        }

        Item {
          width: parent.width
          height: parent.height - root.headerHeight
                                     - Style.font.caption - root.contentSpacing * 2

          GridView {
            id: resultGrid
            anchors.fill: parent
            model: root.filteredItems
            clip: true
            cellWidth: root.cellWidth
            cellHeight: root.cellHeight
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
              required property int index
              required property var modelData

              readonly property bool hasCursor: root.cursorActive && index === root.selectedIndex
              readonly property bool isStop: String(modelData.type) === "stop"

              width: root.cellWidth - root.contentSpacing
              height: root.cellHeight
              radius: root.cornerRadius
              color: hasCursor ? root.selectedBackground : "transparent"

              Column {
                anchors.fill: parent
                spacing: Style.space(4)

                Rectangle {
                  width: parent.width
                  height: root.previewHeight
                  radius: Math.max(2, root.cornerRadius - 2)
                  clip: true
                  color: isStop ? root.selectedBackground : Util.alpha(root.border, 0.28)

                  AnimatedImage {
                    visible: !isStop
                    anchors.fill: parent
                    source: isStop ? "" : (modelData.preview ? Util.fileUrl(modelData.preview) : "")
                    fillMode: Image.PreserveAspectCrop
                    cache: false
                    // gifs obey their embedded loop count — wrap forever.
                    onSourceChanged: { currentFrame = 0; playing = true }
                    onCurrentFrameChanged: if (playing && frameCount > 1 && currentFrame >= frameCount - 1) currentFrame = 0
                  }

                  Text {
                    visible: isStop
                    anchors.centerIn: parent
                    text: "■"
                    color: hasCursor ? root.selectedText : root.foreground
                    opacity: 0.8
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.displayLarge
                  }
                }

                Row {
                  width: parent.width
                  spacing: Style.space(6)

                  Text {
                    textFormat: Text.PlainText
                    width: parent.width - (kindChip.visible ? kindChip.width + parent.spacing : 0)
                    text: modelData.title
                    color: hasCursor ? root.selectedText : root.foreground
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.body
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                  }

                  Rectangle {
                    id: kindChip
                    visible: !isStop
                    height: Style.font.body + Style.space(8)
                    width: kindText.width + Style.space(14)
                    radius: height / 2
                    color: Util.alpha(root.border, 0.35)

                    Text {
                      id: kindText
                      anchors.centerIn: parent
                      text: String(modelData.kind).toUpperCase()
                      color: root.foreground
                      opacity: 0.82
                      font.family: root.fontFamily
                      font.pixelSize: Style.font.caption
                    }
                  }
                }
              }

              MouseArea {
                id: mouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onContainsMouseChanged: if (containsMouse) {
                  root.cursorActive = true
                  root.selectedIndex = index
                }
                onClicked: {
                  root.cursorActive = true
                  root.selectedIndex = index
                  root.activateIndex(index)
                }
              }
            }
          }

          Column {
            anchors.centerIn: parent
            spacing: Style.space(8)
            visible: root.filteredItems.length === 0

            Text {
              text: "󰓩"
              color: root.selectedText
              opacity: 0.8
              font.family: root.fontFamily
              font.pixelSize: Style.font.displayLarge
              horizontalAlignment: Text.AlignHCenter
              width: parent.width
            }

            Text {
              textFormat: Text.PlainText
              text: root.filterText ? "No matches for “" + root.filterText + "”"
                                   : "No wallpapers installed"
              color: root.foreground
              opacity: 0.7
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
              horizontalAlignment: Text.AlignHCenter
              width: parent.width
            }
          }
        }
      }
    }
  }

  Component.onCompleted: loader.running = true
}