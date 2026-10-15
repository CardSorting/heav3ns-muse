/* Native export ranges, codec controls and state restoration. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioFileDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "ProjectRenderer.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Timeline.h"
#include "Track.h"
#include "Clip.h"
#include "lmmsconfig.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& data) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size(); }
QJsonObject manifest(const QString& path) { return QJsonDocument::fromJson(read(QDir(path).filePath("session.json"))).object(); }
QString hash(const QByteArray& data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
bool native(const QString& operation, const QJsonObject& args) { return StudioCapabilities::execute(operation, args).value("ok").toBool(); }
bool wave(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
	SF_INFO info{}; info.samplerate = 48000; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
	auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!audio) { return false; }
	// Four bars at 600 BPM, 4/4, each with a distinct constant amplitude.
	constexpr int framesPerBar = 19200; std::vector<float> samples(4 * framesPerBar * 2);
	for (int frame = 0; frame < 4 * framesPerBar; ++frame)
	{ samples[frame * 2] = samples[frame * 2 + 1] = 0.125f * (frame / framesPerBar + 1); }
	const auto count = sf_writef_float(audio, samples.data(), 4 * framesPerBar); const auto closed = sf_close(audio);
	return count == 4 * framesPerBar && closed == 0 && file.flush();
}
bool arrange(const QString& source)
{
	return native("song.set", {{"tempo", 600}, {"masterVolume", 100}})
		&& native("track.add", {{"type", "sample"}}) && native("clip.add", {{"track", 0}, {"position", 0}})
		&& native("sample.load", {{"track", 0}, {"clip", 0}, {"path", source}})
		&& native("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 4 * 192}, {"autoResize", false}});
}
struct Decoded { SF_INFO info{}; std::vector<float> samples; };
Decoded decode(const QString& path)
{
	Decoded result; QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return result; }
	std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &result.info, false), &sf_close);
	if (!audio || result.info.frames < 1 || result.info.frames > 2000000 || result.info.channels < 1 || result.info.channels > 2) { return {}; }
	result.samples.resize(static_cast<std::size_t>(result.info.frames) * result.info.channels);
	if (sf_readf_float(audio.get(), result.samples.data(), result.info.frames) != result.info.frames || sf_error(audio.get())) { return {}; }
	return result;
}
double level(const Decoded& audio, int frame)
{
	double sum = 0;
	for (int index = frame; index < frame + 1000; ++index)
	{
		if (index < 0 || index >= audio.info.frames) { return -1; }
		sum += std::abs(audio.samples[static_cast<std::size_t>(index) * audio.info.channels]);
	}
	return sum / 1000;
}
void defaults()
{
	const auto song = Engine::getSong(); song->setExportLoop(false); song->setRenderBetweenMarkers(false); song->setLoopRenderCount(1);
}
}

class StudioExportSettingsTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); defaults(); }
	void cleanup() { Engine::getSong()->stop(); defaults(); Engine::getSong()->clearProject(); }

	void nativeRangePreservesMeterTailAndRepeatSemantics_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("mode");
		for (int numerator : {3, 4}) for (int mode = 0; mode < 6; ++mode)
		{ QTest::newRow(qPrintable(QString::number(numerator) + "-" + QString::number(mode))) << numerator << mode; }
	}
	void nativeRangePreservesMeterTailAndRepeatSemantics()
	{
		QFETCH(int, numerator); QFETCH(int, mode); const auto song = Engine::getSong(); song->getTimeSigModel().setNumerator(numerator); song->getTimeSigModel().setDenominator(4);
		const int bar = TimePos::ticksPerBar(); QVERIFY(native("track.add", {{"type", "automation"}})); QVERIFY(native("clip.add", {{"track", 0}, {"position", 0}}));
		QVERIFY(native("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 4 * bar}, {"autoResize", false}})); song->updateLength(); QCOMPARE(song->length(), 4);
		const bool between = mode == 2 || mode == 3; const bool loop = mode == 1 || mode == 3;
		const int count = mode >= 2 ? 3 : 1; const int begin = mode == 5 ? 6 * bar : bar, end = mode == 5 ? 7 * bar : 2 * bar;
		auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setLoopPoints(begin, end); timeline.setLoopEnabled(mode % 2);
		const auto range = song->exportRange(loop, between, count);
		QCOMPARE(range.begin.getTicks(), between ? begin : 0); QCOMPARE(range.end.getTicks(), between ? end : (mode == 5 ? 8 : loop ? 4 : 5) * bar);
		QCOMPARE(range.loopBegin.getTicks(), begin); QCOMPARE(range.loopEnd.getTicks(), end);
		QCOMPARE(range.effectiveTicks, qint64(between ? 3 : mode == 5 ? 10 : mode == 4 ? 7 : loop ? 4 : 5) * bar);
		// Large legal repetition counts require wide effective-length arithmetic.
		const auto huge = song->exportRange(true, true, std::numeric_limits<int>::max());
		QCOMPARE(huge.effectiveTicks, qint64(bar) * std::numeric_limits<int>::max());
		QVERIFY_EXCEPTION_THROWN(song->exportRange(false, false, 0), std::invalid_argument);
		timeline.setLoopPoints(-bar, bar); QVERIFY_EXCEPTION_THROWN(song->exportRange(false, true, 1), std::invalid_argument);
		timeline.setLoopPoints(bar, bar); QVERIFY_EXCEPTION_THROWN(song->exportRange(false, false, 2), std::invalid_argument);
	}

	void renderedPcmFollowsNativeRangeAndRepeatSelection_data()
	{ QTest::addColumn<int>("mode"); for (int mode = 0; mode < 4; ++mode) { QTest::newRow(qPrintable(QString::number(mode))) << mode; } }
	void renderedPcmFollowsNativeRangeAndRepeatSelection()
	{
		QFETCH(int, mode); QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav");
		QVERIFY(wave(source)); QVERIFY(arrange(source)); const auto song = Engine::getSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song);
		timeline.setLoopPoints(192, 384); timeline.setLoopEnabled(true); song->setExportLoop(true); song->setRenderBetweenMarkers(true); song->setLoopRenderCount(17);
		QJsonObject args{{"path", path}, {"sampleRate", 48000}, {"bitDepth", 24}, {"timeoutMs", 30000}};
		if (mode == 1) { args.insert("exportLoop", true); }
		if (mode == 2) { args.insert("betweenMarkers", true); args.insert("loopCount", 2); }
		if (mode == 3) { args.insert("loopCount", 2); }
		const auto response = StudioCapabilities::execute("audio.render", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		const auto result = response.value("result").toObject(), range = result.value("renderRange").toObject();
		const int bars = mode == 0 ? 5 : mode == 1 ? 4 : mode == 2 ? 2 : 6;
		QCOMPARE(range.value("effectiveTicks").toString(), QString::number(bars * 192)); QCOMPARE(range.value("begin").toInt(), mode == 2 ? 192 : 0);
		QCOMPARE(range.value("loopCount").toInt(), mode >= 2 ? 2 : 1); QCOMPARE(result.value("sha256").toString(), hash(read(path)));
		const auto pcm = decode(path); QVERIFY(!pcm.samples.empty()); QCOMPARE(pcm.info.samplerate, 48000); QCOMPARE(pcm.info.channels, 2);
		QVERIFY(std::abs(pcm.info.frames - sf_count_t(bars * 19200)) <= 2 * Engine::audioEngine()->framesPerPeriod());
		const std::vector<int> order = mode == 0 ? std::vector<int>{1, 2, 3, 4, 0} : mode == 1 ? std::vector<int>{1, 2, 3, 4}
			: mode == 2 ? std::vector<int>{2, 2} : std::vector<int>{1, 2, 2, 3, 4, 0};
		for (int bar = 0; bar < static_cast<int>(order.size()); ++bar)
		{ QVERIFY2(std::abs(level(pcm, bar * 19200 + 8000) - order[bar] * 0.125) < 0.01, qPrintable(QString("Unexpected PCM in output bar %1").arg(bar))); }
		QVERIFY(song->exportLoop() && song->renderBetweenMarkers()); QCOMPARE(song->getLoopRenderCount(), 17);
		QCOMPARE(timeline.loopBegin().getTicks(), 192); QCOMPARE(timeline.loopEnd().getTicks(), 384); QVERIFY(timeline.loopEnabled());
		QVERIFY(!song->isExporting()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
	}

	void failuresPreserveOutputAndRestoreExportSettings_data()
	{
		QTest::addColumn<QString>("failure"); for (const auto failure : {"cancel", "timeout", "publication", "empty-range", "codec", "setup", "empty-after-muting"})
		{ QTest::newRow(failure) << QString(failure); }
	}
	void failuresPreserveOutputAndRestoreExportSettings()
	{
		QFETCH(QString, failure); QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav");
		QVERIFY(wave(source)); QVERIFY(arrange(source)); const QByteArray original("Keep prior export");
		if (failure == "publication") { QVERIFY(QDir().mkpath(path)); } else { QVERIFY(write(path, original)); }
		const auto song = Engine::getSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setLoopPoints(192, failure == "empty-range" ? 192 : 384);
		song->setExportLoop(true); song->setRenderBetweenMarkers(true); song->setLoopRenderCount(17);
		bool cancelled = false, installed = false;
		AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; }
			if (!dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { return; }
			installed = true;
			if (failure == "cancel") { cancelled = true; }
			if (failure == "timeout") { QThread::msleep(15); }
			if (failure == "setup") { throw std::runtime_error("Fixture rejects native export setup"); }
		});
		QJsonObject args{{"path", path}, {"betweenMarkers", true}, {"loopCount", 2}, {"timeoutMs", failure == "timeout" ? 1 : 30000}};
		if (failure == "codec") { args.insert("bitRate", 160); }
		if (failure == "empty-after-muting")
		{ QVERIFY(native("track.edit", {{"track", 0}, {"muted", true}})); args.insert("betweenMarkers", false); args.insert("exportLoop", true); args.insert("loopCount", 1); }
		const auto response = StudioCapabilities::execute("audio.render", args, [&] { return cancelled; });
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(song->exportLoop() && song->renderBetweenMarkers()); QCOMPARE(song->getLoopRenderCount(), 17);
		if (failure != "publication") { QCOMPARE(read(path), original); } else { QVERIFY(QFileInfo(path).isDir()); }
		QVERIFY(!song->isExporting()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		if (failure == "cancel" || failure == "setup") { QVERIFY(installed); }
		QVERIFY(QDir(directory.path()).entryList({"*.render-*"}, QDir::Files).isEmpty());
	}

	void codecControlsReachNativeEncoders_data()
	{
		QTest::addColumn<QString>("codec"); QTest::addColumn<int>("bitRate"); QTest::addColumn<QString>("stereo"); QTest::addColumn<double>("compression");
		for (double compression : {0.0, 1.0}) { QTest::newRow(qPrintable("flac-" + QString::number(compression))) << QString("flac") << 160 << QString("stereo") << compression; }
		for (int bitRate : {64, 96, 320})
		{
			QTest::newRow(qPrintable("ogg-" + QString::number(bitRate))) << QString("ogg") << bitRate << QString("stereo") << 0.625;
			for (const auto stereo : {"mono", "stereo", "jointStereo"})
			{ QTest::newRow(qPrintable("mp3-" + QString(stereo) + QString::number(bitRate))) << QString("mp3") << bitRate << QString(stereo) << 0.625; }
		}
	}
	void codecControlsReachNativeEncoders()
	{
		QFETCH(QString, codec); QFETCH(int, bitRate); QFETCH(QString, stereo); QFETCH(double, compression);
		const auto format = ProjectRenderer::getFileFormatFromExtension("." + codec);
		if (!ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(format)).isAvailable()) { QSKIP("Native encoder not compiled"); }
#ifndef LMMS_HAVE_SF_COMPLEVEL
		if (codec == "flac") { QSKIP("Native FLAC compression control not compiled; refusal is checked separately"); }
#endif
		QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("encoded." + codec);
		QVERIFY(wave(source)); QVERIFY(arrange(source)); bool observed = false;
		const auto mode = stereo == "mono" ? OutputSettings::StereoMode::Mono : stereo == "jointStereo" ? OutputSettings::StereoMode::JointStereo : OutputSettings::StereoMode::Stereo;
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (const auto device = dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev()))
			{
				const auto& settings = device->getOutputSettings(); observed = settings.getSampleRate() == 48000 && settings.bitrate() == bitRate
					&& settings.getStereoMode() == mode && settings.getCompressionLevel() == compression;
			}
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QJsonObject args{{"path", path}, {"sampleRate", 48000}, {"exportLoop", true}, {"timeoutMs", 30000}};
		if (codec == "flac") { args.insert("compressionLevel", compression); args.insert("bitDepth", 24); }
		else { args.insert("bitRate", bitRate); if (codec == "mp3") { args.insert("stereoMode", stereo); } }
		const auto response = StudioCapabilities::execute("audio.render", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(observed);
		const auto result = response.value("result").toObject(); QVERIFY(read(path).size() > 100); QCOMPARE(result.value("sha256").toString(), hash(read(path)));
		if (codec == "flac") { QCOMPARE(result.value("requestedCompressionLevel").toDouble(), compression); QVERIFY(result.value("requestedBitRate").isNull()); }
		else { QCOMPARE(result.value("requestedBitRate").toInt(), bitRate); QVERIFY(result.value("requestedBitDepth").isNull()); }
		if (codec == "mp3") { QCOMPARE(result.value("requestedStereoMode").toString(), stereo); }
		if (codec == "ogg") { QVERIFY(result.value("bitRateSemantics").toString().contains("quality = bitRate / 320")); }
		const auto pcm = decode(path);
		if (codec == "mp3" && pcm.samples.empty()) { QSKIP("Native MP3 encoded and settings checked; this libsndfile build cannot independently decode it"); }
		QVERIFY(!pcm.samples.empty()); QCOMPARE(pcm.info.channels, codec == "mp3" && stereo == "mono" ? 1 : 2);
		QVERIFY(level(pcm, pcm.info.samplerate / 5) > 0.01);
		if (codec == "flac") { QCOMPARE(pcm.info.samplerate, 48000); QCOMPARE(pcm.info.format & SF_FORMAT_SUBMASK, SF_FORMAT_PCM_24); }
	}

	void invalidSettingsFailBeforeChangingNativeState()
	{
		QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav"); QVERIFY(wave(source)); QVERIFY(arrange(source));
		const auto song = Engine::getSong(); song->setExportLoop(true); song->setRenderBetweenMarkers(true); song->setLoopRenderCount(17);
		const QJsonArray cases{QJsonObject{{"exportLoop", 1}}, QJsonObject{{"betweenMarkers", "true"}}, QJsonObject{{"loopCount", 0}}, QJsonObject{{"loopCount", 1.5}},
			QJsonObject{{"loopCount", 2147483648.0}}, QJsonObject{{"sampleRate", 32000}}, QJsonObject{{"bitDepth", 20}}, QJsonObject{{"bitRate", 160}},
			QJsonObject{{"stereoMode", "stereo"}}, QJsonObject{{"compressionLevel", 0.5}}};
		for (int index = 0; index < cases.size(); ++index)
		{
			auto args = cases.at(index).toObject(); args.insert("path", path); const auto response = StudioCapabilities::execute("audio.render", args);
			QVERIFY2(!response.value("ok").toBool(), qPrintable(QString::number(index))); QVERIFY(!QFile::exists(path)); QVERIFY(song->exportLoop() && song->renderBetweenMarkers()); QCOMPARE(song->getLoopRenderCount(), 17);
			QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		}
		for (const auto value : QJsonArray{-1, 1.1, "0.5", QJsonValue::Null})
		{ QVERIFY(!StudioCapabilities::execute("audio.render", {{"path", directory.filePath("invalid.flac")}, {"compressionLevel", value}}).value("ok").toBool()); }
#ifndef LMMS_HAVE_SF_COMPLEVEL
		const auto refused = StudioCapabilities::execute("audio.render", {{"path", directory.filePath("unsupported.flac")}, {"compressionLevel", 0.5}});
		QVERIFY(!refused.value("ok").toBool()); QVERIFY(refused.value("error").toString().contains("cannot set FLAC compression"));
#endif
		for (const auto& value : StudioAgent::schemas())
		{
			const auto spec = value.toObject(); if (spec.value("name").toString() != "audio.render") { continue; }
			const auto properties = spec.value("inputSchema").toObject().value("properties").toObject();
			QCOMPARE(properties.value("loopCount").toObject().value("default").toInt(), 1); QCOMPARE(properties.value("compressionLevel").toObject().value("type").toString(), QString("number"));
			QCOMPARE(properties.value("stereoMode").toObject().value("enum").toArray(), QJsonArray({"mono", "stereo", "jointStereo"}));
		}
	}

	void renderReportsTheRangeAcceptedAfterNativeDeviceSetup()
	{
		QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav"); QVERIFY(wave(source)); QVERIFY(arrange(source));
		bool refreshed = false; const auto song = Engine::getSong();
		QVERIFY(native("track.add", {{"type", "sample"}})); QVERIFY(native("clip.add", {{"track", 1}, {"position", 0}}));
		QVERIFY(native("clip.edit", {{"track", 1}, {"clip", 0}, {"length", 20 * 192}, {"autoResize", false}})); QVERIFY(native("track.edit", {{"track", 1}, {"muted", true}}));
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (!refreshed && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev()))
			{ song->tracks().front()->getClip(0)->changeLength(TimePos(5 * 192)); refreshed = true; }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("audio.render", {{"path", path}, {"sampleRate", 48000}, {"timeoutMs", 30000}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(refreshed);
		const auto range = response.value("result").toObject().value("renderRange").toObject(); QCOMPARE(range.value("end").toInt(), 6 * 192); QCOMPARE(range.value("effectiveTicks").toString(), QString::number(6 * 192));
		const auto pcm = decode(path); QVERIFY(!pcm.samples.empty()); QVERIFY(std::abs(pcm.info.frames - sf_count_t(6 * 19200)) <= 2 * Engine::audioEngine()->framesPerPeriod());
	}

	void exportRepetitionsDoNotLeakIntoOrdinaryPlayback()
	{
		QTemporaryDir directory; const auto source = directory.filePath("source.wav"); QVERIFY(wave(source)); QVERIFY(arrange(source));
		const auto song = Engine::getSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setLoopPoints(0, 48); timeline.setLoopEnabled(false); song->setLoopRenderCount(17);
		song->playSong(); timeline.setTicks(47);
		for (int period = 0; period < 100 && timeline.ticks() < 72; ++period) { Engine::audioEngine()->renderNextPeriod(); }
		QVERIFY(timeline.ticks() >= 72); QVERIFY(!song->isExporting()); song->stop();
		timeline.setLoopEnabled(true); song->playSong(); timeline.setTicks(47);
		for (int period = 0; period < 100; ++period) { Engine::audioEngine()->renderNextPeriod(); }
		QVERIFY(timeline.ticks() >= 0 && timeline.ticks() < 48); song->stop();
	}

	void playbackJumpIsDeliveredOnTheNativeCallerThread()
	{
		const auto song = Engine::getSong(); int deliveries = 0; std::thread::id caller, delivered;
		const auto connection = connect(song, &Song::playbackPositionJumped, this, [&] { ++deliveries; delivered = std::this_thread::get_id(); }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); song->playSong(); deliveries = 0;
		std::thread processing([&] { caller = std::this_thread::get_id(); song->getTimeline(Song::PlayMode::Song).setTicks(48); }); processing.join();
		// No application event pumping: native consumers must see the jump now.
		QCOMPARE(deliveries, 1); QVERIFY(caller == delivered); QVERIFY(delivered != std::this_thread::get_id()); song->stop();
	}

	void repeatedRangeProgressKeepsCompletedPassesAndWideLengths()
	{
		QTemporaryDir directory; const auto source = directory.filePath("source.wav"); QVERIFY(wave(source)); QVERIFY(arrange(source));
		const auto song = Engine::getSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setLoopPoints(48, 96);
		song->setRenderBetweenMarkers(true); song->setLoopRenderCount(3); song->startExport();
		const auto stop = qScopeGuard([&] { song->stopExport(); });
		bool wrapped = false; int previous = timeline.ticks();
		for (int period = 0; period < 100 && !wrapped; ++period)
		{ Engine::audioEngine()->renderNextPeriod(); wrapped = timeline.ticks() < previous; previous = timeline.ticks(); }
		QVERIFY(wrapped); timeline.setTicks(48); QCOMPARE(song->getExportProgress(), 33); song->stopExport();
		timeline.setLoopPoints(0, 1000000000); song->setLoopRenderCount(std::numeric_limits<int>::max()); song->startExport();
		QCOMPARE(song->lastExportRange().effectiveTicks, qint64(1000000000) * std::numeric_limits<int>::max());
		timeline.setTicks(500000000); QCOMPARE(song->getExportProgress(), 0); song->stopExport();
		timeline.setLoopPoints(0, std::numeric_limits<tick_t>::max()); song->setRenderBetweenMarkers(false); song->setExportLoop(false); song->setLoopRenderCount(2);
		QVERIFY_EXCEPTION_THROWN(song->startExport(), std::invalid_argument); QVERIFY(!song->isExporting());
	}

	void runtimePreservesTimelineHistoryAndHistoricalRenderReceipts_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-process") << false; QTest::newRow("reopen") << true; }
	void runtimePreservesTimelineHistoryAndHistoricalRenderReceipts()
	{
		QFETCH(bool, restart); QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav"), session = directory.filePath("session");
		QVERIFY(wave(source)); auto studio = std::make_unique<StudioRuntime>(session); QString error; QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("tempo", "song.set", {{"tempo", 600}})).value("ok").toBool());
		QVERIFY(studio->process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
		QVERIFY(studio->process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		QVERIFY(studio->process(request("sample", "sample.load", {{"track", 0}, {"clip", 0}, {"path", source}})).value("ok").toBool());
		QVERIFY(studio->process(request("markers", "timeline.set", {{"begin", 192}, {"end", 384}, {"enabled", false}})).value("ok").toBool());
		const auto before = manifest(session); const auto renderRequest = request("render", "audio.render", {{"path", path}, {"betweenMarkers", true}, {"loopCount", 3}, {"timeoutMs", 30000}});
		const auto rendered = studio->process(renderRequest); QVERIFY2(rendered.value("ok").toBool(), qPrintable(rendered.value("error").toString()));
		QCOMPARE(rendered.value("result").toObject().value("renderRange").toObject().value("effectiveTicks").toString(), QString("576"));
		QCOMPARE(manifest(session).value("history"), before.value("history")); QCOMPARE(manifest(session).value("revision"), before.value("revision"));
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); }
		QVERIFY(QFile::remove(path)); auto replay = studio->process(renderRequest); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rendered); QVERIFY(!QFile::exists(path));
		QVERIFY(studio->process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTimeline(Song::PlayMode::Song).loopBegin().getTicks(), 0);
		QVERIFY(studio->process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTimeline(Song::PlayMode::Song).loopBegin().getTicks(), 192);
		auto fresh = renderRequest; fresh.insert("id", "fresh-render"); QVERIFY(studio->process(fresh).value("ok").toBool()); QVERIFY(!read(path).isEmpty());
	}
};

QTEST_GUILESS_MAIN(StudioExportSettingsTest)
#include "StudioExportSettingsTest.moc"
