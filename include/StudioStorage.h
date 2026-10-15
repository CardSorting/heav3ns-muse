/* Native session file retirement. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_STORAGE_H
#define LMMS_STUDIO_STORAGE_H

#include <functional>
#include <QDir>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include "lmms_export.h"

namespace lmms
{
class LMMS_EXPORT StudioStorage
{
public:
	static constexpr int MaximumRetiredFiles = 4096;
	// The manifest owns this queue. Never infer ownership from directory contents.
	static bool validRetirements(const QJsonObject& state);
	static bool prepare(const QJsonObject& previous, QJsonObject& next,
		const QSet<QString>& collected, QString& error);
	// Call only after next is durably committed, under the session's exclusive lock.
	// No Qt event dispatch is permitted while verifying/removing these paths.
	static QJsonObject collect(const QDir& directory, const QString& canonicalRoot,
		const QJsonObject& committed, QSet<QString>& collected, int& cursor,
		const std::function<bool()>& cancelled = {});
};
}
#endif
