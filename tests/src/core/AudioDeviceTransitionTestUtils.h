/* Ordinary native transition fault points, outside Qt dispatch. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TEST_AUDIO_DEVICE_TRANSITION_UTILS_H
#define LMMS_TEST_AUDIO_DEVICE_TRANSITION_UTILS_H
#include <QPointer>
#include <functional>
#include <utility>
#include "AudioEngine.h"
namespace lmms
{
class AudioDeviceTransitionTestAccess
{
public:
	using Point = AudioEngine::DeviceTransitionPoint;
	using Probe = std::function<void(Point)>;
	static Probe exchange(AudioEngine* engine, Probe probe)
	{ return std::exchange(engine->m_deviceTransitionProbe, std::move(probe)); }
	static void notify(AudioEngine* engine, bool quality) { engine->notifyDeviceChange(quality); }
};
using DeviceTransitionPoint = AudioDeviceTransitionTestAccess::Point;
// Owner-thread, lexically nested test scopes only. This is not a public DAW tool
// or a replacement audio device, encoder, plugin, model or DSP implementation.
class AudioDeviceTransitionProbe
{
public:
	AudioDeviceTransitionProbe(AudioEngine* engine, AudioDeviceTransitionTestAccess::Probe probe)
		: m_engine(engine), m_previous(AudioDeviceTransitionTestAccess::exchange(engine, std::move(probe))) { }
	~AudioDeviceTransitionProbe() { reset(); }
	AudioDeviceTransitionProbe(const AudioDeviceTransitionProbe&) = delete;
	AudioDeviceTransitionProbe& operator=(const AudioDeviceTransitionProbe&) = delete;
	void reset()
	{
		if (m_engine) { AudioDeviceTransitionTestAccess::exchange(m_engine.data(), std::move(m_previous)); m_engine.clear(); }
	}
private:
	QPointer<AudioEngine> m_engine;
	AudioDeviceTransitionTestAccess::Probe m_previous;
};
}
#endif
