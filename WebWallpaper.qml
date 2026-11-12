// hapatapa.omarwalls service — keeps the web-wallpaper renderer process alive.
//
// The wallpaper itself runs in a separate QtWebEngine host process
// (renderer/webwall-host, launched on the wlr-layer-shell Background layer). It
// MUST be a separate process: embedding QtWebEngine inside the omarchy shell
// process FATALs the whole shell. This service only supervises the renderer
// process — it acts on state transitions, so it doesn't thrash the renderer:
// watch the state file, and when it changes from "no wallpaper" to "a
// wallpaper," launch the renderer; when it changes to empty, stop it. The
// renderer itself live-reloads URL→URL changes via its own file watcher.
import Quickshell
import Quickshell.Io
import QtQuick

Item {
  id: root
  property var shell: null

  readonly property string stateHome: (Quickshell.env("HOME") || "") + "/.local/state/omarchy-webwall/current"
  readonly property string pluginDir: Qt.resolvedUrl(".").toString().replace(/^file:\/\//, "").replace(/\/$/, "")
  readonly property string cliPath: {
    var u = Qt.resolvedUrl("bin/omarchy-webwall").toString()
    return u.replace(/^file:\/\//, "")
  }
  property string lastUrl: ""

  // `omarchy plugin add` has no post-install hook: the first time the service
  // loads, link the CLI, add the login autostart and write the menu > Style >
  // Omarwalls row. install.sh is idempotent, and the guard keeps every later
  // shell start from rewriting the menu file.
  Process {
    running: true
    command: ["bash", "-c",
      'test -x "$HOME/.local/bin/omarchy-webwall"'
      + ' && grep -q \'"style.hapatapa-omarwalls".*"label":"Omarwalls"\' "$HOME/.config/omarchy/extensions/omarchy-menu.jsonc" 2>/dev/null'
      + ' || bash ' + JSON.stringify(root.pluginDir + "/install.sh")]
    stdout: StdioCollector {
      onStreamFinished: if (text.trim()) console.log("omarwalls setup:", text.trim())
    }
    stderr: StdioCollector {
      onStreamFinished: if (text.trim()) console.warn("omarwalls setup:", text.trim())
    }
  }

  function openPicker() {
    if (root.shell && typeof root.shell.summon === "function") {
      if (!root.shell.isPluginOpen("hapatapa.omarwalls"))
        root.shell.summon("hapatapa.omarwalls", "{}")
      else root.shell.hide("hapatapa.omarwalls")
    }
  }

  // Read the state file. The FileView keeps `text` in sync asynchronously; the
  // watcher events below drive the transitions. Current state is a wallpaper
  // manifest path (see bin/omarchy-webwall).
  function currentUrl(): string {
    return String(stateFile.text()).trim()
  }

  FileView {
    id: stateFile
    path: root.stateHome
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: root.ensureRenderer()
    onFileChanged: { stateFile.reload() }
    onLoadFailed: root.ensureRenderer()
  }

  // Any content change relaunches: the CLI dispatches shader→shader-host,
  // web/video→webwall-host, so kind switches also need a fresh process. Empty
  // state stops the renderer.
  function ensureRenderer() {
    var url = root.currentUrl()
    if (url === root.lastUrl) return
    root.lastUrl = url
    if (url !== "") {
      Quickshell.execDetached([root.cliPath, "launch"])
    } else {
      Quickshell.execDetached([root.cliPath, "stop"])
    }
  }

  Component.onCompleted: root.ensureRenderer()
}