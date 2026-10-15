/* Native bounded MIDI recording. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_MIDI_RECORDING_H
#define LMMS_STUDIO_MIDI_RECORDING_H
#include <QJsonObject>
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class MidiClip;
class LMMS_EXPORT StudioMidiRecording
{
public:
	static QJsonObject capture(MidiClip* clip, const QJsonObject& arguments,
		const std::function<bool()>& cancelled = {});
};
}
#endif
