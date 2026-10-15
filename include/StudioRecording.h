/* Native bounded recording operation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_RECORDING_H
#define LMMS_STUDIO_RECORDING_H
#include <QJsonObject>
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class SampleClip;
class LMMS_EXPORT StudioRecording
{
public:
	static void requireIdle();
	static void requireNoCapture();
	static QJsonObject capture(SampleClip* clip, const QJsonObject& arguments,
		const std::function<bool()>& cancelled = {});
};
}
#endif
