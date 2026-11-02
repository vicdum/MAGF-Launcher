// SPDX-License-Identifier: GPL-3.0-only
/*
 *  MAGF Launcher - the "MAGF SMP" instance that ships by default.
 */

#include "MagfSmp.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QtConcurrentRun>
#include <cmath>
#include <functional>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "HardwareInfo.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "settings/INISettingsObject.h"

namespace MagfSmp {

namespace {

// ---- Settings (same as MAGF-SMP-Kur.bat) -------------------------------------
const QStringList g_mods = { "fabric-api",      "simple-voice-chat", "sodium",          "lithium",          "ferrite-core",
                             "entityculling",   "immediatelyfast",   "krypton",         "modmenu",          "iris",
                             "xaeros-minimap",  "xaeros-world-map",  "maplink",         "distanthorizons",  "no-chat-reports",
                             "appleskin",       "mouse-tweaks",      "sound-physics-remastered",            "dynamic-fps",
                             "cull-leaves",     "lambdynamiclights", "zoomify",         "jei",              "voxy" };
// Mods that are installed disabled by default (they can be enabled from the instance's Mods page).
const QStringList g_optionalMods = { "voxy" };
const QStringList g_shaderPacks = { "complementary-unbound" };
// Version of the default game settings. Each version is applied once, also to older installs.
constexpr int g_defaultsVersion = 4;

const QString g_disabledSuffix = QStringLiteral(".disabled");
const QString g_manifestFile = QStringLiteral(".magf-modlar.json");
const QString g_defaultsMarker = QStringLiteral(".magf-varsayilanlar");
// -----------------------------------------------------------------------------

using StatusFn = std::function<void(const QString&)>;

// Small blocking HTTP client for use on a worker thread.
class Http {
   public:
    QByteArray get(const QUrl& url, QString& error)
    {
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, BuildConfig.USER_AGENT);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(60000);
        QNetworkReply* reply = m_nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QByteArray data;
        if (reply->error() != QNetworkReply::NoError) {
            error = QString("%1: %2").arg(url.toString(), reply->errorString());
        } else {
            data = reply->readAll();
        }
        reply->deleteLater();
        return data;
    }

    QJsonDocument getJson(const QUrl& url, QString& error)
    {
        auto data = get(url, error);
        if (!error.isEmpty()) {
            return {};
        }
        QJsonParseError parseError;
        auto doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            error = QString("%1: %2").arg(url.toString(), parseError.errorString());
        }
        return doc;
    }

   private:
    QNetworkAccessManager m_nam;
};

QString jsonArrayParam(const QString& value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(QString("[\"%1\"]").arg(value)));
}

QString sha512Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha512);
    hash.addData(&f);
    return QString::fromLatin1(hash.result().toHex());
}

bool writeFile(const QString& path, const QByteArray& data)
{
    FS::ensureFilePathExists(path);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(data);
    return f.commit();
}

QStringList readLines(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    auto text = QString::fromUtf8(f.readAll());
    text.replace("\r\n", "\n");
    auto lines = text.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

void setLine(QStringList& lines, const QString& prefix, const QString& line)
{
    for (auto& l : lines) {
        if (l.startsWith(prefix)) {
            l = line;
            return;
        }
    }
    lines.append(line);
}

// Picks the primary file of a Modrinth version (or the first one).
QJsonObject primaryFile(const QJsonObject& version)
{
    const auto files = version.value("files").toArray();
    for (const auto& f : files) {
        if (f.toObject().value("primary").toBool()) {
            return f.toObject();
        }
    }
    return files.isEmpty() ? QJsonObject() : files.first().toObject();
}

// Downloads `file` to `target` unless it is already there with the right hash. Returns an error message or "".
QString fetchFile(Http& http, const QJsonObject& file, const QString& target, bool& downloaded)
{
    const auto expected = file.value("hashes").toObject().value("sha512").toString().toLower();
    downloaded = false;
    if (QFile::exists(target) && sha512Of(target) == expected) {
        return {};
    }
    QString error;
    auto data = http.get(QUrl(file.value("url").toString()), error);
    if (!error.isEmpty()) {
        return error;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex()) != expected) {
        return QString("%1 doğrulanamadı (hash uyuşmuyor).").arg(file.value("filename").toString());
    }
    if (!writeFile(target, data)) {
        return QString("%1 yazılamadı.").arg(target);
    }
    downloaded = true;
    return {};
}

QByteArray serversDat()
{
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::BigEndian);
    auto str = [&s](const QString& v) {
        auto b = v.toUtf8();
        s << static_cast<quint16>(b.size());
        s.writeRawData(b.constData(), b.size());
    };
    s << static_cast<quint8>(10);  // root compound
    str("");
    s << static_cast<quint8>(9);  // list "servers"
    str("servers");
    s << static_cast<quint8>(10) << static_cast<qint32>(1);  // of 1 compound
    s << static_cast<quint8>(8);
    str("ip");
    str("mc.magf.tr");
    s << static_cast<quint8>(8);
    str("name");
    str(g_instanceName);
    s << static_cast<quint8>(0);  // end of list element
    s << static_cast<quint8>(0);  // end of root
    return out;
}

const char* g_mapLinkGeneral = R"({
	"enabled": true,
	"serverEntries": [
		{
			"maptype": "Bluemap",
			"ip": "mc.magf.tr",
			"link": "map.magf.tr",
			"markerVisibilityMode": "All",
			"markerLayers": [
				"ulke-sinirlari",
				"diger-sehirler",
				"sehirler",
				"il-sinirlari",
				"ulkeler"
			],
			"individualMarkerMode": "BlackList",
			"markers": [],
			"areaMarkerVisibilityMode": "All",
			"areaMarkerLayers": [
				"ulke-sinirlari",
				"diger-sehirler",
				"sehirler",
				"il-sinirlari",
				"ulkeler"
			],
			"individualAreaMarkerMode": "BlackList",
			"areaMarkers": [],
			"iconMarkerVisibilityMode": "All",
			"iconMarkerLayers": [
				"ulke-sinirlari",
				"diger-sehirler",
				"sehirler",
				"il-sinirlari",
				"ulkeler"
			],
			"individualIconMode": "BlackList",
			"icons": [],
			"dimensionMapping": {
				"minecraft:overworld": [
					"world"
				]
			}
		}
	],
	"maxUpdateDelay": 2000,
	"defaultY": 64,
	"minimapWaypointsRenderBelow": "WHEN_PLAYER_LIST_SHOWN",
	"enablePlayerWaypoints": true,
	"showPlayerWaypointsAsTrackedPlayers": true,
	"interpolationTime": 100,
	"enablePlayerIconWaypoints": true,
	"useMcHeadsPlayerNameIcons": false,
	"playerWaypointColor": "Black",
	"showAfkInTabList": false,
	"timeUntilAfk": 120,
	"showAfkTimeInTabList": false,
	"hideAfkMinutes": false,
	"AfkColor": 16733440,
	"unknownAfkStateColor": 6316128,
	"enableMarkerWaypoints": true,
	"enableMarkerIcons": true,
	"showDefaultMarkerIcons": true,
	"markerWaypointColor": "Gray",
	"enableAreaMarkerOverlay": true,
	"areaFillAlphaMul": 100,
	"areaFillAlphaMin": 0,
	"areaFillAlphaMax": 42,
	"areaLineAlphaMul": 100,
	"areaLineAlphaMin": 0,
	"areaLineAlphaMax": 100,
	"blocksPerChunkThreshold": 128,
	"maxChunkArea": 500000,
	"excludeOPAC": true,
	"ignoredServers": [],
	"ignoreMarkerMessage": false,
	"hideAllChatErrors": false,
	"invisibilityRecovery": true,
	"debugMode": false,
	"chatLogInDebugMode": false,
	"ignoreCertificatesUseAtYourOwnRisk": false,
	"configVersionDoNotEdit": 2
}
)";

// Default game settings. Only applied once per settings version, so later runs never override the player's choices.
QString applyDefaults(const QDir& game, const StatusFn& status)
{
    int current = 0;
    const auto markerPath = game.filePath(g_defaultsMarker);
    if (QFile::exists(markerPath)) {
        const auto lines = readLines(markerPath);
        bool ok = false;
        current = lines.isEmpty() ? 1 : lines.first().trimmed().toInt(&ok);
        if (!ok) {
            current = 1;
        }
    }
    if (current >= g_defaultsVersion) {
        return {};
    }
    status(QObject::tr("Varsayılan oyun ayarları uygulanıyor..."));

    // options.txt: keep existing lines, only write these keys
    QList<QPair<QString, QString>> options;
    if (current < 1) {
        options << qMakePair(QString("soundCategory_master"), QString("0.03"))  // master volume 3%
                << qMakePair(QString("soundCategory_music"), QString("0.0"))    // music off
                << qMakePair(QString("fov"), QString("0.5"))                    // FOV 90
                << qMakePair(QString("guiScale"), QString("4"))
                << qMakePair(QString("onboardAccessibility"), QString("false"))
                << qMakePair(QString("skipMultiplayerWarning"), QString("true"))
                << qMakePair(QString("key_key.push_to_talk"), QString("key.keyboard.x"));
    }
    // on a Turkish Q keyboard "ö" sits where the US comma key is
    options << qMakePair(QString("key_key.command"), QString("key.keyboard.comma"))
            << qMakePair(QString("tutorialStep"), QString("none"));
    const auto optionsPath = game.filePath("options.txt");
    auto lines = readLines(optionsPath);
    for (const auto& [key, value] : options) {
        setLine(lines, key + ":", key + ":" + value);
    }
    // without a "version:" line the game treats the file as ancient and runs every data fixer on it
    bool hasVersion = std::any_of(lines.begin(), lines.end(), [](const QString& l) { return l.startsWith("version:"); });
    if (!hasVersion) {
        lines.prepend("version:5023");
    }
    if (!writeFile(optionsPath, (lines.join('\n') + '\n').toUtf8())) {
        return QObject::tr("options.txt yazılamadı.");
    }

    // servers.dat: mc.magf.tr in the server list (only if the file does not exist yet)
    const auto serversPath = game.filePath("servers.dat");
    if (!QFile::exists(serversPath)) {
        writeFile(serversPath, serversDat());
    }

    // Simple Voice Chat: push to talk, skip the onboarding
    if (current < 1) {
        const auto vcPath = game.filePath("config/voicechat/voicechat-client.properties");
        auto vc = readLines(vcPath);
        setLine(vc, "microphone_activation_type=", "microphone_activation_type=PTT");
        setLine(vc, "onboarding_finished=", "onboarding_finished=true");
        writeFile(vcPath, (vc.join('\n') + '\n').toUtf8());
    }

    // Map Link: players only on the open world map, not as waypoints on the minimap/HUD
    const QList<QPair<QString, QString>> waypointModes = { { "minimap", "NEVER" }, { "hud", "NEVER" }, { "worldmap", "ALWAYS" } };
    for (const auto& [file, mode] : waypointModes) {
        const auto path = game.filePath(QString("config/maplink/%1.json5").arg(file));
        if (!QFile::exists(path)) {
            writeFile(path, QString("{\n\t\"showPlayerWaypoints\": \"%1\"\n}\n").arg(mode).toUtf8());
        }
    }

    // Map Link: server entry (mc.magf.tr -> map.magf.tr)
    const auto mapLinkPath = game.filePath("config/maplink/general.json5");
    if (!QFile::exists(mapLinkPath)) {
        writeFile(mapLinkPath, QByteArray(g_mapLinkGeneral));
    } else if (current < 3) {
        // older install: only turn off the AFK entries in the tab list
        QFile f(mapLinkPath);
        if (f.open(QIODevice::ReadOnly)) {
            auto text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QRegularExpression(R"(("showAfkInTabList"\s*:\s*)true)"), "\\1false");
            text.replace(QRegularExpression(R"(("showAfkTimeInTabList"\s*:\s*)true)"), "\\1false");
            writeFile(mapLinkPath, text.toUtf8());
        }
    }

    writeFile(markerPath, QByteArray::number(g_defaultsVersion) + "\n");
    return {};
}

// The actual sync. Runs on a worker thread. Returns an error message, or "" on success.
QString syncGameDir(const QString& gameDir, const std::shared_ptr<std::atomic_bool>& aborted, const StatusFn& status, QStringList* warnings)
{
    Http http;
    QDir game(gameDir);
    game.mkpath("mods");
    game.mkpath("shaderpacks");

    // files installed by a previous run
    QStringList previous;
    {
        QFile f(game.filePath(g_manifestFile));
        if (f.open(QIODevice::ReadOnly)) {
            for (const auto& v : QJsonDocument::fromJson(f.readAll()).array()) {
                previous << v.toString().replace('\\', '/');
            }
        }
    }
    QStringList installed;

    // mods, including their required dependencies
    const QString modQuery = "?loaders=" + jsonArrayParam("fabric") + "&game_versions=" + jsonArrayParam(g_minecraftVersion);
    QQueue<QString> queue;
    for (const auto& m : g_mods) {
        queue.enqueue(m);
    }
    QSet<QString> seen;
    while (!queue.isEmpty()) {
        if (*aborted) {
            return QObject::tr("İptal edildi.");
        }
        const auto id = queue.dequeue();
        if (seen.contains(id)) {
            continue;
        }
        seen.insert(id);

        QString error;
        auto versions = http.getJson(QUrl("https://api.modrinth.com/v2/project/" + id + "/version" + modQuery), error).array();
        if (!error.isEmpty()) {
            return error;
        }
        if (versions.isEmpty()) {
            // only warn for listed mods; loader-specific dependencies are skipped silently
            if (g_mods.contains(id)) {
                warnings->append(QObject::tr("%1 için Minecraft %2 sürümü bulunamadı, atlandı.").arg(id, g_minecraftVersion));
            }
            continue;
        }
        const auto version = versions.first().toObject();
        const auto file = primaryFile(version);
        const auto fileName = file.value("filename").toString();
        if (fileName.isEmpty() || fileName.contains('/') || fileName.contains('\\')) {
            continue;
        }

        // optional mods come disabled; the player's enabled/disabled choice is kept on updates
        QString target = "mods/" + fileName;
        if (!game.exists(target) && (g_optionalMods.contains(id) || game.exists(target + g_disabledSuffix))) {
            target += g_disabledSuffix;
        }
        status(QObject::tr("Mod: %1").arg(fileName));
        bool downloaded = false;
        error = fetchFile(http, file, game.filePath(target), downloaded);
        if (!error.isEmpty()) {
            return error;
        }
        installed << target;

        for (const auto& d : version.value("dependencies").toArray()) {
            const auto dep = d.toObject();
            if (dep.value("dependency_type").toString() == "required" && !dep.value("project_id").toString().isEmpty()) {
                queue.enqueue(dep.value("project_id").toString());
            }
        }
    }

    // shader packs
    const QString shaderQuery = "?game_versions=" + jsonArrayParam(g_minecraftVersion);
    for (const auto& id : g_shaderPacks) {
        if (*aborted) {
            return QObject::tr("İptal edildi.");
        }
        QString error;
        auto versions = http.getJson(QUrl("https://api.modrinth.com/v2/project/" + id + "/version" + shaderQuery), error).array();
        if (!error.isEmpty()) {
            return error;
        }
        if (versions.isEmpty()) {
            warnings->append(QObject::tr("%1 shader paketi bulunamadı, atlandı.").arg(id));
            continue;
        }
        const auto file = primaryFile(versions.first().toObject());
        const auto fileName = file.value("filename").toString();
        if (fileName.isEmpty() || fileName.contains('/') || fileName.contains('\\')) {
            continue;
        }
        const QString target = "shaderpacks/" + fileName;
        status(QObject::tr("Shader: %1").arg(fileName));
        bool downloaded = false;
        error = fetchFile(http, file, game.filePath(target), downloaded);
        if (!error.isEmpty()) {
            return error;
        }
        installed << target;
    }

    // remove files from older versions that are no longer needed (only ones we installed)
    for (const auto& old : previous) {
        if (!installed.contains(old) && !old.contains("..")) {
            QFile::remove(game.filePath(old));
        }
    }
    writeFile(game.filePath(g_manifestFile), QJsonDocument(QJsonArray::fromStringList(installed)).toJson());

    return applyDefaults(game, status);
}

// Latest stable Fabric loader for the configured Minecraft version. Runs on a worker thread.
std::pair<QString, QString> fetchLoaderVersion()
{
    Http http;
    QString error;
    auto loaders = http.getJson(QUrl("https://meta.fabricmc.net/v2/versions/loader/" + g_minecraftVersion), error).array();
    if (!error.isEmpty()) {
        return { {}, error };
    }
    if (loaders.isEmpty()) {
        return { {}, QObject::tr("Fabric henüz Minecraft %1 sürümünü desteklemiyor.").arg(g_minecraftVersion) };
    }
    for (const auto& l : loaders) {
        auto loader = l.toObject().value("loader").toObject();
        if (loader.value("stable").toBool()) {
            return { loader.value("version").toString(), {} };
        }
    }
    return { loaders.first().toObject().value("loader").toObject().value("version").toString(), {} };
}

}  // namespace

MinecraftInstance* findInstance()
{
    auto* instances = APPLICATION->instances();
    const auto savedId = APPLICATION->settings()->get("MagfSmpInstanceId").toString();
    if (auto* inst = instances->getInstanceById(savedId); inst && QFile::exists(FS::PathCombine(inst->gameRoot(), g_manifestFile))) {
        return inst;
    }
    for (int i = 0; i < instances->count(); i++) {
        auto* inst = instances->at(i);
        if (QFile::exists(FS::PathCombine(inst->gameRoot(), g_manifestFile))) {
            return inst;
        }
    }
    return nullptr;
}

// ---- SyncTask ------------------------------------------------------------------

SyncTask::SyncTask(QString gameDir) : m_gameDir(std::move(gameDir)) {}

bool SyncTask::abort()
{
    *m_aborted = true;
    return true;
}

void SyncTask::executeTask()
{
    setStatus(tr("MAGF SMP modları kontrol ediliyor..."));
    auto warnings = std::make_shared<QStringList>();
    QPointer<SyncTask> self(this);
    StatusFn status = [self](const QString& text) {
        QMetaObject::invokeMethod(
            qApp,
            [self, text] {
                if (self) {
                    self->setStatus(text);
                }
            },
            Qt::QueuedConnection);
    };
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, [this, warnings] {
        for (const auto& w : *warnings) {
            logWarning(w);
        }
        if (*m_aborted) {
            emitAborted();
            return;
        }
        const auto error = m_watcher.result();
        if (!error.isEmpty()) {
            emitFailed(error);
            return;
        }
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([gameDir = m_gameDir, aborted = m_aborted, status, warnings] {
        return syncGameDir(gameDir, aborted, status, warnings.get());
    }));
}

// ---- CreationTask --------------------------------------------------------------

CreationTask::CreationTask()
{
    setName(g_instanceName);
    setIcon(g_iconKey);
}

bool CreationTask::abort()
{
    *m_aborted = true;
    if (m_syncTask && m_syncTask->isRunning()) {
        return m_syncTask->abort();
    }
    if (m_loaderWatcher.isRunning()) {
        return true;  // the watcher reports the abort when the request returns
    }
    return InstanceTask::abort();
}

void CreationTask::executeTask()
{
    setStatus(tr("Fabric sürümü bulunuyor..."));
    setAbortable(true);
    connect(&m_loaderWatcher, &QFutureWatcher<std::pair<QString, QString>>::finished, this, [this] {
        if (*m_aborted) {
            emitAborted();
            return;
        }
        auto [version, error] = m_loaderWatcher.result();
        if (!error.isEmpty()) {
            emitFailed(error);
            return;
        }
        createInstance(version);
    });
    m_loaderWatcher.setFuture(QtConcurrent::run(fetchLoaderVersion));
}

void CreationTask::createInstance(const QString& loaderVersion)
{
    setStatus(tr("MAGF SMP oluşturuluyor (Minecraft %1, Fabric %2)...").arg(g_minecraftVersion, loaderVersion));

    m_instance = std::make_unique<MinecraftInstance>(
        m_globalSettings, std::make_unique<INISettingsObject>(FS::PathCombine(m_stagingPath, "instance.cfg")), m_stagingPath);
    {
        const SettingsObject::Lock lock(m_instance->settings());

        auto* components = m_instance->getPackProfile();
        components->buildingFromScratch();
        components->setComponentVersion("net.minecraft", g_minecraftVersion, true);
        components->setComponentVersion("net.fabricmc.fabric-loader", loaderVersion);

        m_instance->setName(name());
        m_instance->setIconKey(m_instIcon);

        // Memory and GC depending on the computer: roughly half of the RAM, 2-8 GB
        const auto ramGB = static_cast<int>(std::lround(static_cast<double>(HardwareInfo::totalRamMiB()) / 1024.0));
        const auto cores = QThread::idealThreadCount();
        int xmx = 8;
        if (ramGB <= 4) {
            xmx = 2;
        } else if (ramGB <= 6) {
            xmx = 3;
        } else if (ramGB <= 8) {
            xmx = 4;
        } else if (ramGB <= 12) {
            xmx = 5;
        } else if (ramGB <= 16) {
            xmx = 6;
        }
        auto* s = m_instance->settings();
        s->set("OverrideMemory", true);
        s->set("MinMemAlloc", 512);
        s->set("MaxMemAlloc", xmx * 1024);
        s->set("OverrideJavaArgs", true);
        // Minecraft 26.x runs on Java 25, where ZGC is generational already
        s->set("JvmArgs", (ramGB >= 12 && cores >= 4) ? QString("-XX:+UseZGC")
                                                      : QString("-XX:+UseG1GC -XX:+ParallelRefProcEnabled -XX:MaxGCPauseMillis=200"));

        components->saveNow();
    }

    m_syncTask = makeShared<SyncTask>(m_instance->gameRoot());
    connect(m_syncTask.get(), &Task::status, this, &CreationTask::setStatus);
    connect(m_syncTask.get(), &Task::warningLogged, this, [this](const QString& line) { logWarning(line); });
    connect(m_syncTask.get(), &Task::failed, this, &CreationTask::emitFailed);
    connect(m_syncTask.get(), &Task::aborted, this, &CreationTask::emitAborted);
    connect(m_syncTask.get(), &Task::succeeded, this, [this] { downloadFiles(m_instance.get()); });
    m_syncTask->start();
}

}  // namespace MagfSmp
