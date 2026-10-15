/* Native bounded automation recording. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_AUTOMATION_RECORDING_H
#define LMMS_STUDIO_AUTOMATION_RECORDING_H
#include <QJsonObject>
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class AutomationClip;
class LMMS_EXPORT StudioAutomationRecording
{
public:
	static QJsonObject capture(AutomationClip* clip, const QJsonObject& arguments,
		const std::function<bool()>& cancelled = {});
};
}
#endif
