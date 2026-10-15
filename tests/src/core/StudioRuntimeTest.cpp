#include "NativeInstrumentTestAccess.h"
/* Native studio integration tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <optional>
#include <QtEndian>
#include <QFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QImage>
#include <QPainter>
#include <QScopeGuard>
#include <QThread>
#include <QEvent>
#include <memory>
#include <array>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <limits>
#include <utility>
#include <thread>
#include <atomic>
#include <sndfile.h>

#include "Clip.h"
#include "AudioEngine.h"
#include "AudioResampler.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "DummyInstrument.h"
#include "DrumSynth.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "InstrumentFunctions.h"
#include "LfoController.h"
#include "Microtuner.h"
#include "Keymap.h"
#include "Scale.h"
#include "TuningFile.h"
#include "Oscillator.h"
#include "MidiClip.h"
#include "MidiEvent.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "PluginFactory.h"
#include "ProjectRenderer.h"
#include "PathUtil.h"
#include "PatternStore.h"
#include "SampleFrame.h"
#include "SampleBuffer.h"
#include "SampleDecoder.h"
#include "../../../src/core/SampleDecoderBackends.h"
#include "SampleClip.h"
#include "SamplePlayHandle.h"
#include "SampleRecordHandle.h"
#include "SampleRecordingBuffer.h"
#include "SampleTrack.h"
#include "SampleThumbnail.h"
#include "Song.h"
#include "Mixer.h"
#include "StudioCapabilities.h"
#include "../core/StudioParameterTestUtils.h"
#include "StudioAgent.h"
#include "StudioAudio.h"
#include "StudioRuntime.h"
#include "StudioAssetStore.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#ifdef LMMS_TEST_LIBGIG
#include "GigNativeFixture.h"
#endif
#include "Track.h"
#include "TempoSyncKnobModel.h"

using namespace lmms;

class StudioRuntimeTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;

	QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", operation}, {"args", args}};
	}

	QJsonObject provider()
	{
		return {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", "chat"},
			{"endpoint", "http://127.0.0.1:12345/test"}, {"model", "test-fixture"}}}};
	}

	QJsonObject tool(const QString& id, const QString& name, const QJsonObject& args)
	{
		return {{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name},
			{"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
	}

	QJsonObject decision(const QJsonArray& calls)
	{
		return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
			{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}}}}}}};
	}

	QJsonObject protocolDecision(const QJsonArray& calls, const QString& protocol)
	{
		if (protocol == "chat") { return decision(calls); }
		QJsonArray output;
		for (const auto& value : calls)
		{
			const auto call = value.toObject(); const auto function = call.value("function").toObject();
			output.append(QJsonObject{{"type", "function_call"}, {"call_id", call.value("id")},
				{"name", function.value("name")}, {"arguments", function.value("arguments")}, {"status", "completed"}});
		}
		return {{"status", "completed"}, {"output", output}};
	}

	QJsonObject manifest(const QTemporaryDir& directory)
	{
		return manifest(directory.path());
	}

	QJsonObject manifest(const QString& directory)
	{
		QFile file(QDir(directory).filePath("session.json"));
		if (!file.open(QIODevice::ReadOnly)) { return {}; }
		return QJsonDocument::fromJson(file.readAll()).object();
	}

	QJsonObject parameterMetadata(const QJsonArray& parameters, AutomatableModel* model)
	{
		for (const auto value : parameters)
		{
			const auto item = value.toObject();
			if (item.value("id").toInt() == static_cast<int>(model->id())) { return item; }
		}
		return {};
	}

	bool constantWave(const QString& path, float value = 0.25f, int format = SF_FORMAT_PCM_16)
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly)) { return false; }
		SF_INFO info{};
		info.samplerate = 8000; info.channels = 2; info.format = SF_FORMAT_WAV | format;
		auto encoder = sf_open_fd(file.handle(), SFM_WRITE, &info, false);
		if (!encoder) { return false; }
		std::vector<float> samples(128, value);
		const bool written = sf_writef_float(encoder, samples.data(), 64) == 64;
		return sf_close(encoder) == 0 && written;
	}

	static float diagnosticAudioValue(int frame, int channel, int rate)
	{
		return static_cast<float>(.125 * std::sin(2 * std::acos(-1.) * (440 + 440 * channel) * frame / rate));
	}

	bool diagnosticAudio(const QString& path, int channels, int rate, int frames, int format, bool nonfinite = false)
	{
		QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
		SF_INFO info{}; info.samplerate = rate; info.channels = channels; info.format = format;
		auto encoder = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!encoder) { return false; }
		std::vector<float> samples(static_cast<std::size_t>(channels) * frames);
		for (int frame = 0; frame < frames; ++frame)
		{
			for (int channel = 0; channel < channels; ++channel)
			{
				samples[static_cast<std::size_t>(frame) * channels + channel] = diagnosticAudioValue(frame, channel, rate);
			}
		}
		if (nonfinite) { samples[samples.size() - 1] = std::numeric_limits<float>::quiet_NaN(); }
		const bool written = sf_writef_float(encoder, samples.data(), frames) == frames;
		return sf_close(encoder) == 0 && written;
	}

	bool writeTuningText(const QString& path, const QString& text)
	{
		QFile file(path);
		const auto bytes = text.toUtf8();
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
	}

	QByteArray nativeFixture(const QString& name)
	{
		QFile file(QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/" + name);
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	}

	bool slicerFixtureWave(const QString& path, int frames = 16000, int rate = 16000, float first = 0.25f, float second = -0.5f)
	{
		QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
		SF_INFO info{}; info.samplerate = rate; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		auto encoder = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!encoder) { return false; }
		std::vector<float> data(frames * 2);
		for (int frame = 0; frame < frames; ++frame) { data[frame * 2] = data[frame * 2 + 1] = frame < frames / 2 ? first : second; }
		const bool written = sf_writef_float(encoder, data.data(), frames) == frames;
		return sf_close(encoder) == 0 && written;
	}

	bool setNamedInstrumentParameter(int track, const QString& suffix, double value)
	{
		const auto parameters = test::executeWithAllParameters("instrument.inspect", {{"track", track}}).value("result").toObject().value("parameters").toArray();
		for (const auto item : parameters)
		{
			if (item.toObject().value("name").toString().endsWith(suffix))
			{
				return StudioCapabilities::execute("parameter.set", {{"parameter", item.toObject().value("id")}, {"value", value}}).value("ok").toBool();
			}
		}
		return false;
	}

	bool writeFixtureBytes(const QString& path, const QByteArray& bytes)
	{
		QFile file(path);
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
	}

	QByteArray readFixtureBytes(const QString& path)
	{
		QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}

	bool addNativeSampleConsumer(const QString& kind, const QString& path)
	{
		if (kind == "lfocontroller")
		{
			const int index = static_cast<int>(Engine::getSong()->controllers().size());
			return StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool()
				&& StudioCapabilities::execute("controller.waveform.set", {{"controller", index}, {"path", path}, {"select", true}}).value("ok").toBool();
		}
		const int index = static_cast<int>(Engine::getSong()->tracks().size());
		if (!StudioCapabilities::execute("track.add", {{"type", kind == "sampleclip" ? "sample" : "instrument"},
			{"name", kind}}).value("ok").toBool()) { return false; }
		if (kind == "sampleclip")
		{
			return StudioCapabilities::execute("clip.add", {{"track", index}, {"position", 0}}).value("ok").toBool()
				&& StudioCapabilities::execute("sample.load", {{"track", index}, {"clip", 0}, {"path", path}}).value("ok").toBool();
		}
		return StudioCapabilities::execute("instrument.load", {{"track", index}, {"instrument", kind}}).value("ok").toBool()
			&& StudioCapabilities::execute("instrument.resource.set", {{"track", index},
				{"resource", kind == "tripleoscillator" ? "oscillator1" : kind == "patman" ? "patch" : "sample"}, {"path", path}}).value("ok").toBool();
	}

	std::vector<SampleFrame> nativePatmanPeriod(int index)
	{
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(index));
		NotePlayHandle voice(track, 0, frames * 100, Note(TimePos(48), TimePos(), 69));
		std::vector<SampleFrame> output(frames); voice.play(output.data()); return output;
	}

	QByteArray silencedFirstGusSample(const QByteArray& source)
	{
		if (source.size() <= 335) { return {}; }
		const auto bytes = qFromLittleEndian<quint32>(source.constData() + 247);
		if (bytes == 0 || bytes > static_cast<quint32>(source.size() - 335)) { return {}; }
		auto result = source;
		std::fill_n(result.data() + 335, bytes, char(0)); return result;
	}

	bool ownsCheckpointResource(const QString& session, const QString& path, const QByteArray& expected, const QString& suffix)
	{
		if (expected.isEmpty()) { return false; }
		const auto hash = QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex());
		const auto owned = QDir(session).filePath("checkpoints/assets/" + hash + "." + suffix);
		const auto actual = PathUtil::toAbsolute(path);
		return QFileInfo(actual).canonicalFilePath() == QFileInfo(owned).canonicalFilePath()
			&& readFixtureBytes(actual) == expected;
	}

	QDomElement instrumentSettings(Instrument* native)
	{
		QDomDocument document; auto root = document.createElement(native->nodeName()); document.appendChild(root);
		NativeInstrumentTestAccess::save(native, document, root); return root;
	}

	std::array<double, 3> measureNativeSoundFont(Instrument* native, std::vector<SampleFrame>& output)
	{
		double left = 0, right = 0;
		std::vector<float> mono;
		for (int block = 0; block < 16; ++block)
		{
			native->play(output.data());
			for (const auto& frame : output)
			{
				if (frame.containsNaN() || frame.containsInf())
				{
					const auto invalid = std::numeric_limits<double>::quiet_NaN();
					return {invalid, invalid, invalid};
				}
				if (block >= 4)
				{
					left += double(frame.left()) * frame.left();
					right += double(frame.right()) * frame.right();
					mono.push_back(frame.left() + frame.right());
				}
			}
		}
		std::vector<int> crossings;
		for (std::size_t i = 1; i < mono.size(); ++i)
		{
			if (mono[i - 1] <= 0 && mono[i] > 0) { crossings.push_back(static_cast<int>(i)); }
		}
		const auto pitch = crossings.size() > 1
			? double(Engine::audioEngine()->outputSampleRate()) * (crossings.size() - 1) / (crossings.back() - crossings.front())
			: 0.;
		return {std::sqrt(left / mono.size()), std::sqrt(right / mono.size()), pitch};
	}

#ifdef LMMS_TEST_LIBGIG
	std::array<double, 2> meanNativeGig(int trackIndex)
	{
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(trackIndex));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		NotePlayHandle voice(track, 0, frames * 64, Note(TimePos(48), TimePos(), 69));
		std::vector<SampleFrame> output(frames); voice.play(output.data());
		std::array<double, 2> sum{};
		for (int block = 0; block < 16; ++block)
		{
			track->instrument()->play(output.data());
			for (const auto& frame : output)
			{
				if (frame.containsNaN() || frame.containsInf())
				{
					return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
				}
				if (block >= 4) { sum[0] += frame.left(); sum[1] += frame.right(); }
			}
		}
		for (auto& value : sum) { value /= double(frames) * 12; }
		return sum;
	}
#endif

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid());
		ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		NotePlayHandleManager::init();
		Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false);
	}

	void cleanupTestCase()
	{
		Engine::destroy();
		NotePlayHandleManager::free();
	}

	void init() { Engine::getSong()->clearProject(); }

	void nativeTransportUsesTimelineAndStopBehaviour_data()
	{
		QTest::addColumn<int>("behaviour"); QTest::addColumn<int>("stoppedPosition");
		QTest::newRow("zero") << static_cast<int>(Timeline::StopBehaviour::BackToZero) << 0;
		QTest::newRow("start") << static_cast<int>(Timeline::StopBehaviour::BackToStart) << 97;
		QTest::newRow("keep") << static_cast<int>(Timeline::StopBehaviour::KeepPosition) << 193;
	}

	void nativeTransportUsesTimelineAndStopBehaviour()
	{
		QFETCH(int, behaviour); QFETCH(int, stoppedPosition);
		const auto song = Engine::getSong(); const auto guard = Engine::audioEngine()->requestChangesGuard();
		const auto cleanup = qScopeGuard([&] { song->stop(); song->getTimeline(Song::PlayMode::Song).setStopBehaviour(Timeline::StopBehaviour::BackToStart); });
		song->getTimeline(Song::PlayMode::Song).setStopBehaviour(static_cast<Timeline::StopBehaviour>(behaviour));
		QVERIFY(StudioCapabilities::execute("transport.seek", {{"position", 97}}).value("ok").toBool());
		QSignalSpy jumped(&song->getTimeline(Song::PlayMode::Song), &Timeline::positionJumped);
		const auto play = StudioCapabilities::execute("transport.play", {});
		QVERIFY2(play.value("ok").toBool(), qPrintable(play.value("error").toString()));
		QCOMPARE(song->playMode(), Song::PlayMode::Song); QCOMPARE(song->getPlayPos().getTicks(), 97);
		// Freeze the device thread with the native model guard, then advance the
		// actual Song clock. This is not a hardware or complete DSP assertion.
		for (int period = 0; period < 32; ++period) { song->processNextBuffer(); }
		QVERIFY(song->getPlayPos().getTicks() > 97);
		QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", true}}).value("ok").toBool());
		const auto paused = song->getPlayPos().getTicks(); song->processNextBuffer(); QCOMPARE(song->getPlayPos().getTicks(), paused);
		QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", true}}).value("ok").toBool()); QVERIFY(song->isPaused());
		QVERIFY(StudioCapabilities::execute("transport.seek", {{"position", 193}}).value("ok").toBool());
		QCOMPARE(song->getPlayPos().getTicks(), 193); QCOMPARE(song->getTimeline().frameOffset(), 0.f); QCOMPARE(jumped.count(), 1);
		QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", false}}).value("ok").toBool()); QVERIFY(song->isPlaying());
		QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", false}}).value("ok").toBool()); QVERIFY(song->isPlaying());
		const auto stopped = StudioCapabilities::execute("transport.stop", {}); QVERIFY(stopped.value("ok").toBool());
		QVERIFY(song->isStopped()); QCOMPARE(song->getPlayPos().getTicks(), stoppedPosition);
		QVERIFY(StudioCapabilities::execute("transport.stop", {}).value("ok").toBool());
		const auto backend = stopped.value("result").toObject().value("backend").toObject();
		QCOMPARE(backend.value("kind").toString(), QString("dummy")); QVERIFY(!backend.value("supportsCapture").toBool());
		QVERIFY(backend.value("renderOnly").toBool()); QVERIFY(backend.value("running").toBool());
	}

	void nativeTransportSelectsSongPatternAndMidiClip_data()
	{
		QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("owner");
		QTest::newRow("song") << QString("song") << QString("song");
		QTest::newRow("pattern") << QString("pattern") << QString("pattern");
		QTest::newRow("song-midi") << QString("midiClip") << QString("song");
		QTest::newRow("pattern-midi") << QString("midiClip") << QString("pattern");
	}

	void nativeTransportSelectsSongPatternAndMidiClip()
	{
		QFETCH(QString, mode); QFETCH(QString, owner); const auto song = Engine::getSong();
		const auto guard = Engine::audioEngine()->requestChangesGuard(); const auto cleanup = qScopeGuard([&] { song->stop(); });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "pattern"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"container", "pattern"}, {"type", "instrument"}}).value("ok").toBool());
		QJsonObject args{{"playMode", mode}};
		if (mode == "pattern") { args.insert("pattern", 0); }
		if (mode == "midiClip") { args.insert("container", owner); args.insert("track", 0); args.insert("clip", 0); args.insert("loop", false); }
		const auto response = StudioCapabilities::execute("transport.play", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("playMode").toString(), mode);
		if (mode == "pattern") { QCOMPARE(Engine::patternStore()->currentPattern(), 0); }
		if (mode == "midiClip")
		{
			const auto selected = result.value("midiClip").toObject(); QCOMPARE(selected.value("container").toString(), owner);
			QCOMPARE(selected.value("track").toInt(), 0); QCOMPARE(selected.value("clip").toInt(), 0); QVERIFY(!selected.value("loop").toBool());
			QVERIFY(song->playingMidiClip()); QVERIFY(!song->midiClipLoopEnabled());
		}
	}

	void invalidTransportArgumentsPreserveNativePlayback_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("mode") << QString("transport.play") << QJsonObject{{"playMode", "invented"}};
		QTest::newRow("song-loop") << QString("transport.play") << QJsonObject{{"loop", true}};
		QTest::newRow("missing-midi") << QString("transport.play") << QJsonObject{{"playMode", "midiClip"}};
		QTest::newRow("missing-pattern") << QString("transport.play") << QJsonObject{{"playMode", "pattern"}, {"pattern", 0}};
		QTest::newRow("pause-type") << QString("transport.pause") << QJsonObject{{"paused", 1}};
		QTest::newRow("negative-seek") << QString("transport.seek") << QJsonObject{{"position", -1}};
		QTest::newRow("overflow-seek") << QString("transport.seek") << QJsonObject{{"position", static_cast<double>(MaxSongLength) + 1}};
		QTest::newRow("fractional-seek") << QString("transport.seek") << QJsonObject{{"position", 1.5}};
		QTest::newRow("inactive-mode-seek") << QString("transport.seek") << QJsonObject{{"playMode", "pattern"}, {"position", 0}};
	}

	void invalidTransportArgumentsPreserveNativePlayback()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, args); const auto song = Engine::getSong();
		const auto guard = Engine::audioEngine()->requestChangesGuard(); const auto cleanup = qScopeGuard([&] { song->stop(); });
		QVERIFY(StudioCapabilities::execute("transport.seek", {{"position", 109}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("transport.play", {}).value("ok").toBool());
		const auto before = StudioCapabilities::execute("transport.inspect", {}).value("result").toObject();
		const auto rejected = StudioCapabilities::execute(operation, args); QVERIFY(!rejected.value("ok").toBool());
		QVERIFY(!rejected.value("error").toString().isEmpty());
		QCOMPARE(StudioCapabilities::execute("transport.inspect", {}).value("result").toObject(), before);
	}

	void playbackControlsRejectNativeRecordingState_data()
	{
		QTest::addColumn<int>("state");
		QTest::newRow("song-recording") << 0; QTest::newRow("armed-sample") << 1;
		QTest::newRow("active-sample") << 2; QTest::newRow("pending-sample") << 3;
		QTest::newRow("armed-global-automation") << 4;
	}

	void playbackControlsRejectNativeRecordingState()
	{
		QFETCH(int, state); const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		const bool wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto cleanup = qScopeGuard([&] { song->stop(); if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto sample = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		sample->setSampleBuffer(previous);
		std::unique_ptr<SampleRecordHandle> active; AutomationClip* automation = nullptr;
		const auto reset = qScopeGuard([&] { delete automation; active.reset(); sample->setRecord(false); QString error; sample->finalizeRecordings(error); song->clearErrors(); });
		if (state == 0) { song->record(); }
		if (state == 1) { sample->setRecord(true); }
		if (state == 2) { active = std::make_unique<SampleRecordHandle>(sample); }
		if (state == 3)
		{
			sample->beginRecordingTake(); std::thread completion([&] { sample->stageRecordingCompletion(nullptr, "diagnostic deferred failure"); }); completion.join();
			QCoreApplication::removePostedEvents(sample, QEvent::MetaCall); QVERIFY(sample->hasPendingRecording());
		}
		if (state == 4) { automation = new AutomationClip(song->globalAutomationTrack()); automation->setRecording(true); }
		for (const auto operation : {QString("transport.play"), QString("transport.pause"), QString("transport.stop"), QString("transport.seek")})
		{
			QJsonObject args; if (operation == "transport.pause") { args.insert("paused", true); } if (operation == "transport.seek") { args.insert("position", 48); }
			const auto rejected = StudioCapabilities::execute(operation, args); QVERIFY(!rejected.value("ok").toBool());
			QVERIFY(rejected.value("error").toString().contains("recording")); QCOMPARE(sample->sample().toBase64(), previous->toBase64());
		}
		QVERIFY(StudioCapabilities::execute("transport.inspect", {}).value("ok").toBool());
	}

	void playbackControlsRejectReentrantNativeRecordingPublication()
	{
		const auto song = Engine::getSong(); const auto guard = Engine::audioEngine()->requestChangesGuard();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto sample = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		auto capture = std::make_shared<SampleRecordingBuffer>(); SampleFrame input(.25f, -.5f);
		QVERIFY(capture->append(&input, 1, 24000)); int checked = 0, notifications = 0;
		const auto connection = QObject::connect(sample, &SampleClip::sampleChanged, sample, [&] {
			++notifications;
			QVERIFY(sample->isFinalizingRecording());
			for (const auto operation : {QString("transport.play"), QString("transport.pause"), QString("transport.stop"), QString("transport.seek")})
			{
				QJsonObject args; if (operation == "transport.pause") { args.insert("paused", true); } if (operation == "transport.seek") { args.insert("position", 48); }
				QVERIFY(!StudioCapabilities::execute(operation, args).value("ok").toBool()); ++checked;
			}
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		sample->beginRecordingTake(); sample->stageRecordingCompletion(std::move(capture)); QVERIFY(notifications > 0); QCOMPARE(checked, 4 * notifications);
		QVERIFY(!sample->isFinalizingRecording()); QCOMPARE(sample->sample().sampleRate(), 24000);
	}

	void nativeTransportRejectsStoppedAndReservedBackends()
	{
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong(); const bool wasRunning = engine->audioDev()->isRunning();
		engine->audioDev()->stopProcessing(); const auto cleanup = qScopeGuard([&] { engine->restoreAudioDevice(); song->stop(); if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(!StudioCapabilities::execute("transport.play", {}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("transport.pause", {{"paused", true}}).value("ok").toBool());
		song->playSong(); song->togglePause(); QVERIFY(song->isPaused());
		QVERIFY(!StudioCapabilities::execute("transport.pause", {{"paused", false}}).value("ok").toBool()); QVERIFY(song->isPaused());
		engine->storeAudioDevice();
		for (const auto operation : {QString("transport.play"), QString("transport.pause"), QString("transport.stop"), QString("transport.seek")})
		{
			QJsonObject args; if (operation == "transport.pause") { args.insert("paused", true); } if (operation == "transport.seek") { args.insert("position", 48); }
			const auto rejected = StudioCapabilities::execute(operation, args); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(rejected.value("error").toString().contains("export"));
		}
		const auto backend = StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("backend").toObject();
		QVERIFY(backend.value("reserved").toBool()); QVERIFY(!backend.value("running").toBool());
	}

	void deletingTheNativePlaybackMidiClipStopsBeforeItsNotesDisappear_data()
	{
		QTest::addColumn<bool>("paused"); QTest::newRow("playing") << false; QTest::newRow("paused") << true;
	}

	void deletingTheNativePlaybackMidiClipStopsBeforeItsNotesDisappear()
	{
		QFETCH(bool, paused); const auto song = Engine::getSong(); const auto guard = Engine::audioEngine()->requestChangesGuard();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("transport.play", {{"playMode", "midiClip"}, {"track", 0}, {"clip", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("transport.play", {{"playMode", "midiClip"}, {"track", 0}, {"clip", 1}}).value("ok").toBool());
		const auto selected = song->playingMidiClip();
		QVERIFY(StudioCapabilities::execute("clip.remove", {{"track", 0}, {"clip", 0}}).value("ok").toBool());
		QVERIFY(song->isPlaying()); QCOMPARE(song->playingMidiClip(), selected);
		if (paused) { QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", true}}).value("ok").toBool()); }
		QVERIFY(StudioCapabilities::execute("clip.remove", {{"track", 0}, {"clip", 0}}).value("ok").toBool());
		QVERIFY(song->isStopped()); QVERIFY(!song->playingMidiClip()); QCOMPARE(song->playMode(), Song::PlayMode::None);
		song->processNextBuffer(); QVERIFY(StudioCapabilities::execute("transport.stop", {}).value("ok").toBool());
	}

	void transportRequestReplayPreservesHistoryAndDoesNotRestartRecoveredPlayback()
	{
		QTemporaryDir directory; const auto guard = Engine::audioEngine()->requestChangesGuard(); const auto song = Engine::getSong();
		const auto cleanup = qScopeGuard([&] { song->stop(); });
		QJsonObject before;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); before = manifest(directory);
			const int revision = before.value("revision").toInt();
			const auto play = request("native-play", "transport.play"); QVERIFY(runtime.process(play).value("ok").toBool());
			QVERIFY(runtime.process(request("native-pause", "transport.pause", {{"paused", true}})).value("ok").toBool()); QVERIFY(song->isPaused());
			QVERIFY(runtime.process(play).value("replayed").toBool()); QVERIFY(song->isPaused());
			QCOMPARE(manifest(directory).value("revision").toInt(), revision); QCOMPARE(manifest(directory).value("history"), before.value("history"));
		}
		{
			StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error)); QVERIFY(song->isStopped());
			QVERIFY(recovered.process(request("native-play", "transport.play")).value("replayed").toBool()); QVERIFY(song->isStopped());
		}
	}

	void conversationExecutesAndPreservesNativeEdits()
	{
		QTemporaryDir directory;
		int rounds = 0;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			++rounds;
			if (rounds == 1)
			{
				return decision({
					tool("plan", "agent_plan_set", {{"goal", "Editable slow piece"}, {"steps", QJsonArray{"Create a track", "Set tempo"}}, {"decisions", QJsonArray{"Leave room for a later motif"}}}),
					tool("memory", "session_memory_set", {{"memory", QJsonObject{{"preference", "Slow tempo"}}}}),
					tool("track", "track_add", {{"type", "instrument"}, {"name", "Conversational track"}}),
					tool("tempo", "song_set", {{"tempo", 91}})});
			}
			// Tool results and observations must be sent back, not an invented project.
			const auto messages = body.value("messages").toArray();
			int results = 0;
			for (const auto& value : messages) { if (value.toObject().value("role").toString() == "tool") { ++results; } }
			if (results != 4) { throw std::runtime_error("Native results were missing from continuation"); }
			return decision({tool("finish", "agent_finish", {{"status", "complete"}, {"summary", "Created an editable track at 91 BPM."}, {"evidence", QJsonArray{"track", "tempo"}}})});
		});
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Set up a slow editable piece"}})).value("ok").toBool());
		for (int i = 0; i < 7; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
		const auto state = manifest(directory);
		const auto agent = state.value("agent").toObject();
		QVERIFY(StudioAgent::validState(agent));
		QCOMPARE(agent.value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(agent.value("job").toObject().value("operations").toInt(), 5);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		QCOMPARE(Engine::getSong()->getTempo(), 91);
		QCOMPARE(state.value("memory").toObject().value("preference").toString(), QString("Slow tempo"));
		QCOMPARE(state.value("history").toArray().size(), 3);
		QVERIFY(studio.advanceAgent().isEmpty());
		QVERIFY(studio.process(request("next", "session.chat", {{"text", "Continue with a motif"}})).value("ok").toBool());
		const auto archive = manifest(directory).value("agent").toObject().value("archives").toArray().at(0).toObject();
		QFile file(directory.filePath(archive.value("path").toString()));
		QVERIFY(file.open(QIODevice::ReadOnly));
		const auto archived = file.readAll();
		QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(archived, QCryptographicHash::Sha256).toHex()), archive.value("sha256").toString());
		QCOMPARE(QJsonDocument::fromJson(archived).object().value("status").toString(), QString("complete"));
	}

	void catalogCallsKeepNativeHistoryReceiptsAndRecovery_data()
	{
		QTest::addColumn<QString>("protocol");
		QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses");
	}
	void catalogCallsKeepNativeHistoryReceiptsAndRecovery()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error;
		{
			int rounds = 0;
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
				if (body.value("tools").toArray() != StudioAgent::tools(protocol)) { throw std::runtime_error("Bounded tool declarations were not sent"); }
				if (++rounds == 1)
				{
					return protocolDecision({tool("catalog", "studio_capabilities", {}),
						tool("reset", "studio_execute", {{"op", "mixer.reset"}, {"args", QJsonObject{{"channel", 2}}}}),
						tool("remove", "studio_execute", {{"op", "mixer.remove"}, {"args", QJsonObject{{"channel", 1}}}})}, protocol);
				}
				return protocolDecision({tool("finish", "agent_finish", {{"status", "complete"}, {"summary", "Reset and removed native buses."},
					{"evidence", QJsonArray{"reset", "remove"}}})}, protocol);
			});
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("first", "mixer.add", {{"name", "First bus"}})).value("ok").toBool());
			QVERIFY(studio.process(request("second", "mixer.add", {{"name", "Second bus"}})).value("ok").toBool());
			QVERIFY(studio.process(request("level", "mixer.edit", {{"channel", 2}, {"volume", .25}})).value("ok").toBool());
			auto providerArgs = provider(); auto configuration = providerArgs.value("configuration").toObject();
			configuration.insert("protocol", protocol); providerArgs.insert("configuration", configuration);
			QVERIFY(studio.process(request("provider", "session.provider.set", providerArgs)).value("ok").toBool());
			const auto before = manifest(directory);
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Reset the second bus and remove the first"}})).value("ok").toBool());
			for (int i = 0; i < 6; ++i) { QVERIFY(!studio.advanceAgent().isEmpty()); }
			const auto state = manifest(directory); const auto agent = state.value("agent").toObject(); const auto job = agent.value("job").toObject();
			QVERIFY(StudioAgent::validState(agent)); QCOMPARE(job.value("status").toString(), QString("complete"));
			QCOMPARE(job.value("operations").toInt(), 4); QCOMPARE(rounds, 2);
			QCOMPARE(state.value("history").toArray().size(), before.value("history").toArray().size() + 2);
			QCOMPARE(state.value("revision").toInt(), before.value("revision").toInt() + 2);
			QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(2)); QCOMPARE(Engine::mixer()->mixerChannel(1)->m_volumeModel.value(), 1.f);
			int routed = 0;
			for (const auto& value : job.value("records").toArray())
			{
				const auto record = value.toObject(); const auto call = record.value("call").toObject();
				if (call.value("name").toString() != "studio_execute") { continue; }
				++routed; QVERIFY(call.value("args").toObject().value("args").isObject()); QVERIFY(record.value("response").toObject().value("ok").toBool());
			}
			QCOMPARE(routed, 2);
			QVERIFY(studio.process(request("undo-remove", "session.undo")).value("ok").toBool());
			QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(3)); QCOMPARE(Engine::mixer()->mixerChannel(2)->m_volumeModel.value(), 1.f);
			auto replay = request("@agent/" + job.value("id").toString() + "/2", "mixer.remove", {{"channel", 1}});
			replay.insert("expectedRevision", before.value("revision").toInt() + 1);
			bool receiptFound = false;
			for (const auto& value : state.value("requests").toArray())
			{
				const auto receipt = value.toObject(); if (receipt.value("id") != replay.value("id")) { continue; }
				receiptFound = true;
				QCOMPARE(receipt.value("sha256").toString(), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(replay).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
			}
			QVERIFY(receiptFound);
			// The external protocol must still refuse the agent's internal ID namespace.
			QVERIFY(!studio.process(replay).value("ok").toBool()); QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(3));
			QVERIFY(studio.process(request("undo-reset", "session.undo")).value("ok").toBool());
			QCOMPARE(Engine::mixer()->mixerChannel(2)->m_name, QString("Second bus")); QCOMPARE(Engine::mixer()->mixerChannel(2)->m_volumeModel.value(), .25f);
			QVERIFY(studio.process(request("redo-reset", "session.redo")).value("ok").toBool());
			QVERIFY(studio.process(request("redo-remove", "session.redo")).value("ok").toBool());
		}
		Engine::getSong()->clearProject(); StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(2)); QCOMPARE(Engine::mixer()->mixerChannel(1)->m_name, Mixer::tr("Channel 1"));
		QCOMPARE(Engine::mixer()->mixerChannel(1)->m_volumeModel.value(), 1.f);
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void catalogFailuresAreDurableAndSkipDependentCalls_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("chat-invalid-wrapper") << QString("chat") << QJsonObject{{"op", "session.shutdown"}, {"args", QJsonObject{}}};
		QTest::newRow("responses-invalid-wrapper") << QString("responses") << QJsonObject{{"op", "session.shutdown"}, {"args", QJsonObject{}}};
		QTest::newRow("chat-invalid-native-arguments") << QString("chat") << QJsonObject{{"op", "mixer.remove"}, {"args", QJsonObject{{"channel", 0}}}};
		QTest::newRow("responses-invalid-native-arguments") << QString("responses") << QJsonObject{{"op", "mixer.remove"}, {"args", QJsonObject{{"channel", 0}}}};
		QTest::newRow("chat-omitted-native-function") << QString("chat") << QJsonObject{{"op", "effect.remove"}, {"args", QJsonObject{{"channel", 0}, {"effect", 0}}}};
		QTest::newRow("responses-omitted-native-function") << QString("responses") << QJsonObject{{"op", "effect.remove"}, {"args", QJsonObject{{"channel", 0}, {"effect", 0}}}};
	}
	void catalogFailuresAreDurableAndSkipDependentCalls()
	{
		QFETCH(QString, protocol); QFETCH(QJsonObject, args); QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return protocolDecision({tool("bad", "studio_execute", args), tool("dependent", "song_set", {{"tempo", 99}})}, protocol);
		});
		QString error; QVERIFY2(studio.open(error), qPrintable(error));
		auto providerArgs = provider(); auto configuration = providerArgs.value("configuration").toObject(); configuration.insert("protocol", protocol); providerArgs.insert("configuration", configuration);
		QVERIFY(studio.process(request("provider", "session.provider.set", providerArgs)).value("ok").toBool());
		const auto before = manifest(directory);
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Exercise a rejected native call"}})).value("ok").toBool());
		studio.advanceAgent(); studio.advanceAgent();
		const auto agent = manifest(directory).value("agent").toObject(); const auto job = agent.value("job").toObject();
		QVERIFY(StudioAgent::validState(agent)); QVERIFY(job.value("pending").toArray().isEmpty()); QCOMPARE(job.value("operations").toInt(), 1);
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo); QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(1));
		QCOMPARE(manifest(directory).value("history"), before.value("history"));
		int failed = 0;
		for (const auto& value : job.value("records").toArray())
		{
			const auto record = value.toObject(); if (record.value("kind").toString() != "tool") { continue; }
			++failed; QVERIFY(!record.value("response").toObject().value("ok").toBool());
			if (args.value("op").toString() == "effect.remove" && record.value("call").toObject().value("id").toString() == "bad")
			{
				QVERIFY(record.value("response").toObject().value("error").toString().contains("Effect chain is empty"));
			}
		}
		QCOMPARE(failed, 2); QVERIFY(studio.process(request("inspect", "session.inspect")).value("ok").toBool());
	}

	void recoveryKeepsCommittedCallsAndReplansPendingCalls()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
				return decision({tool("track", "track_add", {{"type", "automation"}}), tool("tempo", "song_set", {{"tempo", 90}})});
			});
			QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Prepare an automation track"}})).value("ok").toBool());
			studio.advanceAgent(); // Inference saved before native work.
			studio.advanceAgent(); // Track and job cursor committed in one manifest.
			QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("operations").toInt(), 1);
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision({tool("finish", "agent_finish", {{"status", "complete"}, {"summary", "Preserved the automation track."}, {"evidence", QJsonArray{"track"}}})});
		});
		QString error;
		QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		const auto job = manifest(directory).value("agent").toObject().value("job").toObject();
		QVERIFY(job.value("pending").toArray().isEmpty());
		QCOMPARE(job.value("records").toArray().last().toObject().value("response").toObject().value("ok").toBool(), false);
		recovered.advanceAgent(); recovered.advanceAgent();
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void failuresSkipDependentWorkAndCannotBecomeEvidence()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision({tool("bad", "song_set", {{"tempo", 88}, {"masterPitch", 100}}),
				tool("dependent", "track_add", {{"type", "automation"}})});
		});
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Change tempo and create a track"}})).value("ok").toBool());
		studio.advanceAgent(); studio.advanceAgent();
		QVERIFY(Engine::getSong()->tracks().empty());
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		const auto job = manifest(directory).value("agent").toObject().value("job").toObject();
		QVERIFY(job.value("pending").toArray().isEmpty());
		QCOMPARE(job.value("operations").toInt(), 1);
		QVERIFY(!studio.process(request("false-evidence", "agent.finish", {{"status", "complete"}, {"summary", "Unsupported"}, {"evidence", QJsonArray{"bad"}}})).value("ok").toBool());
		QVERIFY(StudioAgent::validState(manifest(directory).value("agent").toObject()));
	}

	void budgetsPauseAndResumeWithoutRepeatingNativeWork()
	{
		QTemporaryDir directory;
		int rounds = 0;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++rounds == 1) { return decision({tool("memory", "session_memory_set", {{"memory", QJsonObject{{"remember", true}}}}), tool("tempo", "song_set", {{"tempo", 99}})}); }
			return decision({tool("finish", "agent_finish", {{"status", "complete"}, {"summary", "Updated tempo."}, {"evidence", QJsonArray{"tempo"}}})});
		});
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Remember and adjust tempo"}, {"maxOperations", 1}})).value("ok").toBool());
		studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("paused"));
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		QVERIFY(studio.process(request("resume", "agent.resume", {{"maxOperations", 3}})).value("ok").toBool());
		studio.advanceAgent(); studio.advanceAgent(); studio.advanceAgent();
		QCOMPARE(Engine::getSong()->getTempo(), 99);
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("operations").toInt(), 3);
		QCOMPARE(rounds, 2);
	}

	void cancelledInferenceCannotExecuteItsReturnedDecision()
	{
		QTemporaryDir directory;
		StudioRuntime* current = nullptr;
		bool cancellationObserved = false;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled& cancelled) {
			if (current->process(request("busy", "song.set", {{"tempo", 200}})).value("ok").toBool()) { throw std::runtime_error("Concurrent mutation was accepted"); }
			if (!current->process(request("cancel", "agent.cancel")).value("ok").toBool()) { throw std::runtime_error("Cancellation was rejected"); }
			cancellationObserved = cancelled();
			return decision({tool("late", "song_set", {{"tempo", 200}})});
		});
		current = &studio;
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Consider a tempo change"}})).value("ok").toBool());
		studio.advanceAgent();
		QVERIFY(cancellationObserved);
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		const auto agent = manifest(directory).value("agent").toObject();
		QCOMPARE(agent.value("job").toObject().value("status").toString(), QString("cancelled"));
		QVERIFY(agent.value("job").toObject().value("pending").toArray().isEmpty());
		QVERIFY(StudioAgent::validState(agent));
	}

	void providerAbsenceAndReplanningRetainConversation()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{{"role", "assistant"}, {"content", "I need a musical preference."}}}}}}};
		});
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Continue my piece"}})).value("ok").toBool());
		studio.advanceAgent();
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("rounds").toInt(), 0);
		QVERIFY(studio.process(request("provider", "session.provider.set", provider())).value("ok").toBool());
		QVERIFY(studio.process(request("resume", "agent.resume")).value("ok").toBool());
		studio.advanceAgent();
		QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 100}})).value("ok").toBool());
		QVERIFY(studio.process(request("replan", "agent.resume")).value("ok").toBool());
		const auto agent = manifest(directory).value("agent").toObject();
		QCOMPARE(agent.value("job").toObject().value("pastInputs").toArray().size(), 1);
		QVERIFY(StudioAgent::validState(agent));
		QCOMPARE(studio.process(request("history", "conversation.history")).value("result").toObject().value("total").toInt(), 2);
	}

	void listeningSubmitsTheActualScopedWaveform()
	{
		QTemporaryDir directory;
		const auto path = directory.filePath("source.wav");
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		SF_INFO info{}; info.samplerate = 8000; info.channels = 1; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
		auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false);
		QVERIFY(audio);
		std::vector<float> samples(800, 0.25f);
		std::fill(samples.begin(), samples.begin() + 400, 0.125f);
		QCOMPARE(sf_writef_float(audio, samples.data(), 800), sf_count_t(800));
		QCOMPARE(sf_close(audio), 0); file.close();
		QVERIFY(file.open(QIODevice::ReadOnly));
		const auto original = file.readAll(); file.close();
		const auto excerpt = StudioAudio::excerpt({{"path", path}, {"offsetSeconds", 0.05}, {"durationSeconds", 0.025}});
		QCOMPARE(excerpt.observation.value("sourceSha256").toString(), QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()));
		QCOMPARE(excerpt.observation.value("durationSeconds").toDouble(), 0.025);
		QCOMPARE(excerpt.observation.value("transcodingClippingSamples").toInt(), 0);
		QFile decoded(directory.filePath("excerpt.wav"));
		QVERIFY(decoded.open(QIODevice::WriteOnly));
		QCOMPARE(decoded.write(excerpt.wave), qint64(excerpt.wave.size())); decoded.close();
		QVERIFY(decoded.open(QIODevice::ReadOnly));
		SF_INFO actual{};
		std::unique_ptr<SNDFILE, decltype(&sf_close)> pcm(sf_open_fd(decoded.handle(), SFM_READ, &actual, false), &sf_close);
		QVERIFY(pcm);
		QCOMPARE(actual.frames, sf_count_t(200));
		float first = 0;
		QCOMPARE(sf_readf_float(pcm.get(), &first, 1), sf_count_t(1));
		QVERIFY(qAbs(first - 0.25f) < 0.0001f);
		bool waveformReceived = false;
		StudioRuntime studio(directory.filePath("session"), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			const auto content = body.value("messages").toArray().last().toObject().value("content").toArray();
			waveformReceived = QByteArray::fromBase64(content.at(1).toObject().value("input_audio").toObject().value("data").toString().toLatin1()) == excerpt.wave
				&& content.at(0).toObject().value("text").toString().contains(excerpt.observation.value("sourceSha256").toString());
			return QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{{"role", "assistant"}, {"content", "Test fixture judgment; no perceptual quality evidence."}}}}}}};
		});
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		auto audioProvider = provider(); audioProvider.insert("role", "audio");
		QVERIFY(studio.process(request("provider", "session.provider.set", audioProvider)).value("ok").toBool());
		const auto response = studio.process(request("listen", "audio.listen", {{"path", path}, {"prompt", "Evaluate this excerpt"}, {"offsetSeconds", 0.05}, {"durationSeconds", 0.025}}));
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(waveformReceived);
		QCOMPARE(response.value("result").toObject().value("excerpt").toObject(), excerpt.observation);
		QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
		QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt({{"path", path}}, [] { return true; }), std::runtime_error);
	}

	void nativeNoteEditing()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 192}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0},
			{"position", 13}, {"length", 48}, {"key", 60}, {"volume", 75}, {"panning", -20}}).value("ok").toBool());
		auto c = dynamic_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		QVERIFY(c);
		QCOMPARE(c->notes().size(), std::size_t(1));
		QCOMPARE(static_cast<int>(c->notes().front()->pos()), 13); // No GUI quantization.
		QCOMPARE(c->notes().front()->key(), 60);
		QCOMPARE(c->notes().front()->getVolume(), volume_t(75));
		QVERIFY(!StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0},
			{"position", 0}, {"length", 48}, {"key", 128}}).value("ok").toBool());
		QCOMPARE(c->notes().size(), std::size_t(1));
		QVERIFY(!StudioCapabilities::execute("note.edit", {{"track", 0}, {"clip", 0}, {"note", 0},
			{"position", MaxSongLength - 2}, {"length", 48}}).value("ok").toBool());
		QCOMPARE(c->notes().front()->pos().getTicks(), 13);
		QVERIFY(StudioCapabilities::execute("clip.clone", {{"track", 0}, {"clip", 0}, {"position", 384}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->tracks().at(0)->numOfClips(), 2);
		QVERIFY(StudioCapabilities::execute("note.edit", {{"track", 0}, {"clip", 1}, {"note", 0}, {"key", 67}}).value("ok").toBool());
		QCOMPARE(c->notes().front()->key(), 60); // Clone remains independently editable.
	}

	void automationEditsUseNativeDiscontinuitiesAndTangents()
	{
		auto instrument = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		auto model = instrument->volumeModel();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "automation"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 1}, {"position", 192}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("automation.connect", {{"track", 1}, {"clip", 0},
			{"parameter", static_cast<int>(model->id())}}).value("ok").toBool());
		auto curve = dynamic_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClip(0));
		QVERIFY(curve);
		QVERIFY(StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 0}, {"value", 20}, {"outValue", 40}, {"progression", 1}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 24}, {"value", 80}, {"outValue", 100}}).value("ok").toBool());
		QCOMPARE(curve->valueAt(0), 20.f);
		QCOMPARE(curve->valueAt(12), 60.f);
		QCOMPARE(curve->valueAt(24), 80.f);
		QCOMPARE(curve->valueAt(25), 100.f);
		QVERIFY(StudioCapabilities::execute("automation.edit", {{"track", 1}, {"clip", 0},
			{"progression", 2}, {"tension", 0.5}}).value("ok").toBool());
		QVERIFY(curve->setNodeTangents(0, 0, 0, true));
		QVERIFY(curve->setNodeTangents(24, 0, 0, true));
		// Zero-tangent Hermite at one quarter: 40 + (3t^2 - 2t^3) * 40.
		QCOMPARE(curve->valueAt(6), 46.25f);
		QVERIFY(StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 13}, {"value", 65}, {"inTangent", 0.25}, {"outTangent", -0.5}}).value("ok").toBool());
		QVERIFY(curve->getTimeMap().contains(13)); // Source ticks bypass GUI quantization.
		QCOMPARE(curve->getTimeMap().value(13).getOutTangent(), -0.5f);
		QVERIFY(curve->getTimeMap().value(13).lockedTangents());
		const auto before = StudioCapabilities::execute("automation.inspect", {{"track", 1}, {"clip", 0}});
		QVERIFY(!StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 13}, {"value", 60}, {"outValue", 10000}, {"progression", 0}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 13}, {"value", 60}, {"inTangent", 1}, {"lockedTangents", false}}).value("ok").toBool());
		QCOMPARE(StudioCapabilities::execute("automation.inspect", {{"track", 1}, {"clip", 0}}), before);
		QVERIFY(StudioCapabilities::execute("automation.point", {{"track", 1}, {"clip", 0},
			{"position", 13}, {"value", 65}, {"lockedTangents", false}}).value("ok").toBool());
		QVERIFY(!curve->getTimeMap().value(13).lockedTangents());
		QVERIFY(std::abs(curve->getTimeMap().value(13).getInTangent() - 40.f / 24) < 0.0001f);
		QVERIFY(StudioCapabilities::execute("automation.removePoint", {{"track", 1}, {"clip", 0},
			{"position", 13}}).value("result").toObject().value("removed").toBool());
		QVERIFY(curve->valuesAfter(25) == nullptr);
		QVERIFY(StudioCapabilities::execute("automation.disconnect", {{"track", 1}, {"clip", 0},
			{"parameter", static_cast<int>(model->id())}}).value("result").toObject().value("disconnected").toBool());
		QVERIFY(curve->objects().empty());
		QCOMPARE(curve->valueAt(6), 46.25f); // Disconnect retains raw native values.
		QVERIFY(StudioCapabilities::execute("automation.clear", {{"track", 1}, {"clip", 0}}).value("ok").toBool());
		QVERIFY(curve->valuesAfter(0) == nullptr);
	}

	void copiedAndDisconnectedAutomationTargetsHaveCorrectLifetimes()
	{
		AutomationClip curve(nullptr);
		auto model = std::make_unique<FloatModel>(20.f, 0.f, 100.f, 0.1f);
		QVERIFY(curve.addObject(model.get()));
		curve.putValues(0, 20, 40, false);
		std::unique_ptr<AutomationClip> copy(curve.clone());
		QCOMPARE(copy->objects().size(), std::size_t(1));
		QVERIFY(curve.removeObject(model.get()));
		QSignalSpy detached(&curve, &Model::dataChanged);
		model.reset();
		QCOMPARE(detached.count(), 0); // A detached model no longer calls this clip.
		QVERIFY(copy->objects().empty()); // The clone owns its destruction callback.
		QCOMPARE(copy->valueAt(1), 40.f);

		auto song = Engine::getSong();
		auto track = static_cast<AutomationTrack*>(Track::create(Track::Type::Automation, song));
		auto clip = static_cast<AutomationClip*>(track->createClip(0));
		FloatModel first(20, 0, 100, 0.1), second(80, 0, 100, 0.1);
		QVERIFY(clip->addObject(&first));
		DataFile file(DataFile::Type::SongProject);
		clip->saveSettings(file, file.content());
		clip->clearObjects();
		QVERIFY(clip->addObject(&second));
		clip->loadSettings(file.content());
		QVERIFY(clip->objects().empty());
		AutomationClip::resolveAllIDs();
		QCOMPARE(clip->objects().size(), std::size_t(1));
		QCOMPARE(clip->objects().front().data(), &first);
		AutomationClip::resolveAllIDs();
		QCOMPARE(clip->objects().size(), std::size_t(1));
	}

	void globalAutomationInspectionDoesNotAllocateAndUsesNativePlayback()
	{
		auto song = Engine::getSong();
		auto instrument = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song));
		auto model = instrument->volumeModel();
		const auto id = static_cast<int>(model->id());
		const auto before = song->globalAutomationTrack()->numOfClips();
		const auto absent = StudioCapabilities::execute("automation.global.inspect", {{"parameter", id}, {"position", 12}});
		QVERIFY(absent.value("ok").toBool());
		QVERIFY(absent.value("result").toObject().value("points").toArray().isEmpty());
		QCOMPARE(song->globalAutomationTrack()->numOfClips(), before);
		QVERIFY(!StudioCapabilities::execute("automation.global.point", {{"parameter", id}, {"position", 0},
			{"value", 40}, {"tension", -1}}).value("ok").toBool());
		QCOMPARE(song->globalAutomationTrack()->numOfClips(), before);
		QVERIFY(StudioCapabilities::execute("automation.global.point", {{"parameter", id}, {"position", 0},
			{"value", 40}, {"progression", 1}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("automation.global.point", {{"parameter", id}, {"position", 24},
			{"value", 80}}).value("ok").toBool());
		QCOMPARE(song->automatedValuesAt(12).value(model), 60.f);
		const auto inspected = StudioCapabilities::execute("automation.global.inspect", {{"parameter", id}, {"position", 12}});
		QCOMPARE(inspected.value("result").toObject().value("value").toDouble(), 60.0);
		auto curve = AutomationClip::globalAutomationClip(model);
		DataFile file(DataFile::Type::SongProject);
		curve->saveSettings(file, file.content());
		curve->loadSettings(file.content());
		AutomationClip::resolveAllIDs();
		QCOMPARE(curve->objects().size(), std::size_t(1)); // Existing global binding is retained once.
		QVERIFY(StudioCapabilities::execute("automation.global.clear", {{"parameter", id}}).value("ok").toBool());
		QVERIFY(!song->automatedValuesAt(12).contains(model));
		QCOMPARE(curve->objects().size(), std::size_t(1));
	}

	void detuningAutomationChangesNativeNoteFrequenciesAndClonesIndependently()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0},
			{"position", 0}, {"length", 48}, {"key", 60}}).value("ok").toBool());
		auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto midi = static_cast<MidiClip*>(instrument->getClip(0));
		auto note = midi->notes().front();
		QVERIFY(!note->detuning());
		QVERIFY(StudioCapabilities::execute("note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}).value("ok").toBool());
		QVERIFY(!note->detuning());
		QVERIFY(!StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0},
			{"position", 0}, {"value", 12}, {"outValue", 61}}).value("ok").toBool());
		QVERIFY(!note->detuning());
		QVERIFY(StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0},
			{"position", 0}, {"value", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0},
			{"position", 24}, {"value", 12}, {"outValue", -12}}).value("ok").toBool());
		QVERIFY(note->hasDetuningInfo());
		NotePlayHandle handle(instrument, 0, 4096, *note);
		const float baseline = handle.unpitchedFrequency();
		QVERIFY(baseline > 0);
		handle.processTimePos(12);
		QCOMPARE(handle.currentDetuning(), 6.f);
		QVERIFY(std::abs(handle.unpitchedFrequency() / baseline - std::sqrt(2.f)) < 0.0001f);
		handle.processTimePos(24);
		QVERIFY(std::abs(handle.unpitchedFrequency() / baseline - 2.f) < 0.0001f);
		handle.processTimePos(25);
		QVERIFY(std::abs(handle.unpitchedFrequency() / baseline - 0.5f) < 0.0001f);
		QVERIFY(StudioCapabilities::execute("clip.clone", {{"track", 0}, {"clip", 0}, {"position", 192}}).value("ok").toBool());
		auto copy = static_cast<MidiClip*>(instrument->getClip(1))->notes().front();
		QVERIFY(copy->detuning() != note->detuning());
		QCOMPARE(copy->detuning()->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(copy->detuning().get()));
		QVERIFY(StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 1}, {"note", 0},
			{"position", 24}, {"value", 3}, {"progression", 2}, {"inTangent", 0.25}}).value("ok").toBool());
		QCOMPARE(note->detuning()->automationClip()->valueAt(24), 12.f);
		QCOMPARE(copy->detuning()->automationClip()->valueAt(24), 3.f);
		QVERIFY(StudioCapabilities::execute("note.detuning.clear", {{"track", 0}, {"clip", 0}, {"note", 0}}).value("ok").toBool());
		QVERIFY(!note->hasDetuningInfo());
		handle.processTimePos(26);
		QCOMPARE(handle.currentDetuning(), 0.f);
		QCOMPARE(handle.unpitchedFrequency(), baseline);
		QVERIFY(copy->hasDetuningInfo());
	}

	void automationAndDetuningPersistThroughNativeRecoveryAndUndo()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path()); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("instrument", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(studio.process(request("midi", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
			QVERIFY(studio.process(request("note", "note.add", {{"track", 0}, {"clip", 0},
				{"position", 0}, {"length", 48}, {"key", 60}})).value("ok").toBool());
			QVERIFY(studio.process(request("track", "track.add", {{"type", "automation"}})).value("ok").toBool());
			QVERIFY(studio.process(request("curve", "clip.add", {{"track", 1}, {"position", 0}})).value("ok").toBool());
			const int id = static_cast<int>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->volumeModel()->id());
			QVERIFY(studio.process(request("connect", "automation.connect", {{"track", 1}, {"clip", 0}, {"parameter", id}})).value("ok").toBool());
			QVERIFY(studio.process(request("point", "automation.point", {{"track", 1}, {"clip", 0}, {"position", 13},
				{"value", 40}, {"outValue", 80}, {"progression", 2}, {"tension", 0.5}, {"inTangent", 0.25}, {"outTangent", -0.5}})).value("ok").toBool());
			QVERIFY(studio.process(request("global", "automation.global.point", {{"parameter", id}, {"position", 12},
				{"value", 60}, {"outValue", 90}, {"progression", 1}})).value("ok").toBool());
			QVERIFY(studio.process(request("detune", "note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0},
				{"position", 13}, {"value", 6}, {"outValue", 12}, {"progression", 2}, {"tension", 0.75}, {"inTangent", 0.125}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime studio(directory.path()); QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto curve = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClip(0));
		QCOMPARE(curve->objects().size(), std::size_t(1));
		QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(instrument->volumeModel()));
		QCOMPARE(curve->progressionType(), AutomationClip::ProgressionType::CubicHermite);
		QCOMPARE(curve->getTension(), 0.5f);
		QCOMPARE(curve->getTimeMap().value(13).getOutValue(), 80.f);
		QCOMPARE(curve->getTimeMap().value(13).getInTangent(), 0.25f);
		QCOMPARE(curve->getTimeMap().value(13).getOutTangent(), -0.5f);
		QVERIFY(curve->getTimeMap().value(13).lockedTangents());
		auto note = static_cast<MidiClip*>(instrument->getClip(0))->notes().front();
		QVERIFY(note->hasDetuningInfo());
		auto detuning = note->detuning()->automationClip();
		QCOMPARE(detuning->objects().size(), std::size_t(1));
		QCOMPARE(detuning->objects().front().data(), static_cast<AutomatableModel*>(note->detuning().get()));
		QCOMPARE(detuning->getTension(), 0.75f);
		QCOMPARE(detuning->getTimeMap().value(13).getInTangent(), 0.125f);
		QCOMPARE(detuning->valueAt(14), 12.f);
		const auto id = static_cast<int>(instrument->volumeModel()->id()); // IDs belong to the restored project.
		const auto global = StudioCapabilities::execute("automation.global.inspect", {{"parameter", id}, {"position", 13}});
		QCOMPARE(global.value("result").toObject().value("value").toDouble(), 90.0);
		QVERIFY(studio.process(request("clear", "note.detuning.clear", {{"track", 0}, {"clip", 0}, {"note", 0}})).value("ok").toBool());
		QVERIFY(studio.process(request("undo", "session.undo")).value("ok").toBool());
		QVERIFY(static_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->notes().front()->hasDetuningInfo());
		QVERIFY(studio.process(request("redo", "session.redo")).value("ok").toBool());
		QVERIFY(!static_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->notes().front()->hasDetuningInfo());
	}

	void emptyAutomationBindingsSurviveSaveAndProjectClear()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path()); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("instrument", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(studio.process(request("track", "track.add", {{"type", "automation"}})).value("ok").toBool());
			QVERIFY(studio.process(request("curve", "clip.add", {{"track", 1}, {"position", 0}})).value("ok").toBool());
			auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
			QVERIFY(studio.process(request("bind", "automation.connect", {{"track", 1}, {"clip", 0},
				{"parameter", static_cast<int>(instrument->volumeModel()->id())}})).value("ok").toBool());
			QVERIFY(studio.process(request("clear", "automation.clear", {{"track", 1}, {"clip", 0}})).value("ok").toBool());
			QVERIFY(!instrument->volumeModel()->isAutomated());
			const auto pitch = static_cast<int>(instrument->pitchModel()->id());
			QVERIFY(studio.process(request("global", "automation.global.point", {{"parameter", pitch},
				{"position", 0}, {"value", 100}})).value("ok").toBool());
			QVERIFY(studio.process(request("clear-global", "automation.global.clear", {{"parameter", pitch}})).value("ok").toBool());
			QVERIFY(!instrument->pitchModel()->isAutomated());
		}
		Engine::getSong()->clearProject();
		{
			StudioRuntime recovered(directory.path()); QString error;
			QVERIFY2(recovered.open(error), qPrintable(error));
			auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
			auto curve = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClip(0));
			QVERIFY(curve->getTimeMap().isEmpty());
			QCOMPARE(curve->objects().size(), std::size_t(1));
			QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(instrument->volumeModel()));
			const auto global = StudioCapabilities::execute("automation.global.inspect", {{"parameter", static_cast<int>(instrument->pitchModel()->id())}});
			QVERIFY(global.value("ok").toBool());
			QVERIFY(global.value("result").toObject().value("points").toArray().isEmpty());
			QCOMPARE(global.value("result").toObject().value("targets").toArray().size(), 1);
		}
		Engine::getSong()->clearProject();
		QCOMPARE(Engine::getSong()->globalAutomationTrack()->numOfClips(), 3);
	}

	void agentObservationsBoundCurveDetailsWithoutEditingTheProject()
	{
		QJsonArray points;
		for (int i = 0; i < 100; ++i) { points.append(QJsonObject{{"position", i}, {"value", i}}); }
		const QJsonObject curve{{"points", points}, {"progression", 2}, {"tension", 0.5},
			{"targets", QJsonArray{QJsonObject{{"parameter", 1}}}}};
		const QJsonObject project{{"globalAutomations", QJsonArray{curve}}, {"tracks", QJsonArray{QJsonObject{
			{"clips", QJsonArray{QJsonObject{{"automation", curve}}}}}}}};
		const auto observation = StudioAgent::observation(project, {}, {}, false).value("content").toString();
		const auto payload = QJsonDocument::fromJson(observation.mid(observation.indexOf('\n') + 1).toUtf8()).object();
		const auto summary = payload.value("projectSummary").toObject();
		const auto global = summary.value("globalAutomations").toArray().at(0).toObject();
		QCOMPARE(global.value("pointCount").toInt(), 100);
		QVERIFY(!global.contains("points"));
		QCOMPARE(global.value("progression").toInt(), 2);
		QCOMPARE(global.value("targets"), curve.value("targets"));
		QVERIFY(!summary.value("tracks").toArray().at(0).toObject().value("clips").toArray().at(0).toObject()
			.value("automation").toObject().contains("points"));
		QCOMPARE(project.value("globalAutomations").toArray().at(0).toObject().value("points").toArray().size(), 100);
	}

	void nativeProgramSelectionDefaultsReportUnsupported()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		DummyInstrument native(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)));
		QString error;
		QVERIFY(native.programs().empty());
		QVERIFY(!native.selectProgram(0, 0, error)); QVERIFY(!error.isEmpty());
	}

	void nativeInstrumentDiscoveryReachesDetachedPluginModels()
	{
		if (getPluginFactory()->pluginInfo("tripleoscillator").isNull()) { QSKIP("This unit build has no TripleOscillator; full process verification still requires it"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		const auto native = track->instrument();
		QVERIFY(!native->parent());
		const auto nativeModels = native->findChildren<AutomatableModel*>();
		QVERIFY(!nativeModels.empty());
		const auto inspection = test::executeWithAllParameters("instrument.inspect", {{"track", 0}}).value("result").toObject().value("parameters").toArray();
		for (const auto model : nativeModels)
		{
			const auto item = parameterMetadata(inspection, model);
			QVERIFY(!item.isEmpty());
			QCOMPARE(item.value("location").toObject().value("track").toInt(-1), 0);
			QCOMPARE(item.value("location").toObject().value("container").toString(), QString("song"));
			QVERIFY(StudioCapabilities::execute("parameter.inspect", {{"parameter", static_cast<int>(model->id())}}).value("ok").toBool());
		}
		const auto listed = test::executeWithAllParameters("parameter.list", {{"track", 0}}).value("result").toObject().value("parameters").toArray();
		QVERIFY(listed.size() >= nativeModels.size());
	}

	void nativeSoundFontLoadingProgramsAndFailuresUseTheNativeLibrary()
	{
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto path = directory.filePath(QString::fromUtf8("\xE9\x9F\xB3") + ".sf2");
		QVERIFY(QFile::copy(QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", path));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		track->setName("Keep my track name");
		QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", path}}).value("ok").toBool());
		QCOMPARE(track->name(), QString("Keep my track name"));
		const auto native = track->instrument();
		QCOMPARE(native->programs().size(), std::size_t(2));
		const auto page = StudioCapabilities::execute("instrument.program.list", {{"track", 0}, {"limit", 1}}).value("result").toObject();
		QCOMPARE(page.value("total").toInt(), 2); QVERIFY(page.value("hasMore").toBool());
		QCOMPARE(page.value("programs").toArray().at(0).toObject().value("name").toString(), QString("Native base"));
		const auto filter = StudioCapabilities::execute("instrument.program.list", {{"track", 0}, {"bank", 1024}, {"query", "OCTAVE"}}).value("result").toObject();
		QCOMPARE(filter.value("total").toInt(), 1);
		const auto bank = native->childModel("bank"), patch = native->childModel("patch");
		QVERIFY(bank && patch);
		QVERIFY(!StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(bank->id())}, {"value", 1024}}).value("ok").toBool());
		QCOMPARE(native->midiPatch()->bank, std::uint16_t(0));
		QVERIFY(StudioCapabilities::execute("instrument.program.select", {{"track", 0}, {"bank", 1024}, {"program", 7}}).value("ok").toBool());
		QCOMPARE(native->midiPatch()->bank, std::uint16_t(1024)); QCOMPARE(native->midiPatch()->program, std::uint8_t(7));
		QCOMPARE(patch->valueLabels().value(7), QString("Native octave"));
		QVERIFY(!StudioCapabilities::execute("instrument.program.select", {{"track", 0}, {"bank", 1024}, {"program", 8}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(patch->id())}, {"value", 8}}).value("ok").toBool());
		const auto bad = directory.filePath("invalid.sf2"); QVERIFY(writeTuningText(bad, "Not a SoundFont"));
		const auto oldPath = native->fileResources().front().path;
		QVERIFY(!StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", bad}}).value("ok").toBool());
		QCOMPARE(native->fileResources().front().path, oldPath);
		QCOMPARE(native->programs().size(), std::size_t(2)); QCOMPARE(native->midiPatch()->program, std::uint8_t(7));
		QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", ""}}).value("ok").toBool());
		QVERIFY(native->fileResources().front().path.isEmpty()); QVERIFY(native->programs().empty());
	}

	void nativeSoundFontHeldVoicesResumeAfterReloadWithoutTheSourceFile()
	{
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto bank = directory.filePath("loaded.sf2");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", bank));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		const auto native = track->instrument(); QString error;
		QVERIFY(native->setFileResource("soundfont", bank, error));
		QVERIFY(native->selectProgram(1024, 7, error));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		NotePlayHandle held(track, 0, frames * 100, Note(TimePos(48), TimePos(), 69));
		std::vector<SampleFrame> output(frames);
		held.play(output.data());
		const auto initial = measureNativeSoundFont(native, output);
		QVERIFY(initial[0] > .0001 && std::abs(initial[2] - 880) < 6);
		QVERIFY(QFile::remove(bank));
		for (int reload = 0; reload < 3; ++reload)
		{
			QVERIFY(QMetaObject::invokeMethod(native, "reloadSynth", Qt::DirectConnection));
			held.play(output.data());
			const auto restored = measureNativeSoundFont(native, output);
			QVERIFY(std::isfinite(restored[0]) && restored[0] > initial[0] / 2);
			QVERIFY(std::abs(restored[2] - 880) < 6);
			QCOMPARE(native->programs().size(), std::size_t(2));
			QCOMPARE(native->midiPatch()->bank, std::uint16_t(1024));
			QCOMPARE(native->midiPatch()->program, std::uint8_t(7));
		}
	}

	void nativeSoundFontOldNoteDeletionPreservesNewSameKeyVoices_data()
	{
		QTest::addColumn<bool>("rebuild");
		QTest::newRow("synth-rebuild") << true;
		QTest::newRow("bank-clear-and-reload") << false;
	}
	void nativeSoundFontOldNoteDeletionPreservesNewSameKeyVoices()
	{
		QFETCH(bool, rebuild);
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full process verification still requires it"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		const auto native = track->instrument(); QString error;
		const auto path = QString(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2";
		QVERIFY(native->setFileResource("soundfont", path, error));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		const Note note(TimePos(48), TimePos(), 69);
		std::optional<NotePlayHandle> old(std::in_place, track, 0, frames * 100, note);
		std::vector<SampleFrame> output(frames);
		old->play(output.data()); QVERIFY(measureNativeSoundFont(native, output)[0] > .0001);
		if (rebuild) { QVERIFY(QMetaObject::invokeMethod(native, "reloadSynth", Qt::DirectConnection)); }
		else
		{
			QVERIFY(native->setFileResource("soundfont", "", error));
			QVERIFY(native->setFileResource("soundfont", path, error));
		}
		NotePlayHandle fresh(track, 0, frames * 100, note);
		fresh.play(output.data());
		const auto initial = measureNativeSoundFont(native, output);
		QVERIFY(initial[0] > .0001);
		old.reset();
		const auto preserved = measureNativeSoundFont(native, output);
		QVERIFY(std::isfinite(preserved[0]) && preserved[0] > initial[0] / 2);
		QVERIFY(std::abs(preserved[2] - 440) < 4);
		native->deleteNotePluginData(&fresh);
		native->deleteNotePluginData(&fresh);
		for (int block = 0; block < 24; ++block) { native->play(output.data()); }
		QVERIFY(measureNativeSoundFont(native, output)[0] < .00001);
	}

	void nativeSoundFontAllLayeredVoicesReceiveInitialDetuningAndLivePanning()
	{
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full process verification still requires it"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		const auto native = track->instrument(); QString error;
		QVERIFY(native->setFileResource("soundfont", QString(LMMS_TEST_FIXTURE_DIR) + "/native-layered.sf2", error));
		Note note(TimePos(48), TimePos(), 69);
		note.createDetuning(); note.detuning()->automationClip()->putValue(0, 12);
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		NotePlayHandle held(track, 0, frames * 100, note);
		std::vector<SampleFrame> output(frames);
		held.play(output.data());
		const auto centered = measureNativeSoundFont(native, output);
		QVERIFY(centered[0] > .0001 && centered[1] > .0001);
		QVERIFY(std::abs(centered[2] - 880) < 6);
		held.setPanning(PanningRight); held.play(output.data());
		const auto right = measureNativeSoundFont(native, output);
		QVERIFY(right[1] > centered[1] / 2 && right[0] < right[1] / 100);
		held.detuning()->automationClip()->putValue(0, 24); held.processTimePos(TimePos());
		held.setPanning(PanningLeft); held.play(output.data());
		const auto left = measureNativeSoundFont(native, output);
		QVERIFY(left[0] > .0001 && left[1] < left[0] / 100);
		QVERIFY(std::abs(left[2] - 1760) < 10);
	}

	void nativeSoundFontProgramRecoveryUndoAndPresetRoundTrips()
	{
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto bank = directory.filePath("native.sf2"), preset = directory.filePath("native.xpf");
		QVERIFY(QFile::copy(QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2", bank));
		{
			StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("plugin", "instrument.load", {{"track", 0}, {"instrument", "sf2player"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", bank}})).value("ok").toBool());
			QVERIFY(runtime.process(request("program", "instrument.program.select", {{"track", 0}, {"bank", 1024}, {"program", 7}})).value("ok").toBool());
			QVERIFY(runtime.process(request("preset", "preset.save", {{"track", 0}, {"path", preset}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.filePath("session")); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto current = [] { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); };
		QCOMPARE(current()->programs().size(), std::size_t(2)); QCOMPARE(current()->midiPatch()->bank, std::uint16_t(1024));
		QVERIFY(recovered.process(request("undo", "session.undo")).value("ok").toBool());
		QCOMPARE(current()->midiPatch()->program, std::uint8_t(0));
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool());
		QCOMPARE(current()->midiPatch()->program, std::uint8_t(7));
		QVERIFY(recovered.process(request("clear", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", ""}})).value("ok").toBool());
		QVERIFY(current()->programs().empty());
		QVERIFY(recovered.process(request("preset-restore", "preset.load", {{"track", 0}, {"path", preset}})).value("ok").toBool());
		QCOMPARE(current()->programs().size(), std::size_t(2)); QCOMPARE(current()->midiPatch()->program, std::uint8_t(7));
	}

	void nativeGigLoadingReportsFailureAndClearsWithoutAnEditor()
	{
		if (getPluginFactory()->pluginInfo("gigplayer").isNull()) { QSKIP("This unit build has no GIG player; the compatibility matrix still requires it"); }
		QTemporaryDir directory;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "gigplayer"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
		QCOMPARE(native->fileResources().front().key, QString("bank"));
		const auto bad = directory.filePath("invalid.gig"); QVERIFY(writeTuningText(bad, "Not a GIG bank"));
		QVERIFY(!StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", bad}}).value("ok").toBool());
		QVERIFY(native->fileResources().front().path.isEmpty()); QVERIFY(native->programs().empty());
		QVERIFY(!StudioCapabilities::execute("instrument.program.select", {{"track", 0}, {"bank", 0}, {"program", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", ""}}).value("ok").toBool());
		QVERIFY(native->programs().empty());
	}

	void nativeGigBundlesRetainPlayableGroupsAfterOriginalDeletion_data()
	{
		QTest::addColumn<bool>("split");
		QTest::addColumn<int>("bitDepth"); QTest::addColumn<int>("channels");
		for (const auto bitDepth : {16, 24})
		{
			for (const auto channels : {1, 2})
			{
				const auto name = QString::number(bitDepth) + (channels == 1 ? "-mono" : "-stereo");
				QTest::newRow(qPrintable("single-" + name)) << false << bitDepth << channels;
				QTest::newRow(qPrintable("split-" + name)) << true << bitDepth << channels;
			}
		}
	}

	void nativeGigBundlesRetainPlayableGroupsAfterOriginalDeletion()
	{
#ifndef LMMS_TEST_LIBGIG
		QSKIP("Playable GIG verification requires a libgig-enabled build");
#else
		QFETCH(bool, split); QFETCH(int, bitDepth); QFETCH(int, channels);
		if (getPluginFactory()->pluginInfo("gigplayer").isNull()) { QSKIP("Playable GIG verification requires the built native plugin"); }
		QTemporaryDir directory; QVERIFY(directory.isValid()); QString error;
		const auto source = directory.filePath("playable.gig");
		QVERIFY2(test::gigNative::create(source, split, error, bitDepth, channels), qPrintable(error));
		auto expected = FileResourceSnapshot::captureGig(source, error); QVERIFY2(expected, qPrintable(error));
		QCOMPARE(expected->members().size(), split ? 2 : 1);
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}, {"name", "Playable native bank"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "gigplayer"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.edit", {{"track", 0}, {"name", "Playable native bank"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", source}}).value("ok").toBool());
		for (const auto& member : expected->members()) { QVERIFY(QFile::remove(directory.filePath(member.name))); }
		const auto originalPcm = meanNativeGig(0); QVERIFY(originalPcm[0] > .01);
		if (channels == 1) { QCOMPARE(originalPcm[0], originalPcm[1]); }
		else { QVERIFY(originalPcm[1] < -.01); QVERIFY(std::abs(originalPcm[1] + originalPcm[0] * 2) < 1e-6); }
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("portable.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString(); DataFile packaged(path);
		const auto resource = packaged.elementsByTagName("gigplayer").item(0).toElement().attribute("src");
		QCOMPARE(resource, "local:resources/" + expected->groupDigest() + "/playable.gig");
		QVERIFY(expected->verifyCopy(QFileInfo(path).absoluteDir().filePath(resource.mid(6)), error));
		QVERIFY(StudioCapabilities::execute("project.load", {{"path", path}}).value("ok").toBool());
		const auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QCOMPARE(restored->name(), QString("Playable native bank"));
		QCOMPARE(restored->instrument()->midiPatch()->bank, std::uint16_t(2));
		QCOMPARE(restored->instrument()->midiPatch()->program, std::uint8_t(7));
		const auto restoredPcm = meanNativeGig(0);
		QVERIFY(std::abs(restoredPcm[0] - originalPcm[0]) < 1e-6); QVERIFY(std::abs(restoredPcm[1] - originalPcm[1]) < 1e-6);
#endif
	}

	void nativeGigClonesKeepSeparateAcceptedCompanionVersions_data()
	{
		QTest::addColumn<bool>("remove"); QTest::newRow("removed-originals") << true; QTest::newRow("changed-companion") << false;
	}
	void nativeGigClonesKeepSeparateAcceptedCompanionVersions()
	{
#ifndef LMMS_TEST_LIBGIG
		QSKIP("Playable GIG cloning verification requires a libgig-enabled build");
#else
		QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo("gigplayer").isNull()) { QSKIP("Playable GIG cloning requires the native plugin"); }
		QTemporaryDir directory; QString error; const auto source = directory.filePath("same.gig"), pool = directory.filePath("same.gx01");
		QVERIFY2(test::gigNative::create(source, true, error, 16, 2), qPrintable(error));
		std::array<std::shared_ptr<FileResourceSnapshot>, 2> accepted; std::array<Track*, 2> originals{};
		for (int index = 0; index < 2; ++index)
		{
			if (index) { QVERIFY(test::gigNative::invertCompanion(pool)); }
			accepted[index] = FileResourceSnapshot::captureGig(source, error); QVERIFY(accepted[index]);
			QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
			QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", index}, {"instrument", "gigplayer"}}).value("ok").toBool());
			QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", index}, {"resource", "bank"}, {"path", source}}).value("ok").toBool());
			originals[index] = Engine::getSong()->tracks().at(index);
		}
		QCOMPARE(accepted[0]->sha256(), accepted[1]->sha256()); QVERIFY(accepted[0]->groupDigest() != accepted[1]->groupDigest());
		const std::array<std::array<double, 2>, 2> before{meanNativeGig(0), meanNativeGig(1)};
		QVERIFY(before[0][0] > .01 && before[1][0] < -.01);
		if (remove) { QVERIFY(QFile::remove(source)); QVERIFY(QFile::remove(pool)); }
		else { QVERIFY(test::gigNative::invertCompanion(pool)); }
		for (auto original : originals) { QVERIFY2(original->clone(), qPrintable(Engine::getSong()->errorSummary())); }
		delete originals[1]; delete originals[0];
		for (int index = 0; index < 2; ++index)
		{
			const auto output = meanNativeGig(index);
			for (int channel = 0; channel < 2; ++channel) { QVERIFY(std::abs(output[channel] - before[index][channel]) < 1e-6); }
		}
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("gig-clones.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString(); DataFile document(path); const auto nodes = document.elementsByTagName("gigplayer"); QCOMPARE(nodes.size(), 2);
		for (int index = 0; index < 2; ++index)
		{
			const auto reference = nodes.at(index).toElement().attribute("src"); QVERIFY(reference.startsWith("local:resources/"));
			QVERIFY(accepted[index]->verifyCopy(QFileInfo(path).absoluteDir().filePath(reference.mid(6)), error));
		}
		QVERIFY(Engine::getSong()->loadProject(path)); QVERIFY(!Engine::getSong()->hasErrors());
		for (int index = 0; index < 2; ++index)
		{
			const auto output = meanNativeGig(index);
			for (int channel = 0; channel < 2; ++channel) { QVERIFY(std::abs(output[channel] - before[index][channel]) < 1e-6); }
		}
#endif
	}

	void nativeGigCheckpointVersionsSurviveUndoRestartAndRelocation_data()
	{
		QTest::addColumn<bool>("remove");
		QTest::newRow("removed-originals") << true;
		QTest::newRow("replaced-originals") << false;
	}

	void nativeGigCheckpointVersionsSurviveUndoRestartAndRelocation()
	{
#ifndef LMMS_TEST_LIBGIG
		QSKIP("Playable GIG recovery verification requires a libgig-enabled build");
#else
		QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo("gigplayer").isNull()) { QSKIP("Playable GIG recovery requires the built native plugin"); }
		QTemporaryDir directory; QVERIFY(directory.isValid()); QString error;
		const auto source = directory.filePath("shared.gig"), pool = directory.filePath("shared.gx01");
		QVERIFY2(test::gigNative::create(source, true, error), qPrintable(error));
		const auto rootBytes = test::gigNative::read(source); auto session = directory.filePath("session");
		{
			StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track-a", "track.add", {{"type", "instrument"}, {"name", "Accepted positive pool"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("player-a", "instrument.load", {{"track", 0}, {"instrument", "gigplayer"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("bank-a", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", source}})).value("ok").toBool());
			// Simulate an external PCM edit which leaves root bytes/IDs unchanged.
			// The optional libgig checksum table is not used as acceptance evidence.
			QVERIFY(test::gigNative::invertCompanion(pool)); QCOMPARE(test::gigNative::read(source), rootBytes);
			QVERIFY(runtime.process(request("track-b", "track.add", {{"type", "instrument"}, {"name", "Accepted negative pool"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("player-b", "instrument.load", {{"track", 1}, {"instrument", "gigplayer"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("bank-b", "instrument.resource.set", {{"track", 1}, {"resource", "bank"}, {"path", source}})).value("ok").toBool());
			if (remove) { QVERIFY(QFile::remove(source)); QVERIFY(QFile::remove(pool)); }
			else { QVERIFY(test::gigNative::write(source, "Replaced root")); QVERIFY(test::gigNative::write(pool, "Replaced pool")); }
			QVERIFY(runtime.process(request("checkpoint", "song.set", {{"tempo", 89}})).value("ok").toBool());
			const auto references = manifest(session).value("history").toArray().last().toObject().value("assets").toArray();
			QCOMPARE(references.size(), 2);
			QCOMPARE(references.at(0).toObject().value("sha256"), references.at(1).toObject().value("sha256"));
			QVERIFY(references.at(0).toObject().value("path") != references.at(1).toObject().value("path"));
			QVERIFY(StudioAssetStore::verify(QDir(session).filePath("checkpoints"), references, error));
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);
		}
		Engine::getSong()->clearProject(); const auto relocated = directory.filePath("relocated-session");
		QVERIFY(QDir().rename(session, relocated)); session = relocated;
		StudioRuntime recovered(session); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 89); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(2));
		const auto positive = meanNativeGig(0), negative = meanNativeGig(1);
		QVERIFY(positive[0] > .01 && negative[0] < -.01);
		QVERIFY(std::abs(positive[0] + negative[0]) < 1e-6); QCOMPARE(positive[0], positive[1]); QCOMPARE(negative[0], negative[1]);
#endif
	}

	void nativeGigMissingReplacementCompanionPreservesPlayableCheckpoint()
	{
#ifndef LMMS_TEST_LIBGIG
		QSKIP("Playable GIG failure recovery requires a libgig-enabled build");
#else
		if (getPluginFactory()->pluginInfo("gigplayer").isNull()) { QSKIP("Playable GIG failure recovery requires the built native plugin"); }
		QTemporaryDir directory; QVERIFY(directory.isValid()); QString error;
		const auto source = directory.filePath("valid.gig"), missing = directory.filePath("missing.gig"), session = directory.filePath("session");
		QVERIFY2(test::gigNative::create(source, true, error), qPrintable(error));
		QVERIFY(test::gigNative::write(missing, test::gigResource::root({"unavailable.gx01"})));
		StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}, {"name", "Keep the native bank"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("player", "instrument.load", {{"track", 0}, {"instrument", "gigplayer"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("name", "track.edit", {{"track", 0}, {"name", "Keep the native bank"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", source}})).value("ok").toBool());
		const auto before = manifest(session);
		QVERIFY(QFile::remove(source)); QVERIFY(QFile::remove(directory.filePath("valid.gx01")));
		QVERIFY(!runtime.process(request("missing", "instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", missing}})).value("ok").toBool());
		const auto after = manifest(session); QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1); QCOMPARE(after.value("history"), before.value("history"));
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QCOMPARE(track->name(), QString("Keep the native bank")); QCOMPARE(track->instrument()->midiPatch()->bank, std::uint16_t(2));
		QCOMPARE(track->instrument()->midiPatch()->program, std::uint8_t(7)); QVERIFY(meanNativeGig(0)[0] > .01);
		const auto references = after.value("history").toArray().last().toObject().value("assets").toArray();
		QVERIFY(StudioAssetStore::verify(QDir(session).filePath("checkpoints"), references, error));
#endif
	}

	void nativePatmanCheckedLoadingUsesAllFourPcmEncodings()
	{
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("This unit build has no Patman; full process verification still requires it"); }
		QTemporaryDir directory;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "patman"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		track->setName("Keep my GUS instrument");
		const auto native = track->instrument();
		QCOMPARE(native->fileResources().front().key, QString("patch"));
		for (const auto encoding : {"s8", "u8", "s16", "u16"})
		{
			const auto bytes = nativeFixture(QString("native-gus-%1.pat").arg(encoding)); QVERIFY(!bytes.isEmpty());
			const auto path = directory.filePath(QString(QChar(0x97f3)) + "-" + encoding + ".pat");
			QVERIFY(writeFixtureBytes(path, bytes));
			QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "patch"}, {"path", path}}).value("ok").toBool());
			QCOMPARE(track->name(), QString("Keep my GUS instrument"));
			const auto frames = Engine::audioEngine()->framesPerPeriod();
			Note note(TimePos(48), TimePos(0), 69);
			NotePlayHandle voice(track, 0, frames * 4, note);
			std::vector<SampleFrame> output(frames);
			voice.play(output.data());
			QVERIFY(getAbsPeakValues(output.data(), frames).left() > 0.01f);
			QVERIFY(std::all_of(output.begin(), output.end(), [](const SampleFrame& frame) { return !frame.containsNaN() && !frame.containsInf(); }));
		}
		const auto parameters = test::executeWithAllParameters("instrument.inspect", {{"track", 0}}).value("result").toObject().value("parameters").toArray();
		bool loop = false, tune = false;
		for (const auto item : parameters)
		{
			loop |= item.toObject().value("name").toString().endsWith("Loop sample");
			tune |= item.toObject().value("name").toString().endsWith("Tune sample");
		}
		QVERIFY(loop && tune);
	}

	void nativePatmanRejectsIncompleteAndInvalidSamplesBeforePublication()
	{
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("This unit build has no Patman; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto good = directory.filePath("good.pat"), bad = directory.filePath("bad.pat");
		const auto source = nativeFixture("native-gus-s16.pat"); QVERIFY(source.size() > 335);
		QVERIFY(writeFixtureBytes(good, source));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "patman"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
		QString error; QVERIFY2(native->setFileResource("patch", good, error), qPrintable(error));
		const auto previous = native->fileResources().front().path;
		const auto reject = [&](const QByteArray& bytes) {
			if (!writeFixtureBytes(bad, bytes)) { return false; }
			error.clear();
			return !native->setFileResource("patch", bad, error) && !error.isEmpty() && native->fileResources().front().path == previous;
		};
		QVERIFY(reject(source.left(238))); QVERIFY(reject(source.left(334))); QVERIFY(reject(source.left(source.size() - 1)));
		auto mutated = source; mutated[198] = 0; QVERIFY(reject(mutated));
		mutated = source; mutated[82] = 2; QVERIFY(reject(mutated));
		mutated = source; mutated[151] = 2; QVERIFY(reject(mutated));
		mutated = source; qToLittleEndian<quint32>(0xffffffffu, mutated.data() + 247); QVERIFY(reject(mutated));
		mutated = source; qToLittleEndian<quint32>(1, mutated.data() + 247); QVERIFY(reject(mutated));
		mutated = source; qToLittleEndian<quint16>(0, mutated.data() + 259); QVERIFY(reject(mutated));
		mutated = source; qToLittleEndian<quint32>(0, mutated.data() + 269); QVERIFY(reject(mutated));
		mutated = source; qToLittleEndian<quint32>(0xffffffffu, mutated.data() + 255); QVERIFY(reject(mutated));
		// A later sample failure must not publish the successfully decoded first sample.
		const auto secondHeader = 335 + static_cast<int>(qFromLittleEndian<quint32>(source.constData() + 247));
		mutated = source; qToLittleEndian<quint32>(0, mutated.data() + secondHeader + 30); QVERIFY(reject(mutated));
		QVERIFY(!native->setFileResource("wrong-key", good, error));
		QCOMPARE(native->fileResources().front().path, previous);
		QVERIFY(!native->setFileResource("patch", directory.filePath("missing.pat"), error));
		QCOMPARE(native->fileResources().front().path, previous);
		QVERIFY(native->setFileResource("patch", "", error)); QVERIFY(native->fileResources().front().path.isEmpty());
	}

	void nativePatmanActiveNotesRetainTheirNativeSampleOnPatchReplacement()
	{
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("This unit build has no Patman; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto original = directory.filePath("original.pat"), replacement = directory.filePath("replacement.pat");
		auto bytes = nativeFixture("native-gus-s16.pat"); QVERIFY(bytes.size() > 335);
		QVERIFY(writeFixtureBytes(original, bytes));
		const auto firstBytes = static_cast<int>(qFromLittleEndian<quint32>(bytes.constData() + 247));
		std::fill_n(bytes.data() + 335, firstBytes, char(0));
		QVERIFY(writeFixtureBytes(replacement, bytes));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "patman"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QString error; QVERIFY(track->instrument()->setFileResource("patch", original, error));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		Note note(TimePos(48), TimePos(0), 69);
		NotePlayHandle held(track, 0, frames * 8, note);
		std::vector<SampleFrame> output(frames);
		held.play(output.data()); QVERIFY(getAbsPeakValues(output.data(), frames).left() > 0.01f);
		QVERIFY(track->instrument()->setFileResource("patch", replacement, error));
		zeroSampleFrames(output.data(), frames);
		held.play(output.data()); QVERIFY(getAbsPeakValues(output.data(), frames).left() > 0.01f);
		NotePlayHandle fresh(track, 0, frames * 4, note);
		zeroSampleFrames(output.data(), frames); fresh.play(output.data());
		QCOMPARE(getAbsPeakValues(output.data(), frames).left(), 0.f);
	}

	void nativePatmanBundlesKeepDifferentAcceptedVersionsOfOneSource_data()
	{
		QTest::addColumn<bool>("remove");
		QTest::newRow("removed-source") << true;
		QTest::newRow("replaced-source") << false;
	}

	void nativePatmanBundlesKeepDifferentAcceptedVersionsOfOneSource()
	{
		QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("Full verification requires native Patman"); }
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + "-shared.pat");
		const auto original = nativeFixture("native-gus-s16.pat"); QVERIFY(!original.isEmpty());
		const auto silenced = silencedFirstGusSample(original); QVERIFY(!silenced.isEmpty()); QVERIFY(silenced != original);
		QVERIFY(writeFixtureBytes(source, original)); QVERIFY(addNativeSampleConsumer("patman", source));
		Engine::getSong()->tracks().at(0)->setName("Accepted sounding patch");
		QVERIFY(writeFixtureBytes(source, silenced)); QVERIFY(addNativeSampleConsumer("patman", source));
		Engine::getSong()->tracks().at(1)->setName("Accepted silenced patch");
		std::array<std::vector<SampleFrame>, 2> before{nativePatmanPeriod(0), nativePatmanPeriod(1)};
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		QVERIFY(getAbsPeakValues(before[0].data(), frames).left() > .01f);
		QCOMPARE(getAbsPeakValues(before[1].data(), frames).left(), 0.f);
		if (remove) { QVERIFY(QFile::remove(source)); }
		else { QVERIFY(writeFixtureBytes(source, "Original replaced after two native patch loads")); }
		const auto second = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(1))->instrument(); QString error;
		const auto pathBeforeFailure = second->fileResources().front().path;
		QVERIFY(!second->setFileResource("patch", source, error)); QVERIFY(!error.isEmpty());
		QCOMPARE(second->fileResources().front().path, pathBeforeFailure);
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("portable.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString(); DataFile packaged(path);
		const auto nodes = packaged.elementsByTagName("patman"); QCOMPARE(nodes.size(), 2);
		QString previousReference;
		for (int index = 0; index < 2; ++index)
		{
			const auto reference = nodes.item(index).toElement().attribute("src");
			QVERIFY(reference.startsWith("local:resources/")); QVERIFY(reference != previousReference); previousReference = reference;
			QCOMPARE(readFixtureBytes(QFileInfo(path).absoluteDir().filePath(reference.mid(6))), index == 0 ? original : silenced);
		}
		QVERIFY(StudioCapabilities::execute("project.load", {{"path", path}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Accepted sounding patch"));
		QCOMPARE(Engine::getSong()->tracks().at(1)->name(), QString("Accepted silenced patch"));
		for (int index = 0; index < 2; ++index)
		{
			const auto restored = nativePatmanPeriod(index); QCOMPARE(restored.size(), before[index].size());
			for (std::size_t frame = 0; frame < restored.size(); ++frame)
			{
				QCOMPARE(restored[frame][0], before[index][frame][0]); QCOMPARE(restored[frame][1], before[index][frame][1]);
			}
		}
	}

	void nativePatmanCheckpointsRetainAcceptedVersionsAcrossRecovery_data()
	{
		QTest::addColumn<bool>("remove");
		QTest::newRow("removed-source") << true;
		QTest::newRow("replaced-source") << false;
	}

	void nativePatmanCheckpointsRetainAcceptedVersionsAcrossRecovery()
	{
		QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("Full verification requires native Patman"); }
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("shared.pat"); auto session = directory.filePath("session");
		const auto original = nativeFixture("native-gus-s16.pat"); const auto silenced = silencedFirstGusSample(original);
		QVERIFY(!original.isEmpty() && !silenced.isEmpty());
		std::array<std::vector<SampleFrame>, 2> before;
		{
			StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(writeFixtureBytes(source, original)); QVERIFY(addNativeSampleConsumer("patman", source));
			QVERIFY(writeFixtureBytes(source, silenced)); QVERIFY(addNativeSampleConsumer("patman", source));
			before = {nativePatmanPeriod(0), nativePatmanPeriod(1)};
			const auto frames = Engine::audioEngine()->framesPerPeriod();
			QVERIFY(getAbsPeakValues(before[0].data(), frames).left() > .01f);
			QCOMPARE(getAbsPeakValues(before[1].data(), frames).left(), 0.f);
			if (remove) { QVERIFY(QFile::remove(source)); }
			else { QVERIFY(writeFixtureBytes(source, "Original replaced before any patch checkpoint")); }
			QVERIFY(runtime.process(request("checkpoint", "song.set", {{"tempo", 89}})).value("ok").toBool());
			const auto snapshot = manifest(session).value("history").toArray().last().toObject();
			const auto references = snapshot.value("assets").toArray(); QCOMPARE(references.size(), 2);
			QVERIFY(StudioAssetStore::verify(QDir(session).filePath("checkpoints"), references, error));
			for (int index = 0; index < 2; ++index)
			{
				const auto expected = index == 0 ? original : silenced;
				DataFile document(QDir(session).filePath(snapshot.value("path").toString()));
				const auto hash = QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex());
				QCOMPARE(document.elementsByTagName("patman").item(index).toElement().attribute("src"), "local:assets/" + hash + ".pat");
			}
			QVERIFY(runtime.process(request("tempo", "song.set", {{"tempo", 90}})).value("ok").toBool());
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Files).size(), 2);
		}
		Engine::getSong()->clearProject(); const auto relocated = directory.filePath("relocated");
		QVERIFY(QDir().rename(session, relocated)); session = relocated;
		StudioRuntime recovered(session); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 90); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(2));
		for (int index = 0; index < 2; ++index)
		{
			const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(index))->instrument();
			QVERIFY(ownsCheckpointResource(session, native->fileResources().front().path, index == 0 ? original : silenced, "pat"));
			const auto restored = nativePatmanPeriod(index); QCOMPARE(restored.size(), before[index].size());
			for (std::size_t frame = 0; frame < restored.size(); ++frame)
			{
				QCOMPARE(restored[frame][0], before[index][frame][0]); QCOMPARE(restored[frame][1], before[index][frame][1]);
			}
		}
	}

	void nativePatmanResourcesSurviveRecoveryUndoAndNativePresets()
	{
		if (getPluginFactory()->pluginInfo("patman").isNull()) { QSKIP("This unit build has no Patman; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto first = directory.filePath("first.pat"), second = directory.filePath("second.pat"), preset = directory.filePath("native.xpf");
		QVERIFY(writeFixtureBytes(first, nativeFixture("native-gus-s8.pat")));
		QVERIFY(writeFixtureBytes(second, nativeFixture("native-gus-u16.pat")));
		{
			StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("plugin", "instrument.load", {{"track", 0}, {"instrument", "patman"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("first", "instrument.resource.set", {{"track", 0}, {"resource", "patch"}, {"path", first}})).value("ok").toBool());
			QVERIFY(runtime.process(request("preset", "preset.save", {{"track", 0}, {"path", preset}})).value("ok").toBool());
			QVERIFY(runtime.process(request("second", "instrument.resource.set", {{"track", 0}, {"resource", "patch"}, {"path", second}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.filePath("session")); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto path = [] { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument()->fileResources().front().path; };
		QVERIFY(ownsCheckpointResource(directory.filePath("session"), path(), nativeFixture("native-gus-u16.pat"), "pat"));
		QVERIFY(recovered.process(request("undo", "session.undo")).value("ok").toBool());
		QVERIFY(ownsCheckpointResource(directory.filePath("session"), path(), nativeFixture("native-gus-s8.pat"), "pat"));
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool());
		QVERIFY(ownsCheckpointResource(directory.filePath("session"), path(), nativeFixture("native-gus-u16.pat"), "pat"));
		QVERIFY(recovered.process(request("clear", "instrument.resource.set", {{"track", 0}, {"resource", "patch"}, {"path", ""}})).value("ok").toBool()); QVERIFY(path().isEmpty());
		QVERIFY(recovered.process(request("preset-restore", "preset.load", {{"track", 0}, {"path", preset}})).value("ok").toBool()); QVERIFY(path().endsWith("first.pat"));
		QDomDocument document; auto empty = document.createElement("patman"); document.appendChild(empty);
		NativeInstrumentTestAccess::load(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(), empty); QVERIFY(path().isEmpty());
	}

	void nativeOpulenzImportsSbiIntoNativeControlsAndLabels()
	{
		if (getPluginFactory()->pluginInfo("opulenz").isNull()) { QSKIP("This unit build has no OpulenZ; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath(QString(QChar(0x97f3)) + ".sbi");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-opl-bitfields.sbi", path));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "opulenz"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); track->setName("Keep my OPL name");
		const auto native = track->instrument(); QString error;
		QVERIFY2(native->setFileResource("patch", path, error), qPrintable(error));
		QCOMPARE(track->name(), QString("Keep my OPL name")); QVERIFY(!native->midiPatch());
		const auto saved = instrumentSettings(native);
		const QMap<QString, int> expected{{"op1_trem", 1}, {"op1_vib", 1}, {"op1_perc", 1}, {"op1_ksr", 1}, {"op1_mul", 5},
			{"op2_trem", 1}, {"op2_vib", 0}, {"op2_perc", 0}, {"op2_ksr", 0}, {"op2_mul", 2},
			{"op1_scale", 2}, {"op1_lvl", 22}, {"op2_scale", 1}, {"op2_lvl", 56},
			{"op1_a", 3}, {"op1_d", 12}, {"op2_a", 7}, {"op2_d", 4},
			{"op1_s", 5}, {"op1_r", 8}, {"op2_s", 9}, {"op2_r", 1},
			{"op1_waveform", 3}, {"op2_waveform", 2}, {"fm", 0}, {"feedback", 6}};
		for (auto item = expected.cbegin(); item != expected.cend(); ++item)
		{
			QVERIFY(saved.hasAttribute(item.key())); QCOMPARE(saved.attribute(item.key()).toInt(), item.value());
		}
		QCOMPARE(saved.attribute("patchSource"), path);
		const auto inspection = test::executeWithAllParameters("instrument.inspect", {{"track", 0}}).value("result").toObject();
		const auto resource = inspection.value("resources").toArray().at(0).toObject();
		QCOMPARE(resource.value("key").toString(), QString("patch")); QVERIFY(!resource.value("requiredForRestore").toBool(true));
		int selectors = 0, patchSelectors = 0;
		for (const auto item : inspection.value("parameters").toArray())
		{
			const auto model = item.toObject(); const auto name = model.value("name").toString();
			if (name.endsWith("Op 1 waveform") || name.endsWith("Op 2 waveform"))
			{
				QCOMPARE(model.value("choices").toArray().size(), 4); ++selectors;
			}
			if (name.endsWith("Patch"))
			{
				QCOMPARE(model.value("choices").toArray().size(), 128); ++patchSelectors;
			}
		}
		QCOMPARE(selectors, 2); QCOMPARE(patchSelectors, 1);
	}

	void nativeOpulenzRejectsMalformedSbiAndClearsFactoryState()
	{
		if (getPluginFactory()->pluginInfo("opulenz").isNull()) { QSKIP("This unit build has no OpulenZ; full process verification still requires it"); }
		QTemporaryDir directory;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "opulenz"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); QString error;
		const auto source = nativeFixture("native-opl-bitfields.sbi"); QCOMPARE(source.size(), 52);
		const auto path = directory.filePath("valid.sbi"), bad = directory.filePath("bad.sbi");
		QVERIFY(writeFixtureBytes(path, source)); QVERIFY(native->setFileResource("patch", path, error));
		const auto previous = instrumentSettings(native);
		for (int length : {0, 3, 4, 35, 36, 46})
		{
			QVERIFY(writeFixtureBytes(bad, source.left(length))); QVERIFY(!native->setFileResource("patch", bad, error)); QVERIFY(!error.isEmpty());
			QCOMPARE(native->fileResources().front().path, path); QCOMPARE(instrumentSettings(native).attribute("op2_mul"), previous.attribute("op2_mul"));
		}
		auto wrong = source; wrong[3] = '\0'; QVERIFY(writeFixtureBytes(bad, wrong)); QVERIFY(!native->setFileResource("patch", bad, error));
		QVERIFY(!native->setFileResource("other", path, error));
		QVERIFY(!native->setFileResource("patch", directory.filePath("absent.sbi"), error));
		QVERIFY(writeFixtureBytes(path, source.left(47))); QVERIFY(native->setFileResource("patch", path, error));
		QVERIFY(writeFixtureBytes(path, source + QByteArray("ignored trailing data"))); QVERIFY(native->setFileResource("patch", path, error));
		// XML changes native init values; clearing must still restore the constructor's defaults.
		NativeInstrumentTestAccess::load(native, previous); QVERIFY(native->setFileResource("patch", "", error));
		const auto cleared = instrumentSettings(native); QVERIFY(native->fileResources().front().path.isEmpty()); QVERIFY(!native->midiPatch());
		QCOMPARE(cleared.attribute("op1_a").toInt(), 14); QCOMPARE(cleared.attribute("op2_mul").toInt(), 1);
		QCOMPARE(cleared.attribute("op2_lvl").toInt(), 63); QCOMPARE(cleared.attribute("feedback").toInt(), 0);
	}

	void nativeOpulenzProgramsPersistControlsAndRejectUnknownPairs()
	{
		if (getPluginFactory()->pluginInfo("opulenz").isNull()) { QSKIP("This unit build has no OpulenZ; full process verification still requires it"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "opulenz"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); QString error;
		QCOMPARE(native->programs().size(), std::size_t(128)); QVERIFY(!native->midiPatch());
		QVERIFY(native->selectProgram(0, 42, error)); QVERIFY(native->midiPatch()); QCOMPARE(int(native->midiPatch()->program), 42);
		const auto selected = instrumentSettings(native); QCOMPARE(selected.attribute("gmProgram").toInt(), 42);
		QVERIFY(!native->selectProgram(1, 42, error)); QVERIFY(!native->selectProgram(0, 128, error)); QCOMPARE(int(native->midiPatch()->program), 42);
		QVERIFY(setNamedInstrumentParameter(0, "Patch", 0)); QCOMPARE(int(native->midiPatch()->program), 0);
		NativeInstrumentTestAccess::load(native, selected); QVERIFY(native->midiPatch()); QCOMPARE(int(native->midiPatch()->program), 42);
		QCOMPARE(instrumentSettings(native).attribute("op1_mul"), selected.attribute("op1_mul"));
		const int previous = selected.attribute("op2_mul").toInt();
		QVERIFY(setNamedInstrumentParameter(0, "Op 2 frequency multiplier", previous == 15 ? 14 : 15)); QVERIFY(!native->midiPatch());
		const auto custom = instrumentSettings(native); NativeInstrumentTestAccess::load(native, custom); QVERIFY(!native->midiPatch());
		auto inconsistent = selected; inconsistent.setAttribute("op2_mul", previous == 15 ? 14 : 15);
		NativeInstrumentTestAccess::load(native, inconsistent); QVERIFY(!native->midiPatch());
	}

	void nativeOpulenzHeldLevelsAndMidiBoundsUseNativeEmulator()
	{
		if (getPluginFactory()->pluginInfo("opulenz").isNull()) { QSKIP("This unit build has no OpulenZ; full process verification still requires it"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "opulenz"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); QString error;
		QVERIFY(native->setFileResource("patch", QString(LMMS_TEST_FIXTURE_DIR) + "/native-opl-carrier.sbi", error));
		for (int key : {-1, 128}) { QVERIFY(!native->handleMidiEvent(MidiEvent(MidiNoteOn, 0, key, 127), TimePos())); }
		QVERIFY(!native->handleMidiEvent(MidiEvent(MidiKeyPressure, 0, 69, 128), TimePos()));
		QVERIFY(!native->handleMidiEvent(MidiEvent(MidiPitchBend, 0, 16384, 0), TimePos()));
		QVERIFY(!native->handleMidiEvent(MidiEvent(MidiControlChange, 0, 128, 127), TimePos()));
		QVERIFY(!native->handleMidiEvent(MidiEvent(MidiControlChange, 0, 1, -1), TimePos()));
		QVERIFY(native->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 69, 127), TimePos()));
		const auto period = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> output(period);
		const auto renderRms = [&] {
			double sum = 0;
			for (int block = 0; block < 24; ++block)
			{
				native->play(output.data());
				for (const auto& value : output)
				{
					if (value.containsNaN() || value.containsInf()) { return std::numeric_limits<double>::quiet_NaN(); }
					if (block >= 4) { sum += double(value.left()) * value.left(); }
				}
			}
			return std::sqrt(sum / (period * 20));
		};
		const auto initial = renderRms(); QVERIFY(std::isfinite(initial) && initial > 0.01);
		QVERIFY(setNamedInstrumentParameter(0, "Op 2 level", 0));
		const auto quiet = renderRms(); QVERIFY(std::isfinite(quiet) && quiet < initial / 50);
		QVERIFY(setNamedInstrumentParameter(0, "Op 2 level", 63)); QVERIFY(renderRms() > initial / 2);
		QVERIFY(native->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 69, 0), TimePos())); QVERIFY(renderRms() < initial / 1000);
	}

	void nativeOpulenzRecoveryAndPresetsRetainSoundWithoutImportFile()
	{
		if (getPluginFactory()->pluginInfo("opulenz").isNull()) { QSKIP("This unit build has no OpulenZ; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath("import.sbi"), preset = directory.filePath("native.xpf");
		QVERIFY(QFile::copy(QString(LMMS_TEST_FIXTURE_DIR) + "/native-opl-octave.sbi", path));
		{
			StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("plugin", "instrument.load", {{"track", 0}, {"instrument", "opulenz"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("import", "instrument.resource.set", {{"track", 0}, {"resource", "patch"}, {"path", path}})).value("ok").toBool());
			QVERIFY(runtime.process(request("preset", "preset.save", {{"track", 0}, {"path", preset}})).value("ok").toBool());
			QVERIFY(runtime.process(request("program", "instrument.program.select", {{"track", 0}, {"bank", 0}, {"program", 42}})).value("ok").toBool());
		}
		QVERIFY(QFile::remove(path)); Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.filePath("session")); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto native = [] { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); };
		QVERIFY(native()->midiPatch()); QCOMPARE(int(native()->midiPatch()->program), 42);
		QVERIFY(recovered.process(request("undo", "session.undo")).value("ok").toBool());
		QVERIFY(!native()->midiPatch()); QCOMPARE(instrumentSettings(native()).attribute("op2_mul").toInt(), 2);
		QCOMPARE(native()->fileResources().front().path, path); QVERIFY(!native()->fileResources().front().requiredForRestore);
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(int(native()->midiPatch()->program), 42);
		QVERIFY(recovered.process(request("restore-preset", "preset.load", {{"track", 0}, {"path", preset}})).value("ok").toBool());
		QVERIFY(!native()->midiPatch()); QCOMPARE(instrumentSettings(native()).attribute("op2_mul").toInt(), 2);
		QDomDocument document; auto empty = document.createElement("opulenz"); NativeInstrumentTestAccess::load(native(), empty);
		QVERIFY(native()->fileResources().front().path.isEmpty()); QVERIFY(!native()->midiPatch());
	}

	void nativeSlicerInterfaceRejectsUnsupportedInstruments()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		DummyInstrument native(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)));
		QString error; std::vector<Note> notes;
		QVERIFY(!native.sliceLayout()); QVERIFY(!native.analyzeSlices(false, error)); QVERIFY(!error.isEmpty());
		QVERIFY(!native.setSlicePoints({0, 1}, error)); QVERIFY(!native.sliceMidi(notes, error));
		QVERIFY(!StudioCapabilities::mutatesProject("instrument.slices.midi"));
		QVERIFY(StudioCapabilities::mutatesProject("clip.fromSlices"));
		for (const auto item : StudioCapabilities::schema())
		{
			const auto specification = item.toObject();
			const auto fields = specification.value("inputSchema").toObject().value("properties").toObject();
			if (specification.value("name").toString() == "instrument.slices.edit")
			{
				QCOMPARE(fields.value("points").toObject().value("type").toString(), QString("array"));
				QCOMPARE(fields.value("points").toObject().value("items").toObject().value("type").toString(), QString("number"));
			}
			if (specification.value("name").toString() == "clip.fromSlices")
			{
				QCOMPARE(fields.value("replace").toObject().value("type").toString(), QString("boolean"));
				QCOMPARE(fields.value("targetContainer").toObject().value("type").toString(), QString("string"));
			}
		}
	}

	void nativeSlicerAnalysisAndLoadingHandleSilenceAndFailure()
	{
		if (getPluginFactory()->pluginInfo("slicert").isNull()) { QSKIP("This unit build has no SlicerT; full process verification still requires it"); }
		QTemporaryDir directory;
		const auto path = directory.filePath(QString(QChar(0x97f3)) + ".wav");
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "slicert"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); track->setName("Keep my slicer name");
		const auto native = track->instrument(); QString error;
		for (float value : {0.f, -0.25f})
		{
			QVERIFY(slicerFixtureWave(path, 16000, 16000, value, value));
			QVERIFY2(native->setFileResource("sample", path, error), qPrintable(error));
			QVERIFY(native->sliceLayout()->points == std::vector<float>({0, 1}));
			QVERIFY(native->analyzeSlices(false, error));
			QCOMPARE(native->sliceLayout()->sourceFrames, 16000); QCOMPARE(native->sliceLayout()->sampleRate, 16000);
		}
		QCOMPARE(track->name(), QString("Keep my slicer name"));
		const auto before = native->sliceLayout()->points;
		const auto resource = native->fileResources().front().path;
		QVERIFY(!native->setFileResource("sample", directory.filePath("missing.wav"), error));
		const auto invalid = directory.filePath("invalid.wav"); QVERIFY(slicerFixtureWave(invalid, 16000, 16000, std::numeric_limits<float>::infinity()));
		QVERIFY(!native->setFileResource("sample", invalid, error));
		QVERIFY(native->sliceLayout()->points == before); QCOMPARE(native->fileResources().front().path, resource);
		QVERIFY(!native->setSlicePoints({0, 0.5f, 0.5f, 1}, error));
		QVERIFY(!native->setSlicePoints({0, std::numeric_limits<float>::quiet_NaN(), 1}, error));
		QVERIFY(!native->setSlicePoints({0, 0.000001f, 1}, error));
		QVERIFY(native->sliceLayout()->points == before);
		QVERIFY(native->setFileResource("sample", "", error)); QVERIFY(native->sliceLayout()->points.empty());
		std::vector<Note> notes; QVERIFY(!native->sliceMidi(notes, error)); QVERIFY(!native->analyzeSlices(false, error));
	}

	void nativeSlicerPlaybackHonorsSliceEndpointsAndZeroFade()
	{
		if (getPluginFactory()->pluginInfo("slicert").isNull()) { QSKIP("This unit build has no SlicerT; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath("two-regions.wav"); QVERIFY(slicerFixtureWave(path));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "slicert"}}).value("ok").toBool());
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QString error;
		QVERIFY(track->instrument()->setFileResource("sample", path, error));
		QVERIFY(track->instrument()->setSlicePoints({0, 0.5f, 1}, error));
		QVERIFY(setNamedInstrumentParameter(0, "FadeOut", 0));
		const auto period = Engine::audioEngine()->framesPerPeriod();
		const auto rate = Engine::audioEngine()->outputSampleRate();
		Note note(TimePos(192), TimePos(0), track->baseNote() + 1);
		NotePlayHandle voice(track, 0, rate * 2, note);
		std::vector<SampleFrame> output(period);
		const auto replacement = directory.filePath("replacement.wav");
		QVERIFY(slicerFixtureWave(replacement, 16000, 16000, -0.75f, -0.75f));
		bool audible = false, replaced = false;
		for (f_cnt_t elapsed = 0; elapsed < rate; elapsed += period)
		{
			if (!replaced && elapsed >= rate / 4)
			{
				QVERIFY(track->instrument()->setFileResource("sample", replacement, error));
				QVERIFY(track->instrument()->setSlicePoints({0, 0.25f, 0.5f, 1}, error));
				replaced = true;
			}
			zeroSampleFrames(output.data(), period); voice.play(output.data());
			QVERIFY(std::all_of(output.begin(), output.end(), [](const SampleFrame& value) { return !value.containsNaN() && !value.containsInf() && value.left() >= 0; }));
			if (elapsed < rate / 4) { audible |= getAbsPeakValues(output.data(), period).left() > 0.01f; }
			if (elapsed > rate * 3 / 4) { QCOMPARE(getAbsPeakValues(output.data(), period).left(), 0.f); }
		}
		QVERIFY(audible && replaced);
		NotePlayHandle nextVoice(track, 0, rate * 2, note);
		zeroSampleFrames(output.data(), period); nextVoice.play(output.data());
		QVERIFY(getAbsPeakValues(output.data(), period).left() > 0.01f);
		QVERIFY(std::all_of(output.begin(), output.end(), [](const SampleFrame& value) { return value.left() <= 0; }));
	}

	void nativeSlicerMidiUsesSourceRateSyncAndAtomicClipInsertion()
	{
		if (getPluginFactory()->pluginInfo("slicert").isNull()) { QSKIP("This unit build has no SlicerT; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath("native.wav"); QVERIFY(slicerFixtureWave(path));
		QVERIFY(StudioCapabilities::execute("song.set", {{"tempo", 120}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "slicert"}}).value("ok").toBool());
		QString error; const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
		QVERIFY(native->setFileResource("sample", path, error)); QVERIFY(native->setSlicePoints({0, 0.25f, 0.5f, 1}, error));
		QVERIFY(setNamedInstrumentParameter(0, "Original bpm", 90));
		std::vector<Note> notes; QVERIFY(native->sliceMidi(notes, error)); QCOMPARE(notes.size(), std::size_t(3));
		QCOMPARE(notes[0].length().getTicks(), 24); QCOMPARE(notes[1].pos().getTicks(), 24); QCOMPARE(notes[2].length().getTicks(), 48);
		QVERIFY(setNamedInstrumentParameter(0, "BPM sync", 1)); QVERIFY(native->sliceMidi(notes, error));
		QCOMPARE(notes[0].length().getTicks(), 18); QCOMPARE(notes[2].pos().getTicks(), 36); QCOMPARE(notes[2].length().getTicks(), 36);
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 1}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.fromSlices", {{"track", 0}, {"targetTrack", 1}, {"clip", 0}, {"position", 12}, {"replace", true}}).value("ok").toBool());
		const auto target = static_cast<MidiClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0));
		QCOMPARE(target->notes().size(), std::size_t(3)); QCOMPARE(target->notes()[0]->pos().getTicks(), 12); QCOMPARE(target->notes()[2]->pos().getTicks(), 48);
		QVERIFY(setNamedInstrumentParameter(0, "Base note", 127));
		QVERIFY(!StudioCapabilities::execute("clip.fromSlices", {{"track", 0}, {"targetTrack", 1}, {"clip", 0}, {"replace", true}}).value("ok").toBool());
		QCOMPARE(target->notes().size(), std::size_t(3));
		QVERIFY(setNamedInstrumentParameter(0, "Base note", 0));
		QVERIFY(StudioCapabilities::execute("song.set", {{"masterPitch", 2}}).value("ok").toBool());
		QVERIFY(!native->sliceMidi(notes, error)); QCOMPARE(notes.size(), std::size_t(3));
		const auto lower = StudioCapabilities::execute("instrument.slices.inspect", {{"track", 0}}).value("result").toObject();
		QCOMPARE(lower.value("baseKey").toInt(), -2); QCOMPARE(lower.value("addressableSlices").toInt(), 2);
		QVERIFY(lower.value("sliceKeys").toArray().at(0).isNull()); QVERIFY(lower.value("wholeSampleKey").isNull());
		QVERIFY(!StudioCapabilities::execute("clip.fromSlices", {{"track", 0}, {"targetTrack", 1}, {"clip", 0}, {"replace", true}}).value("ok").toBool());
		QCOMPARE(target->notes().size(), std::size_t(3));
		QVERIFY(StudioCapabilities::execute("song.set", {{"masterPitch", 0}}).value("ok").toBool());
		QVERIFY(setNamedInstrumentParameter(0, "Base note", DefaultKey));
		QVERIFY(setNamedInstrumentParameter(0, "Pitch range", 12));
		QVERIFY(setNamedInstrumentParameter(0, "Pitch", 1200));
		QVERIFY(native->sliceMidi(notes, error)); QCOMPARE(notes.back().pos().getTicks() + notes.back().length().getTicks(), 36);
	}

	void nativeSlicerXmlPreservesEmbeddedSourceRateAndPreciseBoundaries()
	{
		if (getPluginFactory()->pluginInfo("slicert").isNull()) { QSKIP("This unit build has no SlicerT; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath("embedded.wav"); QVERIFY(slicerFixtureWave(path));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "slicert"}}).value("ok").toBool());
		QString error; const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
		QVERIFY(native->setFileResource("sample", path, error));
		const std::vector<float> points{0, 0.123456789f, 0.7f, 1}; QVERIFY(native->setSlicePoints(points, error));
		QVERIFY(setNamedInstrumentParameter(0, "Slice snap", 4));
		QDomDocument document; auto saved = document.createElement("slicert"); document.appendChild(saved); NativeInstrumentTestAccess::save(native, document, saved);
		saved.setAttribute("src", ""); saved.setAttribute("sampledata", SampleBuffer::fromFile(path)->toBase64());
		QVERIFY(native->setFileResource("sample", "", error)); NativeInstrumentTestAccess::load(native, saved);
		QCOMPARE(native->sliceLayout()->sampleRate, 16000); QVERIFY(native->sliceLayout()->points == points);
		QDomDocument check; auto restored = check.createElement("slicert"); NativeInstrumentTestAccess::save(native, check, restored); QCOMPARE(restored.attribute("snap"), QString("4"));
		saved.setAttribute("totalSlices", 2147483647); NativeInstrumentTestAccess::load(native, saved); QVERIFY(native->sliceLayout()->points == std::vector<float>({0, 1}));
		QDomDocument emptyDocument; auto empty = emptyDocument.createElement("slicert"); NativeInstrumentTestAccess::load(native, empty);
		QCOMPARE(native->sliceLayout()->sourceFrames, 0); QVERIFY(native->sliceLayout()->points.empty());
	}

	void nativeSlicerLayoutSurvivesRecoveryUndoAndPresets()
	{
		if (getPluginFactory()->pluginInfo("slicert").isNull()) { QSKIP("This unit build has no SlicerT; full process verification still requires it"); }
		QTemporaryDir directory; const auto path = directory.filePath("persistent.wav"), preset = directory.filePath("native.xpf"); QVERIFY(slicerFixtureWave(path));
		{
			StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("plugin", "instrument.load", {{"track", 0}, {"instrument", "slicert"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("sample", "instrument.resource.set", {{"track", 0}, {"resource", "sample"}, {"path", path}})).value("ok").toBool());
			QVERIFY(runtime.process(request("first", "instrument.slices.edit", {{"track", 0}, {"points", QJsonArray{0, 0.5, 1}}})).value("ok").toBool());
			QVERIFY(runtime.process(request("preset", "preset.save", {{"track", 0}, {"path", preset}})).value("ok").toBool());
			const auto edit = runtime.process(request("second", "instrument.slices.edit", {{"track", 0}, {"points", QJsonArray{0, 0.25, 0.5, 1}}})); QVERIFY(edit.value("ok").toBool());
			const auto preview = runtime.process(request("preview", "instrument.slices.midi", {{"track", 0}})); QVERIFY(preview.value("ok").toBool());
			QCOMPARE(preview.value("revision"), edit.value("revision"));
		}
		Engine::getSong()->clearProject(); StudioRuntime recovered(directory.filePath("session")); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		const auto points = [] { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument()->sliceLayout()->points; };
		QVERIFY(points() == std::vector<float>({0, 0.25f, 0.5f, 1}));
		QVERIFY(recovered.process(request("undo", "session.undo")).value("ok").toBool()); QVERIFY(points() == std::vector<float>({0, 0.5f, 1}));
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool()); QVERIFY(points() == std::vector<float>({0, 0.25f, 0.5f, 1}));
		QVERIFY(recovered.process(request("clear", "instrument.resource.set", {{"track", 0}, {"resource", "sample"}, {"path", ""}})).value("ok").toBool()); QVERIFY(points().empty());
		QVERIFY(recovered.process(request("restore", "preset.load", {{"track", 0}, {"path", preset}})).value("ok").toBool()); QVERIFY(points() == std::vector<float>({0, 0.5f, 1}));
	}

	void nativeScalaIntervalsAndFilesUseCoreParsing()
	{
		QString error; std::shared_ptr<Scale> scale;
		QVERIFY2(TuningFile::parseScale("Native calibration", "  ! comment\n-5.0\n5\n3 / 2 annotation\n2147483647/2147483647\n2/1", scale, error), qPrintable(error));
		QCOMPARE(scale->getIntervals().size(), std::size_t(6));
		QCOMPARE(scale->getIntervals().at(0).getRatio(), 1.f);
		QCOMPARE(scale->getIntervals().at(2).getRatio(), 5.f);
		QCOMPARE(scale->getIntervals().at(3).getRatio(), 1.5f);
		QCOMPARE(scale->getIntervals().at(4).getRatio(), 1.f);
		QVERIFY(scale->getIntervals().at(1).getRatio() < 1);
		QTemporaryDir directory;
		const auto path = directory.filePath("native.scl");
		QVERIFY(writeTuningText(path, QString(QChar(0xfeff)) + "! header\n\n3\n100.1234567 cents\n3/2 fifth\n2/1 period\n"));
		QVERIFY2(TuningFile::loadScale(path, scale, error), qPrintable(error));
		QCOMPARE(scale->getDescription(), QString());
		QCOMPARE(scale->getIntervals().size(), std::size_t(4));
		const auto cents = scale->getIntervals().at(1).cents();
		QCOMPARE(scale->getIntervals().at(1).getString().toFloat(), cents);
		const auto exported = directory.filePath("exchange.scl");
		QVERIFY2(TuningFile::saveScale(exported, *scale, error), qPrintable(error));
		std::shared_ptr<Scale> reloaded;
		QVERIFY2(TuningFile::loadScale(exported, reloaded, error), qPrintable(error));
		QCOMPARE(reloaded->getIntervals().at(1).cents(), cents);
		QDomDocument document; auto element = document.createElement("scale"); document.appendChild(element);
		scale->saveSettings(document, element);
		Scale restored; restored.loadSettings(element);
		QCOMPARE(restored.getIntervals().at(1).cents(), cents);
		QVERIFY(TuningFile::parseScale("Empty native scale", "", scale, error));
		QCOMPARE(scale->getIntervals().size(), std::size_t(1));
		QVERIFY(TuningFile::parseScale("Fractional cents", ".5\n-.5\n100.0e1", scale, error));
		QCOMPARE(scale->getIntervals().at(1).cents(), 0.5f);
		QCOMPARE(scale->getIntervals().at(2).cents(), -0.5f);
		QCOMPARE(scale->getIntervals().at(3).cents(), 1000.f);
	}

	void invalidTuningDefinitionsDoNotChangeNativeState()
	{
		QVERIFY(StudioCapabilities::execute("tuning.scale.set", {{"slot", 2}, {"name", "Retain"}, {"intervals", "2/1"}}).value("ok").toBool());
		const auto original = Engine::getSong()->getScale(2);
		for (const auto& text : {QString("1/0"), QString("0/1"), QString("-1/2"), QString("3/2/4"),
			QString("100.0e999"), QString("NaN"), QString("1/4294967296"), QString("broken")})
		{
			QVERIFY(!StudioCapabilities::execute("tuning.scale.set", {{"slot", 2}, {"name", "Invalid"}, {"intervals", text}}).value("ok").toBool());
			QCOMPARE(Engine::getSong()->getScale(2).get(), original.get());
		}
		QVERIFY(!StudioCapabilities::execute("tuning.scale.set", {{"slot", 2}, {"name", "! invalid"}, {"intervals", "2/1"}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("tuning.scale.set", {{"slot", 2}, {"name", "Too large"},
			{"intervals", QString("1/1\n").repeated(TuningFile::MaximumEntries + 1)}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getScale(2).get(), original.get());
		const auto originalMap = Engine::getSong()->getKeymap(2);
		QVERIFY(!StudioCapabilities::execute("tuning.keymap.set", {{"slot", 2}, {"name", "Invalid"}, {"mapping", "0"},
			{"firstKey", 100}, {"lastKey", 50}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("tuning.keymap.set", {{"slot", 2}, {"name", "Invalid"}, {"mapping", "0"}, {"baseFrequency", 0}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getKeymap(2).get(), originalMap.get());
		QTemporaryDir directory;
		const auto malformed = directory.filePath("truncated.scl");
		QVERIFY(writeTuningText(malformed, "Incomplete\n3\n2/1\n"));
		QVERIFY(!StudioCapabilities::execute("tuning.scale.load", {{"slot", 2}, {"path", malformed}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getScale(2).get(), original.get());
		QString error; std::shared_ptr<Scale> output;
		QVERIFY(TuningFile::parseScale("Keep output", "2/1", output, error));
		const auto previousOutput = output.get();
		QVERIFY(!TuningFile::loadScale(malformed, output, error)); QCOMPARE(output.get(), previousOutput);
		QVERIFY(!TuningFile::saveScale(directory.filePath("missing/scale.scl"), *output, error)); QVERIFY(!error.isEmpty());
	}

	void nativeTuningAppliesNegativeDegreesFormalPeriodsAndUnmappedKeys()
	{
		QVERIFY(StudioCapabilities::execute("tuning.scale.set", {{"slot", 1}, {"name", "Two degree period"}, {"intervals", "3/2\n2/1"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Signed mapping"}, {"mapping", "-1\n0\nx\n1"},
			{"middleKey", 60}, {"baseKey", 61}, {"baseFrequency", 300}, {"octaveDegree", 4}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto tuner = track->microtuner();
		tuner->enabledModel()->setValue(true); tuner->scaleModel()->setValue(1); tuner->keymapModel()->setValue(1);
		QVERIFY(track->isKeyMapped(60)); QVERIFY(!track->isKeyMapped(62));
		QCOMPARE(track->nominalFrequency(60), 225.f); QCOMPARE(track->nominalFrequency(61), 300.f);
		QCOMPARE(track->nominalFrequency(62), 0.f); QCOMPARE(track->nominalFrequency(63), 450.f);
		QCOMPARE(track->nominalFrequency(64), 900.f); QCOMPARE(track->nominalFrequency(56), 56.25f);
		{
			Note hole(TimePos(48), TimePos(0), 62);
			NotePlayHandle silent(track, 0, 0, hole);
			QCOMPARE(silent.frequency(), 0.f);
			QCOMPARE(silent.frames(), Engine::audioEngine()->framesPerPeriod());
			silent.doProcessing();
			QVERIFY(silent.buffer());
			for (f_cnt_t frame = 0; frame < Engine::audioEngine()->framesPerPeriod(); ++frame)
			{
				QCOMPARE(silent.buffer()[frame].left(), 0.f);
				QCOMPARE(silent.buffer()[frame].right(), 0.f);
			}
		}
		Note reference(TimePos(48), TimePos(0), 61);
		NotePlayHandle handle(track, 0, 4096, reference);
		QCOMPARE(handle.unpitchedFrequency(), 300.f);
		const auto preview = StudioCapabilities::execute("instrument.tuning.inspect", {{"track", 0}, {"key", 61}}).value("result").toObject();
		QCOMPARE(preview.value("keys").toArray().at(0).toObject().value("frequency").toDouble(), double(handle.unpitchedFrequency()));
		const auto negative = StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Reverse period"},
			{"mapping", "-1\n0\nx\n1"}, {"octaveDegree", -4}});
		QVERIFY(negative.value("ok").toBool());
		QCOMPARE(track->nominalFrequency(64), 56.25f); QCOMPARE(track->nominalFrequency(56), 900.f);
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Unmapped reference"}, {"mapping", "x\n0"}, {"baseKey", 60}}).value("ok").toBool());
		QCOMPARE(track->nominalFrequency(61), 0.f);
		QVERIFY(!StudioCapabilities::execute("tuning.keymap.inspect", {{"slot", static_cast<int>(MaxKeymapCount)}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("tuning.scale.set", {{"slot", 1}, {"name", "Constant pitch"}, {"intervals", ""}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Linear"}, {"mapping", ""},
			{"baseKey", 61}, {"baseFrequency", 10000.1234567}}).value("ok").toBool());
		QCOMPARE(tuner->octaveSize(), 1);
		QCOMPARE(track->nominalFrequency(60), static_cast<float>(10000.1234567));
		QCOMPARE(track->nominalFrequency(61), static_cast<float>(10000.1234567));
	}

	void activeNativeNotesFollowTuningLibraryAndControlChanges()
	{
		QVERIFY(StudioCapabilities::execute("tuning.scale.set", {{"slot", 1}, {"name", "Native scale"}, {"intervals", "3/2\n2/1"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Native mapping"}, {"mapping", "-1\n0\nx\n1"},
			{"middleKey", 60}, {"baseKey", 61}, {"baseFrequency", 300}, {"octaveDegree", 4}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto tuner = track->microtuner();
		tuner->enabledModel()->setValue(true); tuner->scaleModel()->setValue(1); tuner->keymapModel()->setValue(1);
		Note note(TimePos(48), TimePos(0), 64);
		NotePlayHandle active(track, 0, 100000, note);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod());
		QCOMPARE(active.unpitchedFrequency(), 900.f);
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Shorter period"}, {"mapping", "-1\n0\nx\n1"}, {"octaveDegree", 2}}).value("ok").toBool());
		active.play(output.data()); QCOMPARE(active.unpitchedFrequency(), 450.f);
		QVERIFY(StudioCapabilities::execute("tuning.scale.set", {{"slot", 1}, {"name", "Other interval"}, {"intervals", "4/3\n2/1"}}).value("ok").toBool());
		active.play(output.data()); QVERIFY(std::abs(active.unpitchedFrequency() - 400.f) < 0.0001f);
		tuner->scaleModel()->setValue(0);
		active.play(output.data()); QCOMPARE(active.unpitchedFrequency(), 300.f);
		tuner->scaleModel()->setValue(1);
		tuner->keyRangeImportModel()->setValue(false);
		// The native A4 base key (69) is degree 4; key 64 is degree 1, so 300 * (4/3) / 4 = 100.
		active.play(output.data()); QVERIFY(std::abs(active.unpitchedFrequency() - 100.f) < 0.0001f);
		tuner->keyRangeImportModel()->setValue(true);
		active.play(output.data()); QVERIFY(std::abs(active.unpitchedFrequency() - 400.f) < 0.0001f);
		tuner->enabledModel()->setValue(false);
		active.play(output.data());
		QVERIFY(std::abs(active.unpitchedFrequency() - DefaultBaseFreq * std::exp2((64 - DefaultKey) / 12.f)) < 0.0001f);
		tuner->enabledModel()->setValue(true);
		active.play(output.data()); QVERIFY(std::abs(active.unpitchedFrequency() - 400.f) < 0.0001f);
		tuner->keymapModel()->setValue(0);
		active.play(output.data());
		QVERIFY(active.unpitchedFrequency() > 0 && std::abs(active.unpitchedFrequency() - 400.f) > 1);
		tuner->keymapModel()->setValue(1);
		active.play(output.data()); QVERIFY(std::abs(active.unpitchedFrequency() - 400.f) < 0.0001f);
		QVERIFY(StudioCapabilities::execute("tuning.keymap.set", {{"slot", 1}, {"name", "Unmapped sustained key"}, {"mapping", "x\n0\nx\n1"}}).value("ok").toBool());
		active.play(output.data()); QCOMPARE(active.frequency(), 0.f); QVERIFY(active.isReleased());
	}

	void nativeKeyboardMappingExchangePreservesSignedAndLegacySemantics()
	{
		QString error; std::shared_ptr<Keymap> keymap;
		QVERIFY(TuningFile::parseKeymap("Signed calibration", "-1\n0\nx\n1", 0, 127, 60, 61, 300.1234567, 4, keymap, error));
		QTemporaryDir directory;
		const auto path = directory.filePath("map.kbm");
		QVERIFY2(TuningFile::saveKeymap(path, *keymap, error), qPrintable(error));
		std::shared_ptr<Keymap> reloaded;
		QVERIFY2(TuningFile::loadKeymap(path, reloaded, error), qPrintable(error));
		QCOMPARE(reloaded->getDescription(), keymap->getDescription());
		QCOMPARE(reloaded->getBaseFreq(), keymap->getBaseFreq()); QCOMPARE(reloaded->octaveDegree(), 4);
		QVERIFY(reloaded->isMapped(60)); QVERIFY(!reloaded->isMapped(62)); QCOMPARE(reloaded->getDegree(60), -1);
		QDomDocument document; auto element = document.createElement("keymap"); document.appendChild(element);
		keymap->saveSettings(document, element);
		element.insertBefore(document.createComment("ignore whitespace/metadata"), element.firstChild());
		Keymap restored; restored.loadSettings(element);
		QVERIFY(restored.getMap() == keymap->getMap()); QVERIFY(restored.isMapped(60)); QVERIFY(!restored.isMapped(62));
		QCOMPARE(restored.getBaseFreq(), keymap->getBaseFreq()); QCOMPARE(restored.octaveDegree(), 4);
		const auto omitted = directory.filePath("omitted.kbm");
		QVERIFY(writeTuningText(omitted, "4\n0\n127\n60\n60\n440.0\n4\n0\n1\n"));
		QVERIFY(TuningFile::loadKeymap(omitted, reloaded, error));
		QCOMPARE(reloaded->getSize(), std::size_t(4)); QVERIFY(!reloaded->isMapped(62)); QVERIFY(!reloaded->isMapped(63));
		QVERIFY(writeTuningText(omitted, "1\n0\n127\n60\n60\n440.0\n1\nx\n"));
		const auto previous = reloaded.get();
		QVERIFY(!TuningFile::loadKeymap(omitted, reloaded, error)); QCOMPARE(reloaded.get(), previous);
		Keymap legacy;
		QVERIFY(TuningFile::saveKeymap(path, legacy, error));
		QVERIFY(TuningFile::loadKeymap(path, reloaded, error));
		QCOMPARE(reloaded->getDegree(60), legacy.getDegree(60));
		QCOMPARE(reloaded->getMiddleKey(), 0); QVERIFY(reloaded->linearFromMiddle());
	}

	void nativeTuningSurvivesRecoveryUndoAndClearsOnProjectReplacement()
	{
		QTemporaryDir directory;
		{
			StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("scale", "tuning.scale.set", {{"slot", 5}, {"name", "Persistent scale"}, {"intervals", "3/2\n2/1"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("keymap", "tuning.keymap.set", {{"slot", 6}, {"name", "Persistent mapping"}, {"mapping", "-1\n0\nx\n1"},
				{"middleKey", 60}, {"baseKey", 61}, {"baseFrequency", 300}, {"octaveDegree", 4}})).value("ok").toBool());
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			auto tuner = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->microtuner();
			QVERIFY(runtime.process(request("choose-scale", "parameter.set", {{"parameter", static_cast<int>(tuner->scaleModel()->id())}, {"value", 5}})).value("ok").toBool());
			QVERIFY(runtime.process(request("choose-map", "parameter.set", {{"parameter", static_cast<int>(tuner->keymapModel()->id())}, {"value", 6}})).value("ok").toBool());
			QVERIFY(runtime.process(request("enable", "parameter.set", {{"parameter", static_cast<int>(tuner->enabledModel()->id())}, {"value", 1}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.filePath("session")); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QCOMPARE(track->nominalFrequency(60), 225.f);
		QCOMPARE(Engine::getSong()->getKeymap(6)->octaveDegree(), 4);
		QVERIFY(recovered.process(request("undo", "session.undo")).value("ok").toBool());
		QVERIFY(!static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->microtuner()->enabled());
		QVERIFY(recovered.process(request("redo", "session.redo")).value("ok").toBool());
		QCOMPARE(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->nominalFrequency(60), 225.f);
		const auto summary = StudioCapabilities::inspect().value("tuning").toObject();
		QCOMPARE(summary.value("scales").toArray().size(), int(MaxScaleCount));
		QVERIFY(!summary.value("scales").toArray().at(5).toObject().contains("intervals"));
		QVERIFY(recovered.process(request("new", "project.new", {{"empty", true}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getScale(5)->getIntervals().size(), std::size_t(1));
		QCOMPARE(Engine::getSong()->getKeymap(6)->getSize(), std::size_t(0));
		DataFile legacy(DataFile::Type::SongProject);
		const auto path = directory.filePath("legacy.mmp"); QVERIFY(legacy.writeFile(path));
		QVERIFY(recovered.process(request("other-scale", "tuning.scale.set", {{"slot", 5}, {"name", "Must clear"}, {"intervals", "2/1"}})).value("ok").toBool());
		QVERIFY(recovered.process(request("legacy", "project.load", {{"path", path}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getScale(5)->getIntervals().size(), std::size_t(1));
	}

	void instrumentDiscoveryIncludesNativeChoicesAndUnparentedControls()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto arp = track->findChild<InstrumentFunctionArpeggio*>();
		QVERIFY(arp);
		const auto enabledModels = arp->findChildren<BoolModel*>(QString(), Qt::FindDirectChildrenOnly);
		QCOMPARE(enabledModels.size(), 1);
		auto enabled = enabledModels.front();
		const auto response = test::executeWithAllParameters("instrument.inspect", {{"track", 0}});
		QVERIFY(response.value("ok").toBool());
		const auto instrument = response.value("result").toObject();
		const auto parameters = instrument.value("parameters").toArray();
		const auto metadata = parameterMetadata(parameters, enabled);
		QCOMPARE(metadata.value("kind").toString(), QString("boolean"));
		QCOMPARE(metadata.value("location").toObject().value("track").toInt(-1), 0);
		QVERIFY(!metadata.value("modelPath").toArray().isEmpty());
		QVERIFY(StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(enabled->id())}, {"value", 1}}).value("ok").toBool());
		QVERIFY(track->isArpeggioEnabled());
		QVERIFY(!StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(enabled->id())}, {"value", 0.5}}).value("ok").toBool());
		QVERIFY(track->isArpeggioEnabled());
		QCOMPARE(instrument.value("nativeChordTables").toArray().size(), InstrumentFunctionNoteStacking::NUM_CHORD_TABLES);
		for (auto model : arp->findChildren<ComboBoxModel*>())
		{
			const auto item = parameterMetadata(parameters, model);
			QCOMPARE(item.value("choices").toArray().size(), model->size());
			QCOMPARE(item.value("choices").toArray().at(0).toObject().value("name").toString(), model->itemText(0));
		}
		QVERIFY(!parameterMetadata(parameters, track->microtuner()->enabledModel()).isEmpty());
		QVERIFY(!parameterMetadata(parameters, track->microtuner()->keyRangeImportModel()).isEmpty());
		QVERIFY(!parameterMetadata(parameters, track->microtuner()->scaleModel()).value("choices").toArray().isEmpty());
		for (int i = 0; i < MidiControllerCount; ++i) { QVERIFY(!parameterMetadata(parameters, track->midiCCModel(i)).isEmpty()); }
		QCOMPARE(parameterMetadata(parameters, track->midiCCEnabledModel()).value("kind").toString(), QString("boolean"));
		const auto concise = test::executeWithAllParameters("instrument.inspect", {{"track", 0}, {"choices", false}}).value("result").toObject();
		QVERIFY(!concise.contains("nativeChordTables"));
		QVERIFY(!parameterMetadata(concise.value("parameters").toArray(), track->microtuner()->scaleModel()).contains("choices"));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		const auto filtered = test::executeWithAllParameters("parameter.list", {{"track", 1}, {"choices", false}}).value("result").toObject().value("parameters").toArray();
		QVERIFY(!filtered.isEmpty());
		for (const auto item : filtered) { QCOMPARE(item.toObject().value("location").toObject().value("track").toInt(-1), 1); }
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(!test::executeWithAllParameters("parameter.list", {{"track", 0}, {"controller", 0}}).value("ok").toBool());
		auto lfo = static_cast<LfoController*>(Engine::getSong()->controllers().at(0));
		const auto wave = StudioCapabilities::execute("parameter.inspect", {{"parameter", static_cast<int>(lfo->waveModel().id())}}).value("result").toObject();
		QCOMPARE(wave.value("choices").toArray().size(), int(Oscillator::NumWaveShapes));
		QCOMPARE(wave.value("maximum").toInt(), int(Oscillator::NumWaveShapes) - 1);
		QCOMPARE(wave.value("location").toObject().value("controller").toInt(-1), 0);
		lfo->waveModel().setValue(static_cast<int>(Oscillator::WaveShape::WhiteNoise));
		QString error;
		QVERIFY(lfo->setUserWave({}, error));
		QCOMPARE(lfo->currentValue(0), 0.5f); // Hold a finite initial sample before the first noise resampling cycle.
	}

	void nativeInstrumentFunctionSettingsSurviveRecovery()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path()); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
			auto arp = track->findChild<InstrumentFunctionArpeggio*>();
			QVERIFY(arp);
			auto enabled = arp->findChildren<BoolModel*>(QString(), Qt::FindDirectChildrenOnly).front();
			QVERIFY(studio.process(request("enable", "parameter.set", {{"parameter", static_cast<int>(enabled->id())}, {"value", 1}})).value("ok").toBool());
			QVERIFY(studio.process(request("tuning", "parameter.set", {{"parameter", static_cast<int>(track->microtuner()->enabledModel()->id())}, {"value", 1}})).value("ok").toBool());
			QVERIFY(studio.process(request("range", "parameter.set", {{"parameter", static_cast<int>(track->microtuner()->keyRangeImportModel()->id())}, {"value", 0}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.path()); QString error;
		QVERIFY2(recovered.open(error), qPrintable(error));
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QVERIFY(track->isArpeggioEnabled());
		QVERIFY(track->microtuner()->enabled());
		QVERIFY(!track->microtuner()->keyRangeImport());
		const auto metadata = test::executeWithAllParameters("instrument.inspect", {{"track", 0}});
		QVERIFY(metadata.value("ok").toBool());
		QVERIFY(metadata.value("result").toObject().value("arpeggioEnabled").toBool());
	}

	void nativeSaveFinalizesQueuedRecordingBeforeSerializingWithoutEventPolling_data()
	{
		QTest::addColumn<QString>("extension"); QTest::newRow("mmp") << QString("mmp"); QTest::newRow("mmpz") << QString("mmpz");
	}
	void nativeSaveFinalizesQueuedRecordingBeforeSerializingWithoutEventPolling()
	{
		QFETCH(QString, extension); const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QTemporaryDir directory; QVERIFY(directory.isValid());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); clip->setRecord(true); QSignalSpy changed(clip, &SampleClip::sampleChanged);
		std::vector<SampleFrame> input(7777, SampleFrame(.25f, -.5f)); const auto rate = engine->inputSampleRate();
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		std::thread remove([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }); remove.join();
		QVERIFY(clip->hasPendingRecording() && !clip->hasActiveRecording()); QCOMPARE(changed.count(), 0);
		const auto path = directory.filePath("recorded." + extension); QVERIFY(Engine::getSong()->saveProjectFile(path));
		QVERIFY(!clip->hasPendingRecording() && !clip->isRecord()); QVERIFY(changed.count() > 0); const int publishedNotifications = changed.count();
		const SampleBuffer expected(input, static_cast<int>(rate)); DataFile saved(path); const auto node = saved.elementsByTagName("sampleclip").at(0).toElement();
		QCOMPARE(node.attribute("data"), expected.toBase64()); QCOMPARE(node.attribute("sample_rate").toInt(), static_cast<int>(rate));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QCOMPARE(changed.count(), publishedNotifications);
	}

	void failedDeferredRecordingCompletionRejectsSaveAndPreservesPriorPcm()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("recorded.mmp");
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); QVERIFY(Engine::getSong()->saveProjectFile(path)); const auto before = readFixtureBytes(path);
		clip->setRecord(true); const auto recorder = new SampleRecordHandle(clip);
		std::thread worker([&] { const auto guard = engine->requestChangesGuard(); delete recorder; }); worker.join();
		QVERIFY(clip->hasPendingRecording() && !clip->hasActiveRecording()); QVERIFY(!Engine::getSong()->saveProjectFile(path));
		QCOMPARE(readFixtureBytes(path), before); QCOMPARE(clip->sample().toBase64(), previous->toBase64());
		QVERIFY(!clip->hasPendingRecording() && !clip->isRecord()); QVERIFY(Engine::getSong()->hasErrors());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); Engine::getSong()->clearErrors();
	}

	void activeRecordingSavePreservesTheFileAndNativeStopFinishesTheTake()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("recorded.mmp");
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); QVERIFY(Engine::getSong()->saveProjectFile(path)); const auto before = readFixtureBytes(path);
		QVERIFY(writeFixtureBytes(path + ".bak", "Prior native backup")); clip->setRecord(true);
		const auto recorder = new SampleRecordHandle(clip); QVERIFY(engine->addPlayHandle(recorder));
		std::vector<SampleFrame> input(1537, SampleFrame(.25f, -.5f)); engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		QVERIFY(clip->hasActiveRecording()); QVERIFY(!Engine::getSong()->saveProjectFile(path));
		QCOMPARE(readFixtureBytes(path), before); QCOMPARE(readFixtureBytes(path + ".bak"), QByteArray("Prior native backup"));
		QCOMPARE(clip->sample().toBase64(), previous->toBase64());
		Engine::getSong()->stop(); QVERIFY(!clip->hasActiveRecording() && !clip->hasPendingRecording());
		QCOMPARE(clip->sample().sampleSize(), input.size()); QVERIFY(!clip->isRecord()); QVERIFY(Engine::getSong()->saveProjectFile(path));
		Engine::getSong()->clearErrors();
	}

	void nativeStopFinishesRecordHandlesStillWaitingInTheNewHandleQueue()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		clip->setRecord(true); QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); QVERIFY(clip->hasActiveRecording());
		Engine::getSong()->stop(); QVERIFY(!clip->hasActiveRecording() && !clip->hasPendingRecording() && !clip->isRecord());
		QVERIFY(Engine::getSong()->hasErrors()); Engine::getSong()->clearErrors();
	}

	void aWorkerCannotPublishQueuedRecordingOrBypassThePersistenceBarrier()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		clip->setRecord(true); QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip)));
		std::vector<SampleFrame> input(32, SampleFrame(.25f)); engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		bool result = true; QString error;
		std::thread worker([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); result = SampleRecordHandle::finalizePendingRecordings(error); }); worker.join();
		QVERIFY(!result && !error.isEmpty()); QVERIFY(clip->hasPendingRecording());
		QVERIFY(SampleRecordHandle::finalizePendingRecordings(error)); QVERIFY(error.isEmpty()); QCOMPARE(clip->sample().sampleSize(), input.size());
	}

	void aCheckpointOwnsCompletedNativeRecordingWithoutThePostedCallbackRunning()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto session = directory.filePath("session");
		StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		clip->setRecord(true); QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip)));
		std::vector<SampleFrame> input(1537, SampleFrame(.25f, -.5f)); const auto rate = engine->inputSampleRate();
		engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		std::thread worker([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }); worker.join();
		QCoreApplication::removePostedEvents(clip, QEvent::MetaCall);
		QVERIFY(clip->hasPendingRecording()); QVERIFY(runtime.process(request("checkpoint", "song.set", {{"tempo", 91}})).value("ok").toBool());
		const auto state = manifest(session); const auto entry = state.value("history").toArray().at(state.value("cursor").toInt()).toObject();
		DataFile checkpoint(QDir(session).filePath(entry.value("path").toString())); const auto saved = checkpoint.elementsByTagName("sampleclip").at(0).toElement();
		const SampleBuffer expected(input, static_cast<int>(rate)); QCOMPARE(saved.attribute("data"), expected.toBase64());
	}

	void nativeRecordHandleUsesEngineInputAndFinalizesOnClipThread_data()
	{
		QTest::addColumn<bool>("queued"); QTest::addColumn<bool>("invalid");
		QTest::newRow("control-thread-completion") << false << false;
		QTest::newRow("worker-removal-queued-completion") << true << false;
		QTest::newRow("invalid-native-input-preserves-prior-sample") << false << true;
	}
	void nativeRecordHandleUsesEngineInputAndFinalizesOnClipThread()
	{
		QFETCH(bool, queued); QFETCH(bool, invalid);
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); clip->setRecord(true); QSignalSpy changed(clip, &SampleClip::sampleChanged);
		std::atomic<bool> onOwnerThread{true};
		const auto connection = QObject::connect(clip, &SampleClip::sampleChanged, clip, [&] {
			if (QThread::currentThread() != clip->thread()) { onOwnerThread.store(false); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		auto recorder = std::make_unique<SampleRecordHandle>(clip);
		QCOMPARE(recorder->audioBusHandle(), track->audioBusHandle()); QVERIFY(!recorder->usesBuffer());
		const auto handle = recorder.get(); QVERIFY(engine->addPlayHandle(recorder.release()));
		std::vector<SampleFrame> input(queued && !invalid ? 100003 : 16703, SampleFrame(.25f, -.5f));
		if (invalid) { input.back().right() = std::numeric_limits<float>::quiet_NaN(); }
		const auto rate = engine->inputSampleRate();
		engine->pushInputFrames(input.data(), 9000); engine->pushInputFrames(input.data() + 9000, input.size() - 9000); engine->renderNextPeriod();
		if (invalid)
		{
			QVERIFY(Engine::getSong()->hasErrors()); QCOMPARE(clip->sample().toBase64(), previous->toBase64());
			QVERIFY(!clip->isRecord()); Engine::getSong()->clearErrors();
		}
		else
		{
			QCOMPARE(handle->framesRecorded(), f_cnt_t(input.size()));
			if (queued)
			{
				std::thread remove([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }); remove.join();
				QCOMPARE(clip->sample().toBase64(), previous->toBase64()); QVERIFY(clip->isRecord()); QCOMPARE(changed.count(), 0);
				QTRY_VERIFY(!clip->isRecord());
			}
			else { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }
			QVERIFY(!clip->isRecord()); QCOMPARE(clip->sample().sampleSize(), input.size()); QCOMPARE(clip->sample().sampleRate(), static_cast<int>(rate));
			QVERIFY(changed.count() > 0); QVERIFY(onOwnerThread.load());
			for (std::size_t index = 0; index < input.size(); ++index)
			{
				QCOMPARE(clip->sample().data()[index][0], input[index][0]); QCOMPARE(clip->sample().data()[index][1], input[index][1]);
			}
		}
	}

	void rejectedNativeInputInvalidatesTheTakeBeforeCompletion_data()
	{
		QTest::addColumn<bool>("oversized"); QTest::newRow("missing-input") << false; QTest::newRow("frame-count-overflow") << true;
	}
	void rejectedNativeInputInvalidatesTheTakeBeforeCompletion()
	{
		QFETCH(bool, oversized);
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); clip->setRecord(true); auto recorder = new SampleRecordHandle(clip);
		QVERIFY(engine->addPlayHandle(recorder)); std::vector<SampleFrame> valid(32, SampleFrame(.25f));
		engine->pushInputFrames(valid.data(), valid.size()); engine->renderNextPeriod(); QCOMPARE(recorder->framesRecorded(), f_cnt_t(32));
		const auto epoch = engine->inputErrorEpoch();
		engine->pushInputFrames(oversized ? valid.data() : nullptr, oversized ? std::numeric_limits<f_cnt_t>::max() : f_cnt_t(1));
		QCOMPARE(engine->inputErrorEpoch(), epoch + 1);
		// Complete immediately; no additional capture period may hide a lost final input block.
		QVERIFY(!recorder->createSampleBuffer()); engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle);
		QCOMPARE(clip->sample().toBase64(), previous->toBase64()); QVERIFY(!clip->isRecord()); QVERIFY(Engine::getSong()->hasErrors());
		Engine::getSong()->clearErrors();
	}

	void rejectedFirstInputPeriodCannotBecomeASuccessfulLateStartingTake()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000);
		clip->setSampleBuffer(previous); clip->setRecord(true);
		engine->pushInputFrames(nullptr, 1); const auto epoch = engine->inputErrorEpoch();
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); engine->renderNextPeriod();
		QCOMPARE(engine->inputErrorEpoch(), epoch); QVERIFY(engine->inputBufferFault());
		QVERIFY(!clip->isRecord()); QCOMPARE(clip->sample().toBase64(), previous->toBase64());
		QVERIFY(Engine::getSong()->hasErrors()); Engine::getSong()->clearErrors();
	}

	void queuedNativeRecordingCompletionCannotOutliveItsClip()
	{
		const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); const auto clip = static_cast<SampleClip*>(track->getClip(0));
		QPointer<SampleClip> guarded(clip); clip->setRecord(true);
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip)));
		std::vector<SampleFrame> input(32, SampleFrame(.25f)); engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		std::thread remove([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }); remove.join();
		{ const auto guard = engine->requestChangesGuard(); delete clip; }
		QVERIFY(guarded.isNull()); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void noNativeRecordingInputLeavesTheExistingSampleIntact()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.25f)), 24000);
		clip->setSampleBuffer(previous); clip->setRecord(true);
		{ SampleRecordHandle recorder(clip); }
		QCOMPARE(clip->sample().toBase64(), previous->toBase64()); QVERIFY(!clip->isRecord()); QVERIFY(Engine::getSong()->hasErrors());
		Engine::getSong()->clearErrors();
	}

	void nativeStopClearsRecordingStateAcrossActiveAndStoppedModes()
	{
		const auto song = Engine::getSong(); song->playAndRecord(); QVERIFY(song->isRecording());
		song->stop(); QVERIFY(song->isStopped() && !song->isRecording());
		song->record(); QVERIFY(song->isRecording()); song->stop(); QVERIFY(song->isStopped() && !song->isRecording());
		song->playSong(); QVERIFY(song->isPlaying() && !song->isRecording()); song->stop();
	}

	void nativeHandleRemovalDetachesBeforeReentrantCompletion_data()
	{
		QTest::addColumn<int>("path");
		QTest::newRow("remove-by-track-and-type") << 0; QTest::newRow("finished-handle") << 1; QTest::newRow("queued-removal") << 2;
	}
	void nativeHandleRemovalDetachesBeforeReentrantCompletion()
	{
		QFETCH(int, path); const auto engine = Engine::audioEngine(); const auto wasRunning = engine->audioDev()->isRunning(); engine->audioDev()->stopProcessing();
		const auto restart = qScopeGuard([&] { if (wasRunning) { engine->audioDev()->startProcessing(); } });
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		const auto track = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(0)); int destroyed = 0; bool detached = true;
		class Completing : public PlayHandle
		{
		public:
			Completing(AudioEngine* engine, SampleTrack* track, int& destroyed, bool& detached, bool finished)
				: PlayHandle(Type::SamplePlayHandle), m_engine(engine), m_track(track), m_destroyed(destroyed), m_detached(detached), m_finished(finished)
			{
				setAudioBusHandle(track->audioBusHandle()); setUsesBuffer(false);
			}
			~Completing() override
			{
				++m_destroyed; m_detached = m_detached && !m_engine->playHandles().contains(this);
				m_engine->removePlayHandlesOfTypes(m_track, Type::SamplePlayHandle);
			}
			void play(SampleFrame*) override {}
			bool isFinished() const override { return m_finished; }
			bool isFromTrack(const Track* track) const override { return track == m_track; }
		private:
			AudioEngine* m_engine; SampleTrack* m_track; int& m_destroyed; bool& m_detached; bool m_finished;
		};
		const auto first = new Completing(engine, track, destroyed, detached, path == 1);
		QVERIFY(engine->addPlayHandle(first));
		QVERIFY(engine->addPlayHandle(new Completing(engine, track, destroyed, detached, false)));
		engine->renderNextPeriod();
		if (path == 0) { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); }
		if (path == 2) { engine->removePlayHandle(first); engine->renderNextPeriod(); }
		QCOMPARE(destroyed, 2); QVERIFY(detached);
	}

	void nativeEngineRejectsUnattachedHandlesWithoutDereferencingTheirBus()
	{
		class Unattached : public PlayHandle
		{
		public:
			explicit Unattached(bool& destroyed) : PlayHandle(Type::SamplePlayHandle), m_destroyed(destroyed) {}
			~Unattached() override { m_destroyed = true; }
			void play(SampleFrame*) override {}
			bool isFinished() const override { return true; }
			bool isFromTrack(const Track*) const override { return false; }
		private: bool& m_destroyed;
		};
		bool destroyed = false; auto handle = new Unattached(destroyed); QVERIFY(handle->audioBusHandle() == nullptr);
		QVERIFY(!Engine::audioEngine()->addPlayHandle(handle)); QVERIFY(destroyed);
		QVERIFY(!Engine::audioEngine()->addPlayHandle(nullptr));
	}

	void cancelledEmbeddedCheckpointEncodingRestoresPriorPcmAndHistory()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto session = directory.filePath("session");
		StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		const std::vector<SampleFrame> frames(16703, SampleFrame(.25f, -.125f));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(frames, 24000));
		QVERIFY(runtime.process(request("accepted", "song.set", {{"tempo", 91}})).value("ok").toBool());
		const auto before = manifest(session); const auto pcm = clip->sample().toBase64(); bool cancelled = false;
		{
			FileResourceIO::Scope scope([&] { cancelled = cancelled || Engine::getSong()->getTempo() == 73; return cancelled; });
			QVERIFY(!runtime.process(request("cancelled", "song.set", {{"tempo", 73}})).value("ok").toBool());
		}
		QVERIFY(cancelled); const auto after = manifest(session);
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1); QCOMPARE(Engine::getSong()->getTempo(), 91);
		const auto restored = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		QCOMPARE(restored->sample().toBase64(), pcm); QCOMPARE(restored->sample().sampleRate(), 24000);
		QVERIFY(runtime.process(request("next", "song.set", {{"tempo", 89}})).value("ok").toBool());
	}

	void failedEmbeddedJournalSerializationPreservesUndoAndRedo()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		const auto healthy = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.25f)), 24000);
		const auto invalid = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(std::numeric_limits<float>::quiet_NaN())), 24000);
		clip->setSampleBuffer(healthy); clip->setName("Before");
		const auto journal = Engine::projectJournal(); const auto wasJournalling = journal->isJournalling();
		journal->clearJournal(); journal->setJournalling(true);
		journal->addJournalCheckPoint(clip); clip->setName("After"); QVERIFY(journal->canUndo());
		journal->undo(); QCOMPARE(clip->name(), QString("Before")); QVERIFY(journal->canRedo());
		clip->setSampleBuffer(invalid); journal->addJournalCheckPoint(clip);
		QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(journal->canRedo());
		Engine::getSong()->clearErrors(); journal->redo();
		QCOMPARE(clip->name(), QString("Before")); QVERIFY(journal->canRedo()); QVERIFY(Engine::getSong()->hasErrors());
		Engine::getSong()->clearErrors(); clip->setSampleBuffer(healthy); journal->redo();
		QCOMPARE(clip->name(), QString("After")); QVERIFY(journal->canUndo());
		clip->setSampleBuffer(invalid); journal->undo();
		QCOMPARE(clip->name(), QString("After")); QVERIFY(journal->canUndo()); QVERIFY(Engine::getSong()->hasErrors());
		Engine::getSong()->clearErrors(); clip->setSampleBuffer(healthy); journal->undo();
		QCOMPARE(clip->name(), QString("Before")); QVERIFY(journal->canRedo());
		QCOMPARE(clip->sample().toBase64(), healthy->toBase64());
		journal->setJournalling(wasJournalling); journal->clearJournal();
	}

	void incompleteEmbeddedProjectAndPresetLoadsRollbackNativeState_data()
	{
		QTest::addColumn<QString>("kind");
		QTest::newRow("sample-clip") << QString("sampleclip");
		QTest::newRow("audiofileprocessor") << QString("audiofileprocessor");
		QTest::newRow("slicert") << QString("slicert");
	}
	void incompleteEmbeddedProjectAndPresetLoadsRollbackNativeState()
	{
		QFETCH(QString, kind);
		if (kind != "sampleclip" && getPluginFactory()->pluginInfo(kind.toUtf8().constData()).isNull())
		{
			QSKIP("Full embedded project verification requires this native instrument");
		}
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("original.wav");
		QVERIFY(constantWave(source)); const auto accepted = SampleBuffer::fromFile(source); QVERIFY(accepted && !accepted->empty());
		const auto pcm = accepted->toBase64(); const auto rate = accepted->sampleRate();
		const auto session = directory.filePath("session"); StudioRuntime runtime(session); QString error;
		QVERIFY2(runtime.open(error), qPrintable(error)); QVERIFY(addNativeSampleConsumer(kind, source));
		const auto original = directory.filePath("original.mmp"), embedded = directory.filePath("embedded.mmp"), broken = directory.filePath("broken.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(original)); DataFile fixture(original);
		auto node = fixture.elementsByTagName(kind).at(0).toElement(); QVERIFY(!node.isNull());
		const auto dataAttribute = kind == "sampleclip" ? "data" : "sampledata";
		const auto rateAttribute = kind == "sampleclip" ? "sample_rate" : "sampleRate";
		node.setAttribute("src", ""); node.setAttribute(dataAttribute, pcm); node.setAttribute(rateAttribute, rate);
		QVERIFY(fixture.writeFile(embedded)); QVERIFY(QFile::remove(source));
		QVERIFY(runtime.process(request("embedded", "project.load", {{"path", embedded}})).value("ok").toBool());
		QVERIFY(!Engine::getSong()->hasErrors());
		const auto before = manifest(session);
		node.setAttribute(dataAttribute, pcm + "!"); QVERIFY(fixture.writeFile(broken));
		const auto rejected = runtime.process(request("broken-project", "project.load", {{"path", broken}}));
		QVERIFY(!rejected.value("ok").toBool()); QVERIFY(rejected.value("nativeWarnings").toString().contains("embedded", Qt::CaseInsensitive));
		const auto after = manifest(session);
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
		QVERIFY(!Engine::getSong()->hasErrors());
		const auto restored = directory.filePath("restored.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(restored));
		DataFile restoredFile(restored); const auto restoredNode = restoredFile.elementsByTagName(kind).at(0).toElement();
		QCOMPARE(restoredNode.attribute(dataAttribute), pcm); QCOMPARE(restoredNode.attribute(rateAttribute).toInt(), static_cast<int>(rate));
		QVERIFY(restoredNode.attribute("src").isEmpty());
		if (kind != "sampleclip")
		{
			const auto preset = directory.filePath("broken.xpf");
			QVERIFY(runtime.process(request("preset-save", "preset.save", {{"track", 0}, {"path", preset}})).value("ok").toBool());
			DataFile invalidPreset(preset); auto native = invalidPreset.elementsByTagName(kind).at(0).toElement(); QVERIFY(!native.isNull());
			native.setAttribute(dataAttribute, pcm + "!"); QVERIFY(invalidPreset.writeFile(preset));
			const auto priorPreset = manifest(session);
			const auto presetRejected = runtime.process(request("broken-preset", "preset.load", {{"track", 0}, {"path", preset}}));
			QVERIFY(!presetRejected.value("ok").toBool()); QVERIFY(presetRejected.value("nativeWarnings").toString().contains("embedded", Qt::CaseInsensitive));
			QCOMPARE(manifest(session).value("history"), priorPreset.value("history"));
			QCOMPARE(manifest(session).value("revision").toInt(), priorPreset.value("revision").toInt() + 1);
			const auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
			const auto state = instrumentSettings(instrument);
			QCOMPARE(state.attribute(dataAttribute), pcm); QCOMPARE(state.attribute(rateAttribute).toInt(), static_cast<int>(rate));
		}
	}

	void nativeAudioFileProcessorEmbeddedRateAndLegacyDefaultSurviveSerialization_data()
	{
		QTest::addColumn<bool>("explicitRate"); QTest::newRow("explicit-rate") << true; QTest::newRow("legacy-engine-rate") << false;
	}
	void nativeAudioFileProcessorEmbeddedRateAndLegacyDefaultSurviveSerialization()
	{
		QFETCH(bool, explicitRate);
		if (getPluginFactory()->pluginInfo("audiofileprocessor").isNull()) { QSKIP("Full verification requires native AudioFileProcessor"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "audiofileprocessor"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument();
		const std::vector<SampleFrame> frames(16703, SampleFrame(.25f, -.125f));
		const SampleBuffer source(frames, 24000); const auto pcm = source.toBase64();
		QDomDocument document; auto input = document.createElement("audiofileprocessor"); document.appendChild(input);
		input.setAttribute("src", ""); input.setAttribute("sampledata", pcm);
		if (explicitRate) { input.setAttribute("sampleRate", 24000); }
		NativeInstrumentTestAccess::load(native, input); QVERIFY(!Engine::getSong()->hasErrors());
		const auto restored = instrumentSettings(native);
		QCOMPARE(restored.attribute("sampledata"), pcm);
		QCOMPARE(restored.attribute("sampleRate").toInt(), explicitRate ? 24000 : static_cast<int>(Engine::audioEngine()->outputSampleRate()));
		input.setAttribute("sampleRate", "invalid"); NativeInstrumentTestAccess::load(native, input);
		QVERIFY(Engine::getSong()->hasErrors()); const auto retained = instrumentSettings(native);
		QCOMPARE(retained.attribute("sampledata"), pcm); QCOMPARE(retained.attribute("sampleRate"), restored.attribute("sampleRate"));
		Engine::getSong()->clearErrors();
	}

	void nativeRecoveryRejectsIncompleteEmbeddedCheckpointEvenWithMatchingDigest()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto session = directory.filePath("session");
		QJsonObject invalidManifest;
		{
			StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
			invalidManifest = manifest(session); auto history = invalidManifest.value("history").toArray();
			const auto cursor = invalidManifest.value("cursor").toInt(); auto snapshot = history.at(cursor).toObject();
			const auto checkpoint = QDir(session).filePath(snapshot.value("path").toString()); DataFile corrupted(checkpoint);
			auto clip = corrupted.elementsByTagName("sampleclip").at(0).toElement(); QVERIFY(!clip.isNull());
			const std::vector<SampleFrame> bad{SampleFrame(.25f, std::numeric_limits<float>::quiet_NaN())};
			const QByteArray nativeBytes(reinterpret_cast<const char*>(bad.data()), sizeof(SampleFrame));
			clip.setAttribute("src", ""); clip.setAttribute("data", QString::fromLatin1(nativeBytes.toBase64())); clip.setAttribute("sample_rate", 24000);
			QVERIFY(corrupted.writeFile(checkpoint));
			snapshot.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(readFixtureBytes(checkpoint), QCryptographicHash::Sha256).toHex()));
			history[cursor] = snapshot; invalidManifest.insert("history", history);
			QVERIFY(writeFixtureBytes(QDir(session).filePath("session.json"), QJsonDocument(invalidManifest).toJson()));
		}
		Engine::getSong()->clearProject(); StudioRuntime recovery(session); QString error;
		QVERIFY(!recovery.open(error)); QVERIFY(error.contains("nonfinite", Qt::CaseInsensitive));
		QCOMPARE(manifest(session), invalidManifest); Engine::getSong()->clearErrors();
	}

	void nativeDrumSynthSampleLoadingRetainsGeneratedPcmAndAcceptedInput()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath(QString(QChar(0x97f3)) + ".ds");
		QVERIFY(QFile::copy(QString(LMMS_TEST_DRUMSYNTH_DIR) + "/tr909/TR909_Kick.ds", path));
		const auto rate = Engine::audioEngine()->outputSampleRate(); DrumSynth synth; int16_t* samples = nullptr;
		const auto frames = synth.GetDSFileSamples(path, samples, 2, rate);
		const std::unique_ptr<int16_t[]> owner(samples); QVERIFY(frames > 0 && owner);
		const auto decoded = SampleDecoder::decode(path); QVERIFY(decoded); QCOMPARE(decoded->sampleRate, static_cast<int>(rate));
		QCOMPARE(decoded->data.size(), static_cast<std::size_t>(frames));
		const auto buffer = SampleBuffer::fromFile(path); QVERIFY(buffer && !buffer->empty());
		QCOMPARE(buffer->sampleRate(), rate); QCOMPARE(buffer->size(), decoded->data.size());
		QCOMPARE(buffer->audioFile(), path); QVERIFY(!buffer->acceptedFile().isEmpty());
		for (int frame = 0; frame < frames; ++frame)
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				QCOMPARE(decoded->data[frame][channel], samples[static_cast<std::size_t>(frame) * 2 + channel] / 32768.f);
				QCOMPARE(buffer->data()[frame][channel], decoded->data[frame][channel]);
			}
		}
		QVERIFY(QFile::remove(path));
		const auto retained = SampleDecoder::decode(buffer->acceptedFile()); QVERIFY(retained);
		QCOMPARE(retained->data.size(), decoded->data.size());
		QVERIFY(std::equal(retained->data.begin(), retained->data.end(), decoded->data.begin(), [](const SampleFrame& a, const SampleFrame& b) { return a.left() == b.left() && a.right() == b.right(); }));
		{ FileResourceIO::Scope cancelled([] { return true; }); QVERIFY(!SampleDecoder::decode(buffer->acceptedFile())); }
		QVERIFY(SampleDecoder::decode(buffer->acceptedFile()));
	}

	void nativeSampleDecoderReadsMultipleBlocksAndRetainsChannelBehavior_data()
	{
		QTest::addColumn<int>("channels");
		QTest::newRow("mono") << 1; QTest::newRow("stereo") << 2; QTest::newRow("six-channel") << 6;
	}
	void nativeSampleDecoderReadsMultipleBlocksAndRetainsChannelBehavior()
	{
		QFETCH(int, channels); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath(QString(QChar(0x97f3)) + ".wav");
		constexpr int frames = 16701, rate = 24000;
		QVERIFY(diagnosticAudio(path, channels, rate, frames, SF_FORMAT_WAV | SF_FORMAT_FLOAT));
		const auto decoded = SampleDecoder::decode(path); QVERIFY(decoded); QCOMPARE(decoded->sampleRate, rate);
		QCOMPARE(decoded->data.size(), std::size_t(frames));
		for (int frame = 0; frame < frames; ++frame)
		{
			QCOMPARE(decoded->data[frame][0], diagnosticAudioValue(frame, 0, rate));
			QCOMPARE(decoded->data[frame][1], diagnosticAudioValue(frame, channels == 1 ? 0 : 1, rate));
		}
	}

	void nativeSampleDecoderRejectsLateNonfiniteChannelsAndCancelledReads()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("large.wav");
		QVERIFY(diagnosticAudio(path, 6, 24000, 16701, SF_FORMAT_WAV | SF_FORMAT_FLOAT, true));
		QVERIFY(!SampleDecoder::decode(path));
		QVERIFY(diagnosticAudio(path, 2, 24000, 16701, SF_FORMAT_WAV | SF_FORMAT_FLOAT));
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 9; });
			QVERIFY(!SampleDecoder::decode(path)); QVERIFY(checks >= 9);
		}
		QVERIFY(SampleDecoder::decode(path)); QVERIFY(QFile::remove(path));
	}

	void nativeVorbisFallbackPreservesPlanarChannelsAndCompleteFrames_data()
	{
		QTest::addColumn<int>("channels"); QTest::newRow("mono") << 1; QTest::newRow("stereo") << 2;
	}
	void nativeVorbisFallbackPreservesPlanarChannelsAndCompleteFrames()
	{
#ifdef LMMS_HAVE_OGGVORBIS
		QFETCH(int, channels); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto path = directory.filePath(QString(QChar(0x97f3)) + ".ogg");
		constexpr int frames = 16701, rate = 24000;
		QVERIFY(diagnosticAudio(path, channels, rate, frames, SF_FORMAT_OGG | SF_FORMAT_VORBIS));
		const auto decoded = detail::decodeSampleOggVorbis(path); QVERIFY(decoded); QCOMPARE(decoded->sampleRate, rate);
		QCOMPARE(decoded->data.size(), std::size_t(frames));
		std::array<double, 2> squares{};
		for (int frame = 256; frame < frames - 256; ++frame)
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				const auto value = decoded->data[frame][channel]; QVERIFY(std::isfinite(value));
				const auto difference = value - diagnosticAudioValue(frame, channels == 1 ? 0 : channel, rate);
				squares[channel] += difference * difference;
			}
		}
		QVERIFY(squares[0] / (frames - 512) < .0001); QVERIFY(squares[1] / (frames - 512) < .0001);
		if (channels == 1)
		{
			for (const auto& frame : decoded->data) { QCOMPARE(frame[0], frame[1]); }
		}
		const auto& types = SampleDecoder::supportedAudioTypes();
		QCOMPARE(std::count_if(types.begin(), types.end(), [](const auto& type) { return type.extension == "ogg"; }), 1);
		QVERIFY(QFile::remove(path));
#else
		QSKIP("Native Vorbis fallback verification requires the full Vorbis build");
#endif
	}

	void nativeVorbisChainsNormalizeRateAndRetainChangingChannels_data()
	{
		QTest::addColumn<int>("firstRate"); QTest::addColumn<int>("secondRate");
		QTest::addColumn<int>("firstChannels"); QTest::addColumn<int>("secondChannels");
		QTest::newRow("same-rate-mono-to-stereo") << 24000 << 24000 << 1 << 2;
		QTest::newRow("downsample-stereo-to-mono") << 24000 << 48000 << 2 << 1;
		QTest::newRow("upsample-mono-to-stereo") << 48000 << 16000 << 1 << 2;
	}

	void nativeVorbisChainsNormalizeRateAndRetainChangingChannels()
	{
#ifdef LMMS_HAVE_OGGVORBIS
		QFETCH(int, firstRate); QFETCH(int, secondRate); QFETCH(int, firstChannels); QFETCH(int, secondChannels);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto firstPath = directory.filePath("first.ogg"), secondPath = directory.filePath("second.ogg"), chainPath = directory.filePath("chain.ogg");
		const int firstFrames = firstRate / 2 + 137, secondFrames = secondRate / 2 + 213;
		QVERIFY(diagnosticAudio(firstPath, firstChannels, firstRate, firstFrames, SF_FORMAT_OGG | SF_FORMAT_VORBIS));
		const auto first = readFixtureBytes(firstPath); QVERIFY(first.size() >= 27 && first.startsWith("OggS"));
		QByteArray second;
		for (int attempt = 0; attempt < 4; ++attempt)
		{
			QVERIFY(diagnosticAudio(secondPath, secondChannels, secondRate, secondFrames, SF_FORMAT_OGG | SF_FORMAT_VORBIS));
			second = readFixtureBytes(secondPath); QVERIFY(second.size() >= 27 && second.startsWith("OggS"));
			if (qFromLittleEndian<quint32>(first.constData() + 14) != qFromLittleEndian<quint32>(second.constData() + 14)) { break; }
		}
		QVERIFY(qFromLittleEndian<quint32>(first.constData() + 14) != qFromLittleEndian<quint32>(second.constData() + 14));
		QVERIFY(writeFixtureBytes(chainPath, first + second));
		const auto decoded = detail::decodeSampleOggVorbis(chainPath), baseline = detail::decodeSampleOggVorbis(firstPath);
		QVERIFY(decoded && baseline); QCOMPARE(decoded->sampleRate, firstRate);
		const auto automatic = SampleDecoder::decode(chainPath); QVERIFY(automatic);
		QCOMPARE(automatic->data.size(), decoded->data.size()); QCOMPARE(automatic->sampleRate, decoded->sampleRate);
		QCOMPARE(baseline->data.size(), std::size_t(firstFrames));
		const auto scaled = qint64(secondFrames) * firstRate;
		const auto normalizedFrames = static_cast<std::size_t>(scaled / secondRate + (scaled % secondRate != 0));
		QVERIFY(decoded->data.size() >= firstFrames + normalizedFrames - 1 && decoded->data.size() <= firstFrames + normalizedFrames);
		for (int frame = 0; frame < firstFrames; ++frame)
		{
			QCOMPARE(decoded->data[frame][0], baseline->data[frame][0]); QCOMPARE(decoded->data[frame][1], baseline->data[frame][1]);
		}
		for (std::size_t frame = 0; frame < decoded->data.size(); ++frame)
		{
			QCOMPARE(automatic->data[frame][0], decoded->data[frame][0]); QCOMPARE(automatic->data[frame][1], decoded->data[frame][1]);
		}
		std::array<double, 2> squares{}; std::size_t compared = 0;
		for (std::size_t frame = 512; frame + firstFrames + 512 < decoded->data.size(); ++frame)
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				const auto sample = decoded->data[frame + firstFrames][channel]; QVERIFY(std::isfinite(sample));
				const auto difference = sample - diagnosticAudioValue(static_cast<int>(frame), secondChannels == 1 ? 0 : channel, firstRate);
				squares[channel] += difference * difference;
			}
			++compared;
		}
		QVERIFY(compared > 1000); QVERIFY(squares[0] / compared < .0001); QVERIFY(squares[1] / compared < .0001);
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 40; });
			QVERIFY(!detail::decodeSampleOggVorbis(chainPath)); QVERIFY(checks >= 40);
		}
		QVERIFY(detail::decodeSampleOggVorbis(chainPath)); QVERIFY(QFile::remove(chainPath));
#else
		QSKIP("Native chain verification requires the full Vorbis build");
#endif
	}

	void nativeResamplerEndOfInputDrainsTheCompleteConvertedTail()
	{
		constexpr int frames = 16701, sourceRate = 32000, targetRate = 48000;
		std::vector<SampleFrame> input(frames);
		for (int frame = 0; frame < frames; ++frame)
		{
			input[frame] = {diagnosticAudioValue(frame, 0, sourceRate), diagnosticAudioValue(frame, 1, sourceRate)};
		}
		AudioResampler resampler(AudioResampler::Mode::SincBest); resampler.setRatio(sourceRate, targetRate);
		std::array<SampleFrame, 257> output; std::vector<SampleFrame> converted; std::size_t consumed = 0;
		while (consumed < input.size())
		{
			const auto block = resampler.process({input[consumed].data(), 2, input.size() - consumed}, {output[0].data(), 2, output.size()});
			QVERIFY(block.inputFramesUsed > 0 || block.outputFramesGenerated > 0);
			QVERIFY(block.inputFramesUsed <= input.size() - consumed && block.outputFramesGenerated <= output.size());
			converted.insert(converted.end(), output.begin(), output.begin() + block.outputFramesGenerated); consumed += block.inputFramesUsed;
			QVERIFY(converted.size() <= (frames * 3 + 1) / 2);
		}
		const auto beforeDrain = converted.size();
		for (;;)
		{
			const auto tail = resampler.process({input[0].data(), 2, 0}, {output[0].data(), 2, output.size()}, true);
			QCOMPARE(tail.inputFramesUsed, f_cnt_t(0)); QVERIFY(tail.outputFramesGenerated <= output.size());
			converted.insert(converted.end(), output.begin(), output.begin() + tail.outputFramesGenerated);
			QVERIFY(converted.size() <= (frames * 3 + 1) / 2);
			if (tail.outputFramesGenerated == 0) { break; }
		}
		QVERIFY(converted.size() > beforeDrain);
		QVERIFY(converted.size() >= frames * 3 / 2 && converted.size() <= (frames * 3 + 1) / 2);
		for (std::size_t frame = 512; frame + 512 < converted.size(); ++frame)
		{
			QVERIFY(std::isfinite(converted[frame][0]));
			QVERIFY(std::abs(converted[frame][0] - diagnosticAudioValue(static_cast<int>(frame), 0, targetRate)) < .002);
		}
	}

	void nativeVorbisFallbackRejectsCorruptionAndStopsWithoutRetainingItsFile()
	{
#ifdef LMMS_HAVE_OGGVORBIS
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("sound.ogg");
		QVERIFY(diagnosticAudio(path, 2, 24000, 16701, SF_FORMAT_OGG | SF_FORMAT_VORBIS));
		const auto bytes = readFixtureBytes(path); QVERIFY(bytes.size() > 64);
		QVERIFY(writeFixtureBytes(path, bytes.left(32))); QVERIFY(!detail::decodeSampleOggVorbis(path));
		QVERIFY(writeFixtureBytes(path, bytes));
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 9; });
			QVERIFY(!detail::decodeSampleOggVorbis(path)); QVERIFY(checks >= 9);
		}
		QVERIFY(detail::decodeSampleOggVorbis(path)); QVERIFY(QFile::remove(path));
#else
		QSKIP("Native Vorbis fallback verification requires the full Vorbis build");
#endif
	}

	void nativeSampleDecoderRejectsEmptyAndNonfiniteFiles()
	{
		QTemporaryDir directory;
		const auto invalidPath = directory.filePath("empty.wav");
		QFile invalid(invalidPath);
		QVERIFY(invalid.open(QIODevice::WriteOnly)); invalid.close();
		QVERIFY(SampleBuffer::fromFile(invalidPath)->empty());
		const auto nonfinitePath = directory.filePath("nonfinite.wav");
		QVERIFY(constantWave(nonfinitePath, std::numeric_limits<float>::quiet_NaN(), SF_FORMAT_FLOAT));
		QVERIFY(SampleBuffer::fromFile(nonfinitePath)->empty());
	}

	void nativeSampleReadScopeRetainsExactBuffersOnlyWhenRequested_data()
	{
		QTest::addColumn<bool>("retain"); QTest::newRow("clone-resident-audio") << true; QTest::newRow("ordinary-export") << false;
	}
	void nativeSampleReadScopeRetainsExactBuffersOnlyWhenRequested()
	{
		QFETCH(bool, retain); QTemporaryDir directory; const auto source = directory.filePath("same.wav");
		QVERIFY(constantWave(source, .25f)); auto first = SampleBuffer::fromFile(source); QVERIFY(!first->empty());
		QVERIFY(constantWave(source, -.5f)); auto second = SampleBuffer::fromFile(source); QVERIFY(!second->empty());
		const auto firstPath = first->acceptedFile(); std::weak_ptr<const SampleBuffer> firstOwner = first;
		std::shared_ptr<const SampleBuffer> copy;
		{
			DataFile document(DataFile::Type::JournalData); auto node = document.createElement("tripleoscillator");
			node.setAttribute("userwavefile0", source); node.setAttribute("userwavefile1", source);
			{
				DataFile::ResourceSourceScope save(document, true, retain);
				first->registerResourceSource(document, node, "userwavefile0", first);
				second->registerResourceSource(document, node, "userwavefile1", second);
			}
			QVERIFY(QFile::remove(source));
			{
				DataFile::ResourceReadScope read(document); DataFile::ResourceReferenceScope resource(node, "userwavefile0");
				copy = SampleBuffer::fromFile(source); QVERIFY(!copy->empty());
				QCOMPARE(copy->data()[0][0], .25f); QCOMPARE(copy->sampleRate(), first->sampleRate()); QCOMPARE(copy->audioFile(), first->audioFile());
				QCOMPARE(copy.get() == first.get(), retain);
				{
					DataFile::ResourceReferenceScope other(node, "userwavefile1"); auto otherCopy = SampleBuffer::fromFile(source);
					QCOMPARE(otherCopy->data()[0][0], -.5f); QCOMPARE(otherCopy.get() == second.get(), retain);
				}
				QCOMPARE(SampleBuffer::fromFile(source)->data()[0][0], .25f);
			}
			first.reset(); QCOMPARE(firstOwner.expired(), !retain); QVERIFY(QFile::exists(firstPath));
		}
		QCOMPARE(QFile::exists(firstPath), retain); QCOMPARE(copy->data()[0][0], .25f);
		const auto copyPath = copy->acceptedFile(); copy.reset(); QVERIFY(firstOwner.expired()); QVERIFY(!QFile::exists(copyPath));
	}

	void nativeResourceTrackClonesKeepDistinctAcceptedVersions_data()
	{
		QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("remove");
		for (const auto kind : {"audiofileprocessor", "tripleoscillator", "slicert", "patman", "sf2player"})
		{
			QTest::newRow(qPrintable(QString(kind) + "-removed")) << QString(kind) << true;
			QTest::newRow(qPrintable(QString(kind) + "-changed")) << QString(kind) << false;
		}
	}
	void nativeResourceTrackClonesKeepDistinctAcceptedVersions()
	{
		QFETCH(QString, kind); QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo(kind.toUtf8().constData()).isNull()) { QSKIP("Full cloning verification requires this native instrument"); }
		QTemporaryDir directory; const bool patch = kind == "patman", soundfont = kind == "sf2player", pcm = !patch && !soundfont;
		const auto source = directory.filePath("same." + QString(patch ? "pat" : soundfont ? "sf2" : "wav"));
		std::array<QByteArray, 2> accepted; std::array<InstrumentTrack*, 2> originals{}, copies{};
		std::array<std::shared_ptr<const SampleBuffer>, 2> buffers;
		const auto attribute = kind == "tripleoscillator" ? QString("userwavefile0") : QString("src");
		const auto resident = [&](InstrumentTrack* track) {
			DataFile document(DataFile::Type::JournalData); auto node = document.createElement(kind);
			{ DataFile::ResourceSourceScope save(document, true, true); NativeInstrumentTestAccess::save(track->instrument(), document, node); }
			DataFile::ResourceReadScope read(document); DataFile::ResourceReferenceScope reference(node, attribute);
			return SampleBuffer::fromFile(node.attribute(attribute));
		};
		for (int index = 0; index < 2; ++index)
		{
			if (pcm) { QVERIFY(constantWave(source, index ? -.5f : .25f)); }
			else
			{
				auto bytes = nativeFixture(soundfont ? (index ? "native-layered.sf2" : "native-calibration.sf2") : "native-gus-s16.pat");
				if (patch && index) { bytes = silencedFirstGusSample(bytes); }
				QVERIFY(!bytes.isEmpty()); QVERIFY(writeFixtureBytes(source, bytes));
			}
			accepted[index] = readFixtureBytes(source);
			if (soundfont)
			{
				QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
				QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", index}, {"instrument", kind}}).value("ok").toBool());
				QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", index}, {"resource", "soundfont"}, {"path", source}}).value("ok").toBool());
			}
			else { QVERIFY(addNativeSampleConsumer(kind, source)); }
			originals[index] = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(index));
			if (pcm) { buffers[index] = resident(originals[index]); QVERIFY(!buffers[index]->empty()); }
		}
		QVERIFY(accepted[0] != accepted[1]);
		if (remove) { QVERIFY(QFile::remove(source)); }
		else if (pcm) { QVERIFY(constantWave(source, .75f)); }
		else { QVERIFY(writeFixtureBytes(source, accepted[1])); }
		for (int index = 0; index < 2; ++index)
		{
			copies[index] = static_cast<InstrumentTrack*>(originals[index]->clone()); QVERIFY2(copies[index], qPrintable(Engine::getSong()->errorSummary()));
			QCOMPARE(instrumentSettings(copies[index]->instrument()).attribute(attribute), instrumentSettings(originals[index]->instrument()).attribute(attribute));
			if (pcm) { QCOMPARE(resident(copies[index]).get(), buffers[index].get()); }
		}
		std::array<std::vector<SampleFrame>, 2> before;
		if (patch) { before = {nativePatmanPeriod(0), nativePatmanPeriod(1)}; }
		delete originals[1]; delete originals[0]; // Bank files must not be tied to their previous owner.
		if (patch)
		{
			for (int index = 0; index < 2; ++index)
			{
				const auto output = nativePatmanPeriod(index); QCOMPARE(output.size(), before[index].size());
				for (std::size_t frame = 0; frame < output.size(); ++frame) { QCOMPARE(output[frame][0], before[index][frame][0]); QCOMPARE(output[frame][1], before[index][frame][1]); }
			}
		}
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("clones.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString(); DataFile packaged(path); const auto nodes = packaged.elementsByTagName(kind); QCOMPARE(nodes.size(), 2);
		for (int index = 0; index < 2; ++index)
		{
			const auto reference = nodes.at(index).toElement().attribute(attribute); QVERIFY(reference.startsWith("local:resources/"));
			QCOMPARE(readFixtureBytes(QFileInfo(path).absoluteDir().filePath(reference.mid(6))), accepted[index]);
		}
		QVERIFY(StudioCapabilities::execute("project.load", {{"path", path}}).value("ok").toBool()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void nativeOscillatorCloneKeepsEachAcceptedWaveAttribute()
	{
		if (getPluginFactory()->pluginInfo("tripleoscillator").isNull()) { QSKIP("Full verification requires native TripleOscillator"); }
		QTemporaryDir directory; const auto source = directory.filePath("wave.wav"); QVERIFY(constantWave(source, .25f));
		QVERIFY(addNativeSampleConsumer("tripleoscillator", source));
		auto original = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QString error;
		QVERIFY(constantWave(source, -.5f)); QVERIFY(original->instrument()->setFileResource("oscillator2", source, error));
		QVERIFY(constantWave(source, .75f)); QVERIFY(original->instrument()->setFileResource("oscillator3", source, error));
		QVERIFY(QFile::remove(source));
		auto copy = static_cast<InstrumentTrack*>(original->clone()); QVERIFY2(copy, qPrintable(Engine::getSong()->errorSummary())); delete original;
		DataFile document(DataFile::Type::JournalData); auto node = document.createElement("tripleoscillator");
		{ DataFile::ResourceSourceScope save(document, true, true); NativeInstrumentTestAccess::save(copy->instrument(), document, node); }
		DataFile::ResourceReadScope read(document); const std::array<float, 3> expected{.25f, -.5f, .75f};
		for (int index = 0; index < 3; ++index)
		{
			const auto attribute = "userwavefile" + QString::number(index); DataFile::ResourceReferenceScope reference(node, attribute);
			const auto buffer = SampleBuffer::fromFile(node.attribute(attribute)); QVERIFY(!buffer->empty());
			QVERIFY(std::abs(buffer->data()[0][0] - expected[index]) <= 1.f / 32768.f); // One 16-bit PCM quantization step.
		}
	}

	void nativeResourceCloneRefusesDamagedSamplesAndCleansPartialTracks_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("changed-accepted") << 0;
		QTest::newRow("missing-accepted") << 1; QTest::newRow("cancelled-accepted-read") << 2;
	}
	void nativeResourceCloneRefusesDamagedSamplesAndCleansPartialTracks()
	{
		QFETCH(int, fault);
		if (getPluginFactory()->pluginInfo("audiofileprocessor").isNull()) { QSKIP("Full verification requires native AudioFileProcessor"); }
		QTemporaryDir directory; const auto source = directory.filePath("sample.wav"); QVERIFY(constantWave(source));
		QVERIFY(addNativeSampleConsumer("audiofileprocessor", source));
		auto original = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		DataFile document(DataFile::Type::JournalData); auto node = document.createElement("audiofileprocessor");
		{ DataFile::ResourceSourceScope save(document, true, true); NativeInstrumentTestAccess::save(original->instrument(), document, node); }
		std::shared_ptr<const SampleBuffer> buffer;
		{
			DataFile::ResourceReadScope read(document); DataFile::ResourceReferenceScope resource(node, "src"); buffer = SampleBuffer::fromFile(source);
		}
		QVERIFY(!buffer->empty()); const auto accepted = buffer->acceptedFile(); const auto bytes = readFixtureBytes(accepted); QVERIFY(!bytes.isEmpty());
		if (fault == 0) { QVERIFY(constantWave(accepted, -.5f)); }
		if (fault == 1) { QVERIFY(QFile::remove(accepted)); }
		{
			// Cancellation starts only after the duplicate has been constructed.
			FileResourceIO::Scope work([&] { return fault == 2 && Engine::getSong()->tracks().size() > 1; });
			QVERIFY(original->clone() == nullptr);
		}
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks().front(), original);
		QCOMPARE(buffer->data()[0][0], .25f); QVERIFY(Engine::getSong()->hasErrors());
		QVERIFY(writeFixtureBytes(accepted, bytes)); Engine::getSong()->clearErrors(); QVERIFY(QFile::remove(source));
		auto recovered = original->clone(); QVERIFY2(recovered, qPrintable(Engine::getSong()->errorSummary())); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(2));
	}

	void nativeSampleBufferKeepsAcceptedBytesAndSnapshotLifetime()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + ".wav");
		QVERIFY(constantWave(source, .25f)); const auto firstBytes = readFixtureBytes(source);
		auto first = SampleBuffer::fromFile(source); QVERIFY(!first->empty());
		const auto firstPath = first->acceptedFile(); QVERIFY(!firstPath.isEmpty());
		QVERIFY(firstPath != source); QCOMPARE(first->sampleRate(), sample_rate_t(8000));
		QCOMPARE(first->size(), std::size_t(64)); QCOMPARE(first->data()[0][0], .25f);
		QVERIFY(constantWave(source, -.5f)); const auto secondBytes = readFixtureBytes(source);
		auto second = SampleBuffer::fromFile(source); QVERIFY(!second->empty());
		const auto secondPath = second->acceptedFile(); QVERIFY(firstPath != secondPath);
		QVERIFY(QFile::remove(source));
		QCOMPARE(readFixtureBytes(firstPath), firstBytes); QCOMPARE(readFixtureBytes(secondPath), secondBytes);
		QCOMPARE(first->data()[0][0], .25f); QCOMPARE(second->data()[0][0], -.5f);
		{
			SampleBuffer copy(*first), empty;
			swap(copy, empty); QVERIFY(copy.empty()); QVERIFY(copy.acceptedFile().isEmpty());
			QCOMPARE(empty.acceptedFile(), firstPath); QCOMPARE(empty.audioFile(), first->audioFile());
			QCOMPARE(empty.data()[0][0], .25f);
			first.reset(); QVERIFY(QFile::exists(firstPath));
		}
		QVERIFY(!QFile::exists(firstPath)); QVERIFY(QFile::exists(secondPath));
		second.reset(); QVERIFY(!QFile::exists(secondPath));
	}

	void nativeLfoWaveformUsesDecodedSamplesAndKeepsStateOnFailure()
	{
		QTemporaryDir directory;
		const auto path = directory.filePath("wave.wav");
		QVERIFY(constantWave(path));
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", path}, {"select", true}}).value("ok").toBool());
		auto lfo = static_cast<LfoController*>(Engine::getSong()->controllers().at(0));
		QCOMPARE(lfo->userWave().size(), std::size_t(64));
		QCOMPARE(lfo->userWave().sampleRate(), sample_rate_t(8000));
		QCOMPARE(lfo->currentValue(0), 0.625f); // Native LFO: base 0.5 + amount 1 * sample 0.25 / 2.
		const auto source = lfo->userWave().data();
		QFile invalid(directory.filePath("invalid.wav"));
		QVERIFY(invalid.open(QIODevice::WriteOnly)); QCOMPARE(invalid.write("invalid waveform"), qint64(16)); invalid.close();
		QVERIFY(!StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", invalid.fileName()}}).value("ok").toBool());
		QCOMPARE(lfo->userWave().data(), source);
		QCOMPARE(lfo->currentValue(0), 0.625f);
		QVERIFY(StudioCapabilities::execute("controller.waveform.set", {{"controller", 0}, {"path", ""}}).value("ok").toBool());
		QVERIFY(lfo->userWave().empty());
		QCOMPARE(lfo->currentValue(0), 0.5f);
	}

	void lfoWaveformAndNativeSelectorSurviveRecovery()
	{
		QTemporaryDir directory;
		const auto path = directory.filePath("wave.wav");
		QVERIFY(constantWave(path));
		{
			StudioRuntime studio(directory.filePath("session")); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("lfo", "controller.add", {{"type", "lfo"}})).value("ok").toBool());
			QVERIFY(studio.process(request("wave", "controller.waveform.set", {{"controller", 0}, {"path", path}, {"select", true}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.filePath("session")); QString error;
		QVERIFY2(recovered.open(error), qPrintable(error));
		auto lfo = static_cast<LfoController*>(Engine::getSong()->controllers().at(0));
		QCOMPARE(lfo->waveModel().value(), static_cast<int>(Oscillator::WaveShape::UserDefined));
		QCOMPARE(lfo->userWave().size(), std::size_t(64));
		QCOMPARE(lfo->currentValue(0), 0.625f);
	}

	void samplePlaybackSnapshotsDoNotEditSourceState()
	{
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong()));
		auto clip = static_cast<SampleClip*>(track->createClip(0));
		const auto rate = Engine::audioEngine()->outputSampleRate();
		std::vector<SampleFrame> frames(rate * 2, SampleFrame{0.125f, 0.125f});
		std::fill(frames.begin() + rate, frames.end(), SampleFrame{0.75f, 0.75f});
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(frames.data(), frames.size(), rate));
		clip->setAutoResize(false);
		clip->changeLength(192);
		QVERIFY(clip->setSampleWindow(1000, rate / 2));
		clip->sample().setAmplification(0.5f);
		const auto playback = clip->playbackSample(0);
		QVERIFY(playback);
		SamplePlayHandle handle(clip, *playback);
		const auto duration = handle.totalFrames();
		QCOMPARE(duration, f_cnt_t(rate / 2 - 1000));
		const auto later = clip->playbackSample(3);
		QVERIFY(later);
		QCOMPARE(later->startFrame(), 1000 + static_cast<int>(3 * Engine::framesPerTick(rate)));
		QCOMPARE(clip->sample().startFrame(), 1000);
		QCOMPARE(clip->sample().endFrame(), rate / 2);
		QVERIFY(!clip->setSampleWindow(2000, 1000));
		QCOMPARE(clip->sample().startFrame(), 1000);
		clip->sample().setAmplification(2.0f);
		clip->reverseSample();
		QCOMPARE(handle.totalFrames(), duration);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod());
		handle.play(output.data());
		// The actual native DSP must use the snapshot gain, not subsequent edits.
		QVERIFY(std::abs(output.at(32).left() - 0.0625f) < 0.0001f);
		QCOMPARE(clip->sample().startFrame(), rate * 2 - rate / 2);
		QCOMPARE(clip->sample().endFrame(), rate * 2 - 1000);
		clip->reverseSample();
		QCOMPARE(clip->sample().startFrame(), 1000);
		QCOMPARE(clip->sample().endFrame(), rate / 2);
		QCOMPARE(clip->startTimeOffset().getTicks(), 0);
		clip->setStartTimeOffset(-3);
		QVERIFY(clip->playbackSample(0)->startFrame() > 1000);
		clip->setStartTimeOffset(3);
		QVERIFY(!clip->playbackSample(0));
		QVERIFY(!clip->playbackSample(clip->length()));
	}

	void tinyAndTrimmedSampleWaveformsUseValidSourceRanges()
	{
		const SampleFrame frames[]{{0.125f, -0.125f}, {0.125f, -0.125f}, {0.75f, -0.75f}, {0.75f, -0.75f}};
		Sample sample(frames, 4, 44100);
		gui::SampleThumbnail thumbnail(sample);
		QImage image(100, 100, QImage::Format_ARGB32);
		image.fill(Qt::black);
		{
			QPainter painter(&image); painter.setPen(Qt::white);
			thumbnail.visualize({.sampleRect = image.rect(), .sampleStart = 0.5f, .sampleEnd = 1.0f}, painter);
		}
		QVERIFY(qRed(image.pixel(50, 20)) > 0); // Draw the selected loud half, including both channels.
		image.fill(Qt::black);
		{
			QPainter painter(&image); painter.setPen(Qt::white);
			thumbnail.visualize({.sampleRect = image.rect(), .viewportRect = QRect(20, 0, 10, 100),
				.sampleStart = 0.0f, .sampleEnd = 0.5f, .reversed = true}, painter);
		}
		QVERIFY(qRed(image.pixel(25, 20)) > 0);
		QCOMPARE(qRed(image.pixel(50, 20)), 0); // Cropping must not move the source window.
		gui::SampleThumbnail empty{Sample{}};
		{
			QPainter painter(&image);
			empty.visualize({.sampleRect = image.rect()}, painter); // Empty data must not be dereferenced.
		}
	}

	void sampleSourceSettingsSurviveXmlAndLegacyReload()
	{
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong()));
		auto clip = static_cast<SampleClip*>(track->createClip(192));
		std::vector<SampleFrame> frames(8000, SampleFrame{0.125f, -0.125f});
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(frames.data(), frames.size(), 8000));
		clip->setAutoResize(false);
		clip->changeLength(96);
		clip->setStartTimeOffset(-12);
		clip->setName("Trimmed recording");
		QVERIFY(clip->setSampleWindow(1000, 7000));
		clip->sample().setAmplification(-0.5f);
		clip->sample().setReversed(true);
		QDomDocument document;
		auto element = document.createElement("sampleclip");
		clip->saveSettings(document, element);
		auto restored = static_cast<SampleClip*>(track->createClip(0));
		restored->loadSettings(element);
		QCOMPARE(restored->sample().startFrame(), 1000);
		QCOMPARE(restored->sample().endFrame(), 7000);
		QCOMPARE(restored->sample().amplification(), -0.5f);
		QVERIFY(restored->sample().reversed());
		QCOMPARE(restored->sample().sampleRate(), 8000);
		QCOMPARE(restored->name(), QString("Trimmed recording"));
		QCOMPARE(restored->startTimeOffset().getTicks(), -12);
		QCOMPARE(restored->length().getTicks(), 96);
		// A legacy project has no new attributes and must restore the full buffer.
		element.removeAttribute("sample_start"); element.removeAttribute("sample_end");
		element.removeAttribute("sample_gain"); element.removeAttribute("reversed");
		restored->loadSettings(element);
		QCOMPARE(restored->sample().startFrame(), 0);
		QCOMPARE(restored->sample().endFrame(), 8000);
		QCOMPARE(restored->sample().amplification(), 1.0f);
		QVERIFY(!restored->sample().reversed());
	}

	void nativeSplitPreservesSamplesAndEmptyNoteDetuning()
	{
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong()));
		auto clip = static_cast<SampleClip*>(track->createClip(192));
		std::vector<SampleFrame> frames(44100, SampleFrame{0.125f, 0.125f});
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(frames.data(), frames.size(), 44100));
		clip->setAutoResize(false); clip->changeLength(96); clip->setStartTimeOffset(-6);
		QVERIFY(clip->setSampleWindow(1000, 40000));
		QVERIFY(!clip->splitAt(0)); QVERIFY(!clip->splitAt(96));
		auto right = dynamic_cast<SampleClip*>(clip->splitAt(48));
		QVERIFY(right);
		QCOMPARE(clip->length().getTicks(), 48);
		QCOMPARE(right->startPosition().getTicks(), 240);
		QCOMPARE(right->length().getTicks(), 48);
		QCOMPARE(right->startTimeOffset().getTicks(), -54);
		QCOMPARE(right->sample().startFrame(), 1000);
		QCOMPARE(right->sample().endFrame(), 40000);
		QVERIFY(right->sample().buffer() == clip->sample().buffer());
		// The source clip is now shorter; compare against the original native offset formula.
		QCOMPARE(right->playbackSample(0)->startFrame(), 1000 + static_cast<int>(54 * Engine::framesPerTick(44100)));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 1}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.add", {{"track", 1}, {"clip", 0}, {"position", 0}, {"length", 96}, {"key", 60}}).value("ok").toBool());
		auto midi = dynamic_cast<MidiClip*>(Engine::getSong()->tracks().at(1)->getClip(0));
		DetuningHelper empty;
		DetuningHelper copy(empty); // A helper can exist before its lazy curve.
		QVERIFY(!copy.hasAutomation());
		midi->notes().front()->createDetuning();
		QVERIFY(midi->splitAt(48));
		QVERIFY(Engine::getSong()->tracks().at(1)->getClip(1) != midi);
	}

	void sampleEditingPersistsThroughRuntimeRecovery()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path()); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool());
			QVERIFY(studio.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
			auto clip = dynamic_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
			std::vector<SampleFrame> frames(8000, SampleFrame{0.125f, 0.125f});
			clip->setSampleBuffer(std::make_shared<SampleBuffer>(frames.data(), frames.size(), 8000));
			QVERIFY(studio.process(request("edit", "sample.edit", {{"track", 0}, {"clip", 0},
				{"startFrame", 1000}, {"endFrame", 7000}, {"gain", 0.25}, {"reversed", true}})).value("ok").toBool());
			QVERIFY(!studio.process(request("bad", "sample.edit", {{"track", 0}, {"clip", 0},
				{"startFrame", 7500}, {"endFrame", 7000}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.path()); QString error;
		QVERIFY2(recovered.open(error), qPrintable(error));
		auto clip = dynamic_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		QCOMPARE(clip->sample().startFrame(), 1000);
		QCOMPARE(clip->sample().endFrame(), 7000);
		QCOMPARE(clip->sample().amplification(), 0.25f);
		QVERIFY(clip->sample().reversed());
	}

	void controllerRewiringAndDeletionRetainParameterOwnership()
	{
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}, {"name", "First"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}, {"name", "Second"}}).value("ok").toBool());
		auto first = Engine::getSong()->controllers().at(0);
		auto second = Engine::getSong()->controllers().at(1);
		FloatModel target(0.3f, 0.f, 1.f, 0.001f, Engine::getSong(), "Test parameter");
		const auto id = static_cast<int>(target.id());
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", id}}).value("ok").toBool());
		auto connection = target.controllerConnection();
		QVERIFY(connection); QCOMPARE(first->connectionCount(), 1);
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"controller", 1}, {"parameter", id}}).value("ok").toBool());
		QCOMPARE(target.controllerConnection(), connection);
		QCOMPARE(first->connectionCount(), 0); QCOMPARE(second->connectionCount(), 1);
		QVERIFY(StudioCapabilities::execute("controller.remove", {{"controller", 0}}).value("ok").toBool());
		QCOMPARE(target.controllerConnection(), connection); // Old destroyed signal must have been disconnected.
		QCOMPARE(connection->getController(), second);
		QVERIFY(StudioCapabilities::execute("controller.remove", {{"controller", 0}}).value("ok").toBool());
		QVERIFY(!target.controllerConnection());
		QCOMPARE(target.storedValue(), 0.3f);
		QCOMPARE(target.value(), 0.3f);
		ControllerConnection empty(static_cast<Controller*>(nullptr));
		empty.setController(nullptr); // Null means the native dummy controller.
		QCOMPARE(empty.getController()->type(), Controller::ControllerType::Dummy);
	}

	void controllerFeedbackAndLinkedParameterCyclesAreRejected()
	{
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		auto first = Engine::getSong()->controllers().at(0);
		auto second = Engine::getSong()->controllers().at(1);
		auto firstModel = first->findChildren<AutomatableModel*>().front();
		auto secondModel = second->findChildren<AutomatableModel*>().front();
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", static_cast<int>(secondModel->id())}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("controller.connect", {{"controller", 1}, {"parameter", static_cast<int>(firstModel->id())}}).value("ok").toBool());
		QVERIFY(!firstModel->controllerConnection());
		FloatModel linked(0.3f, 0.f, 1.f, 0.001f, Engine::getSong(), "Linked parameter");
		linked.linkToModel(firstModel);
		QVERIFY(!StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", static_cast<int>(linked.id())}}).value("ok").toBool());
		linked.unlink();
		// Simulate a malformed persisted graph. Inspection must terminate, and
		// native finalization must break the cycle before DSP can recurse.
		firstModel->setControllerConnection(new ControllerConnection(second));
		FloatModel unrelated(0.f, 0.f, 1.f);
		QVERIFY(!first->hasModel(&unrelated));
		ControllerConnection::finalizeConnections();
		QVERIFY(!firstModel->controllerConnection() || !secondModel->controllerConnection());
		QVERIFY(Engine::getSong()->hasErrors());
	}

	void controllerSettingsAndTempoSyncSurviveRecovery()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path()); QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("lfo", "controller.add", {{"type", "lfo"}, {"name", "Pulse"}})).value("ok").toBool());
			QVERIFY(studio.process(request("other", "controller.add", {{"type", "lfo"}, {"name", "Dependent"}})).value("ok").toBool());
			auto first = Engine::getSong()->controllers().at(0);
			auto sync = first->findChild<TempoSyncKnobModel*>();
			QVERIFY(sync);
			QVERIFY(studio.process(request("sync", "parameter.sync", {{"parameter", static_cast<int>(sync->id())},
				{"mode", 8}, {"customNumerator", 3}, {"customDenominator", 8}})).value("ok").toBool());
			QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 120}})).value("ok").toBool());
			QVERIFY(std::abs(sync->storedValue() - 0.1875f) < 0.0001f);
			auto target = Engine::getSong()->controllers().at(1)->findChildren<AutomatableModel*>().front();
			QVERIFY(studio.process(request("connect", "controller.connect", {{"controller", 0}, {"parameter", static_cast<int>(target->id())}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(directory.path()); QString error;
		QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(2));
		auto first = Engine::getSong()->controllers().at(0);
		QCOMPARE(first->name(), QString("Pulse"));
		auto sync = first->findChild<TempoSyncKnobModel*>();
		QVERIFY(sync); QCOMPARE(sync->syncMode(), TempoSyncKnobModel::SyncMode::Custom);
		QCOMPARE(sync->getCustomMeterModel().getNumerator(), 3);
		QCOMPARE(sync->getCustomMeterModel().getDenominator(), 8);
		auto target = Engine::getSong()->controllers().at(1)->findChildren<AutomatableModel*>().front();
		QVERIFY(target->controllerConnection());
		QCOMPARE(target->controllerConnection()->getController(), first);
		QCOMPARE(first->connectionCount(), 1);
	}

	void durableRevisionsAndReplay()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto add = request("add", "track.add", {{"type", "automation"}, {"name", "Dynamics"}});
		{
			StudioRuntime studio(directory.path());
			QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(add).value("ok").toBool());
			QVERIFY(studio.process(add).value("replayed").toBool());
			QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
			QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 96}})).value("ok").toBool());
			QVERIFY(studio.process(request("memory", "session.memory.set", {{"memory", QJsonObject{{"intent", "Three movements"}}}})).value("ok").toBool());
			QVERIFY(studio.process(request("undo", "session.undo")).value("ok").toBool());
			QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
			QVERIFY(studio.process(request("redo", "session.redo")).value("ok").toBool());
			QCOMPARE(Engine::getSong()->getTempo(), 96);
		}
		Engine::getSong()->clearProject();
		StudioRuntime resumed(directory.path());
		QString error;
		QVERIFY2(resumed.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 96);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		QVERIFY(resumed.process(add).value("replayed").toBool());
		const auto info = resumed.process(request("info", "session.inspect")).value("result").toObject();
		QCOMPARE(info.value("memory").toObject().value("intent").toString(), QString("Three movements"));
		auto stale = request("stale", "song.set", {{"tempo", 150}});
		stale.insert("expectedRevision", 0);
		QVERIFY(!resumed.process(stale).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 96);
		auto conflicting = add;
		conflicting.insert("args", QJsonObject{{"type", "sample"}});
		QVERIFY(!resumed.process(conflicting).value("ok").toBool());
	}

	void failedMutationRollsBack()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path());
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		const auto response = studio.process(request("bad", "song.set", {{"tempo", 88}, {"masterPitch", 100}}));
		QVERIFY(!response.value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), DefaultTempo);
		QVERIFY(!Engine::getSong()->isLoadingProject());
		QVERIFY(Engine::projectJournal()->isJournalling());
		QVERIFY(studio.process(request("good", "song.set", {{"tempo", 110}})).value("ok").toBool());
	}

	void nativeRoutingRejectsCycles()
	{
		QVERIFY(StudioCapabilities::execute("mixer.add", {}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("mixer.add", {}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("mixer.route", {{"from", 1}, {"to", 2}, {"amount", 0.5}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("mixer.route", {{"from", 2}, {"to", 1}, {"amount", 0.5}}).value("ok").toBool());
	}

	void headlessMetadataAndTimelineRoundTrip()
	{
		QTemporaryDir directory;
		DataFile fixture(DataFile::Type::SongProject);
		fixture.head().setAttribute("bpm", 123);
		fixture.head().setAttribute("mastervol", 100);
		fixture.content().appendChild(fixture.createElement("trackcontainer"));
		auto notes = fixture.createElement("projectnotes");
		notes.appendChild(fixture.createCDATASection("<p>Preserve the transition idea.</p>"));
		fixture.content().appendChild(notes);
		auto extension = fixture.createElement("externalannotation");
		extension.setAttribute("value", "keep");
		fixture.content().appendChild(extension);
		auto timeline = fixture.createElement("timeline");
		timeline.setAttribute("lp0pos", 192);
		timeline.setAttribute("lp1pos", 576);
		timeline.setAttribute("lpstate", 1);
		fixture.content().appendChild(timeline);
		const auto input = directory.filePath("input.mmp");
		const auto output = directory.filePath("output.mmp");
		QVERIFY(fixture.writeFile(input));
		QVERIFY(Engine::getSong()->loadProject(input));
		QCOMPARE(static_cast<int>(Engine::getSong()->getTimeline(Song::PlayMode::Song).loopBegin()), 192);
		QVERIFY(Engine::getSong()->getTimeline(Song::PlayMode::Song).loopEnabled());
		Engine::getSong()->setTempo(124);
		QVERIFY(Engine::getSong()->saveProjectFile(output));
		DataFile saved(output);
		QCOMPARE(saved.content().firstChildElement("projectnotes").text(), notes.text());
		QCOMPARE(saved.content().firstChildElement("externalannotation").attribute("value"), QString("keep"));
		QCOMPARE(saved.content().elementsByTagName("timeline").size(), 1);
		QCOMPARE(saved.content().firstChildElement("timeline").attribute("lp1pos"), QString("576"));
	}

	void failedLoadRestoresLifecycleFlags()
	{
		QTemporaryDir directory;
		Engine::getSong()->setLoadOnLaunch(false);
		Engine::getSong()->setTempo(111);
		Engine::projectJournal()->setJournalling(false);
		QVERIFY(!Engine::getSong()->loadProject(directory.filePath("missing.mmp")));
		QVERIFY(!Engine::getSong()->isLoadingProject());
		QVERIFY(!Engine::projectJournal()->isJournalling());
		QCOMPARE(Engine::getSong()->getTempo(), 111);
		Engine::projectJournal()->setJournalling(true);
		DataFile missing(directory.filePath("missing.mmp"));
		QCOMPARE(missing.type(), DataFile::Type::Unknown);
	}

	void loadAdvancesPastTrackCommentsAndPreservesJournalState()
	{
		QTemporaryDir directory;
		DataFile fixture(DataFile::Type::SongProject);
		fixture.head().setAttribute("bpm", 117);
		auto tracks = fixture.createElement("trackcontainer");
		tracks.appendChild(fixture.createComment("Track annotations must not block loading"));
		tracks.appendChild(fixture.createElement("externalannotation"));
		fixture.content().appendChild(tracks);
		const auto path = directory.filePath("comments.mmp");
		QVERIFY(fixture.writeFile(path));
		Engine::projectJournal()->setJournalling(false);
		const bool loaded = Engine::getSong()->loadProject(path);
		const bool journalling = Engine::projectJournal()->isJournalling();
		Engine::projectJournal()->setJournalling(true);
		QVERIFY(loaded);
		QVERIFY(!journalling);
		QVERIFY(!Engine::getSong()->isLoadingProject());
		QCOMPARE(Engine::getSong()->getTempo(), 117);
	}

	void singleWriterAndCorruptRecovery()
	{
		QTemporaryDir directory;
		{
			StudioRuntime first(directory.path());
			QString error;
			QVERIFY2(first.open(error), qPrintable(error));
			StudioRuntime second(directory.path());
			QVERIFY(!second.open(error));
		}
		QFile manifest(directory.filePath("session.json"));
		QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(manifest.write("corrupt"), qint64(7));
		manifest.close();
		StudioRuntime recovery(directory.path());
		QString error;
		QVERIFY(!recovery.open(error));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		QCOMPARE(manifest.readAll(), QByteArray("corrupt"));
	}

	void nativeSaveKeepsPreviousBackup()
	{
		QTemporaryDir directory;
		ConfigManager::inst()->setValue("app", "disablebackup", "0");
		const auto path = directory.filePath("project.mmp");
		DataFile first(DataFile::Type::SongProject);
		first.head().setAttribute("bpm", 90);
		QVERIFY(first.writeFile(path));
		QFile original(path);
		QVERIFY(original.open(QIODevice::ReadOnly));
		const auto originalBytes = original.readAll();
		original.close();
		DataFile second(DataFile::Type::SongProject);
		second.head().setAttribute("bpm", 120);
		QVERIFY(second.writeFile(path));
		QFile backup(path + ".bak");
		QVERIFY(backup.open(QIODevice::ReadOnly));
		QCOMPARE(backup.readAll(), originalBytes);
		DataFile published(path);
		QCOMPARE(published.head().hasAttribute("bpm") ? published.head().attribute("bpm")
			: published.head().firstChildElement("bpm").attribute("value"), QString("120"));
		QVERIFY(QDir(directory.path()).mkdir("unwritable.mmp"));
		QVERIFY(!second.writeFile(directory.filePath("unwritable.mmp")));
	}

	void resourcePathsRetainOriginalBundleBase()
	{
		QTemporaryDir directory;
		DataFile project(DataFile::Type::SongProject);
		project.content().appendChild(project.createElement("trackcontainer"));
		const auto original = directory.filePath("original.mmp");
		QVERIFY(project.writeFile(original));
		QVERIFY(Engine::getSong()->loadProject(original));
		DataFile checkpoint(DataFile::Type::SongProject);
		auto sf2 = checkpoint.createElement("sf2player");
		sf2.setAttribute("src", "local:resources/strings.sf2");
		checkpoint.content().appendChild(sf2);
		QVERIFY(!checkpoint.hasLocalPlugins());
		QVERIFY(checkpoint.makeLocalResourcePathsAbsolute());
		QCOMPARE(sf2.attribute("src"), directory.filePath("resources/strings.sf2"));
		// Allowing local assets does not allow plugin attributes on the same node.
		sf2.setAttribute("plugin", "local:resources/untrusted.dll");
		QVERIFY(checkpoint.hasLocalPlugins());
	}

	void nativeResourceBundlesSkipEmptyPathsAndReuseSharedFiles()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("shared.wav"); QVERIFY(constantWave(source));
		const auto destination = directory.filePath("resources"); QVERIFY(QDir().mkpath(destination));
		DataFile document(DataFile::Type::SongProject);
		const auto add = [&](const QString& tag, const QString& attribute, const QString& path) {
			auto node = document.createElement(tag); node.setAttribute(attribute, path);
			document.content().appendChild(node); return node;
		};
		const auto empty = add("sf2player", "src", "");
		const auto clip = add("sampleclip", "src", source);
		const auto sampler = add("audiofileprocessor", "src", source);
		const auto lfo = add("lfocontroller", "userwavefile", source);
		QVERIFY(document.copyResources(destination));
		QCOMPARE(empty.attribute("src"), QString());
		QCOMPARE(clip.attribute("src"), QString("local:resources/shared.wav"));
		QCOMPARE(sampler.attribute("src"), clip.attribute("src"));
		QCOMPARE(lfo.attribute("userwavefile"), clip.attribute("src"));
		QCOMPARE(QDir(destination).entryList(QDir::Files).size(), 1);
		QVERIFY(!document.hasLocalPlugins());
		auto executable = lfo; executable.setAttribute("plugin", "local:resources/untrusted.dll");
		QVERIFY(document.hasLocalPlugins());
	}

	void nativeResourceBundleNamesAvoidCollisionChainsAndHandleExtensionlessFiles()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto destination = directory.filePath("resources"); QVERIFY(QDir().mkpath(destination));
		DataFile document(DataFile::Type::SongProject);
		const QStringList names{"voice.wav", "voice-1.wav", "voice.wav", "voice", "voice"};
		const QStringList expected{"voice.wav", "voice-1.wav", "voice-2.wav", "voice", "voice-1"};
		for (int i = 0; i < names.size(); ++i)
		{
			const auto folder = directory.filePath(QString::number(i)); QVERIFY(QDir().mkdir(folder));
			const auto path = QDir(folder).filePath(names[i]);
			QVERIFY(writeFixtureBytes(path, QByteArray::number(i)));
			auto node = document.createElement("sampleclip"); node.setAttribute("src", path);
			document.content().appendChild(node);
		}
		QVERIFY(document.copyResources(destination));
		const auto clips = document.elementsByTagName("sampleclip");
		for (int i = 0; i < expected.size(); ++i)
		{
			QCOMPARE(clips.item(i).toElement().attribute("src"), "local:resources/" + expected[i]);
			QFile copied(QDir(destination).filePath(expected[i])); QVERIFY(copied.open(QIODevice::ReadOnly));
			QCOMPARE(copied.readAll(), QByteArray::number(i));
		}
	}

	void nativeResourceBundleFailureLeavesItsDocumentUnrebased()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("valid.wav"); QVERIFY(constantWave(source));
		const auto destination = directory.filePath("resources"); QVERIFY(QDir().mkpath(destination));
		DataFile document(DataFile::Type::SongProject);
		for (const auto& path : {source, directory.filePath("missing.wav")})
		{
			auto node = document.createElement("sampleclip"); node.setAttribute("src", path);
			document.content().appendChild(node);
		}
		const auto before = document.toByteArray();
		QTest::ignoreMessage(QtWarningMsg, "ERROR: Resource source is unavailable");
		QVERIFY(!document.copyResources(destination));
		QCOMPARE(document.toByteArray(), before);
	}

	void nativeResourceBundlesUseTheirOwnOriginWithAnotherProjectOpen()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto originFolder = directory.filePath("origin"), otherFolder = directory.filePath("other");
		QVERIFY(QDir().mkdir(originFolder)); QVERIFY(QDir().mkdir(otherFolder));
		const auto source = QDir(originFolder).filePath("strings.sf2"); QVERIFY(writeFixtureBytes(source, "Origin bank"));
		DataFile origin(DataFile::Type::SongProject);
		auto node = origin.createElement("sf2player"); node.setAttribute("src", "local:strings.sf2");
		origin.content().appendChild(node);
		const auto originPath = QDir(originFolder).filePath("original.mmp"); QVERIFY(origin.writeFile(originPath));
		DataFile other(DataFile::Type::SongProject);
		other.content().appendChild(other.createElement("trackcontainer"));
		const auto otherPath = QDir(otherFolder).filePath("other.mmp"); QVERIFY(other.writeFile(otherPath));
		QVERIFY(Engine::getSong()->loadProject(otherPath));
		DataFile reloaded(originPath);
		const auto destination = directory.filePath("resources"); QVERIFY(QDir().mkdir(destination));
		QVERIFY(reloaded.copyResources(destination));
		QFile copied(QDir(destination).filePath("strings.sf2")); QVERIFY(copied.open(QIODevice::ReadOnly));
		QCOMPARE(copied.readAll(), QByteArray("Origin bank"));
	}

	void nativeResourceSourceBindingsKeepDifferentVersionsOfTheSameOriginalPath()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto first = directory.filePath("accepted-a.sf2"), second = directory.filePath("accepted-b.sf2");
		QVERIFY(writeFixtureBytes(first, "Accepted A")); QVERIFY(writeFixtureBytes(second, "Accepted B"));
		DataFile document(DataFile::Type::SongProject), nested(DataFile::Type::SongProject);
		const auto original = directory.filePath("removed-original.sf2");
		const auto add = [&](DataFile& target) {
			auto node = target.createElement("sf2player"); node.setAttribute("src", original);
			target.content().appendChild(node); return node;
		};
		const auto a = add(document), b = add(document), other = add(nested);
		{
			DataFile::ResourceSourceScope scope(document);
			DataFile::registerResourceSource(document, a, "src", first);
			DataFile::registerResourceSource(nested, other, "src", second); // Wrong active document.
			{
				DataFile::ResourceSourceScope disabled(document, false);
				DataFile::registerResourceSource(document, a, "src", second);
			}
			{
				DataFile::ResourceSourceScope inner(nested);
				DataFile::registerResourceSource(nested, other, "src", first);
			}
			DataFile::registerResourceSource(document, b, "src", second);
		}
		DataFile::registerResourceSource(document, a, "src", second); // No active save context.
		QCOMPARE(a.attribute("src"), original); QCOMPARE(b.attribute("src"), original);
		const auto destination = directory.filePath("resources"), nestedDestination = directory.filePath("nested/resources");
		QVERIFY(QDir().mkpath(destination)); QVERIFY(QDir().mkpath(nestedDestination));
		QVERIFY(document.copyResources(destination)); QVERIFY(nested.copyResources(nestedDestination));
		QCOMPARE(a.attribute("src"), QString("local:resources/removed-original.sf2"));
		QCOMPARE(b.attribute("src"), QString("local:resources/removed-original-1.sf2"));
		for (const auto& entry : {std::pair{a, QByteArray("Accepted A")}, std::pair{b, QByteArray("Accepted B")}})
		{
			QFile copied(QDir(directory.path()).filePath(entry.first.attribute("src").mid(6)));
			QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), entry.second);
		}
	}

	void nativeSoundFontBundlePreservesAcceptedInputAfterTheOriginalChanges_data()
	{
		QTest::addColumn<bool>("remove");
		QTest::newRow("removed-original") << true;
		QTest::newRow("replaced-original") << false;
	}
	void nativeSoundFontBundlePreservesAcceptedInputAfterTheOriginalChanges()
	{
		QFETCH(bool, remove);
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("This unit build has no SF2 player; full verification still requires it"); }
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + "-strings.sf2");
		const auto bytes = nativeFixture("native-calibration.sf2"); QVERIFY(!bytes.isEmpty());
		QVERIFY(writeFixtureBytes(source, bytes));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}, {"name", "Editable bank"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.edit", {{"track", 0}, {"name", "Editable bank"}}).value("ok").toBool());
		const auto native = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument(); QString error;
		QVERIFY(native->setFileResource("soundfont", source, error)); QVERIFY(native->selectProgram(1024, 7, error));
		const auto originalPath = native->fileResources().front().path;
		if (remove) { QVERIFY(QFile::remove(source)); }
		else { QVERIFY(writeFixtureBytes(source, "Replaced after the native bank was accepted")); }
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("portable.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString();
		QVERIFY(QFile::exists(path));
		QCOMPARE(native->fileResources().front().path, originalPath);
		DataFile packaged(path);
		const auto resource = packaged.elementsByTagName("sf2player").item(0).toElement().attribute("src");
		QCOMPARE(resource, "local:resources/" + QFileInfo(source).fileName());
		QFile copied(QFileInfo(path).absoluteDir().filePath(resource.mid(6)));
		QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), bytes); copied.close();
		QVERIFY(StudioCapabilities::execute("project.load", {{"path", path}}).value("ok").toBool());
		const auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QCOMPARE(restored->name(), QString("Editable bank"));
		QCOMPARE(restored->instrument()->midiPatch()->bank, std::uint16_t(1024));
		QCOMPARE(restored->instrument()->midiPatch()->program, std::uint8_t(7));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		NotePlayHandle held(restored, 0, frames * 100, Note(TimePos(48), TimePos(), 69));
		std::vector<SampleFrame> output(frames); held.play(output.data());
		const auto audio = measureNativeSoundFont(restored->instrument(), output);
		QVERIFY(audio[0] > .0001 && std::abs(audio[2] - 880) < 6);
	}

	void nativeSampleBundlesPreserveAcceptedInputsAfterSourceChanges_data()
	{
		QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("remove");
		for (const auto& kind : {"sampleclip", "audiofileprocessor", "tripleoscillator", "lfocontroller", "slicert"})
		{
			QTest::newRow(qPrintable(QString(kind) + "-removed")) << QString(kind) << true;
			QTest::newRow(qPrintable(QString(kind) + "-replaced")) << QString(kind) << false;
		}
	}

	void nativeSampleBundlesPreserveAcceptedInputsAfterSourceChanges()
	{
		QFETCH(QString, kind); QFETCH(bool, remove);
		if (kind != "sampleclip" && kind != "lfocontroller" && getPluginFactory()->pluginInfo(kind.toUtf8().constData()).isNull())
		{
			QSKIP("This unit build lacks the requested instrument; full verification still requires it");
		}
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + "-source.wav");
		QVERIFY(constantWave(source)); const auto bytes = readFixtureBytes(source); QVERIFY(!bytes.isEmpty());
		QVERIFY(addNativeSampleConsumer(kind, source));
		if (remove) { QVERIFY(QFile::remove(source)); }
		else { QVERIFY(constantWave(source, -.5f)); }
		const auto saved = StudioCapabilities::execute("project.save", {{"path", directory.filePath("portable.mmp")}, {"withResources", true}});
		QVERIFY2(saved.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(saved).toJson())));
		const auto path = saved.value("result").toObject().value("path").toString(); DataFile packaged(path);
		const QString attribute = kind == "lfocontroller" ? "userwavefile" : kind == "tripleoscillator" ? "userwavefile0" : "src";
		const auto resource = packaged.elementsByTagName(kind).item(0).toElement().attribute(attribute);
		QCOMPARE(resource, "local:resources/" + QFileInfo(source).fileName());
		QCOMPARE(readFixtureBytes(QFileInfo(path).absoluteDir().filePath(resource.mid(6))), bytes);
		QVERIFY(StudioCapabilities::execute("project.load", {{"path", path}}).value("ok").toBool());
		if (kind == "sampleclip")
		{
			const auto sample = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->sample();
			QCOMPARE(sample.sampleRate(), 8000); QCOMPARE(sample.sampleSize(), std::size_t(64));
			QCOMPARE(sample.data()[0][0], .25f);
		}
		else if (kind == "lfocontroller")
		{
			const auto lfo = static_cast<LfoController*>(Engine::getSong()->controllers().at(0));
			QCOMPARE(lfo->userWave().data()[0][0], .25f); QCOMPARE(lfo->currentValue(0), .625f);
		}
		// Saving the restored native buffers again proves they accepted the bundled
		// input rather than retaining the replaced/deleted original's source path.
		QJsonArray references;
		QVERIFY(Engine::getSong()->saveProjectFile(directory.filePath("restored.mmp"), false, true, &references));
		QCOMPARE(references.size(), 1); const auto reference = references.at(0).toObject();
		QCOMPARE(reference.value("sha256").toString(), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
		QCOMPARE(readFixtureBytes(directory.filePath(reference.value("path").toString())), bytes);
	}

	void nativeSampleCheckpointsKeepDistinctLoadedVersionsAcrossRelocation_data()
	{
		QTest::addColumn<bool>("remove");
		QTest::newRow("removed-original") << true;
		QTest::newRow("replaced-original") << false;
	}

	void nativeSampleCheckpointsKeepDistinctLoadedVersionsAcrossRelocation()
	{
		QFETCH(bool, remove);
		for (const auto& plugin : {"audiofileprocessor", "tripleoscillator", "slicert"})
		{
			if (getPluginFactory()->pluginInfo(plugin).isNull()) { QSKIP("Full verification requires all three native sample instruments"); }
		}
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("shared.wav"); auto session = directory.filePath("session");
		QByteArray firstBytes, secondBytes;
		{
			StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(constantWave(source, .25f)); firstBytes = readFixtureBytes(source);
			QVERIFY(addNativeSampleConsumer("sampleclip", source)); QVERIFY(addNativeSampleConsumer("audiofileprocessor", source));
			QVERIFY(constantWave(source, -.5f)); secondBytes = readFixtureBytes(source);
			QVERIFY(addNativeSampleConsumer("tripleoscillator", source)); QVERIFY(addNativeSampleConsumer("lfocontroller", source));
			QVERIFY(addNativeSampleConsumer("slicert", source));
			if (remove) { QVERIFY(QFile::remove(source)); }
			else { QVERIFY(writeFixtureBytes(source, "Original replaced before the first media checkpoint")); }
			QVERIFY(runtime.process(request("checkpoint", "song.set", {{"tempo", 89}})).value("ok").toBool());
			const auto checkpoint = manifest(session).value("history").toArray().last().toObject();
			const auto references = checkpoint.value("assets").toArray(); QCOMPARE(references.size(), 2);
			QVERIFY(StudioAssetStore::verify(QDir(session).filePath("checkpoints"), references, error));
			DataFile stored(QDir(session).filePath(checkpoint.value("path").toString()));
			for (const auto& kind : {"sampleclip", "audiofileprocessor", "tripleoscillator", "lfocontroller", "slicert"})
			{
				const QString tag(kind);
				const QString attribute = tag == "lfocontroller" ? "userwavefile" : tag == "tripleoscillator" ? "userwavefile0" : "src";
				const auto expected = tag == "sampleclip" || tag == "audiofileprocessor" ? firstBytes : secondBytes;
				const auto hash = QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex());
				QCOMPARE(stored.elementsByTagName(tag).item(0).toElement().attribute(attribute), "local:assets/" + hash + ".wav");
			}
			QVERIFY(runtime.process(request("another", "song.set", {{"tempo", 90}})).value("ok").toBool());
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Files).size(), 2);
		}
		Engine::getSong()->clearProject(); const auto relocated = directory.filePath("relocated");
		QVERIFY(QDir().rename(session, relocated)); session = relocated;
		StudioRuntime recovered(session); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 90); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(4));
		QCOMPARE(static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->sample().data()[0][0], .25f);
		const auto lfo = static_cast<LfoController*>(Engine::getSong()->controllers().at(0));
		QCOMPARE(lfo->userWave().data()[0][0], -.5f); QCOMPARE(lfo->currentValue(0), .25f);
		QJsonArray references;
		QVERIFY(Engine::getSong()->saveProjectFile(directory.filePath("recovered.mmp"), false, true, &references));
		QCOMPARE(references.size(), 2);
		for (const auto& value : references)
		{
			const auto bytes = readFixtureBytes(directory.filePath(value.toObject().value("path").toString()));
			QVERIFY(bytes == firstBytes || bytes == secondBytes);
		}
	}

	void nativeCheckpointOwnsSoundFontInputAcrossUndoAndRestart()
	{
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("Full verification requires the SF2 player"); }
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("original.sf2");
		QVERIFY(writeFixtureBytes(source, nativeFixture("native-calibration.sf2")));
		auto session = directory.filePath("session");
		{
			StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}, {"name", "Durable bank"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("player", "instrument.load", {{"track", 0}, {"instrument", "sf2player"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("name", "track.edit", {{"track", 0}, {"name", "Durable bank"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", source}})).value("ok").toBool());
			QVERIFY(runtime.process(request("program", "instrument.program.select", {{"track", 0}, {"bank", 1024}, {"program", 7}})).value("ok").toBool());
			const auto state = manifest(session);
			const auto references = state.value("history").toArray().last().toObject().value("assets").toArray();
			QCOMPARE(references.size(), 1);
			QVERIFY(StudioAssetStore::verify(QDir(session).filePath("checkpoints"), references, error));
			QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Files).size(), 1);
			QVERIFY(QFile::remove(source));
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QCOMPARE(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->instrument()->midiPatch()->program, std::uint8_t(0));
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			QVERIFY(runtime.process(request("tempo", "song.set", {{"tempo", 89}})).value("ok").toBool());
			QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Files).size(), 1);
		}
		Engine::getSong()->clearProject();
		const auto relocated = directory.filePath("relocated-session");
		QVERIFY(QDir().rename(session, relocated)); session = relocated;
		StudioRuntime recovered(session); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->getTempo(), 89);
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		QCOMPARE(track->name(), QString("Durable bank"));
		QCOMPARE(track->instrument()->midiPatch()->bank, std::uint16_t(1024));
		QCOMPARE(track->instrument()->midiPatch()->program, std::uint8_t(7));
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		NotePlayHandle held(track, 0, frames * 100, Note(TimePos(48), TimePos(), 69));
		std::vector<SampleFrame> output(frames); held.play(output.data());
		const auto audio = measureNativeSoundFont(track->instrument(), output);
		QVERIFY(audio[0] > .0001 && std::abs(audio[2] - 880) < 6);
	}

	void cancelledCheckpointTransferStillRestoresThePriorNativeProject()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("sample.wav");
		QVERIFY(constantWave(source)); const auto session = directory.filePath("session");
		StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}, {"name", "Restore after cancellation"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		QVERIFY(runtime.process(request("sample", "sample.load", {{"track", 0}, {"clip", 0}, {"path", source}})).value("ok").toBool());
		const auto before = manifest(session); const auto tempo = Engine::getSong()->getTempo();
		const auto original = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->sample().toBase64();
		QVERIFY(QFile::remove(source)); bool cancelled = false;
		{
			FileResourceIO::Scope work([&] {
				cancelled = cancelled || Engine::getSong()->getTempo() == 73; return cancelled;
			});
			QVERIFY(!runtime.process(request("cancelled-edit", "song.set", {{"tempo", 73}})).value("ok").toBool());
		}
		QVERIFY(cancelled); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->getTempo(), tempo);
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Restore after cancellation"));
		QCOMPARE(static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0))->sample().toBase64(), original);
		QCOMPARE(manifest(session).value("history"), before.value("history"));
		QCOMPARE(manifest(session).value("revision").toInt(), before.value("revision").toInt() + 1);
		QVERIFY(runtime.process(request("next-edit", "song.set", {{"tempo", 89}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 89);
	}

	void alteredAcceptedSampleRejectsPersistenceAndRestoresThePreviousNativeCheckpoint()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("sample.wav");
		QVERIFY(constantWave(source, .25f)); const auto session = directory.filePath("session");
		StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}, {"name", "Retain accepted audio"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		QVERIFY(runtime.process(request("sample", "sample.load", {{"track", 0}, {"clip", 0}, {"path", source}})).value("ok").toBool());
		const auto state = manifest(session); const auto tempo = Engine::getSong()->getTempo();
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		const auto loaded = clip->sample().buffer(); const auto pcm = loaded->toBase64();
		QVERIFY(constantWave(loaded->acceptedFile(), -.5f)); QVERIFY(QFile::remove(source));
		QCOMPARE(loaded->data()[0][0], .25f); QCOMPARE(loaded->toBase64(), pcm);
		QVERIFY(!StudioCapabilities::execute("project.save", {{"path", directory.filePath("rejected.mmp")}, {"withResources", true}}).value("ok").toBool());
		QCOMPARE(clip->sample().toBase64(), pcm); QCOMPARE(Engine::getSong()->getTempo(), tempo);
		QVERIFY(!runtime.process(request("rejected-edit", "song.set", {{"tempo", 73}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), tempo);
		QCOMPARE(Engine::getSong()->tracks().at(0)->name(), QString("Retain accepted audio"));
		QCOMPARE(manifest(session).value("history"), state.value("history"));
		// Rollback replaces native objects, so the runtime invalidates old model
		// handles by advancing its revision while preserving musical history.
		QCOMPARE(manifest(session).value("revision").toInt(), state.value("revision").toInt() + 1);
		const auto restored = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		QCOMPARE(restored->sample().toBase64(), pcm); QVERIFY(restored->sample().buffer()->acceptedFile() != loaded->acceptedFile());
		QCOMPARE(QDir(QDir(session).filePath("checkpoints/assets")).entryList(QDir::Files | QDir::Hidden).size(), 1);
		QVERIFY(runtime.process(request("valid-next-edit", "song.set", {{"tempo", 89}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 89); QCOMPARE(restored->sample().toBase64(), pcm);
	}

	void alteredCheckpointAssetsRejectEditsAndRecoveryBeforeChangingTheSong()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("sample.wav"); QVERIFY(constantWave(source));
		const auto session = directory.filePath("session");
		{
			StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}, {"name", "Keep this track"}})).value("ok").toBool());
			QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
			QVERIFY(runtime.process(request("sample", "sample.load", {{"track", 0}, {"clip", 0}, {"path", source}})).value("ok").toBool());
			const auto state = manifest(session);
			const auto reference = state.value("history").toArray().last().toObject().value("assets").toArray().at(0).toObject();
			const auto asset = QDir(session).filePath("checkpoints/" + reference.value("path").toString());
			QFile file(asset); QVERIFY(file.open(QIODevice::ReadWrite));
			QVERIFY(file.seek(file.size() - 1)); QCOMPARE(file.write("x"), qint64(1)); file.close();
			const auto tempo = Engine::getSong()->getTempo();
			QVERIFY(!runtime.process(request("blocked-edit", "song.set", {{"tempo", 73}})).value("ok").toBool());
			QCOMPARE(Engine::getSong()->getTempo(), tempo);
			QCOMPARE(Engine::getSong()->tracks().front()->name(), QString("Keep this track"));
			QVERIFY(runtime.process(request("inspect", "project.inspect")).value("ok").toBool());
		}
		const auto tempo = Engine::getSong()->getTempo();
		StudioRuntime recovered(session); QString error; QVERIFY(!recovered.open(error));
		QVERIFY(error.contains("asset")); QCOMPARE(Engine::getSong()->getTempo(), tempo);
		QCOMPARE(Engine::getSong()->tracks().front()->name(), QString("Keep this track"));
	}

	void decodedAudioIsMeasuredWithoutListeningClaims()
	{
		QTemporaryDir directory;
		const auto path = directory.filePath("known.wav");
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		SF_INFO info{};
		info.samplerate = 44100;
		info.channels = 1;
		info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
		auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false);
		QVERIFY(audio);
		const float samples[]{0, 0.5f, -0.5f, 0};
		QCOMPARE(sf_writef_float(audio, samples, 4), sf_count_t(4));
		QCOMPARE(sf_close(audio), 0);
		file.close();
		const auto response = StudioCapabilities::execute("audio.analyze", {{"path", path}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		const auto observation = response.value("result").toObject();
		QCOMPARE(observation.value("frames").toInt(), 4);
		QVERIFY(!observation.value("perceptualListening").toBool());
		const auto channel = observation.value("channels").toArray().at(0).toObject();
		QVERIFY(qAbs(channel.value("peak").toDouble() - 0.5) < 0.0001);
		QVERIFY(qAbs(channel.value("rms").toDouble() - 0.35355339059) < 0.0001);
	}

	void alteredCheckpointIsRejected()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path());
			QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
		}
		QFile manifest(directory.filePath("session.json"));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		const auto manifestBytes = manifest.readAll();
		const auto state = QJsonDocument::fromJson(manifestBytes).object();
		manifest.close();
		QFile checkpoint(directory.filePath(state.value("history").toArray().at(0).toObject().value("path").toString()));
		QVERIFY(checkpoint.open(QIODevice::Append));
		QCOMPARE(checkpoint.write("altered"), qint64(7));
		checkpoint.close();
		StudioRuntime recovery(directory.path());
		QString error;
		QVERIFY(!recovery.open(error));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		QCOMPARE(manifest.readAll(), manifestBytes);
	}

	void nativeEncoderFailuresPreserveProcess()
	{
		QTemporaryDir directory;
		for (const auto& encoder : ProjectRenderer::fileEncodeDevices)
		{
			if (!encoder.isAvailable()) { continue; }
			bool successful = true;
			std::unique_ptr<AudioFileDevice> device(encoder.m_getDevInst(
				directory.filePath(QString("missing-parent/output") + encoder.m_extension),
				OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
				2, Engine::audioEngine(), successful));
			QVERIFY(device);
			QVERIFY(!successful);
			QVERIFY(!device->finalizeOutput());
			device.reset(); // Failed initialization must also be safe to destroy.
		}
		QVERIFY(StudioCapabilities::execute("song.set", {{"tempo", 101}}).value("ok").toBool());
		QCOMPARE(Engine::getSong()->getTempo(), 101);
	}

	void rejectedUndoRestoresCurrentProject()
	{
		QTemporaryDir directory;
		StudioRuntime studio(directory.path());
		QString error;
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 112}})).value("ok").toBool());
		QFile manifest(directory.filePath("session.json"));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		const auto history = QJsonDocument::fromJson(manifest.readAll()).object().value("history").toArray();
		manifest.close();
		QFile earlier(directory.filePath(history.at(0).toObject().value("path").toString()));
		QVERIFY(earlier.open(QIODevice::Append));
		QCOMPARE(earlier.write("altered"), qint64(7));
		earlier.close();
		const auto rejected = studio.process(request("undo", "session.undo"));
		QVERIFY(!rejected.value("ok").toBool());
		QCOMPARE(rejected.value("id").toString(), QString("undo"));
		QCOMPARE(Engine::getSong()->getTempo(), 112);
		QVERIFY(studio.process(request("continue", "song.set", {{"tempo", 113}})).value("ok").toBool());
	}

	void invalidCachedRequestIsRejected()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path());
			QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("tempo", "song.set", {{"tempo", 112}})).value("ok").toBool());
		}
		QFile manifest(directory.filePath("session.json"));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		auto state = QJsonDocument::fromJson(manifest.readAll()).object();
		manifest.close();
		auto ledger = state.value("requests").toArray();
		auto entry = ledger.at(0).toObject();
		entry.insert("sha256", QString(64, '0'));
		ledger[0] = entry;
		state.insert("requests", ledger);
		const auto corrupt = QJsonDocument(state).toJson(QJsonDocument::Compact);
		QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(manifest.write(corrupt), qint64(corrupt.size()));
		manifest.close();
		StudioRuntime recovery(directory.path());
		QString error;
		QVERIFY(!recovery.open(error));
		QVERIFY(manifest.open(QIODevice::ReadOnly));
		QCOMPARE(manifest.readAll(), corrupt);
	}

	void invalidConversationStateIsRejectedWithoutOverwrite()
	{
		QTemporaryDir directory;
		{
			StudioRuntime studio(directory.path());
			QString error;
			QVERIFY2(studio.open(error), qPrintable(error));
			QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Remember this unfinished idea"}})).value("ok").toBool());
		}
		auto state = manifest(directory);
		auto agent = state.value("agent").toObject();
		auto job = agent.value("job").toObject();
		job.insert("input", QJsonArray{QJsonObject{{"role", "system"}, {"content", "Corrupt persisted instructions"}}});
		agent.insert("job", job); state.insert("agent", agent);
		const auto corrupt = QJsonDocument(state).toJson(QJsonDocument::Compact);
		QFile file(directory.filePath("session.json"));
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(file.write(corrupt), qint64(corrupt.size())); file.close();
		StudioRuntime recovery(directory.path());
		QString error;
		QVERIFY(!recovery.open(error));
		QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), corrupt);
	}

	void losslessEncoderFinalizationAndUnicodePaths()
	{
		QTemporaryDir directory;
		for (const auto& encoder : ProjectRenderer::fileEncodeDevices)
		{
			if (encoder.m_fileFormat != ProjectRenderer::ExportFileFormat::Wave
				&& encoder.m_fileFormat != ProjectRenderer::ExportFileFormat::Flac) { continue; }
			const auto path = directory.filePath(QString::fromUtf8("\xE9\x9F\xB3\xE6\xA5\xBD") + encoder.m_extension);
			bool successful = false;
			std::unique_ptr<AudioFileDevice> device(encoder.m_getDevInst(path,
				OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
				2, Engine::audioEngine(), successful));
			QVERIFY(successful);
			const SampleFrame samples[]{{0.25f, -0.25f}, {0.5f, -0.5f}, {0, 0}, {0.125f, -0.125f}};
			device->writeBuffer(samples, 4);
			QVERIFY(device->finalizeOutput());
			QVERIFY(device->finalizeOutput()); // Headers must be finalized exactly once.
			device.reset();
			QFile file(path);
			QVERIFY(file.open(QIODevice::ReadOnly));
			SF_INFO info{};
			std::unique_ptr<SNDFILE, decltype(&sf_close)> decoded(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
			QVERIFY(decoded);
			QCOMPARE(info.frames, sf_count_t(4));
			QCOMPARE(info.channels, 2);
			float actual[8]{};
			QCOMPARE(sf_readf_float(decoded.get(), actual, 4), sf_count_t(4));
			QVERIFY(qAbs(actual[0] - 0.25f) < 0.0001f);
			QVERIFY(qAbs(actual[1] + 0.25f) < 0.0001f);
		}
	}
};

QTEST_GUILESS_MAIN(StudioRuntimeTest)
#include "StudioRuntimeTest.moc"
