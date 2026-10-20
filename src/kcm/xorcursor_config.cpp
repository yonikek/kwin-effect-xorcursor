/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xorcursor_config.h"

#include "kwineffects_interface.h"
#include "xorcursorsettings.h"

#include <KPluginFactory>
#include <QDBusConnection>

namespace KWin
{

K_PLUGIN_CLASS(XorCursorEffectConfig)

XorCursorEffectConfig::XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    ui.setupUi(widget());

    // The group "Effect-xorcursor" of kwinrc, shared with the effect itself.
    XorCursorSettings::instance(QStringLiteral("kwinrc"));
    addConfig(XorCursorSettings::self(), widget());
}

void XorCursorEffectConfig::save()
{
    KCModule::save();

    // The compositor reloads effect options on request, not on config writes.
    OrgKdeKwinEffectsInterface interface(QStringLiteral("org.kde.KWin"),
                                         QStringLiteral("/Effects"),
                                         QDBusConnection::sessionBus());
    interface.reconfigureEffect(QStringLiteral("xorcursor"));
}

} // namespace KWin

#include "xorcursor_config.moc"
