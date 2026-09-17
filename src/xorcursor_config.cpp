/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor_config.h"

#include "xorcursorconfig.h"
#include "kwineffects_interface.h"

#include <KPluginFactory>
#include <KSharedConfig>

#include <QDBusConnection>

namespace KWin
{

	K_PLUGIN_CLASS(XorCursorEffectConfig)

	XorCursorEffectConfig::XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data)
	: KCModule(parent, data)
	{
		m_ui.setupUi(widget());

		// Prefer the effect handler's config object so the KCM and the running
		// effect read and write the same file. If `effects` isn't visible here
		// in your build, replace with:
		//     KSharedConfig::openConfig(QStringLiteral("kwinrc"))
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

#include "xorcursor_config.moc"
