/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor_config.h"

#include "xorcursorconfig.h"
#include "kwineffects_interface.h"

#include <KPluginFactory>

#include <QDBusConnection>

namespace KWin
{

	K_PLUGIN_CLASS(XorCursorEffectConfig)

	XorCursorEffectConfig::XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data)
	: KCModule(parent, data)
	{
		m_ui.setupUi(widget());

		XorCursorConfig::instance(effects->config());
		addConfig(XorCursorConfig::self(), widget());
	}

	XorCursorEffectConfig::~XorCursorEffectConfig() = default;

	void XorCursorEffectConfig::save()
	{
		KCModule::save();

		OrgKdeKwinEffectsInterface interface(QStringLiteral("org.kde.KWin"),
											 QStringLiteral("/Effects"),
											 QDBusConnection::sessionBus());
		interface.reconfigureEffect(QStringLiteral("xorcursor"));
	}

} // namespace KWin

#include "moc_xorcursor_config.cpp"
