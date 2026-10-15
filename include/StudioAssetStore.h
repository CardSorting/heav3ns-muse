/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_ASSET_STORE_H
#define LMMS_STUDIO_ASSET_STORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include "lmms_export.h"

namespace lmms
{

class FileResourceSnapshot;

class LMMS_EXPORT StudioAssetStore
{
public:
	static bool preserve(const QString& source, const QString& directory, const QString& suffix,
		QJsonObject& reference, QString& error, const FileResourceSnapshot* accepted = nullptr);
	static bool validReferences(const QJsonArray& references);
	static bool verify(const QString& checkpointDirectory, const QJsonArray& references, QString& error);
};

} // namespace lmms

#endif // LMMS_STUDIO_ASSET_STORE_H
