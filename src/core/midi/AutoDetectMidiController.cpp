/* Native MIDI controller detection, extracted from ControllerConnectionDialog.
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "AutoDetectMidiController.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "MidiClient.h"
#include "Song.h"
#include <memory>

namespace lmms
{
AutoDetectMidiController::AutoDetectMidiController(Model* parent) : MidiController(parent) { updateName(); }

void AutoDetectMidiController::processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t)
{
	if (isRetiring()) { return; }
	if (!event.isValidControlChange()) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if (event.source() == MidiEvent::Source::External
		&& (Engine::audioEngine()->hasStoredAudioDevice() || (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup())))) { return; }
	if ((m_midiPort.inputChannel() != 0 && m_midiPort.inputChannel() != event.channel() + 1)
		|| (m_firstOnly && m_detection.received)) { return; }
	m_detection = {true, event.channel() + 1, event.controllerNumber(), event.controllerValue(),
		m_midiPort.client()->sourcePortName(event)};
	notifyValueChange();
}

MidiController* AutoDetectMidiController::copyToMidiController(Model* parent, bool retainDetectedValue)
{
	auto controller = std::make_unique<MidiController>(parent);
	controller->m_midiPort.setInputChannel(m_midiPort.inputChannel());
	controller->m_midiPort.setInputController(m_midiPort.inputController());
	controller->subscribeReadablePorts(m_midiPort.readablePorts());
	controller->updateName();
	if (retainDetectedValue && m_detection.received)
	{
		controller->m_previousValue = controller->m_lastValue = static_cast<float>(m_detection.value) / 127.f;
		controller->updateValueBuffer();
	}
	return controller.release();
}

void AutoDetectMidiController::useDetected()
{
	if (!m_detection.received) { return; }
	m_midiPort.setInputChannel(m_detection.channel);
	m_midiPort.setInputController(m_detection.controller);
	const auto ports = m_midiPort.readablePorts();
	for (auto it = ports.cbegin(); it != ports.cend(); ++it)
	{
		m_midiPort.subscribeReadablePort(it.key(), m_detection.port.isEmpty() || it.key() == m_detection.port);
	}
	updateName();
}

void AutoDetectMidiController::reset(bool firstOnly)
{
	m_detection = {}; m_firstOnly = firstOnly;
	m_midiPort.setInputChannel(0); m_midiPort.setInputController(NONE); updateName();
}
}
