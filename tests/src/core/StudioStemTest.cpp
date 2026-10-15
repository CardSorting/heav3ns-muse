/* Native track stems without rewriting project mute/solo models. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioFileDevice.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "RenderManager.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"
#include "TrackRenderScope.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {}) { return {{"id", id}, {"name", name}, {"args", args}}; }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QJsonObject manifest(const QString& dir) { return QJsonDocument::fromJson(read(QDir(dir).filePath("session.json"))).object(); }
QJsonObject job(const QString& dir) { return manifest(dir).value("agent").toObject().value("job").toObject(); }
bool native(const QString& op, const QJsonObject& args) { return StudioCapabilities::execute(op, args).value("ok").toBool(); }
bool wave(const QString& path, float amplitude)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
	SF_INFO info{}; info.samplerate = 44100; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
	auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!audio) { return false; }
	std::vector<float> samples(35280 * 2, amplitude); const auto written = sf_writef_float(audio, samples.data(), 35280); const auto closed = sf_close(audio);
	return written == 35280 && closed == 0 && file.flush();
}
struct Pcm { int rate = 0; sf_count_t frames = 0; std::vector<float> samples; };
Pcm decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return {}; }
	SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.frames < 1 || info.frames > 1000000 || info.channels != 2) { return {}; }
	Pcm result{info.samplerate, info.frames, std::vector<float>(static_cast<std::size_t>(info.frames) * 2)};
	return sf_readf_float(audio.get(), result.samples.data(), info.frames) == info.frames && !sf_error(audio.get()) ? result : Pcm{};
}
double mean(const Pcm& pcm, int start)
{
	double sum = 0; for (int frame = start; frame < start + 1000; ++frame) { if (frame >= pcm.frames) { return -1; } sum += pcm.samples[frame * 2]; } return sum / 1000;
}
QJsonArray states()
{
	QJsonArray result; for (const auto owner : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
		for (const auto track : owner->tracks()) { result.append(QJsonObject{{"muted", track->isMutedByModel()}, {"solo", track->isSolo()}, {"beforeSolo", track->isMutedBeforeSolo()}}); }
	return result;
}
bool fixture(const QTemporaryDir& dir, bool patterns = true)
{
	if (!native("song.set", {{"tempo", 600}, {"masterVolume", 100}})) { return false; }
	for (int i = 0; i < 2; ++i)
	{
		const auto path = dir.filePath(QString::number(i) + ".wav");
		if (!wave(path, 0.125f * (i + 1)) || !native("track.add", {{"type", "sample"}, {"name", i == 0 ? "Left" : "Right"}})
			|| !native("clip.add", {{"track", i}, {"position", 0}}) || !native("sample.load", {{"track", i}, {"clip", 0}, {"path", path}})) { return false; }
	}
	if (patterns)
	{
		const auto path = dir.filePath("pattern.wav");
		if (!wave(path, 0.375f) || !native("track.add", {{"type", "pattern"}}) || !native("clip.add", {{"track", 2}, {"position", 0}})
			|| !native("track.add", {{"container", "pattern"}, {"type", "sample"}, {"name", "Pattern source"}})
			|| !native("sample.load", {{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"path", path}})) { return false; }
	}
	return native("timeline.set", {{"begin", 0}, {"end", 192}, {"enabled", false}});
}
bool seed(StudioRuntime& studio, const QTemporaryDir& dir)
{
	int id = 0; const auto perform = [&](const QString& op, const QJsonObject& args) { return studio.process(request("seed-" + QString::number(id++), op, args)).value("ok").toBool(); };
	if (!perform("song.set", {{"tempo", 600}, {"masterVolume", 100}})) { return false; }
	for (int i = 0; i < 2; ++i)
	{
		const auto path = dir.filePath(QString::number(i) + ".wav");
		if (!wave(path, 0.125f * (i + 1)) || !perform("track.add", {{"type", "sample"}, {"name", i == 0 ? "Left" : "Right"}})
			|| !perform("clip.add", {{"track", i}, {"position", 0}}) || !perform("sample.load", {{"track", i}, {"clip", 0}, {"path", path}})) { return false; }
	}
	return perform("timeline.set", {{"begin", 0}, {"end", 192}, {"enabled", false}});
}
QJsonObject render(const QString& path, int track, const QString& container = "song")
{ return StudioCapabilities::execute("audio.render", {{"path", path}, {"track", track}, {"container", container}, {"betweenMarkers", true}, {"bitDepth", 24}, {"timeoutMs", 30000}}); }
const Plugin::Descriptor fixtureDescriptor{"stem-effect-fixture", "Stem effect fixture", "Native stem processing fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class FixtureEffect final : public Effect
{
	DummyEffectControls m_controls{this};
	float m_gain, m_bias;
public:
	int calls = 0;
	FixtureEffect(Model* parent, float gain, float bias = 0) : Effect(&fixtureDescriptor, parent, nullptr), m_gain(gain), m_bias(bias) { wakeUp(); }
	EffectControls* controls() override { return &m_controls; }
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{ ++calls; for (f_cnt_t frame = 0; frame < frames; ++frame) { for (int channel = 0; channel < 2; ++channel) { buffer[frame][channel] = buffer[frame][channel] * m_gain + m_bias; } } return ProcessStatus::Continue; }
};
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
}

class StudioStemTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); QVERIFY(!TrackRenderScope::hasActiveSelection()); }
	void cleanup()
	{
		auto song = Engine::getSong(); song->stopExport(); song->setRenderBetweenMarkers(false); song->setExportLoop(false); song->setLoopRenderCount(1);
		song->clearProject(); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}

	void scopeAffectsOnlyExportAndNeverRewritesNativeModels_data()
	{
		QTest::addColumn<QString>("container"); QTest::addColumn<QString>("kind");
		for (const auto container : {QString("song"), QString("pattern")}) for (const auto kind : {QString("sample"), QString("instrument")})
		{ QTest::newRow(qPrintable(container + kind)) << container << kind; }
	}
	void scopeAffectsOnlyExportAndNeverRewritesNativeModels()
	{
		QFETCH(QString, container); QFETCH(QString, kind); QTemporaryDir dir; QVERIFY(fixture(dir));
		const auto owner = container == "song" ? static_cast<TrackContainer*>(Engine::getSong()) : static_cast<TrackContainer*>(Engine::patternStore());
		Track* target = owner->tracks().front();
		if (kind == "instrument") { target = Track::create(Track::Type::Instrument, owner); }
		const auto before = states(); const auto song = Engine::getSong(); auto other = Engine::getSong()->tracks().at(1);
		QSignalSpy changed(other->getMutedModel(), &Model::dataChanged);
		{
			TrackRenderScope selected(target); QVERIFY(TrackRenderScope::hasActiveSelection()); QVERIFY(!other->isRenderExcluded()); QVERIFY(!other->isMuted()); QVERIFY(!target->isRenderSelected());
			song->setRenderBetweenMarkers(true); song->startExport(); QVERIFY(other->isRenderExcluded()); QVERIFY(other->isMuted()); QVERIFY(!target->isRenderExcluded()); QVERIFY(target->isRenderSelected());
			other->getMutedModel()->setValue(0, true); QVERIFY(other->isMuted()); QCOMPARE(states(), before); QCOMPARE(changed.count(), 0);
			song->stopExport(); QVERIFY(!other->isMuted()); QVERIFY(!target->isRenderSelected()); QCOMPARE(states(), before);
		}
		QVERIFY(!TrackRenderScope::hasActiveSelection()); song->setRenderBetweenMarkers(false); QCOMPARE(states(), before);
	}

	void nativeSoloSurvivesExportAndCanBeReleased_data()
	{ QTest::addColumn<QString>("container"); QTest::newRow("song") << QString("song"); QTest::newRow("pattern") << QString("pattern"); }
	void nativeSoloSurvivesExportAndCanBeReleased()
	{
		QFETCH(QString, container); QTemporaryDir dir; QVERIFY(fixture(dir));
		const auto owner = container == "song" ? static_cast<TrackContainer*>(Engine::getSong()) : static_cast<TrackContainer*>(Engine::patternStore());
		if (owner->tracks().size() < 2) { Track::create(Track::Type::Sample, owner); }
		auto target = owner->tracks().at(0); auto other = owner->tracks().at(1);
		target->setMuted(true); target->setSolo(true); QVERIFY(!target->isMutedByModel()); QVERIFY(target->isSolo()); QVERIFY(target->isMutedBeforeSolo()); QVERIFY(other->isMutedByModel());
		const auto before = states(); const auto path = dir.filePath("solo.wav"); const auto response = render(path, 0, container);
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(states(), before);
		const auto pcm = decode(path); QVERIFY(!pcm.samples.empty()); QVERIFY(std::abs(mean(pcm, 5000) - (container == "song" ? 0.125 : 0.375)) < 0.01);
		target->setSolo(false); QVERIFY(target->isMutedByModel()); QVERIFY(!other->isMutedByModel()); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}

	void stemPcmKeepsSelectedEffectsRoutingAndAutomation_data()
	{ QTest::addColumn<QString>("container"); QTest::newRow("song") << QString("song"); QTest::newRow("pattern") << QString("pattern"); }
	void stemPcmKeepsSelectedEffectsRoutingAndAutomation()
	{
		QFETCH(QString, container); QTemporaryDir dir; QVERIFY(fixture(dir)); const auto owner = container == "song" ? static_cast<TrackContainer*>(Engine::getSong()) : static_cast<TrackContainer*>(Engine::patternStore());
		auto target = static_cast<SampleTrack*>(owner->tracks().front()); auto other = static_cast<SampleTrack*>(Engine::getSong()->tracks().at(1));
		const int channel = Engine::mixer()->createChannel(); target->mixerChannelModel()->setValue(channel);
		auto volume = &Engine::mixer()->mixerChannel(channel)->m_volumeModel; volume->setValue(0.5f);
		auto curve = AutomationClip::globalAutomationClip(volume); curve->putValue(0, 0.5f, false, true); curve->putValue(48, 0.25f, false, true); curve->putValue(192, 0.25f, false, true);
		auto unmute = AutomationClip::globalAutomationClip(other->getMutedModel()); unmute->putValue(0, 0, false, true); unmute->putValue(192, 0, false, true);
		auto gain = new FixtureEffect(target->audioBusHandle()->effects(), 2); QVERIFY(target->audioBusHandle()->effects()->appendEffect(gain)); target->audioBusHandle()->effects()->enabledModel()->setValue(true);
		auto leak = new FixtureEffect(other->audioBusHandle()->effects(), 1, 0.8f); QVERIFY(other->audioBusHandle()->effects()->appendEffect(leak)); other->audioBusHandle()->effects()->enabledModel()->setValue(true);
		auto master = &Engine::mixer()->mixerChannel(0)->m_fxChain; auto masterGain = new FixtureEffect(master, 2);
		QVERIFY(master->appendEffect(masterGain)); master->enabledModel()->setValue(true);
		const auto before = states(); const auto path = dir.filePath("stem.wav"); const auto response = render(path, 0, container);
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("stem").toObject(), QJsonObject({{"container", container}, {"track", 0}, {"type", "sample"}}));
		const auto pcm = decode(path); QVERIFY(!pcm.samples.empty()); const double source = container == "song" ? 0.125 : 0.375;
		QVERIFY(std::abs(mean(pcm, 1000) - source * 2) < 0.01); QVERIFY(std::abs(mean(pcm, 10000) - source) < 0.01);
		QVERIFY(gain->calls > 0); QVERIFY(masterGain->calls > 0); QCOMPARE(leak->calls, 0); QCOMPARE(states(), before); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}

	void invalidSelectionAndNestedScopesPreserveExistingState()
	{
		QTemporaryDir dir; QVERIFY(fixture(dir)); const auto target = Engine::getSong()->tracks().front(); const auto other = Engine::getSong()->tracks().at(1); const auto before = states();
		QVERIFY_EXCEPTION_THROWN(TrackRenderScope{nullptr}, std::invalid_argument);
		QVERIFY_EXCEPTION_THROWN(TrackRenderScope{Engine::getSong()->tracks().at(2)}, std::invalid_argument);
		other->setMuted(true); QVERIFY_EXCEPTION_THROWN(TrackRenderScope{other}, std::invalid_argument); other->setMuted(false);
		{
			TrackRenderScope selected(target); QVERIFY_EXCEPTION_THROWN(TrackRenderScope{other}, std::runtime_error); QVERIFY(TrackRenderScope::hasActiveSelection());
			QVERIFY(!StudioCapabilities::execute("audio.render", {{"path", dir.filePath("nested.wav")}}).value("ok").toBool());
		}
		bool refused = false; std::thread foreign([&] { try { TrackRenderScope selection(target); } catch (const std::invalid_argument&) { refused = true; } }); foreign.join(); QVERIFY(refused);
		const QJsonArray invalid{QJsonObject{{"container", "song"}}, QJsonObject{{"track", -1}}, QJsonObject{{"track", 9}}, QJsonObject{{"track", 0.5}},
			QJsonObject{{"track", 0}, {"container", "missing"}}, QJsonObject{{"track", 2}}, QJsonObject{{"track", QJsonValue::Null}}};
		for (auto value : invalid)
		{
			auto args = value.toObject(); const auto path = dir.filePath("invalid.wav"); args.insert("path", path);
			QVERIFY(!StudioCapabilities::execute("audio.render", args).value("ok").toBool()); QVERIFY(!QFile::exists(path)); QCOMPARE(states(), before); QVERIFY(!TrackRenderScope::hasActiveSelection());
		}
	}

	void failedStemRestoresIsolationAndPreservesPublishedAudio_data()
	{ QTest::addColumn<QString>("failure"); for (const auto failure : {"cancel", "setup", "publication", "selection-cancel"}) { QTest::newRow(failure) << QString(failure); } }
	void failedStemRestoresIsolationAndPreservesPublishedAudio()
	{
		QFETCH(QString, failure); QTemporaryDir dir; QVERIFY(fixture(dir)); const auto before = states(); const auto path = dir.filePath("stem.wav"); const QByteArray prior("Prior authoritative audio");
		if (failure == "publication") { QVERIFY(QDir().mkpath(path)); } else { QVERIFY(write(path, prior)); }
		bool cancelled = false; AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; }
			if (!dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { return; }
			if (failure == "cancel") { cancelled = true; } if (failure == "setup") { throw std::runtime_error("Fixture refuses native setup"); }
		});
		const auto result = StudioCapabilities::execute("audio.render", {{"path", path}, {"track", 0}, {"betweenMarkers", true}, {"timeoutMs", 30000}},
			[&] { return cancelled || (failure == "selection-cancel" && TrackRenderScope::hasActiveSelection()); });
		transitionProbe.reset(); QVERIFY(!result.value("ok").toBool()); QCOMPARE(states(), before); QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		if (failure != "publication") { QCOMPARE(read(path), prior); } else { QVERIFY(QDir(path).exists()); }
		QVERIFY(QDir(dir.path()).entryList({"*.render-*"}, QDir::Files).isEmpty()); QVERIFY(render(dir.filePath("retry.wav"), 1).value("ok").toBool()); QCOMPARE(states(), before);
	}

	void sharedNativeManagerExportsEveryEligibleStemWithoutMuteEdits()
	{
		QTemporaryDir dir; QVERIFY(fixture(dir)); const auto before = states(); const auto path = dir.filePath("stems"); QVERIFY(QDir().mkpath(path));
		QSignalSpy left(Engine::getSong()->tracks().at(0)->getMutedModel(), &Model::dataChanged), right(Engine::getSong()->tracks().at(1)->getMutedModel(), &Model::dataChanged);
		Engine::getSong()->setRenderBetweenMarkers(true); RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
		QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool finished = false; connect(&manager, &RenderManager::finished, &loop, [&] { finished = true; loop.quit(); }); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		timer.start(30000); manager.renderTracks(); if (!finished) { loop.exec(); } if (!finished) { manager.abortProcessing(); } QVERIFY(finished); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString()));
		const auto paths = QDir(path).entryList({"*.wav"}, QDir::Files, QDir::Name); QCOMPARE(paths.size(), 3); sf_count_t frames = 0;
		for (int i = 0; i < paths.size(); ++i) { const auto pcm = decode(QDir(path).filePath(paths.at(i))); QVERIFY(!pcm.samples.empty()); QVERIFY(std::abs(mean(pcm, 5000) - 0.125 * (i + 1)) < 0.01); if (i) { QCOMPARE(pcm.frames, frames); } frames = pcm.frames; }
		QCOMPARE(left.count(), 0); QCOMPARE(right.count(), 0); QCOMPARE(states(), before); QVERIFY(!TrackRenderScope::hasActiveSelection()); Engine::getSong()->setRenderBetweenMarkers(false);
		const auto mixPath = dir.filePath("mix.wav"); const auto mix = StudioCapabilities::execute("audio.render", {{"path", mixPath}, {"betweenMarkers", true}, {"bitDepth", 24}, {"timeoutMs", 30000}});
		QVERIFY2(mix.value("ok").toBool(), qPrintable(mix.value("error").toString())); QVERIFY(mix.value("result").toObject().value("stem").isNull());
		const auto pcm = decode(mixPath); QVERIFY(!pcm.samples.empty()); QVERIFY(std::abs(mean(pcm, 5000) - 0.75) < 0.01); QCOMPARE(states(), before);
	}

	void nativeManagerFailureReleasesTrackSelection_data()
	{ QTest::addColumn<bool>("cancel"); QTest::newRow("cancel") << true; QTest::newRow("setup-error") << false; }
	void nativeManagerFailureReleasesTrackSelection()
	{
		QFETCH(bool, cancel); QTemporaryDir dir; QVERIFY(fixture(dir)); const auto before = states();
		{
			RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), ProjectRenderer::ExportFileFormat::Wave, dir.path());
			bool entered = false; AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; }
				if (!dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { return; } entered = true;
				if (cancel) { manager.abortProcessing(); } else { throw std::runtime_error("Fixture refuses manager setup"); }
			}); manager.renderTracks(); QVERIFY(entered); QVERIFY(!TrackRenderScope::hasActiveSelection()); QCOMPARE(states(), before);
		}
		QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(render(dir.filePath("after.wav"), 0).value("ok").toBool()); QCOMPARE(states(), before);
	}

	void bulkEligibilitySurvivesAutomationFromEarlierStems()
	{
		QTemporaryDir dir; QVERIFY(fixture(dir, false)); const auto path = dir.filePath("stems"); QVERIFY(QDir().mkpath(path));
		for (const auto track : Engine::getSong()->tracks())
		{
			auto curve = AutomationClip::globalAutomationClip(track->getMutedModel()); curve->putValue(0, 0, false, true); curve->putValue(96, 1, false, true); curve->putValue(192, 1, false, true);
		}
		Engine::getSong()->setExportLoop(true); Engine::getSong()->setRenderBetweenMarkers(false);
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
		QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool finished = false; connect(&manager, &RenderManager::finished, &loop, [&] { finished = true; loop.quit(); }); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		timer.start(30000); manager.renderTracks(); if (!finished) { loop.exec(); } if (!finished) { manager.abortProcessing(); }
		QVERIFY(finished); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString()));
		const auto paths = QDir(path).entryList({"*.wav"}, QDir::Files, QDir::Name); QCOMPARE(paths.size(), 2); sf_count_t frames = 0;
		for (int i = 0; i < paths.size(); ++i)
		{
			const auto pcm = decode(QDir(path).filePath(paths.at(i))); QVERIFY(pcm.frames > 30000); QVERIFY(std::abs(mean(pcm, 1000) - 0.125 * (i + 1)) < 0.01); QVERIFY(std::abs(mean(pcm, 15000)) < 0.001);
			if (i) { QCOMPARE(pcm.frames, frames); } frames = pcm.frames;
			// Native automation retains its ending value; isolation must not
			// overwrite it with the manager's initially unmuted eligibility.
			QVERIFY(Engine::getSong()->tracks().at(i)->isMutedByModel());
		}
		QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		// A new headless request has fresh admission and rejects a muted target.
		QVERIFY(!render(dir.filePath("muted.wav"), 0).value("ok").toBool()); QVERIFY(!QFile::exists(dir.filePath("muted.wav")));
		Engine::getSong()->tracks().at(0)->setMuted(false); const auto freshPath = dir.filePath("fresh.wav");
		QVERIFY(render(freshPath, 0).value("ok").toBool()); const auto fresh = decode(freshPath);
		// The other track starts muted but its automation unmutes it. Its audio
		// must remain excluded throughout this independently admitted stem.
		QVERIFY(!fresh.samples.empty()); QVERIFY(std::abs(mean(fresh, 1000) - 0.125) < 0.01); QVERIFY(std::abs(mean(fresh, 15000)) < 0.001);
	}

	void stemReceiptsPreserveHistoryAndReplayAcrossReopen_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-process") << false; QTest::newRow("reopen") << true; }
	void stemReceiptsPreserveHistoryAndReplayAcrossReopen()
	{
		QFETCH(bool, restart); QTemporaryDir dir; const auto session = dir.filePath("session"), path = dir.filePath("stem.wav"); QString error;
		auto studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); QVERIFY(seed(*studio, dir));
		QVERIFY(studio->process(request("mute", "track.edit", {{"track", 1}, {"muted", true}})).value("ok").toBool()); const auto before = manifest(session); const auto trackStates = states();
		const auto renderRequest = request("stem", "audio.render", {{"path", path}, {"track", 0}, {"betweenMarkers", true}, {"timeoutMs", 30000}}); const auto response = studio->process(renderRequest);
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(states(), trackStates); QCOMPARE(manifest(session).value("history"), before.value("history")); QCOMPARE(manifest(session).value("revision"), before.value("revision"));
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); }
		QVERIFY(QFile::remove(path)); auto replay = studio->process(renderRequest); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QVERIFY(!QFile::exists(path)); QCOMPARE(states(), trackStates);
		QVERIFY(studio->process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(!Engine::getSong()->tracks().at(1)->isMutedByModel());
		QVERIFY(studio->process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(states(), trackStates); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}

	void agentRediscoversStemTargetAfterTrackRemoval_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void agentRediscoversStemTargetAfterTrackRemoval()
	{
		QFETCH(QString, protocol); QTemporaryDir dir; const auto session = dir.filePath("session"), wrong = dir.filePath("wrong.wav"), path = dir.filePath("right.wav"); QString error; int round = 0;
		StudioRuntime studio(session, [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {call("remove", "track_remove", {{"track", 0}}), call("stale", "audio_render", {{"path", wrong}, {"track", 0}})}); }
			if (round == 2) { return decision(protocol, {call("discover", "project_inspect", {{"section", "tracks"}}), call("render", "audio_render", {{"path", path}, {"track", 0}, {"betweenMarkers", true}})}); }
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native stem fixture exported the surviving track"}, {"evidence", QJsonArray{"discover", "render"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error)); QVERIFY(seed(studio, dir));
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-stem-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Remove the first track and export the remaining native sample stem."}})).value("ok").toBool());
		for (int i = 0; i < 12 && StudioAgent::active(job(session)); ++i) { studio.advanceAgent(); }
		QCOMPARE(job(session).value("status").toString(), QString("complete")); QCOMPARE(round, 3); QVERIFY(!QFile::exists(wrong)); const auto pcm = decode(path); QVERIFY(!pcm.samples.empty()); QVERIFY(std::abs(mean(pcm, 5000) - 0.25) < 0.01);
		bool skipped = false; for (const auto& value : job(session).value("records").toArray()) { const auto record = value.toObject(); if (record.value("call").toObject().value("id").toString() == "stale") { skipped = !record.value("response").toObject().value("ok").toBool(); } }
		QVERIFY(skipped); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}
};

QTEST_GUILESS_MAIN(StudioStemTest)
#include "StudioStemTest.moc"
