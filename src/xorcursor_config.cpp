/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor_config.h"

#include "xorcursorconfig.h"

#include <KPluginFactory>

namespace KWin
{

	K_PLUGIN_CLASS(XorCursorEffectConfig)

	XorCursorEffectConfig::XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data)
	: KCModule(parent, data)
	{
		m_ui.setupUi(widget());

		// KWin 6 watches kwinrc and calls reconfigure() on all effects when it
		// changes, so writing the config is sufficient. No D-Bus call needed.
		XorCursorConfig::instance(KSharedConfig::openConfig(QStringLiteral("kwinrc")));
		addConfig(XorCursorConfig::self(), widget());
	}

	XorCursorEffectConfig::~XorCursorEffectConfig() = default;

} // namespace KWin

#include "xorcursor_config.moc"
