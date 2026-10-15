/*
 * SampleDecoder.cpp - Decodes audio files in various formats
 *
 * Copyright (c) 2023 saker <sakertooth@gmail.com>
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

#include "SampleDecoder.h"
#include "SampleDecoderBackends.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <array>
#include <stdexcept>
#include <QScopeGuard>
#include <sndfile.h>

#ifdef LMMS_HAVE_OGGVORBIS
#include <vorbis/vorbisfile.h>
#endif

#include "AudioEngine.h"
#include "AudioResampler.h"
#include "DrumSynth.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "LmmsTypes.h"

namespace lmms {

namespace {

using Decoder = std::optional<SampleDecoder::Result> (*)(const QString&);

auto decodeSampleSF(const QString& audioFile) -> std::optional<SampleDecoder::Result>;
auto decodeSampleDS(const QString& audioFile) -> std::optional<SampleDecoder::Result>;

static constexpr Decoder decoders[] = {&decodeSampleSF,
#ifdef LMMS_HAVE_OGGVORBIS
	&detail::decodeSampleOggVorbis,
#endif
	&decodeSampleDS};

auto decodeSampleSF(const QString& audioFile) -> std::optional<SampleDecoder::Result>
{
	QString error; if (!FileResourceIO::check(error)) { return std::nullopt; }
	auto sfInfo = SF_INFO{};

	// TODO: Remove use of QFile
	auto file = QFile{audioFile};
	if (!file.open(QIODevice::ReadOnly)) { return std::nullopt; }

	auto sndFile = std::unique_ptr<SNDFILE, decltype(&sf_close)>(
		sf_open_fd(file.handle(), SFM_READ, &sfInfo, false), &sf_close);
	if (!sndFile || sf_error(sndFile.get()) != 0 || sfInfo.frames <= 0 || sfInfo.channels <= 0
		|| sfInfo.samplerate <= 0 || sfInfo.frames > std::numeric_limits<int>::max()) { return std::nullopt; }
	const auto frames = static_cast<std::size_t>(sfInfo.frames);
	const auto channels = static_cast<std::size_t>(sfInfo.channels);
	const auto blockFrames = std::min(frames, std::size_t(4096));
	if (blockFrames > std::vector<sample_t>().max_size() / channels
		|| frames > std::vector<SampleFrame>().max_size() || !FileResourceIO::check(error)) { return std::nullopt; }
	auto buf = std::vector<sample_t>(channels * blockFrames);
	auto result = std::vector<SampleFrame>(frames);
	std::size_t decoded = 0;
	while (decoded < frames)
	{
		if (!FileResourceIO::check(error)) { return std::nullopt; }
		const auto count = std::min(blockFrames, frames - decoded);
		if (sf_readf_float(sndFile.get(), buf.data(), static_cast<sf_count_t>(count)) != static_cast<sf_count_t>(count)
			|| sf_error(sndFile.get()) != 0
			|| std::any_of(buf.begin(), buf.begin() + count * channels, [](sample_t value) { return !std::isfinite(value); }))
		{
			return std::nullopt;
		}
		for (std::size_t index = 0; index < count; ++index)
		{
			// Retain native mono upmix / first-two-channel behavior.
			const auto left = buf[index * channels];
			result[decoded + index] = {left, channels == 1 ? left : buf[index * channels + 1]};
		}
		decoded += count;
	}
	if (!FileResourceIO::check(error) || sf_close(sndFile.release()) != 0) { return std::nullopt; }
	file.close();

	return SampleDecoder::Result{std::move(result), static_cast<int>(sfInfo.samplerate)};
}

auto decodeSampleDS(const QString& audioFile) -> std::optional<SampleDecoder::Result>
{
	QString error; if (!FileResourceIO::check(error)) { return std::nullopt; }
	// Populated by DrumSynth::GetDSFileSamples
	int_sample_t* dataPtr = nullptr;

	auto ds = DrumSynth{};
	const auto engineRate = Engine::audioEngine()->outputSampleRate();
	const auto frames = ds.GetDSFileSamples(audioFile, dataPtr, DEFAULT_CHANNELS, engineRate);
	const auto data = std::unique_ptr<int_sample_t[]>{dataPtr}; // NOLINT, we have to use a C-style array here

	if (frames <= 0 || frames > std::numeric_limits<int>::max() / DEFAULT_CHANNELS || !data
		|| !FileResourceIO::check(error)) { return std::nullopt; }

	auto result = std::vector<SampleFrame>(frames);
	for (int offset = 0; offset < frames;)
	{
		if (!FileResourceIO::check(error)) { return std::nullopt; }
		const int count = std::min(4096, frames - offset);
		src_short_to_float_array(data.get() + offset * DEFAULT_CHANNELS, &result[offset][0], count * DEFAULT_CHANNELS);
		offset += count;
	}

	return SampleDecoder::Result{std::move(result), static_cast<int>(engineRate)};
}

} // namespace

#ifdef LMMS_HAVE_OGGVORBIS
auto detail::decodeSampleOggVorbis(const QString& audioFile) -> std::optional<SampleDecoder::Result>
{
	return decodeSampleOggVorbis(audioFile, nullptr);
}

auto detail::decodeSampleOggVorbis(const QString& audioFile, bool* recognized) -> std::optional<SampleDecoder::Result>
{
	if (recognized) { *recognized = false; }
	QString error; if (!FileResourceIO::check(error)) { return std::nullopt; }
	static auto s_read = [](void* buffer, size_t size, size_t count, void* stream) -> size_t {
		auto file = static_cast<QFile*>(stream);
		if (size == 0 || count == 0 || count > std::numeric_limits<size_t>::max() / size
			|| count > static_cast<quint64>(std::numeric_limits<qint64>::max()) / size) { return 0; }
		const auto bytes = file->read(static_cast<char*>(buffer), static_cast<qint64>(size * count));
		return bytes > 0 ? static_cast<size_t>(bytes) / size : 0;
	};

	static auto s_seek = [](void* stream, ogg_int64_t offset, int whence) -> int {
		auto file = static_cast<QFile*>(stream);
		qint64 base;
		if (whence == SEEK_SET) { base = 0; }
		else if (whence == SEEK_CUR) { base = file->pos(); }
		else if (whence == SEEK_END) { base = file->size(); }
		else { return -1; }
		if (base < 0 || offset < -base || offset > std::numeric_limits<qint64>::max() - base) { return -1; }
		return file->seek(base + offset) ? 0 : -1;
	};

	static auto s_tell = [](void* stream) -> long {
		auto file = static_cast<QFile*>(stream);
		const auto position = file->pos();
		return position >= 0 && position <= std::numeric_limits<long>::max() ? static_cast<long>(position) : -1;
	};

	// QFile owns its descriptor; ov_clear releases only the native decoder state.
	static ov_callbacks s_callbacks = {s_read, s_seek, nullptr, s_tell};

	// TODO: Remove use of QFile
	auto file = QFile{audioFile};
	if (!file.open(QIODevice::ReadOnly)) { return std::nullopt; }

	auto vorbisFile = OggVorbis_File{};
	if (ov_open_callbacks(&file, &vorbisFile, nullptr, 0, s_callbacks) < 0) { return std::nullopt; }
	const auto cleanup = qScopeGuard([&] { ov_clear(&vorbisFile); });
	if (recognized) { *recognized = true; }

	const auto vorbisInfo = ov_info(&vorbisFile, 0);
	if (vorbisInfo == nullptr || vorbisInfo->channels <= 0 || vorbisInfo->rate <= 0
		|| vorbisInfo->rate > std::numeric_limits<int>::max()) { return std::nullopt; }

	const auto sampleRate = vorbisInfo->rate;
	const auto frames = ov_pcm_total(&vorbisFile, -1);
	if (frames <= 0 || frames > std::numeric_limits<int>::max()
		|| static_cast<std::size_t>(frames) > std::vector<SampleFrame>().max_size()) { return std::nullopt; }
	const auto links = ov_streams(&vorbisFile);
	if (links <= 0 || links > std::numeric_limits<int>::max()) { return std::nullopt; }
	struct Link { ogg_int64_t frames; int rate; int channels; std::size_t outputLimit; };
	std::vector<Link> layout;
	std::size_t outputLimit = 0;
	ogg_int64_t plannedInput = 0;
	for (long index = 0; index < links; ++index)
	{
		if (!FileResourceIO::check(error)) { return std::nullopt; }
		const auto info = ov_info(&vorbisFile, static_cast<int>(index));
		const auto length = ov_pcm_total(&vorbisFile, static_cast<int>(index));
		if (!info || info->channels <= 0 || info->rate <= 0 || info->rate > std::numeric_limits<int>::max()
			|| length < 0 || length > frames - plannedInput) { return std::nullopt; }
		const double ratio = static_cast<double>(sampleRate) / info->rate;
		if (!src_is_valid_ratio(ratio)) { return std::nullopt; }
		// Both factors fit positive native int, so this product fits qint64.
		const qint64 scaled = length * sampleRate;
		const auto limit = static_cast<std::size_t>(scaled / info->rate + (scaled % info->rate != 0));
		if (limit > static_cast<std::size_t>(std::numeric_limits<int>::max()) - outputLimit) { return std::nullopt; }
		layout.push_back({length, static_cast<int>(info->rate), info->channels, limit});
		outputLimit += limit; plannedInput += length;
	}
	if (plannedInput != frames || outputLimit == 0 || outputLimit > std::vector<SampleFrame>().max_size()
		|| !FileResourceIO::check(error)) { return std::nullopt; }
	std::vector<SampleFrame> result;
	result.reserve(outputLimit);
	std::array<SampleFrame, 4096> input, converted;
	auto output = static_cast<float**>(nullptr);
	ogg_int64_t position = 0;
	for (std::size_t index = 0; index < layout.size(); ++index)
	{
		if (!FileResourceIO::check(error)) { return std::nullopt; }
		const auto& section = layout[index];
		if (section.frames == 0) { continue; }
		if (ov_pcm_seek(&vorbisFile, position) != 0) { return std::nullopt; }
		position += section.frames;
		std::unique_ptr<AudioResampler> resampler;
		try
		{
			if (section.rate != sampleRate)
			{
				resampler = std::make_unique<AudioResampler>(AudioResampler::Mode::SincBest);
				resampler->setRatio(static_cast<sample_rate_t>(section.rate), static_cast<sample_rate_t>(sampleRate));
			}
			const auto begin = result.size();
			const auto append = [&](const SampleFrame* data, std::size_t count) {
				if (count > section.outputLimit - (result.size() - begin)
					|| std::any_of(data, data + count, [](const SampleFrame& value) { return value.containsNaN() || value.containsInf(); })) { return false; }
				result.insert(result.end(), data, data + count); return true;
			};
			ogg_int64_t decoded = 0;
			while (decoded < section.frames)
			{
				if (!FileResourceIO::check(error)) { return std::nullopt; }
				int link = -1;
				const auto count = ov_read_float(&vorbisFile, &output,
					static_cast<int>(std::min<ogg_int64_t>(input.size(), section.frames - decoded)), &link);
				if (count <= 0 || link != static_cast<int>(index) || !output || !output[0]
					|| count > section.frames - decoded || count > static_cast<long>(input.size())) { return std::nullopt; }
				for (int channel = 0; channel < section.channels; ++channel)
				{
					if (!output[channel] || std::any_of(output[channel], output[channel] + count,
						[](float sample) { return !std::isfinite(sample); })) { return std::nullopt; }
				}
				for (long frame = 0; frame < count; ++frame)
				{
					const auto left = output[0][frame];
					input[frame] = {left, section.channels == 1 ? left : output[1][frame]};
				}
				decoded += count;
				if (!resampler) { if (!append(input.data(), static_cast<std::size_t>(count))) { return std::nullopt; } continue; }
				std::size_t consumed = 0;
				while (consumed < static_cast<std::size_t>(count))
				{
					if (!FileResourceIO::check(error)) { return std::nullopt; }
					const auto convertedBlock = resampler->process(
						{input[consumed].data(), 2, static_cast<std::size_t>(count) - consumed},
						{converted[0].data(), 2, converted.size()}, decoded == section.frames);
					if (static_cast<std::size_t>(convertedBlock.inputFramesUsed) > static_cast<std::size_t>(count) - consumed
						|| static_cast<std::size_t>(convertedBlock.outputFramesGenerated) > converted.size()
						|| (convertedBlock.inputFramesUsed == 0 && convertedBlock.outputFramesGenerated == 0)
						|| !append(converted.data(), static_cast<std::size_t>(convertedBlock.outputFramesGenerated))) { return std::nullopt; }
					consumed += static_cast<std::size_t>(convertedBlock.inputFramesUsed);
				}
			}
			if (resampler)
			{
				for (;;)
				{
					if (!FileResourceIO::check(error)) { return std::nullopt; }
					const auto tail = resampler->process({input[0].data(), 2, 0}, {converted[0].data(), 2, converted.size()}, true);
					if (tail.inputFramesUsed != 0
						|| static_cast<std::size_t>(tail.outputFramesGenerated) > converted.size()
						|| !append(converted.data(), static_cast<std::size_t>(tail.outputFramesGenerated))) { return std::nullopt; }
					if (tail.outputFramesGenerated == 0) { break; }
				}
			}
			// Native conversion can round the link duration by at most one frame.
			const auto generated = result.size() - begin;
			if (section.outputLimit - generated > 1) { return std::nullopt; }
		}
		catch (const std::runtime_error&) { return std::nullopt; }
	}
	if (position != frames || result.empty() || file.error() != QFileDevice::NoError || !FileResourceIO::check(error)) { return std::nullopt; }
	return SampleDecoder::Result{std::move(result), static_cast<int>(sampleRate)};
}
#endif // LMMS_HAVE_OGGVORBIS

auto SampleDecoder::supportedAudioTypes() -> const std::vector<AudioType>&
{
	static const auto s_audioTypes = [] {
		auto types = std::vector<AudioType>();

		// Add DrumSynth by default since that support comes from us
		types.push_back(AudioType{"DrumSynth", "ds"});
#ifdef LMMS_HAVE_OGGVORBIS
		types.push_back(AudioType{"OGG", "ogg"});
#endif

		auto sfFormatInfo = SF_FORMAT_INFO{};
		auto simpleTypeCount = 0;
		sf_command(nullptr, SFC_GET_SIMPLE_FORMAT_COUNT, &simpleTypeCount, sizeof(int));

		// TODO: Ideally, this code should be iterating over the major formats, but some important extensions such as
		// *.ogg are not included. This is planned for future versions of sndfile.
		for (int simple = 0; simple < simpleTypeCount; ++simple)
		{
			sfFormatInfo.format = simple;
			sf_command(nullptr, SFC_GET_SIMPLE_FORMAT, &sfFormatInfo, sizeof(sfFormatInfo));

			auto it = std::find_if(types.begin(), types.end(),
				[&](const AudioType& type) { return sfFormatInfo.extension == type.extension; });
			if (it != types.end()) { continue; }

			auto name = std::string{sfFormatInfo.extension};
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::toupper(ch); });

			types.push_back(AudioType{std::move(name), sfFormatInfo.extension});
		}

		std::sort(types.begin(), types.end(), [&](const AudioType& a, const AudioType& b) { return a.name < b.name; });
		return types;
	}();
	return s_audioTypes;
}

auto SampleDecoder::decode(const QString& audioFile) -> std::optional<Result>
{
	auto result = std::optional<Result>{};
	QString error;
	try
	{
#ifdef LMMS_HAVE_OGGVORBIS
		// libsndfile can accept only the first logical stream of a chained file.
		// Route recognized Vorbis through its complete native chain reader. Other
		// Ogg codecs can still use the ordinary libsndfile/backend fallback path.
		if (!FileResourceIO::check(error)) { return std::nullopt; }
		QFile source(audioFile);
		if (source.open(QIODevice::ReadOnly) && source.read(4) == "OggS")
		{
			source.close(); bool recognized = false;
			result = detail::decodeSampleOggVorbis(audioFile, &recognized);
			if (result || recognized) { return result; }
		}
#endif
		for (const auto& decoder : decoders)
		{
			if (!FileResourceIO::check(error)) { return std::nullopt; }
			result = decoder(audioFile);
			if (result) { break; }
		}
	}
	catch (const std::bad_alloc&) { return std::nullopt; }
	catch (const std::length_error&) { return std::nullopt; }

	return result;
}

} // namespace lmms
