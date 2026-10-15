/* Native render and measured audio observations. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_AUDIO_H
#define LMMS_STUDIO_AUDIO_H
#include <QByteArray>
#include <QJsonObject>
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class LMMS_EXPORT StudioAudio
{
public:
	struct Excerpt { QByteArray wave; QJsonObject observation; };
	using Cancelled = std::function<bool()>;
	static QJsonObject render(const QJsonObject& arguments, const Cancelled& cancelled = {});
	static QJsonObject analyze(const QJsonObject& arguments, const Cancelled& cancelled = {});
	static Excerpt excerpt(const QJsonObject& arguments, const Cancelled& cancelled = {});
};
} // namespace lmms
#endif
