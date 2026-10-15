/*
 * AudioPortAudio.cpp - device-class that performs PCM-output via PortAudio
 *
 * Copyright (c) 2008 Csaba Hruska <csaba.hruska/at/gmail.com>
 * Copyright (c) 2010 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_PORTAUDIO

#include "AudioEngine.h"
#include "AudioPortAudio.h"
#include "ConfigManager.h"
#include "LcdSpinBox.h"
#include "PortAudioCallback.h"
#include "lmms_constants.h"

namespace {
enum class Direction
{
	Input,
	Output
};

constexpr auto tag()
{
	return "audioportaudio";
}

constexpr auto backendAttribute()
{
	return "backend";
}

constexpr auto deviceNameAttribute(Direction direction)
{
	switch (direction)
	{
	case Direction::Input:
		return "inputdevice";
	case Direction::Output:
		return "outputdevice";
	}

	return "";
}

constexpr auto channelsAttribute(Direction direction)
{
	switch (direction)
	{
	case Direction::Input:
		return "inputchannels";
	case Direction::Output:
		return "outputchannels";
	}

	return "";
}

int maxChannels(const PaDeviceInfo* info, Direction direction)
{
	switch (direction)
	{
	case Direction::Input:
		return info->maxInputChannels;
	case Direction::Output:
		return info->maxOutputChannels;
	}

	return 0;
}

} // namespace

namespace lmms {
AudioPortAudio::AudioPortAudio(bool& successful, AudioEngine* engine)
	: AudioDevice(DEFAULT_CHANNELS, engine)
	, m_initGuard(false)
{
	successful = false;
	if (m_initGuard.error() != paNoError)
	{
		std::cerr << "Pa_Initialize() failed: " << Pa_GetErrorText(m_initGuard.error()) << '\n'; return;
	}
	const auto numDevices = Pa_GetDeviceCount();
	if (numDevices < 0)
	{
		std::cerr << "Pa_GetDeviceCount() failed: " << Pa_GetErrorText(numDevices) << '\n';
		successful = false;
		return;
	}

	const auto backend = ConfigManager::inst()->value(tag(), backendAttribute());
	
	const auto inputDeviceName = ConfigManager::inst()->value(tag(), deviceNameAttribute(Direction::Input));
	const auto inputDeviceChannels = ConfigManager::inst()->value(tag(), channelsAttribute(Direction::Input)).toInt();

	const auto outputDeviceName = ConfigManager::inst()->value(tag(), deviceNameAttribute(Direction::Output));
	const auto outputDeviceChannels = ConfigManager::inst()->value(tag(), channelsAttribute(Direction::Output)).toInt();

	auto inputDeviceIndex = paNoDevice;
	auto outputDeviceIndex = paNoDevice;

	for (auto i = 0; i < numDevices && (inputDeviceIndex == paNoDevice || outputDeviceIndex == paNoDevice); ++i)
	{
		const auto deviceInfo = Pa_GetDeviceInfo(i);
		if (!deviceInfo) { continue; }
		const auto hostApiInfo = Pa_GetHostApiInfo(deviceInfo->hostApi);
		if (!hostApiInfo) { continue; }
		if (hostApiInfo->name != backend) { continue; }
		if (deviceInfo->name == inputDeviceName && inputDeviceChannels > 0
			&& inputDeviceChannels <= deviceInfo->maxInputChannels
			&& inputDeviceChannels < std::numeric_limits<ch_cnt_t>::max()) { inputDeviceIndex = i; }
		if (deviceInfo->name == outputDeviceName && outputDeviceChannels > 0
			&& outputDeviceChannels <= deviceInfo->maxOutputChannels
			&& outputDeviceChannels < std::numeric_limits<ch_cnt_t>::max()) { outputDeviceIndex = i; }
	}
	const auto validChannels = [](PaDeviceIndex index, int count, Direction direction) {
		if (index == paNoDevice) { return true; }
		const auto info = Pa_GetDeviceInfo(index);
		return info && count > 0 && count <= maxChannels(info, direction)
			&& count < std::numeric_limits<ch_cnt_t>::max();
	};
	if ((inputDeviceIndex == paNoDevice && outputDeviceIndex == paNoDevice)
		|| !validChannels(inputDeviceIndex, inputDeviceChannels, Direction::Input)
		|| !validChannels(outputDeviceIndex, outputDeviceChannels, Direction::Output))
	{
		std::cerr << "PortAudio: invalid or unavailable native device/channel selection\n"; return;
	}

	const auto sampleRate = engine->baseSampleRate();
	const auto framesPerBuffer = engine->framesPerPeriod();
	const auto inputLatency
		= inputDeviceIndex == paNoDevice ? 0. : Pa_GetDeviceInfo(inputDeviceIndex)->defaultLowInputLatency;
	const auto outputLatency
		= outputDeviceIndex == paNoDevice ? 0. : Pa_GetDeviceInfo(outputDeviceIndex)->defaultLowOutputLatency;

	const auto inputParameters = PaStreamParameters {
		.device = inputDeviceIndex,
		.channelCount = inputDeviceChannels,
		.sampleFormat = paFloat32,
		.suggestedLatency = inputLatency,
		.hostApiSpecificStreamInfo = nullptr
	};
	
	const auto outputParameters = PaStreamParameters {
		.device = outputDeviceIndex,
		.channelCount = outputDeviceChannels,
		.sampleFormat = paFloat32,
		.suggestedLatency = outputLatency,
		.hostApiSpecificStreamInfo = nullptr
	};

	const auto inputParametersPtr = inputDeviceIndex == paNoDevice ? nullptr : &inputParameters; 
	const auto outputParametersPtr = outputDeviceIndex == paNoDevice ? nullptr : &outputParameters;
	auto err = Pa_IsFormatSupported(inputParametersPtr, outputParametersPtr, sampleRate);

	if (err != paFormatIsSupported)
	{
		std::cerr << "Pa_IsFormatSupported() failed: " << Pa_GetErrorText(err) << '\n';
		successful = false;
		return;
	}

	err = Pa_OpenStream(&m_paStream, inputParametersPtr, outputParametersPtr, sampleRate, framesPerBuffer, paNoFlag,
		&AudioPortAudio::processCallback, this);
	
	if (err != paNoError)
	{
		std::cerr << "Pa_OpenStream() failed: " << Pa_GetErrorText(err) << '\n';
		successful = false;
		return;
	}

	err = Pa_SetStreamFinishedCallback(m_paStream, &AudioPortAudio::streamFinished);
	if (err != paNoError)
	{
		std::cerr << "Pa_SetStreamFinishedCallback() failed: " << Pa_GetErrorText(err) << '\n'; return;
	}
	m_inputChannels = inputParametersPtr ? static_cast<ch_cnt_t>(inputDeviceChannels) : 0;
	m_outputChannels = outputParametersPtr ? static_cast<ch_cnt_t>(outputDeviceChannels) : 0;
	m_supportsCapture = m_inputChannels != 0;
	setSampleRate(sampleRate);
	setChannels(m_outputChannels == 0 ? DEFAULT_CHANNELS : m_outputChannels);
	successful = true;
}

AudioPortAudio::~AudioPortAudio()
{
	stopProcessing();
	if (m_paStream)
	{
		const auto error = Pa_CloseStream(m_paStream);
		if (error != paNoError) { std::cerr << "Pa_CloseStream() failed: " << Pa_GetErrorText(error) << '\n'; }
	}
}

void AudioPortAudio::startProcessingImpl()
{
	if (!m_paStream) { reportProcessingFailure(ProcessingFailure::Start, paBadStreamPtr); return; }
	const auto error = Pa_StartStream(m_paStream);
	if (error != paNoError) { reportProcessingFailure(ProcessingFailure::Start, error); }
}

void AudioPortAudio::stopProcessingImpl()
{
	if (!m_paStream) { return; }
	const auto error = Pa_StopStream(m_paStream);
	if (error != paNoError && error != paStreamIsStopped) { reportProcessingFailure(ProcessingFailure::Stop, error); }
}

void AudioPortAudio::streamFinished(void* userData)
{
	const auto device = static_cast<AudioPortAudio*>(userData);
	if (device->isRunning()) { device->reportProcessingFailure(ProcessingFailure::Disconnected); }
}

int detail::processPortAudioBuffer(AudioEngine& engine, ch_cnt_t inputChannels, ch_cnt_t outputChannels,
	const void* input, void* output, unsigned long frames, bool inputLost)
{
	if (frames == 0) { return paContinue; }
	if ((inputChannels == 0 && outputChannels == 0) || inputChannels == DynamicChannelCount
		|| outputChannels == DynamicChannelCount || frames > static_cast<unsigned long>(std::numeric_limits<int>::max())
		|| (outputChannels && !output)) { return paAbort; }
	const auto maximumSamples = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(float);
	const auto widest = std::max(inputChannels, outputChannels);
	if (frames > maximumSamples / widest) { return paAbort; }
	const auto source = static_cast<const float*>(input);
	bool invalid = inputChannels && (inputLost || !source);
	if (inputChannels && !invalid)
	{
		for (std::size_t sample = 0, count = static_cast<std::size_t>(frames) * inputChannels; sample < count; ++sample)
		{
			if (!std::isfinite(source[sample])) { invalid = true; break; }
		}
	}
	std::array<SampleFrame, DEFAULT_BUFFER_SIZE> scratch;
	if (invalid) { engine.pushInputFrames(nullptr, frames); }
	else if (inputChannels)
	{
		for (unsigned long start = 0; start < frames;)
		{
			const auto count = std::min<unsigned long>(scratch.size(), frames - start);
			for (unsigned long frame = 0; frame < count; ++frame)
			{
				const auto index = static_cast<std::size_t>(start + frame) * inputChannels;
				scratch[frame] = SampleFrame(source[index], source[index + (inputChannels > 1 ? 1 : 0)]);
			}
			engine.pushInputFrames(scratch.data(), count); start += count;
		}
	}
	if (outputChannels) { engine.renderNextBuffer({static_cast<float*>(output), outputChannels, frames}); }
	else
	{
		// Input-only streams still drive the authoritative native DSP clock.
		for (unsigned long rendered = 0; rendered < frames;)
		{
			const auto count = std::min<unsigned long>(scratch.size(), frames - rendered);
			engine.renderNextBuffer(InterleavedBufferView<float, DEFAULT_CHANNELS>{scratch.data(), count}); rendered += count;
		}
	}
	return paContinue;
}

int AudioPortAudio::processCallback(const void* input, void* output, unsigned long frameCount,
	const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* userData)
{
	const auto device = static_cast<AudioPortAudio*>(userData);
	const auto channels = device->m_outputChannels;
	const auto outputBuffer = reinterpret_cast<float*>(output);
	const auto silenceOutput = [&] {
		if (outputBuffer && channels && channels != DynamicChannelCount
			&& frameCount <= static_cast<unsigned long>(std::numeric_limits<int>::max())
			&& frameCount <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(float) / channels)
		{
			std::fill_n(outputBuffer, static_cast<std::size_t>(frameCount) * channels, 0.f);
		}
	};

	if (!device->isRunning())
	{
		silenceOutput();
		return paComplete;
	}
	try
	{
		const auto result = detail::processPortAudioBuffer(*device->audioEngine(), device->m_inputChannels, channels,
			input, output, frameCount, flags & (paInputUnderflow | paInputOverflow));
		if (result == paAbort) { silenceOutput(); device->reportProcessingFailure(ProcessingFailure::InvalidCallback); }
		return result;
	}
	catch (...)
	{
		device->reportProcessingFailure(ProcessingFailure::Exception);
		silenceOutput();
		return paAbort;
	}
}
} // namespace lmms

namespace lmms::gui {

class AudioPortAudioSetupWidget::DeviceSelectorWidget : public QGroupBox
{
public:
	DeviceSelectorWidget(const QString& deviceLabel, Direction direction, QWidget* parent = nullptr)
		: QGroupBox{parent}
		, m_deviceComboBox{new QComboBox{this}}
		, m_channelSpinBox{new LcdSpinBox{1, this}}
		, m_direction(direction)
	{
		m_channelSpinBox->setModel(&m_channelModel);

		const auto layout = new QFormLayout{this};
		layout->addRow(deviceLabel, m_deviceComboBox);
		layout->addRow(tr("Channels"), m_channelSpinBox);

		connect(m_deviceComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this](int index) { refreshChannels(m_deviceComboBox->itemData(index).toInt()); });
	}

	void refreshFromConfig(PaHostApiIndex backendIndex)
	{
		using namespace lmms;

		m_deviceComboBox->clear();

		for (auto i = 0, deviceCount = Pa_GetDeviceCount(); i < deviceCount; ++i)
		{
			const auto deviceInfo = Pa_GetDeviceInfo(i);

			if (maxChannels(deviceInfo, m_direction) > 0 && deviceInfo->hostApi == backendIndex)
			{
				m_deviceComboBox->addItem(deviceInfo->name, i);
			}
		}

		const auto selectedDeviceName = ConfigManager::inst()->value(tag(), deviceNameAttribute(m_direction));
		const auto selectedDeviceIndex = std::max(0, m_deviceComboBox->findText(selectedDeviceName));
		m_deviceComboBox->setCurrentIndex(selectedDeviceIndex);
	}

	void refreshChannels(PaDeviceIndex deviceIndex)
	{
		const auto maxChannelCount = maxChannels(Pa_GetDeviceInfo(deviceIndex), m_direction);
		const auto channelCount = ConfigManager::inst()->value(tag(), channelsAttribute(m_direction)).toInt();
		m_channelModel.setRange(1, maxChannelCount);
		m_channelModel.setValue(channelCount == 0 ? DEFAULT_CHANNELS : channelCount);
		m_channelSpinBox->setNumDigits(QString::number(maxChannelCount).length());
	}

	void saveToConfig()
	{
		ConfigManager::inst()->setValue(tag(), deviceNameAttribute(m_direction), m_deviceComboBox->currentText());
		ConfigManager::inst()->setValue(tag(), channelsAttribute(m_direction), QString::number(m_channelModel.value()));
	}

private:
	QComboBox* m_deviceComboBox = nullptr;
	LcdSpinBox* m_channelSpinBox = nullptr;
	IntModel m_channelModel;
	Direction m_direction;
};

AudioPortAudioSetupWidget::AudioPortAudioSetupWidget(QWidget* parent)
	: AudioDeviceSetupWidget{AudioPortAudio::name(), parent}
	, m_backendComboBox{new QComboBox{this}}
	, m_inputDevice{new DeviceSelectorWidget{tr("Input device"), Direction::Input}}
	, m_outputDevice(new DeviceSelectorWidget{tr("Output device"), Direction::Output})
{
	constexpr auto formVerticalSpacing = 10;
	const auto form = new QFormLayout{this};
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setVerticalSpacing(formVerticalSpacing);

	form->addRow(tr("Backend"), m_backendComboBox);
	form->addRow(m_outputDevice);
	form->addRow(m_inputDevice);

	connect(m_backendComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		m_inputDevice->refreshFromConfig(m_backendComboBox->itemData(index).toInt());
		m_outputDevice->refreshFromConfig(m_backendComboBox->itemData(index).toInt());
	});

}

void AudioPortAudioSetupWidget::show()
{
	static auto s_initGuard = detail::PortAudioInitializationGuard{};

	if (m_backendComboBox->count() == 0)
	{
		for (auto i = 0, backendCount = Pa_GetHostApiCount(); i < backendCount; ++i)
		{
			m_backendComboBox->addItem(Pa_GetHostApiInfo(i)->name, i);
		}

		const auto selectedBackendName = ConfigManager::inst()->value(tag(), backendAttribute());
		const auto selectedBackendIndex = std::max(0, m_backendComboBox->findText(selectedBackendName));
		m_backendComboBox->setCurrentIndex(selectedBackendIndex);
	}

	AudioDeviceSetupWidget::show();
}

void AudioPortAudioSetupWidget::saveSettings()
{
	ConfigManager::inst()->setValue(tag(), backendAttribute(), m_backendComboBox->currentText());
	m_inputDevice->saveToConfig();
	m_outputDevice->saveToConfig();
}
} // namespace lmms::gui

#endif // LMMS_HAVE_PORTAUDIO
