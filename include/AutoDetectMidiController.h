/* Native MIDI controller detection shared with the connection dialog.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_AUTO_DETECT_MIDI_CONTROLLER_H
#define LMMS_AUTO_DETECT_MIDI_CONTROLLER_H
#include "MidiController.h"
namespace lmms
{
class LMMS_EXPORT AutoDetectMidiController : public MidiController
{
public:
	struct Detection
	{
		bool received = false;
		int channel = 0;
		int controller = NONE;
		int value = 0;
		QString port;
	};
	explicit AutoDetectMidiController(Model* parent);
	void processInEvent(const MidiEvent& event, const TimePos& time, f_cnt_t offset = 0) override;
	MidiController* copyToMidiController(Model* parent, bool retainDetectedValue = false);
	void useDetected();
	void reset(bool firstOnly = false);
	const Detection& detection() const { return m_detection; }
private:
	Detection m_detection;
	bool m_firstOnly = false;
};
}
#endif
