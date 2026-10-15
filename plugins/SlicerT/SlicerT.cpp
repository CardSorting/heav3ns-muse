/*
 * SlicerT.cpp - simple slicer plugin
 *
 * Copyright (c) 2023 Daniel Kauss Serna <daniel.kauss.serna@gmail.com>
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

#include "SlicerT.h"

#include <QDomElement>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <fftw3.h>

#include "AudioEngine.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "PathUtil.h"
#include "SlicerTView.h"
#include "Song.h"
#include "embed.h"
#include "interpolation.h"
#include "plugin_export.h"

namespace lmms {

extern "C" {
Plugin::Descriptor PLUGIN_EXPORT slicert_plugin_descriptor = {
	LMMS_STRINGIFY(PLUGIN_NAME),
	"SlicerT",
	QT_TRANSLATE_NOOP("PluginBrowser", "Basic Slicer"),
	"Daniel Kauss Serna <daniel.kauss.serna@gmail.com>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader("logo"),
	nullptr,
	nullptr,
};
} // end extern

// ################################# SlicerT ####################################

SlicerT::SlicerT(InstrumentTrack* instrumentTrack)
	: Instrument(instrumentTrack, &slicert_plugin_descriptor)
	, m_noteThreshold(0.6f, 0.0f, 2.0f, 0.01f, this, tr("Note threshold"))
	, m_fadeOutFrames(10.0f, 0.0f, 100.0f, 0.1f, this, tr("FadeOut"))
	, m_originalBPM(1, 1, 999, this, tr("Original bpm"))
	, m_sliceSnap(this, tr("Slice snap"))
	, m_enableSync(false, this, tr("BPM sync"))
	, m_originalSample()
	, m_parentTrack(instrumentTrack)
{
	m_sliceSnap.addItem("Off");
	m_sliceSnap.addItem("1/1");
	m_sliceSnap.addItem("1/2");
	m_sliceSnap.addItem("1/4");
	m_sliceSnap.addItem("1/8");
	m_sliceSnap.addItem("1/16");
	m_sliceSnap.addItem("1/32");
	m_sliceSnap.setValue(0);
}

namespace {
constexpr std::size_t MaxSlicePoints = 129;

bool validSlicerBuffer(const std::shared_ptr<const SampleBuffer>& buffer)
{
	return buffer && buffer->size() > 1 && buffer->size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())
		&& buffer->sampleRate() > 0 && std::all_of(buffer->begin(), buffer->end(), [](const SampleFrame& frame) {
			return !frame.containsNaN() && !frame.containsInf();
		});
}

struct SlicePlayback
{
	Sample sample;
	Sample::PlaybackState state;
	float start;
	float end;
	int originalFrames;
	double playedSourceFrames = 0;

	SlicePlayback(const Sample& source, float from, float to)
		: sample(source), state(AudioResampler::Mode::Linear, static_cast<int>(static_cast<double>(from) * source.sampleSize()))
		, start(from), end(to), originalFrames(static_cast<int>(source.sampleSize()))
	{
		sample.setStartFrame(state.frameIndex());
		sample.setEndFrame(static_cast<int>(static_cast<double>(to) * source.sampleSize()));
	}
};
} // namespace

double SlicerT::playbackRatio() const
{
	const double syncRatio = m_enableSync.value() ? static_cast<double>(m_originalBPM.value()) / Engine::getSong()->getTempo() : 1;
	return syncRatio * std::exp2(-m_parentTrack->pitchModel()->value() / 1200.0);
}

void SlicerT::playNote(NotePlayHandle* handle, SampleFrame* workingBuffer)
{
	if (m_originalSample.sampleSize() <= 1 || !workingBuffer) { return; }
	if (!handle->m_pluginData)
	{
		const int noteIndex = handle->key() - m_parentTrack->baseNote();
		float from = 0, to = 1;
		if (noteIndex != 0)
		{
			if (noteIndex < 1 || static_cast<std::size_t>(noteIndex) >= m_slicePoints.size()) { emit isPlaying(-1, 0, 0); return; }
			from = m_slicePoints[noteIndex - 1]; to = m_slicePoints[noteIndex];
		}
		handle->m_pluginData = new SlicePlayback(m_originalSample, from, to);
	}
	const auto data = static_cast<SlicePlayback*>(handle->m_pluginData);
	const auto frames = handle->framesLeftForCurrentPeriod(), offset = handle->noteOffset();
	const double ratio = playbackRatio();
	const double outputPerSourceFrame = static_cast<double>(Engine::audioEngine()->outputSampleRate()) / data->sample.sampleRate() * ratio;
	if (!std::isfinite(outputPerSourceFrame) || outputPerSourceFrame <= 0) { return; }
	const double remaining = (data->sample.endFrame() - data->sample.startFrame() - data->playedSourceFrames) * outputPerSourceFrame;
	if (data->sample.play(workingBuffer + offset, &data->state, frames, Sample::Loop::Off, ratio))
	{
		const double fadeFrames = m_fadeOutFrames.value() / 1000.0 * Engine::audioEngine()->outputSampleRate();
		for (f_cnt_t index = 0; index < frames; ++index)
		{
			const float gain = fadeFrames > 0 ? std::clamp((remaining - index) / fadeFrames, 0.0, 1.0) : (remaining > index ? 1 : 0);
			workingBuffer[offset + index] *= cosinusInterpolate(0, 1, gain);
		}
		data->playedSourceFrames += frames / outputPerSourceFrame;
		const float current = std::min<double>(data->end, data->start + data->playedSourceFrames / data->originalFrames);
		emit isPlaying(current, data->start, data->end);
	}
	else { emit isPlaying(-1, 0, 0); }
}

void SlicerT::deleteNotePluginData(NotePlayHandle* handle)
{
	delete static_cast<SlicePlayback*>(handle->m_pluginData);
	emit isPlaying(-1, 0, 0);
}

bool SlicerT::validateSlicePoints(const Sample& sample, const std::vector<float>& points, QString& error) const
{
	if (sample.sampleSize() <= 1) { error = tr("Load a native sample before editing slices"); return false; }
	if (points.size() < 2 || points.size() > MaxSlicePoints || points.front() != 0 || points.back() != 1)
	{
		error = tr("Slice points must include endpoints 0 and 1 and contain 2 through 129 values"); return false;
	}
	int previousFrame = -1;
	float previous = -1;
	for (const float point : points)
	{
		if (!std::isfinite(point) || point < 0 || point > 1 || point <= previous)
		{
			error = tr("Slice points must be finite and strictly increasing within 0 through 1"); return false;
		}
		const int frame = static_cast<int>(static_cast<double>(point) * sample.sampleSize());
		if (frame <= previousFrame) { error = tr("Each native slice must contain at least one source frame"); return false; }
		previous = point; previousFrame = frame;
	}
	error.clear(); return true;
}

int SlicerT::estimateBpm(const Sample& sample)
{
	double bpm = 240.0 * sample.sampleRate() / sample.sampleSize();
	while (bpm < 100) { bpm *= 2; }
	while (bpm > 200) { bpm /= 2; }
	return static_cast<int>(std::round(bpm));
}

bool SlicerT::computeSlices(const Sample& sample, int originalBpm, std::vector<float>& points, QString& error) const
{
	if (!validSlicerBuffer(sample.buffer())) { error = tr("Slicer sample must contain finite audio within native frame bounds"); return false; }
	// Existing native spectral-flux detection; plans and working storage are now bounded and owned.
	// Exclude native audio processing while analyzing. This is not a process-wide FFTW planner lock.
	auto engineGuard = Engine::audioEngine()->requestChangesGuard();
	constexpr int windowSize = 512;
	const int total = static_cast<int>(sample.sampleSize());
	const int minDistance = std::max(1, static_cast<int>(sample.sampleRate() * 0.05));
	const auto mono = [&sample](int frame) { return sample.data()[frame].left() / 2 + sample.data()[frame].right() / 2; };
	float maximum = 0;
	for (int frame = 0; frame < total; ++frame) { maximum = std::max(maximum, std::abs(mono(frame))); }
	std::vector<int> detected{0};
	if (maximum > 0 && total >= windowSize)
	{
		std::array<float, windowSize> input{};
		std::array<fftwf_complex, windowSize / 2 + 1> output{};
		std::array<float, windowSize / 2> previousMagnitudes{};
		const auto plan = fftwf_plan_dft_r2c_1d(windowSize, input.data(), output.data(), FFTW_ESTIMATE);
		if (!plan) { error = tr("Could not create the native slicing FFT plan"); return false; }
		auto cleanup = qScopeGuard([plan] { fftwf_destroy_plan(plan); });
		float previousFlux = 1e-10f;
		int last = 0;
		for (int position = 0; position <= total - windowSize; position += windowSize)
		{
			for (int index = 0; index < windowSize; ++index) { input[index] = mono(position + index) / maximum; }
			fftwf_execute(plan);
			float flux = 1e-10f;
			for (int bin = 0; bin < windowSize / 2; ++bin)
			{
				const float magnitude = std::hypot(output[bin][0], output[bin][1]);
				flux += std::abs(magnitude - previousMagnitudes[bin]); previousMagnitudes[bin] = magnitude;
			}
			if (flux / previousFlux > 1 + m_noteThreshold.value() && position - last > minDistance)
			{
				detected.push_back(position); last = position;
				if (detected.size() >= MaxSlicePoints - 1) { break; }
			}
			previousFlux = flux;
		}
	}
	const double samplesPerBar = sample.sampleRate() * 240.0 / originalBpm;
	const int snap = m_sliceSnap.value() == 0 ? 1 : static_cast<int>(std::clamp(samplesPerBar / std::exp2(m_sliceSnap.value() + 1),
		1.0, static_cast<double>(std::numeric_limits<int>::max())));
	std::vector<float> candidate{0};
	for (std::size_t index = 1; index < detected.size(); ++index)
	{
		int frame = detected[index];
		// The original detector snaps forward to a nearby zero crossing.
		for (int crossing = frame; crossing < std::min(total, frame + windowSize); ++crossing)
		{
			if (crossing > 0 && ((mono(crossing - 1) >= 0) != (mono(crossing) >= 0))) { frame = crossing; break; }
		}
		const double snapped = std::floor((static_cast<double>(frame) + snap / 2.0) / snap) * snap;
		if (snapped > 0 && snapped < total) { candidate.push_back(static_cast<float>(snapped / total)); }
	}
	candidate.push_back(1);
	std::sort(candidate.begin(), candidate.end());
	candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
	if (!validateSlicePoints(sample, candidate, error)) { return false; }
	points.swap(candidate); error.clear(); return true;
}

std::optional<Instrument::SliceLayout> SlicerT::sliceLayout() const
{
	return SliceLayout{static_cast<int>(m_originalSample.sampleSize()), m_originalSample.sampleRate(),
		m_originalBPM.value(), m_enableSync.value(), m_parentTrack->baseNote(), m_slicePoints};
}

bool SlicerT::setSlicePoints(const std::vector<float>& points, QString& error)
{
	if (!validateSlicePoints(m_originalSample, points, error)) { return false; }
	auto candidate = points;
	{ auto guard = Engine::audioEngine()->requestChangesGuard(); m_slicePoints.swap(candidate); }
	emit dataChanged(); error.clear(); return true;
}

bool SlicerT::analyzeSlices(bool estimateTempo, QString& error)
{
	if (!validSlicerBuffer(m_originalSample.buffer())) { error = tr("Load a native sample before analyzing slices"); return false; }
	const int bpm = estimateTempo ? estimateBpm(m_originalSample) : m_originalBPM.value();
	std::vector<float> points;
	if (!computeSlices(m_originalSample, bpm, points, error)) { return false; }
	{
		auto guard = Engine::audioEngine()->requestChangesGuard(); QSignalBlocker blocked(&m_originalBPM);
		m_slicePoints.swap(points); m_originalBPM.setValue(bpm);
	}
	if (estimateTempo) { emit m_originalBPM.dataChanged(); }
	emit dataChanged(); error.clear(); return true;
}

void SlicerT::findSlices() { QString error; if (!analyzeSlices(false, error)) { collectErrorForUI(error); } }
void SlicerT::findBPM()
{
	if (m_originalSample.sampleSize() > 1) { m_originalBPM.setValue(estimateBpm(m_originalSample)); }
}

bool SlicerT::sliceMidi(std::vector<Note>& notes, QString& error) const
{
	if (!validateSlicePoints(m_originalSample, m_slicePoints, error)) { return false; }
	const auto count = m_slicePoints.size() - 1;
	const int base = m_parentTrack->baseNote();
	if (base + 1 < 0 || count > static_cast<std::size_t>(std::max(0, NumKeys - base - 1)))
	{
		error = tr("Slice MIDI exceeds the native keyboard; adjust the base note/master pitch or reduce slices"); return false;
	}
	const double ticksPerSecond = Engine::audioEngine()->outputSampleRate() / Engine::framesPerTick();
	const double ticks = static_cast<double>(m_originalSample.sampleSize()) / m_originalSample.sampleRate() * playbackRatio() * ticksPerSecond;
	if (!std::isfinite(ticks) || ticks <= 0 || ticks > MaxSongLength) { error = tr("Slice MIDI duration exceeds native timeline bounds"); return false; }
	std::vector<Note> candidate;
	candidate.reserve(count);
	for (std::size_t index = 0; index < count; ++index)
	{
		const int start = static_cast<int>(std::round(ticks * m_slicePoints[index]));
		const int end = static_cast<int>(std::round(ticks * m_slicePoints[index + 1]));
		if (end <= start) { error = tr("A slice is shorter than native MIDI tick resolution"); return false; }
		candidate.emplace_back(TimePos(end - start), TimePos(start), base + static_cast<int>(index) + 1);
	}
	notes.swap(candidate); error.clear(); return true;
}

std::vector<Note> SlicerT::getMidi()
{
	std::vector<Note> result; QString error;
	if (!sliceMidi(result, error)) { collectErrorForUI(error); }
	return result;
}

std::vector<Plugin::FileResource> SlicerT::fileResources() const
{
	return {{"sample", tr("Slicer sample"), m_originalSample.sampleFile(), {"wav", "ogg", "flac", "aif", "aiff", "au", "voc", "spx", "ds", "raw"}}};
}

bool SlicerT::setFileResource(const QString& key, const QString& path, QString& error)
{
	if (key != "sample") { error = tr("Unknown SlicerT resource"); return false; }
	Sample candidate;
	std::vector<float> points;
	int bpm = m_originalBPM.value();
	if (!path.isEmpty())
	{
		bool invalid = false; const auto absolute = PathUtil::toAbsolute(path, &invalid);
		const QFileInfo file(absolute);
		if (invalid || !file.isFile() || !file.isReadable()) { error = tr("Slicer sample path is not a readable file"); return false; }
		auto buffer = SampleBuffer::fromFile(path);
		if (!validSlicerBuffer(buffer)) { error = tr("Slicer sample could not be decoded into finite native audio"); return false; }
		candidate = Sample(std::move(buffer)); bpm = estimateBpm(candidate);
		if (!computeSlices(candidate, bpm, points, error)) { return false; }
	}
	{
		auto guard = Engine::audioEngine()->requestChangesGuard(); QSignalBlocker blocked(&m_originalBPM);
		m_originalSample = std::move(candidate); m_slicePoints.swap(points); m_originalBPM.setValue(bpm);
	}
	emit m_originalBPM.dataChanged(); emit dataChanged(); error.clear(); return true;
}

void SlicerT::updateFile(QString file) { QString error; if (!setFileResource("sample", file, error)) { collectErrorForUI(error); } }
void SlicerT::loadFile(const QString& file) { updateFile(file); }
void SlicerT::updateSlices() { findSlices(); }

void SlicerT::saveSettings(QDomDocument& document, QDomElement& element)
{
	element.setAttribute("version", "1");
	element.setAttribute("src", m_originalSample.sampleFile());
	m_originalSample.buffer()->registerResourceSource(document, element, "src", m_originalSample.buffer());
	element.setAttribute("sampleRate", m_originalSample.sampleRate());
	if (m_originalSample.sampleFile().isEmpty()) { m_originalSample.buffer()->saveEmbeddedData(document, element, "sampledata"); }
	element.setAttribute("totalSlices", static_cast<int>(m_slicePoints.size()));
	for (std::size_t index = 0; index < m_slicePoints.size(); ++index)
	{
		element.setAttribute(QString("slice_%1").arg(index), QString::number(m_slicePoints[index], 'g', std::numeric_limits<float>::max_digits10));
	}
	m_fadeOutFrames.saveSettings(document, element, "fadeOut");
	m_noteThreshold.saveSettings(document, element, "threshold");
	m_originalBPM.saveSettings(document, element, "origBPM");
	m_enableSync.saveSettings(document, element, "syncEnable");
	m_sliceSnap.saveSettings(document, element, "snap");
}

void SlicerT::loadSettings(const QDomElement& element)
{
	DataFile::ResourceReferenceScope resource(element, "src");
	m_fadeOutFrames.loadSettings(element, "fadeOut"); m_noteThreshold.loadSettings(element, "threshold");
	m_originalBPM.loadSettings(element, "origBPM"); m_enableSync.loadSettings(element, "syncEnable"); m_sliceSnap.loadSettings(element, "snap");
	Sample candidate;
	const auto path = element.attribute("src"), embedded = element.attribute("sampledata");
	if (!path.isEmpty() || !embedded.isEmpty())
	{
		bool rateOk = true;
		const int rate = element.hasAttribute("sampleRate") ? element.attribute("sampleRate").toInt(&rateOk) : Engine::audioEngine()->outputSampleRate();
		if (!rateOk || rate <= 0) { collectErrorForUI(tr("Invalid embedded slicer sample rate")); return; }
		QString sampleError;
		auto buffer = path.isEmpty() ? SampleBuffer::fromBase64(embedded, rate, sampleError) : SampleBuffer::fromFile(path);
		if (!buffer) { collectErrorForUI(tr("Invalid embedded slicer sample: %1").arg(sampleError)); return; }
		if (!validSlicerBuffer(buffer)) { collectErrorForUI(tr("Invalid or unavailable native slicer sample")); return; }
		candidate = Sample(std::move(buffer));
	}
	std::vector<float> points;
	QString error;
	if (candidate.sampleSize() > 1)
	{
		bool countOk = false; const int count = element.attribute("totalSlices").toInt(&countOk);
		if (countOk && count >= 2 && count <= static_cast<int>(MaxSlicePoints))
		{
			for (int index = 0; index < count; ++index)
			{
				bool ok = false; const auto value = element.attribute(QString("slice_%1").arg(index)).toFloat(&ok);
				if (!ok) { points.clear(); break; } points.push_back(value);
			}
		}
		if (!validateSlicePoints(candidate, points, error))
		{
			if (element.hasAttribute("totalSlices")) { collectErrorForUI(tr("Invalid native slicer points; restoring the complete sample")); }
			points = {0, 1};
		}
	}
	{ auto guard = Engine::audioEngine()->requestChangesGuard(); m_originalSample = std::move(candidate); m_slicePoints.swap(points); }
	emit dataChanged();
}

QString SlicerT::nodeName() const
{
	return slicert_plugin_descriptor.name;
}

gui::PluginView* SlicerT::instantiateView(QWidget* parent)
{
	return new gui::SlicerTView(this, parent);
}

extern "C" {
PLUGIN_EXPORT Plugin* lmms_plugin_main(Model* m, void*)
{
	return new SlicerT(static_cast<InstrumentTrack*>(m));
}
} // extern
} // namespace lmms
