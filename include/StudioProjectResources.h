/* Declared native project dependencies. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_PROJECT_RESOURCES_H
#define LMMS_STUDIO_PROJECT_RESOURCES_H
#include <QJsonObject>
#include "lmms_export.h"
namespace lmms
{
class LMMS_EXPORT StudioProjectResources
{
public:
	// Observes native state without serializing plugins, decoding files or hashing
	// asset bytes. Acquires native model ownership on the application thread.
	static QJsonObject inspect(const QJsonObject& arguments);
};
}
#endif
