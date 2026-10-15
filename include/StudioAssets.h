/* Native asset discovery, independent of file browser widgets. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_ASSETS_H
#define LMMS_STUDIO_ASSETS_H
#include <QJsonObject>
#include "lmms_export.h"
namespace lmms
{
class LMMS_EXPORT StudioAssets
{
public:
	static QJsonObject roots(const QJsonObject& arguments);
	static QJsonObject search(const QJsonObject& arguments);
};
}
#endif
