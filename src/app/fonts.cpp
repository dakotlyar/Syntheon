// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/fonts.h"

#include <QFontDatabase>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcFonts, "synbench.fonts")

namespace sb {

bool loadBundledFonts()
{
    bool ok = true;
    for (const char *file : {"Archivo-Variable.ttf", "IBMPlexSans-Variable.ttf", "IBMPlexMono-Regular.ttf",
                             "IBMPlexMono-Medium.ttf"}) {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/synbench/fonts/") + QLatin1StringView(file));
        if (id < 0) {
            qCWarning(lcFonts) << "couldn't load the bundled font" << file;
            ok = false;
        }
    }
    return ok;
}

} // namespace sb
