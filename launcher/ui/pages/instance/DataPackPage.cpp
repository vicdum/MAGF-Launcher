// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "DataPackPage.h"
#include "ui_ExternalResourcesPage.h"

#include "minecraft/PackProfile.h"
#include "settings/Setting.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"

DataPackPage::DataPackPage(MinecraftInstance* instance, DataPackFolderModel* model, QWidget* parent)
    : ExternalResourcesPage(instance, model, parent), m_model(model)
{
}

void DataPackPage::updateFrame(const QModelIndex& current, [[maybe_unused]] const QModelIndex& previous)
{
    auto sourceCurrent = m_filterModel->mapToSource(current);
    int row = sourceCurrent.row();
    auto& dp = m_model->at(row);
    m_ui->frame->updateWithDataPack(dp);
}

GlobalDataPackPage::GlobalDataPackPage(MinecraftInstance* instance, QWidget* parent) : QWidget(parent), m_instance(instance)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    connect(instance->settings()->getSetting("GlobalDataPacksEnabled").get(), &Setting::SettingChanged, this, [this] {
        updateContent();
        if (m_container != nullptr) {
            m_container->refreshContainer();
        }
    });

    connect(instance->settings()->getSetting("GlobalDataPacksPath").get(), &Setting::SettingChanged, this,
            &GlobalDataPackPage::updateContent);
}

QString GlobalDataPackPage::displayName() const
{
    if (m_underlyingPage == nullptr) {
        return {};
    }

    return m_underlyingPage->displayName();
}

QIcon GlobalDataPackPage::icon() const
{
    if (m_underlyingPage == nullptr) {
        return {};
    }

    return m_underlyingPage->icon();
}

QString GlobalDataPackPage::helpPage() const
{
    if (m_underlyingPage == nullptr) {
        return {};
    }

    return m_underlyingPage->helpPage();
}

bool GlobalDataPackPage::shouldDisplay() const
{
    return m_instance->settings()->get("GlobalDataPacksEnabled").toBool();
}

bool GlobalDataPackPage::apply()
{
    return m_underlyingPage == nullptr || m_underlyingPage->apply();
}

void GlobalDataPackPage::openedImpl()
{
    if (m_underlyingPage != nullptr) {
        m_underlyingPage->openedImpl();
    }
}

void GlobalDataPackPage::closedImpl()
{
    if (m_underlyingPage != nullptr) {
        m_underlyingPage->closedImpl();
    }
}

void GlobalDataPackPage::updateContent()
{
    if (m_underlyingPage != nullptr) {
        if (m_container->selectedPage() == this) {
            m_underlyingPage->closedImpl();
        }

        m_underlyingPage->apply();

        layout()->removeWidget(m_underlyingPage);

        delete m_underlyingPage;
        m_underlyingPage = nullptr;
    }

    if (shouldDisplay()) {
        m_underlyingPage = new DataPackPage(m_instance, m_instance->dataPackList());
        m_underlyingPage->setParentContainer(m_container);
        m_underlyingPage->updateExtraInfo = [this](QString id, QString value) { updateExtraInfo(std::move(id), std::move(value)); };

        if (m_container->selectedPage() == this) {
            m_underlyingPage->openedImpl();
        }

        layout()->addWidget(m_underlyingPage);
    }
}
void GlobalDataPackPage::setParentContainer(BasePageContainer* container)
{
    BasePage::setParentContainer(container);
    updateContent();
}
