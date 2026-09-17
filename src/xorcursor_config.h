/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <KCModule>

#include "ui_xorcursor_config.h"

namespace KWin
{

	class XorCursorEffectConfig : public KCModule
	{
		Q_OBJECT

	public:
		explicit XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data);
		~XorCursorEffectConfig() override;

	private:
		Ui::XorCursorEffectConfigForm m_ui;
	};

} // namespace KWin
