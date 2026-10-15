/* Native process resource observations. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_RESOURCES_H
#define LMMS_STUDIO_RESOURCES_H

#include <QJsonObject>
#include <QString>
#include "lmms_export.h"

namespace lmms
{
class LMMS_EXPORT StudioResources
{
public:
	// Current LMMS process only; child plugins and external inference are excluded.
	// Unsupported or failed measurements are null, with an explicit source/error.
	static QJsonObject sample();
	static bool validSample(const QJsonObject& sample);
	static QJsonObject interval(const QJsonObject& before, const QJsonObject& after);
	static bool validUsage(const QJsonObject& usage);
	static QJsonObject accumulate(const QJsonObject& usage, const QJsonObject& before, const QJsonObject& after);
	static QString budgetError(const QJsonObject& job, const QJsonObject& before, const QJsonObject& current);
};
}
#endif
