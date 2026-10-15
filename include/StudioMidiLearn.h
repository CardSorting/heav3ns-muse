/* Bounded native parameter MIDI learn. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_MIDI_LEARN_H
#define LMMS_STUDIO_MIDI_LEARN_H
#include <QJsonObject>
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class AutomatableModel;
class LMMS_EXPORT StudioMidiLearn
{
public:
	// Preserve the linked source while waiting; checked native assignment owns
	// adoption and retires detector/prior resources before final notification.
	// Post-publication failure recovery belongs to the enclosing Runtime.
	static QJsonObject learn(AutomatableModel* model, const QJsonObject& arguments,
		const std::function<bool()>& cancelled = {});
};
}
#endif
