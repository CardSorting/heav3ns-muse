/* In-process inference adapters. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_INFERENCE_H
#define LMMS_STUDIO_INFERENCE_H

#include <functional>
#include <QJsonArray>
#include <QJsonObject>
#include "lmms_export.h"

namespace lmms
{

// Providers return decisions; LMMS owns every execution and durability boundary.
class LMMS_EXPORT StudioInference
{
public:
	using Cancelled = std::function<bool()>;
	using Transport = std::function<QJsonObject(const QJsonObject&, const QJsonObject&, const Cancelled&)>;
	static QJsonObject configuration(const QJsonObject& input, bool audio = false);
	static QJsonObject requestBody(const QJsonObject& configuration, const QJsonArray& input,
		const QJsonArray& tools, const QString& instructions);
	static QJsonObject interpret(const QJsonObject& response, const QString& protocol);
	static QJsonObject generate(const QJsonObject& configuration, const QJsonArray& input,
		const QJsonArray& tools, const QString& instructions, const Cancelled& cancelled = {}, const Transport& transport = {});
	static QJsonObject listen(const QJsonObject& configuration, const QByteArray& wave,
		const QString& prompt, const Cancelled& cancelled = {}, const Transport& transport = {});
};

} // namespace lmms
#endif
