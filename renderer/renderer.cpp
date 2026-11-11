// webwall-host — wallpaper renderer for the hapatapa.omarwalls plugin.
//
// Reads ~/.local/state/omarchy-webwall/current (a path to a wallpaper.json
// manifest in the wallpaper store) and renders the wallpaper on the
// wlr-layer-shell Background layer. Two kinds are handled here (the shader
// kind runs in the separate pure-Vulkan `shader-host`):
//
//   web   — the manifest's entry is an HTML page, rendered with QtWebEngine.
//           The active theme (Omarchy colors.toml) is injected as
//           `window.webwallTheme` so pages can be theme-aware.
//   video — the manifest's entry is a video file, played with QtMultimedia.
//
// Must run in a separate process (embedding QtWebEngine in the shell FATALs).
//
// Usage: webwall-host <web|video> <manifest> <state-file> <theme-file>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <map>
#include <cstdio>
#include <cstdlib>

#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QtWebEngineCore/qwebengineprofile.h>
#include <QtWebEngineCore/qwebenginescript.h>
#include <QtWebEngineCore/qwebenginescriptcollection.h>

static QString readFileText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Read a string field out of a JSON manifest (syntactically naive but accurate
// enough for the flat manifests this plugin generates).
static QString jsonString(QString js, const char *field) {
    int p = js.indexOf(QString("\"%1\"").arg(field));
    if (p < 0) return QString();
    int colon = js.indexOf(':', p + (int)strlen(field) + 2);
    if (colon < 0) return QString();
    int q1 = js.indexOf('"', colon);
    if (q1 < 0) return QString();
    int q2 = js.indexOf('"', q1 + 1);
    if (q2 < 0) return QString();
    return js.mid(q1 + 1, q2 - q1 - 1);
}

// Read Omarchy colors.toml into a JS object literal passed to web wallpapers
// as `window.webwallTheme`. Everything is a "color"-shaped string (#rrggbb).
// The Omarchy theme is the native source of truth (no hooks). colors.toml
// uses flat `key = "#rrggbb"` lines (background/foreground/accent/selection
// + color0..color15); the JS object keeps the same keys the wallpapers read:
//   bg = background, fg = foreground, accent = accent,
//   muted = color8, cursor = accent, selection = selection,
//   ansi0..ansi15 = color0..color15
static QString themeJsObject(const QString &themePath) {
    QString js = readFileText(themePath);
    if (js.isEmpty()) return QStringLiteral("{}");

    // Parse flat TOML: `key = "#rrggbb"` on its own line.
    std::map<QString, QString> toml;
    QStringList lines = js.split('\n', Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1).trimmed();
        // TOML values here are quoted color strings; strip a trailing comment
        // (`key = "#302270" # Night Crossing`) by honoring the quotes.
        if (val.startsWith('"') || val.startsWith('\'')) {
            int end = val.indexOf(val[0], 1);
            val = (end > 1) ? val.mid(1, end - 1) : val.mid(1);
        } else {
            int cut = val.indexOf(' ');
            if (cut < 0 && val.size() > 1) cut = val.indexOf('#', 1);
            if (cut > 0) val = val.left(cut);
        }
        val = val.trimmed();
        if (val.isEmpty()) continue;
        toml[key] = val;
    }
    if (!toml.empty()) {
        auto first = [&](const QStringList &keys, const QString &fallback) {
            for (const QString &k : keys)
                if (toml.count(k)) return toml[k];
            return fallback;
        };
        QStringList parts;
        QString bg  = first({"background"}, "#1e1e2e");
        QString fg  = first({"foreground"}, "#cdd6f4");
        QString acc = first({"accent"}, "#89b4fa");
        QString sel = first({"selection", "selection_background"}, acc);
        QString mut = first({"muted", "color8", "background"}, "#6c7086");
        parts << QString("\"bg\":\"%1\"").arg(bg);
        parts << QString("\"fg\":\"%1\"").arg(fg);
        parts << QString("\"accent\":\"%1\"").arg(acc);
        parts << QString("\"muted\":\"%1\"").arg(mut);
        parts << QString("\"cursor\":\"%1\"").arg(first({"cursor"}, acc));
        parts << QString("\"selection\":\"%1\"").arg(sel);
        for (int i = 0; i < 16; i++) {
            QString k = QString("color%1").arg(i);
            QString v = toml.count(k) ? toml[k] : (i < 6 ? (i == 0 ? bg : i == 1 ? fg : i == 2 ? acc : i == 5 ? sel : mut) : QString());
            if (v.isEmpty()) continue;
            parts << QString("\"ansi%1\":\"%2\"").arg(i).arg(v);
        }
        parts << QStringLiteral("\"_theme\":true");
        return "{" + parts.join(",") + "}";
    }

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(js.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        // fall back to the naive key:value scan (flat generated format)
        QString out;
        int i = 0;
        while (i < js.size()) {
            int q1 = js.indexOf('"', i);
            if (q1 < 0) break;
            int q2 = js.indexOf('"', q1 + 1);
            if (q2 < 0) break;
            QString key = js.mid(q1 + 1, q2 - q1 - 1);
            int col = js.indexOf(':', q2);
            int v1 = js.indexOf('"', col);
            int v2 = js.indexOf('"', v1 + 1);
            if (col < 0 || v1 < 0 || v2 < 0) break;
            QString val = js.mid(v1 + 1, v2 - v1 - 1);
            if (!out.isEmpty()) out += ",";
            out += QString("\"%1\":\"%2\"").arg(key, val);
            i = v2 + 1;
        }
        return "{" + out + "}";
    }
    QJsonObject obj = doc.object();
    QStringList parts2;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        QJsonValue v = it.value();
        if (v.isString())
            parts2 << QString("\"%1\":\"%2\"").arg(it.key(), v.toString());
        else if (v.isDouble())
            parts2 << QString("\"%1\":%2").arg(it.key()).arg(v.toDouble(), 0, 'f', 0);
    }
    return "{" + parts2.join(",") + "}";
}

// Inject the theme + the hi-res scale trick at document creation.
static void installScripts(const QString &themeJson) {
    QWebEngineProfile *prof = QWebEngineProfile::defaultProfile();
    prof->scripts()->clear();

    QWebEngineScript theme;
    theme.setName("webwall-theme");
    theme.setInjectionPoint(QWebEngineScript::DocumentCreation);
    theme.setWorldId(QWebEngineScript::MainWorld);
    theme.setSourceCode(
        "window.webwallTheme = Object.assign({}, " + themeJson + ");");
    prof->scripts()->insert(theme);

    QWebEngineScript hires;
    hires.setName("webwall-hires");
    hires.setInjectionPoint(QWebEngineScript::DocumentCreation);
    hires.setWorldId(QWebEngineScript::MainWorld);
    hires.setSourceCode(QString::fromUtf8(
        "(function(){"
        "  var ow = window.innerWidth, oh = window.innerHeight;"
        "  Object.defineProperty(window, 'innerWidth', { get: function(){ return ow * 10; }, configurable: true });"
        "  Object.defineProperty(window, 'innerHeight', { get: function(){ return oh * 10; }, configurable: true });"
        "})();"
    ));
    prof->scripts()->insert(hires);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: webwall-host <web|video> <manifest> <state-file> <theme-file> <renderer-dir>\n");
        return 1;
    }
    QString mode = QString::fromLatin1(argv[1]);
    QString manifestPath = QString::fromLatin1(argv[2]);
    QString statePath = QString::fromLatin1(argv[3]);
    QString themePath = QString::fromLatin1(argv[4]);
    QString rendererDir = QString::fromLatin1(argv[5]);

    QString man = readFileText(manifestPath);
    if (man.isEmpty()) { fprintf(stderr, "WEBWALL: cannot read manifest %s\n", qUtf8Printable(manifestPath)); return 1; }
    QString entry = jsonString(man, "entry");
    if (entry.isEmpty()) { fprintf(stderr, "WEBWALL: manifest has no entry\n"); return 1; }
    QFileInfo manInfo(manifestPath);
    QString entryAbs = manInfo.dir().filePath(entry);
    QString entryUrl = QUrl::fromLocalFile(entryAbs).toString();

    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
    if (mode == "web") {
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--enable-unsafe-webgpu --autoplay-policy=no-user-gesture-required");
        QtWebEngineQuick::initialize();
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName(mode == "video" ? "hapatapa-webwall-video" : "hapatapa-webwall");

    QString themeJson = themeJsObject(themePath);
    if (mode == "web") installScripts(themeJson);

    QString qml = (mode == "video")
        ? QStringLiteral("renderer-video.qml")
        : QStringLiteral("renderer.qml");

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError> &errs) {
        for (const QQmlError &e : errs)
            fprintf(stderr, "QML: %s:%d %s\n", qUtf8Printable(e.url().toString()),
                    e.line(), qUtf8Printable(e.description()));
    });
    engine.rootContext()->setContextProperty("wallpaperUrl", entryUrl);
    engine.rootContext()->setContextProperty("wallpaperThemeJson", themeJson);
    engine.load(QUrl::fromLocalFile(rendererDir + "/" + qml));
    if (engine.rootObjects().isEmpty()) { fprintf(stderr, "WEBWALL: failed to load %s\n", qUtf8Printable(qml)); return 1; }
    fprintf(stderr, "WEBWALL: mode=%s entry=%s\n", qUtf8Printable(mode), qUtf8Printable(entryUrl));

    // Poll the manifest (entry may change → reload) and colors.toml (colors
    // change) by mtime, like shader-host does. The Omarchy theme is the
    // native source of truth — no hooks. (QFileSystemWatcher is the wrong
    // tool here: omarchy-theme-set replaces colors.toml, which drops the
    // watch mid-replacement and can re-read an empty file.)
    QDateTime themeMtime = QFileInfo(themePath).lastModified();
    QDateTime manMtime = QFileInfo(manifestPath).lastModified();
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        QFileInfo ti(themePath);
        if (ti.exists() && ti.lastModified() != themeMtime) {
            QString tj = themeJsObject(themePath);
            if (tj != QStringLiteral("{}")) {   // {} = raced a replace; retry next tick
                themeMtime = ti.lastModified();
                if (tj != themeJson) {
                    themeJson = tj;
                    engine.rootContext()->setContextProperty("wallpaperThemeJson", themeJson);
                    fprintf(stderr, "WEBWALL: theme reloaded\n");
                    if (mode == "web") {
                        installScripts(themeJson);
                        engine.rootObjects().first()->setProperty("reloadRequested", true);
                    }
                }
            }
        }
        QFileInfo mi(manifestPath);
        if (mi.exists() && mi.lastModified() != manMtime) {
            QString m2 = readFileText(manifestPath);
            if (!m2.isEmpty()) {   // raced a replace; retry next tick
                manMtime = mi.lastModified();
                QString e2 = jsonString(m2, "entry");
                if (!e2.isEmpty()) {
                    QString e2abs = QFileInfo(manifestPath).dir().filePath(e2);
                    QString e2url = QUrl::fromLocalFile(e2abs).toString();
                    if (e2url != entryUrl) {
                        entryUrl = e2url;
                        engine.rootContext()->setContextProperty("wallpaperUrl", entryUrl);
                        fprintf(stderr, "WEBWALL: manifest reload %s\n", qUtf8Printable(entryUrl));
                    }
                }
            }
        }
    });
    poll.start(500);

    return app.exec();
}