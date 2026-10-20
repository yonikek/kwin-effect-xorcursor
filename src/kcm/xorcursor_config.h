/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "ui_xorcursor_config.h"

#include <KCModule>

namespace KWin
{

class XorCursorEffectConfig : public KCModule
{
    Q_OBJECT

public:
    explicit XorCursorEffectConfig(QObject *parent, const KPluginMetaData &data);

    void save() override;

private:
    ::Ui::XorCursorEffectConfig ui;
};

} // namespace KWin
