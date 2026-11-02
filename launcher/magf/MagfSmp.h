// SPDX-License-Identifier: GPL-3.0-only
/*
 *  MAGF Launcher - the "MAGF SMP" instance that ships by default.
 *
 *  Port of MAGF-SMP-Kur.bat: Fabric on a fixed Minecraft version, a curated Modrinth mod list (with required
 *  dependencies), a shader pack and first-run game settings for the mc.magf.tr server.
 */

#pragma once

#include <QFutureWatcher>
#include <QPointer>
#include <utility>
#include <QStringList>
#include <atomic>
#include <memory>

#include "InstanceTask.h"
#include "tasks/Task.h"

class MinecraftInstance;

namespace MagfSmp {

inline const QString g_instanceName = QStringLiteral("MAGF SMP");
inline const QString g_minecraftVersion = QStringLiteral("26.3");
inline const QString g_iconKey = QStringLiteral("magf");

// Returns the MAGF SMP instance, or nullptr if there is none (recognised by the mod manifest in its game folder).
MinecraftInstance* findInstance();

// Downloads/updates the mods, shader packs and default settings in an instance's game folder.
class SyncTask : public Task {
    Q_OBJECT
   public:
    explicit SyncTask(QString gameDir);
    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    QString m_gameDir;
    std::shared_ptr<std::atomic_bool> m_aborted = std::make_shared<std::atomic_bool>(false);
    QFutureWatcher<QString> m_watcher;
};

// Creates the MAGF SMP instance (Fabric + mods + settings) and downloads the game files.
class CreationTask final : public InstanceTask {
    Q_OBJECT
   public:
    CreationTask();
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    void createInstance(const QString& loaderVersion);

    std::unique_ptr<MinecraftInstance> m_instance;
    std::shared_ptr<std::atomic_bool> m_aborted = std::make_shared<std::atomic_bool>(false);
    QFutureWatcher<std::pair<QString, QString>> m_loaderWatcher;
    Task::Ptr m_syncTask;
};

}  // namespace MagfSmp
