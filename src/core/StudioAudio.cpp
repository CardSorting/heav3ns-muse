/* Native render and measured audio observations. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sndfile.h>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QTemporaryFile>
#include <QUuid>

#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "OutputSettings.h"
#include "PatternStore.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "Song.h"
#include "StudioRecording.h"
#include "Track.h"
#include "TrackRenderScope.h"
#include "lmmsconfig.h"

namespace lmms
{
namespace
{

int setting(const QJsonObject& a, const QString& key, int fallback, int min, int max)
{
	if (!a.contains(key)) { return fallback; }
	const auto value = a.value(key);
	const auto number = value.toDouble();
	if (!value.isDouble() || !std::isfinite(number) || number < min || number > max || std::floor(number) != number)
	{
		throw std::invalid_argument((key + " is outside its supported integer range").toStdString());
	}
	return static_cast<int>(number);
}

bool flag(const QJsonObject& a, const QString& key)
{
	if (!a.contains(key)) { return false; }
	if (!a.value(key).isBool()) { throw std::invalid_argument((key + " must be a boolean").toStdString()); }
	return a.value(key).toBool();
}

QString outputPath(const QJsonObject& a)
{
	if (!a.value("path").isString() || a.value("path").toString().isEmpty())
	{
		throw std::invalid_argument("path must be a nonempty string");
	}
	return QFileInfo(a.value("path").toString()).absoluteFilePath();
}

void checkCancellation(const StudioAudio::Cancelled& cancelled, bool processEvents = false)
{
	if (processEvents && cancelled) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); }
	if (cancelled && cancelled()) { throw std::runtime_error("Native audio operation cancelled"); }
}

QString expectedSourceDigest(const QJsonObject& arguments)
{
	if (!arguments.contains("sourceSha256")) { return {}; }
	const auto value = arguments.value("sourceSha256");
	static const QRegularExpression digest("^[0-9a-f]{64}$");
	if (!value.isString() || value.toString().size() != 64 || !digest.match(value.toString()).hasMatch())
	{ throw std::invalid_argument("sourceSha256 must be a lowercase SHA-256 digest"); }
	return value.toString();
}

std::shared_ptr<FileResourceSnapshot> acceptedSource(const QString& path, const QString& expected, qint64 maximumBytes = -1)
{
	QString error;
	auto snapshot = maximumBytes < 0 ? FileResourceSnapshot::capture(path, error) : FileResourceSnapshot::captureBounded(path, maximumBytes, error);
	if (!snapshot) { throw std::runtime_error(error.toStdString()); }
	if (!expected.isEmpty() && snapshot->sha256() != expected)
	{ throw std::runtime_error("Audio source differs from sourceSha256; render or rediscover the intended audio before evaluation"); }
	return snapshot;
}

// Keep returned control-thread continuations tied to the project and native
// reservation they admitted. This does not freeze the project's deep graph.
class StudioRenderOwnership
{
public:
	StudioRenderOwnership(AudioEngine* engine, Song* song, const QString& temporary)
		: m_engine(engine), m_song(song), m_temporary(temporary),
		m_generation(song->replacementGeneration()), m_filename(song->projectFileName()),
		m_reservation(engine->audioDeviceReservationGeneration()),
		m_loop(song->exportLoop()), m_markers(song->renderBetweenMarkers()), m_count(song->getLoopRenderCount()) { }
	StudioRenderOwnership(const StudioRenderOwnership&) = delete;
	StudioRenderOwnership& operator=(const StudioRenderOwnership&) = delete;
	~StudioRenderOwnership()
	{
		FileResourceIO::Suspension recovery;
		if (m_reserved && sameReservation()) { m_engine->restoreAudioDevice(); }
		// Restoration and renderer destruction can themselves run native slots.
		if (m_settingsApplied && current())
		{ m_song->setExportLoop(m_loop); m_song->setRenderBetweenMarkers(m_markers); m_song->setLoopRenderCount(m_count); }
		if (sameReservation()) { QFile::remove(m_temporary); }
	}
	bool sameReservation() const
	{ return m_engine && Engine::audioEngine() == m_engine.data() && m_engine->audioDeviceReservationGeneration() == m_reservation; }
	bool current() const
	{
		return sameReservation() && m_song && Engine::getSong() == m_song.data()
			&& m_song->replacementGeneration() == m_generation && m_song->projectFileName() == m_filename;
	}
	void check() const
	{ if (!current()) { throw std::runtime_error("Native render project or audio reservation was superseded"); } }
	void reserve()
	{
		check(); if (!m_engine->storeAudioDevice()) { throw std::runtime_error("Another native export is active"); }
		m_reservation = m_engine->audioDeviceReservationGeneration(); m_reserved = true; check();
	}
	void applySettings(bool loop, bool markers, int count)
	{ check(); m_settingsApplied = true; m_song->setExportLoop(loop); m_song->setRenderBetweenMarkers(markers); m_song->setLoopRenderCount(count); }
	bool restoreDevice()
	{
		check(); FileResourceIO::Suspension recovery;
		const bool restored = m_engine->restoreAudioDevice(); check(); return restored;
	}
private:
	QPointer<AudioEngine> m_engine;
	QPointer<Song> m_song;
	QString m_temporary;
	std::uint64_t m_generation;
	QString m_filename;
	std::uint64_t m_reservation;
	bool m_loop, m_markers;
	int m_count;
	bool m_reserved = false, m_settingsApplied = false;
};

} // namespace

QJsonObject StudioAudio::render(const QJsonObject& a, const Cancelled& cancelled)
{
	checkCancellation(cancelled);
	if (!Engine::audioEngine() || !Engine::getSong()) { throw std::runtime_error("Native rendering requires initialized engine and project roots"); }
	StudioRecording::requireNoCapture();
	if (RemotePlugin::hasActiveRenderContext() || Engine::audioEngine()->hasStoredAudioDevice() || Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup() || TrackRenderScope::hasActiveSelection())
	{
		throw std::runtime_error("Another native export is active");
	}
	const auto path = outputPath(a);
	const int rate = setting(a, "sampleRate", 44100, 44100, 192000);
	if (std::find(SUPPORTED_SAMPLERATES.begin(), SUPPORTED_SAMPLERATES.end(), rate) == SUPPORTED_SAMPLERATES.end())
	{
		throw std::invalid_argument("Unsupported LMMS sample rate");
	}
	const int bits = setting(a, "bitDepth", 16, 16, 32);
	const auto depth = bits == 16 ? OutputSettings::BitDepth::Depth16Bit
		: bits == 24 ? OutputSettings::BitDepth::Depth24Bit : OutputSettings::BitDepth::Depth32Bit;
	if (bits != 16 && bits != 24 && bits != 32) { throw std::invalid_argument("bitDepth must be 16, 24 or 32"); }
	const int timeout = setting(a, "timeoutMs", 300000, 1, 1800000);
	const bool exportLoop = flag(a, "exportLoop"), betweenMarkers = flag(a, "betweenMarkers");
	const int loopCount = setting(a, "loopCount", 1, 1, std::numeric_limits<int>::max());
	const auto extension = "." + QFileInfo(path).suffix().toLower();
	const auto format = ProjectRenderer::getFileFormatFromExtension(extension);
	const auto& encoder = ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(format));
	if (extension != encoder.m_extension || !encoder.isAvailable())
	{
		throw std::invalid_argument("Output extension has no available native LMMS encoder");
	}
	if (format == ProjectRenderer::ExportFileFormat::Flac && bits == 32)
	{
		throw std::invalid_argument("The native FLAC encoder supports 16 or 24 bits; choose explicitly");
	}
	const bool lossless = format == ProjectRenderer::ExportFileFormat::Wave || format == ProjectRenderer::ExportFileFormat::Flac;
	if (!lossless && a.contains("bitDepth"))
	{
		throw std::invalid_argument("bitDepth applies to WAV and FLAC; lossy encoders use native bitrate settings");
	}
	const int bitRate = setting(a, "bitRate", 160, 64, 384);
	if (a.contains("bitRate") && lossless) { throw std::invalid_argument("bitRate applies only to OGG and MP3"); }
	const auto stereoName = a.contains("stereoMode") ? a.value("stereoMode").toString() : QString("stereo");
	if (a.contains("stereoMode") && format != ProjectRenderer::ExportFileFormat::MP3)
	{ throw std::invalid_argument("stereoMode applies only to MP3"); }
	if (stereoName != "mono" && stereoName != "stereo" && stereoName != "jointStereo")
	{ throw std::invalid_argument("stereoMode must be mono, stereo or jointStereo"); }
	const auto stereo = stereoName == "mono" ? OutputSettings::StereoMode::Mono
		: stereoName == "jointStereo" ? OutputSettings::StereoMode::JointStereo : OutputSettings::StereoMode::Stereo;
	double compression = 0.625;
	if (a.contains("compressionLevel"))
	{
		if (format != ProjectRenderer::ExportFileFormat::Flac) { throw std::invalid_argument("compressionLevel applies only to FLAC"); }
		const auto value = a.value("compressionLevel"); compression = value.toDouble();
		if (!value.isDouble() || !std::isfinite(compression) || compression < 0 || compression > 1)
		{ throw std::invalid_argument("compressionLevel must be in 0..1"); }
#ifndef LMMS_HAVE_SF_COMPLEVEL
		throw std::invalid_argument("This native build cannot set FLAC compression levels");
#endif
	}
	OutputSettings settings(rate, bitRate, depth, stereo); settings.setCompressionLevel(compression);
	const auto temporary = path + ".render-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + extension;
	const QPointer<Song> song(Engine::getSong());
	StudioRenderOwnership ownership(Engine::audioEngine(), song.data(), temporary);
	song->updateLength(); ownership.check();
	QPointer<Track> selected; QString selectedContainer; int selectedIndex = -1;
	if (a.contains("container") && !a.contains("track")) { throw std::invalid_argument("container requires an individual export track"); }
	if (a.contains("track"))
	{
		selectedContainer = a.contains("container") ? a.value("container").toString() : QString("song");
		if (selectedContainer != "song" && selectedContainer != "pattern") { throw std::invalid_argument("Export container must be song or pattern"); }
		const auto owner = selectedContainer == "song" ? static_cast<TrackContainer*>(song.data()) : static_cast<TrackContainer*>(Engine::patternStore());
		selectedIndex = setting(a, "track", -1, 0, static_cast<int>(owner->tracks().size()) - 1);
		selected = owner->tracks().at(selectedIndex);
	}
	if (song->length() == 0) { throw std::invalid_argument("Project has no arrangement to render"); }
	const auto range = song->exportRange(exportLoop, betweenMarkers, loopCount);
	if (range.effectiveTicks < 1) { throw std::invalid_argument("Native export range is empty"); }
	if (Engine::audioEngine()->dspStatus().failure != AudioEngine::DspFailure::None)
	{
		throw std::runtime_error("Native DSP has a latched failure; stop playback, repair the project and explicitly reset audio processing before rendering");
	}
	QElapsedTimer elapsed;
	elapsed.start();
	FileResourceIO::Scope assetWork([&] {
		ownership.check(); const bool stop = elapsed.elapsed() >= timeout || (cancelled && cancelled()); ownership.check(); return stop;
	}, true);
	std::unique_ptr<TrackRenderScope> trackSelection;
	if (selectedIndex >= 0) { trackSelection = std::make_unique<TrackRenderScope>(selected.data()); }
	ownership.check();
	const QJsonValue stem = selected ? QJsonValue(QJsonObject{{"container", selectedContainer}, {"track", selectedIndex},
		{"type", selected->type() == Track::Type::Instrument ? "instrument" : "sample"}}) : QJsonValue(QJsonValue::Null);
	ownership.reserve();
	ProjectRenderer renderer(settings, format, temporary);
	if (!renderer.isReady()) { throw std::runtime_error("Native renderer could not open its output device"); }
	QEventLoop loop;
	QTimer timer;
	QTimer cancellation;
	timer.setSingleShot(true);
	bool timedOut = false;
	bool aborted = false;
	std::exception_ptr cancellationFailure;
	QObject::connect(&renderer, &QThread::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
		timedOut = true;
		renderer.abortProcessing();
		loop.quit();
	});
	QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
		try
		{
			if (!ownership.current() || (cancelled && cancelled()) || !ownership.current()) { aborted = true; renderer.abortProcessing(); loop.quit(); }
		}
		catch (...)
		{
			// Exceptions must not unwind Qt's signal/event dispatch. Retain the
			// original callback failure until native processing has been joined.
			cancellationFailure = std::current_exception(); cancellation.stop(); timer.stop();
			renderer.abortProcessing(); loop.quit();
		}
	});
	ownership.applySettings(exportLoop, betweenMarkers, loopCount);
	const auto remaining = static_cast<qint64>(timeout) - elapsed.elapsed();
	if (remaining <= 0) { throw std::runtime_error("Native render exceeded its cooperative timeout"); }
	timer.start(static_cast<int>(remaining));
	cancellation.start(25);
	renderer.setExecutionTimeout(static_cast<int>(remaining));
	const auto started = renderer.startProcessing();
	if (started && !renderer.isFinished()) { loop.exec(); }
	timer.stop();
	cancellation.stop();
	renderer.wait();
	if (cancellationFailure) { std::rethrow_exception(cancellationFailure); }
	ownership.check();
	if (timedOut || elapsed.elapsed() >= timeout) { throw std::runtime_error("Native render exceeded its cooperative timeout"); }
	if (aborted) { throw std::runtime_error("Native render cancelled"); }
	checkCancellation(cancelled);
	ownership.check();
	if (renderer.hasPluginFailure()) { throw std::runtime_error("Native remote plugin failed during rendering"); }
	if (renderer.hasDspFailure()) { throw std::runtime_error("Native audio engine DSP failed during rendering"); }
	if (renderer.hasExecutionFailure()) { throw std::runtime_error("Native export processing raised an exception"); }
	if (renderer.hasEncodingFailure()) { throw std::runtime_error("Native audio encoding or output writing failed during rendering"); }
	if (!started) { throw std::runtime_error("Native render did not start"); }
	if (!renderer.finalizeOutput())
	{
		if (elapsed.elapsed() >= timeout) { throw std::runtime_error("Native render exceeded its cooperative timeout"); }
		if (renderer.wasAborted()) { throw std::runtime_error("Native render cancelled"); }
		if (renderer.hasPluginFailure()) { throw std::runtime_error("Native remote plugin failed during rendering"); }
		if (renderer.hasDspFailure()) { throw std::runtime_error("Native audio engine DSP failed during rendering"); }
		if (renderer.hasExecutionFailure()) { throw std::runtime_error("Native export processing raised an exception"); }
		if (renderer.hasEncodingFailure()) { throw std::runtime_error("Native audio encoding or output writing failed during rendering"); }
		throw std::runtime_error("Native audio encoding or output finalization failed");
	}
	ownership.check();
	// Device setup can refresh native clip lengths. Report the range actually
	// accepted by Song::startExport, not the earlier admission calculation.
	const auto renderedRange = song->lastExportRange();
	// Closing the file device finalizes codec headers before publication.
	if (!ownership.restoreDevice())
	{ throw std::runtime_error("Native audio device restoration failed; the existing export was preserved and the reservation remains available for retry"); }
	QFile file(temporary);
	if (renderer.hasDspFailure()) { throw std::runtime_error("Native DSP failed during restored export publication"); }
	QSaveFile destination(path);
	destination.setDirectWriteFallback(false);
	if (!file.open(QIODevice::ReadOnly) || file.size() == 0 || !destination.open(QIODevice::WriteOnly))
	{
		throw std::runtime_error("Rendered audio could not be published");
	}
	const auto publishedBytes = file.size(); QCryptographicHash hash(QCryptographicHash::Sha256); QString publicationError;
	if (!FileResourceIO::stream(file, &destination, &hash, publishedBytes, publicationError))
	{
		if (elapsed.elapsed() >= timeout) { throw std::runtime_error("Native render exceeded its cooperative timeout"); }
		checkCancellation(cancelled);
		ownership.check();
		throw std::runtime_error(("Rendered audio publication failed: " + publicationError).toStdString());
	}
	checkCancellation(cancelled);
	ownership.check();
	if (elapsed.elapsed() >= timeout) { throw std::runtime_error("Native render exceeded its cooperative timeout"); }
	if (renderer.hasDspFailure()) { throw std::runtime_error("Native DSP failed during restored export publication"); }
	if (!destination.commit()) { throw std::runtime_error("Rendered audio atomic publication failed"); }
	QJsonValue requestedCompression = QJsonValue::Null;
#ifdef LMMS_HAVE_SF_COMPLEVEL
	if (format == ProjectRenderer::ExportFileFormat::Flac) { requestedCompression = compression; }
#endif
	return {{"path", path}, {"stem", stem}, {"requestedSampleRate", rate},
		{"requestedBitDepth", lossless ? QJsonValue(bits) : QJsonValue(QJsonValue::Null)},
		{"requestedBitRate", lossless ? QJsonValue(QJsonValue::Null) : QJsonValue(bitRate)},
		{"requestedStereoMode", format == ProjectRenderer::ExportFileFormat::MP3 ? QJsonValue(stereoName) : QJsonValue(QJsonValue::Null)},
		{"requestedCompressionLevel", requestedCompression},
		{"bitRateSemantics", format == ProjectRenderer::ExportFileFormat::Ogg ? QJsonValue("native Vorbis VBR quality = bitRate / 320; not a target kbit/s")
			: format == ProjectRenderer::ExportFileFormat::MP3 ? QJsonValue("native LAME CBR kbit/s setting; output rate may be resampled by the encoder") : QJsonValue(QJsonValue::Null)},
		{"renderRange", QJsonObject{{"exportLoop", exportLoop}, {"betweenMarkers", betweenMarkers}, {"loopCount", loopCount},
			{"begin", renderedRange.begin.getTicks()}, {"end", renderedRange.end.getTicks()}, {"loopBegin", renderedRange.loopBegin.getTicks()}, {"loopEnd", renderedRange.loopEnd.getTicks()},
			{"effectiveTicks", QString::number(renderedRange.effectiveTicks)}}},
		{"codec", extension.mid(1)}, {"bytes", static_cast<double>(publishedBytes)}, {"sha256", QString::fromLatin1(hash.result().toHex())},
		{"wallTimeMs", static_cast<double>(elapsed.elapsed())}, {"evaluation", "rendered; perceptual listening not performed"}};
}

QJsonObject StudioAudio::analyze(const QJsonObject& a, const Cancelled& cancelled)
{
	checkCancellation(cancelled);
	const auto path = outputPath(a), expected = expectedSourceDigest(a);
	const int maxSeconds = setting(a, "maximumSeconds", 7200, 1, 86400);
	const auto openAudio = [maxSeconds](QFile& file, SF_INFO& info)
	{
		if (!file.open(QIODevice::ReadOnly)) { throw std::invalid_argument("Audio file is unreadable"); }
		std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
		if (!audio || info.channels < 1 || info.channels > 256 || info.samplerate < 1 || info.frames < 1)
		{ throw std::invalid_argument("Audio file cannot be decoded by the native audio library"); }
		if (static_cast<double>(info.frames) / info.samplerate > maxSeconds)
		{ throw std::invalid_argument("Audio exceeds the requested analysis duration budget"); }
		return audio;
	};
	// Keep cheap admission ahead of the full source copy. Only the accepted
	// header below determines measurement scope; the original can change here.
	{
		QFile probe(path); SF_INFO info{};
		const auto audio = openAudio(probe, info);
	}
	checkCancellation(cancelled);
	FileResourceIO::Scope sourceWork(cancelled, true);
	const auto accepted = acceptedSource(path, expected);
	QFile file(accepted->path());
	SF_INFO info{};
	auto audio = openAudio(file, info);
	std::vector<float> buffer(4096 * info.channels);
	std::vector<double> peaks(info.channels, 0), squares(info.channels, 0);
	sf_count_t frames = 0;
	sf_count_t clipped = 0;
	sf_count_t nonfinite = 0;
	while (const auto count = sf_readf_float(audio.get(), buffer.data(), 4096))
	{
		checkCancellation(cancelled, true);
		if (count < 0) { throw std::runtime_error("Native audio decoding failed"); }
		frames += count;
		for (sf_count_t i = 0; i < count * info.channels; ++i)
		{
			const auto sample = buffer[static_cast<std::size_t>(i)];
			if (!std::isfinite(sample)) { ++nonfinite; continue; }
			const auto ch = static_cast<std::size_t>(i % info.channels);
			peaks[ch] = std::max(peaks[ch], static_cast<double>(std::abs(sample)));
			squares[ch] += static_cast<double>(sample) * sample;
			if (std::abs(sample) >= 1.0f) { ++clipped; }
		}
	}
	if (sf_error(audio.get()) || frames != info.frames) { throw std::runtime_error("Audio decode ended before the declared frame count"); }
	checkCancellation(cancelled);
	QJsonArray channels;
	for (int i = 0; i < info.channels; ++i)
	{
		channels.append(QJsonObject{{"index", i}, {"peak", peaks[i]}, {"rms", std::sqrt(squares[i] / frames)}});
	}
	return {{"kind", "technical measurement"}, {"perceptualListening", false}, {"path", path},
		{"sourceSha256", accepted->sha256()}, {"sourceBytes", static_cast<double>(accepted->bytes())},
		{"sampleRate", info.samplerate}, {"frames", static_cast<double>(frames)},
		{"durationSeconds", static_cast<double>(frames) / info.samplerate}, {"channels", channels},
		{"samplesAtOrAboveFullScale", static_cast<double>(clipped)}, {"nonfiniteSamples", static_cast<double>(nonfinite)}};
}

StudioAudio::Excerpt StudioAudio::excerpt(const QJsonObject& a, const Cancelled& cancelled)
{
	checkCancellation(cancelled);
	const auto path = outputPath(a);
	const auto expected = expectedSourceDigest(a);
	const auto seconds = [&](const QString& key, double fallback, double minimum, double maximum)
	{
		if (!a.contains(key)) { return fallback; }
		const auto value = a.value(key);
		const auto n = value.toDouble();
		if (!value.isDouble() || !std::isfinite(n) || n < minimum || n > maximum)
		{
			throw std::invalid_argument((key + " is outside the supported seconds range").toStdString());
		}
		return n;
	};
	const double offset = seconds("offsetSeconds", 0, 0, 86400);
	const double duration = seconds("durationSeconds", 30, 0.01, 120);
	FileResourceIO::Scope sourceWork(cancelled, true);
	const auto accepted = acceptedSource(path, expected, 1024LL * 1024 * 1024);
	QFile input(accepted->path());
	if (!input.open(QIODevice::ReadOnly))
	{
		throw std::invalid_argument("Audio source is unreadable or exceeds the 1 GiB listening-source budget");
	}
	SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> source(sf_open_fd(input.handle(), SFM_READ, &info, false), &sf_close);
	if (!source || info.channels < 1 || info.channels > 2 || info.samplerate < 8000 || info.samplerate > 192000 || info.frames < 1)
	{
		throw std::invalid_argument("Listening requires decodable mono/stereo audio at 8..192 kHz");
	}
	const auto first = static_cast<sf_count_t>(offset * info.samplerate);
	const auto count = std::min(static_cast<sf_count_t>(duration * info.samplerate), info.frames - first);
	if (first >= info.frames || count < 1 || count * info.channels * 2 + 4096 > 20 * 1024 * 1024
		|| sf_seek(source.get(), first, SEEK_SET) != first)
	{
		throw std::invalid_argument("Listening excerpt is outside the source or exceeds the 20 MiB audio budget");
	}
	QTemporaryFile temporary;
	if (!temporary.open()) { throw std::runtime_error("Could not create a native listening excerpt"); }
	SF_INFO encoding{};
	encoding.channels = info.channels;
	encoding.samplerate = info.samplerate;
	encoding.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	std::unique_ptr<SNDFILE, decltype(&sf_close)> output(sf_open_fd(temporary.handle(), SFM_WRITE, &encoding, false), &sf_close);
	if (!output) { throw std::runtime_error("Could not initialize the native excerpt encoder"); }
	sf_command(output.get(), SFC_SET_CLIPPING, nullptr, SF_TRUE);
	std::vector<float> samples(4096 * info.channels);
	sf_count_t remaining = count;
	sf_count_t transcodingClippingSamples = 0;
	while (remaining > 0)
	{
		checkCancellation(cancelled, true);
		const auto frames = std::min(remaining, sf_count_t(4096));
		if (sf_readf_float(source.get(), samples.data(), frames) != frames)
		{
			throw std::runtime_error("Audio source ended before the requested listening excerpt");
		}
		for (sf_count_t i = 0; i < frames * info.channels; ++i)
		{
			const auto sample = samples[static_cast<std::size_t>(i)];
			if (!std::isfinite(sample)) { throw std::runtime_error("Listening excerpt contains nonfinite samples"); }
			if (std::abs(sample) > 1.0f) { ++transcodingClippingSamples; }
		}
		if (sf_writef_float(output.get(), samples.data(), frames) != frames) { throw std::runtime_error("Native excerpt encoding failed"); }
		remaining -= frames;
	}
	if (sf_error(source.get())) { throw std::runtime_error("Native excerpt decoding failed"); }
	if (sf_close(output.release()) != 0 || !temporary.flush())
	{
		throw std::runtime_error("Native excerpt finalization failed");
	}
	QFile encoded(temporary.fileName());
	if (!encoded.open(QIODevice::ReadOnly)) { throw std::runtime_error("Could not read the finalized listening excerpt"); }
	const auto wave = encoded.readAll();
	if (wave.isEmpty() || wave.size() > 20 * 1024 * 1024) { throw std::runtime_error("Native excerpt is empty or exceeds its byte budget"); }
	checkCancellation(cancelled);
	return {wave, {{"path", path}, {"sourceSha256", accepted->sha256()}, {"sourceBytes", static_cast<double>(accepted->bytes())},
		{"excerptSha256", QString::fromLatin1(QCryptographicHash::hash(wave, QCryptographicHash::Sha256).toHex())},
		{"offsetSeconds", static_cast<double>(first) / info.samplerate}, {"durationSeconds", static_cast<double>(count) / info.samplerate},
		{"sampleRate", info.samplerate}, {"channels", info.channels},
		{"transcodingClippingSamples", static_cast<double>(transcodingClippingSamples)}, {"encoding", "16-bit PCM WAV listening excerpt"}}};
}

} // namespace lmms
