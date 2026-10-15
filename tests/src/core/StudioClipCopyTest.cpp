/* Native clip ownership, resident audio, independent curves and recovery tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AutomationClip.h"
#include "AutomationNode.h"
#include "ConfigManager.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

class StudioClipCopyTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	Song* song() const { return Engine::getSong(); }
	PatternStore* store() const { return Engine::patternStore(); }
	Track::Type kind(int type) const { return type == 0 ? Track::Type::Instrument : type == 1 ? Track::Type::Sample : Track::Type::Automation; }
	InstrumentTrack* bindingTrack() const { return static_cast<InstrumentTrack*>(song()->tracks().at(0)); }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	void fill(Clip* clip)
	{
		if (auto midi = dynamic_cast<MidiClip*>(clip))
		{
			midi->clearNotes(); Note note(37, 12, 63, 91, -23); note.createDetuning();
			note.detuning()->automationClip()->putValue(0, .5f, false);
			note.detuning()->automationClip()->putValue(19, -.75f, false);
			midi->addNote(note, false);
		}
		if (auto sample = dynamic_cast<SampleClip*>(clip))
		{
			std::vector<SampleFrame> frames(1024);
			for (std::size_t i = 0; i < frames.size(); ++i) { frames[i][0] = i / 2048.f; frames[i][1] = -frames[i][0]; }
			sample->setSampleBuffer(std::make_shared<SampleBuffer>(std::move(frames), 24000));
			QVERIFY(sample->setSampleWindow(17, 900)); sample->sample().setAmplification(.375f); sample->sample().setReversed(true);
		}
		if (auto curve = dynamic_cast<AutomationClip*>(clip))
		{
			QVERIFY(curve->addObject(bindingTrack()->volumeModel()));
			QVERIFY(curve->addObject(bindingTrack()->panningModel()));
			curve->setProgressionType(AutomationClip::ProgressionType::CubicHermite);
			curve->putValues(0, 25, 35, false, true); curve->putValues(72, 60, 65, false, true);
			QVERIFY(curve->setNodeTangents(0, .25f, -.5f, true));
		}
		clip->setName("Copied native state"); clip->setMuted(true); clip->setColor(QColor("#37ab91"));
		clip->setAutoResize(false); clip->changeLength(311); clip->setStartTimeOffset(-13);
	}
	void assertContent(Clip* clip)
	{
		QCOMPARE(clip->name(), QString("Copied native state")); QVERIFY(clip->isMuted());
		QVERIFY(clip->color()); QCOMPARE(*clip->color(), QColor("#37ab91")); QVERIFY(!clip->getAutoResize());
		QCOMPARE(clip->length().getTicks(), 311); QCOMPARE(clip->startTimeOffset().getTicks(), -13);
		if (auto midi = dynamic_cast<MidiClip*>(clip))
		{
			QCOMPARE(midi->notes().size(), std::size_t(1)); const auto note = midi->notes().front();
			QCOMPARE(note->key(), 63); QCOMPARE(note->pos().getTicks(), 12); QCOMPARE(note->length().getTicks(), 37);
			QCOMPARE(note->getVolume(), 91); QCOMPARE(note->getPanning(), -23); QVERIFY(note->hasDetuningInfo());
			QCOMPARE(note->detuning()->automationClip()->valueAt(19), -.75f);
		}
		if (auto sample = dynamic_cast<SampleClip*>(clip))
		{
			QCOMPARE(sample->sample().sampleRate(), 24000); QCOMPARE(sample->sample().sampleSize(), std::size_t(1024));
			QCOMPARE(sample->sample().startFrame(), 17); QCOMPARE(sample->sample().endFrame(), 900);
			QCOMPARE(sample->sample().amplification(), .375f); QVERIFY(sample->sample().reversed());
			QCOMPARE(sample->sample().data()[500][0], 500 / 2048.f); QCOMPARE(sample->sample().data()[500][1], -500 / 2048.f);
		}
		if (auto curve = dynamic_cast<AutomationClip*>(clip))
		{
			QCOMPARE(curve->objects().size(), std::size_t(2)); QCOMPARE(curve->objects().at(0).data(), bindingTrack()->volumeModel());
			QCOMPARE(curve->objects().at(1).data(), bindingTrack()->panningModel()); QCOMPARE(curve->getTimeMap().size(), 2);
			QCOMPARE(curve->progressionType(), AutomationClip::ProgressionType::CubicHermite);
			const auto node = curve->getTimeMap().value(0); QCOMPARE(node.getInValue(), 25.f); QCOMPARE(node.getOutValue(), 35.f);
			QCOMPARE(node.getInTangent(), .25f); QCOMPARE(node.getOutTangent(), -.5f); QVERIFY(node.lockedTangents());
		}
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000"); ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256");
		NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { song()->clearProject(); song()->clearErrors(); }
	void cleanup() { song()->stop(); song()->clearProject(); }

	void midiCopyOwnsNotesAndDestinationInstrument_data()
	{
		QTest::addColumn<bool>("patternSource"); QTest::newRow("song") << false; QTest::newRow("pattern") << true;
	}
	void midiCopyOwnsNotesAndDestinationInstrument()
	{
		QFETCH(bool, patternSource); Track::create(Track::Type::Instrument, song());
		if (patternSource) { Track::create(Track::Type::Pattern, song()); }
		auto sourceTrack = Track::create(Track::Type::Instrument, patternSource ? static_cast<TrackContainer*>(store()) : song());
		auto source = static_cast<MidiClip*>(patternSource ? sourceTrack->getClip(0) : sourceTrack->createClip(0)); fill(source);
		auto destination = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song()));
		auto copy = static_cast<MidiClip*>(source->copyTo(destination)); assertContent(copy);
		QCOMPARE(copy->instrumentTrack(), destination); QCOMPARE(copy->getTrack(), static_cast<Track*>(destination));
		QVERIFY(copy->notes().front() != source->notes().front());
		QVERIFY(copy->notes().front()->detuning() != source->notes().front()->detuning());
		copy->notes().front()->detuning()->automationClip()->putValue(19, 2.f, false);
		QCOMPARE(source->notes().front()->detuning()->automationClip()->valueAt(19), -.75f);
		source->setAutoResize(true); source->changeLength(577); source->setStartTimeOffset(23); source->setSolo(true);
		auto resized = static_cast<MidiClip*>(source->copyTo(destination)); QVERIFY(resized->getAutoResize()); QVERIFY(resized->isSolo());
		QCOMPARE(resized->length().getTicks(), 577); QCOMPARE(resized->startTimeOffset().getTicks(), 23);
	}

	void sampleCopyRetainsResidentPcmAndIndependentPlayback_data()
	{
		QTest::addColumn<bool>("missingSource"); QTest::newRow("unsaved-pcm") << false; QTest::newRow("missing-original-file") << true;
	}
	void sampleCopyRetainsResidentPcmAndIndependentPlayback()
	{
		QFETCH(bool, missingSource); auto sourceTrack = Track::create(Track::Type::Sample, song());
		auto source = static_cast<SampleClip*>(sourceTrack->createClip(0)); fill(source);
		if (missingSource)
		{
			const auto buffer = source->sample().buffer(); const auto absent = m_configuration.filePath("does-not-exist.wav"); QVERIFY(!QFile::exists(absent));
			source->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(buffer->begin(), buffer->end()), 24000, absent));
			QVERIFY(source->setSampleWindow(17, 900)); source->sample().setAmplification(.375f); source->sample().setReversed(true);
			source->setName("Copied native state"); source->setAutoResize(false); source->changeLength(311); source->setStartTimeOffset(-13);
		}
		auto destination = Track::create(Track::Type::Sample, song()); auto copy = static_cast<SampleClip*>(source->copyTo(destination));
		assertContent(copy); QCOMPARE(copy->sample().buffer().get(), source->sample().buffer().get()); QVERIFY(!copy->isPlaying()); QVERIFY(!copy->isRecord());
		QCOMPARE(copy->sampleFile(), source->sampleFile()); copy->sample().setAmplification(.75f); QVERIFY(copy->setSampleWindow(50, 800));
		QCOMPARE(source->sample().amplification(), .375f); QCOMPARE(source->sample().startFrame(), 17);
		delete sourceTrack; QCOMPARE(copy->sample().data()[500][0], 500 / 2048.f);
		copy->setIsPlaying(true); destination->setMuted(true); QVERIFY(!copy->isPlaying());
	}

	void automationCopyRebindsNodeOwnersAndRetainsTargets()
	{
		Track::create(Track::Type::Instrument, song()); auto sourceTrack = Track::create(Track::Type::Automation, song());
		auto source = static_cast<AutomationClip*>(sourceTrack->createClip(0)); fill(source);
		auto destination = Track::create(Track::Type::Automation, song()); auto copy = static_cast<AutomationClip*>(source->copyTo(destination)); assertContent(copy);
		copy->getTimeMap()[72].setInValue(80); QCOMPARE(source->getTimeMap().value(72).getInValue(), 60.f);
		delete sourceTrack; copy->getTimeMap()[72].setOutValue(90); QCOMPARE(copy->getTimeMap().value(72).getOutValue(), 90.f);
		delete bindingTrack(); QVERIFY(copy->objects().empty());
	}

	void patternReferenceUsesDestinationPattern()
	{
		auto first = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song()));
		auto second = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song()));
		auto row = Track::create(Track::Type::Instrument, store());
		const auto firstCell = row->getClip(0); const auto secondCell = row->getClip(1);
		auto source = static_cast<PatternClip*>(first->createClip(0)); source->changeLength(384); source->setStartTimeOffset(-24);
		auto copy = static_cast<PatternClip*>(source->copyTo(second)); QCOMPARE(copy->patternIndex(), 1); QCOMPARE(source->patternIndex(), 0);
		QCOMPARE(copy->length().getTicks(), 384); QCOMPARE(copy->startTimeOffset().getTicks(), -24);
		QCOMPARE(row->numOfClips(), 2); QCOMPARE(row->getClip(0), firstCell); QCOMPARE(row->getClip(1), secondCell);
	}

	void patternCellReplacementPreservesEveryOtherColumn_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<bool>("sameRow");
		QTest::newRow("midi-same-row") << 0 << true; QTest::newRow("midi-other-row") << 0 << false;
		QTest::newRow("sample-same-row") << 1 << true; QTest::newRow("sample-other-row") << 1 << false;
		QTest::newRow("automation-same-row") << 2 << true; QTest::newRow("automation-other-row") << 2 << false;
	}
	void patternCellReplacementPreservesEveryOtherColumn()
	{
		QFETCH(int, type); QFETCH(bool, sameRow); Track::create(Track::Type::Instrument, song());
		song()->getTimeSigModel().setNumerator(7); song()->getTimeSigModel().setDenominator(8);
		for (int i = 0; i < 3; ++i) { Track::create(Track::Type::Pattern, song()); }
		auto sourceTrack = Track::create(kind(type), store()); auto destination = sameRow ? sourceTrack : Track::create(kind(type), store());
		auto source = sourceTrack->getClip(0); fill(source); QPointer<Clip> replaced = destination->getClip(2);
		const auto zero = destination->getClip(0); const auto one = destination->getClip(1); store()->setCurrentPattern(1);
		const auto response = StudioCapabilities::execute("clip.clone", {{"container", "pattern"}, {"track", 0}, {"clip", 0},
			{"targetTrack", sameRow ? 0 : 1}, {"targetClip", 2}, {"replace", true}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(replaced.isNull());
		QCOMPARE(response.value("result").toObject().value("clip").toInt(), 2); assertContent(destination->getClip(2)); assertContent(source);
		QCOMPARE(destination->numOfClips(), 3); QCOMPARE(destination->getClip(0), zero); QCOMPARE(destination->getClip(1), one);
		QCOMPARE(store()->currentPattern(), 1); for (int i = 0; i < 3; ++i) { QCOMPARE(destination->getClip(i)->startPosition().getTicks(), i * TimePos::ticksPerBar()); }
	}

	void invalidDestinationsLeaveNativeClipsUntouched_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("wrong-type") << 0; QTest::newRow("missing-position") << 1; QTest::newRow("overflow") << 2;
		QTest::newRow("unknown-track") << 3; QTest::newRow("container-without-track") << 4; QTest::newRow("unknown-container") << 5;
		QTest::newRow("song-replace") << 6; QTest::newRow("pattern-missing-replace") << 7; QTest::newRow("pattern-missing-cell") << 8;
		QTest::newRow("pattern-with-position") << 9; QTest::newRow("pattern-cell-out-of-range") << 10; QTest::newRow("pattern-self-copy") << 11;
	}
	void invalidDestinationsLeaveNativeClipsUntouched()
	{
		QFETCH(int, failure); auto sourceTrack = Track::create(Track::Type::Instrument, song()); fill(sourceTrack->createClip(0));
		Track::create(Track::Type::Sample, song()); Track::create(Track::Type::Pattern, song()); auto row = Track::create(Track::Type::Instrument, store());
		QJsonObject args{{"track", 0}, {"clip", 0}, {"position", 384}};
		if (failure == 0) { args.insert("targetTrack", 1); }
		if (failure == 1) { args.remove("position"); }
		if (failure == 2) { args.insert("position", MaxSongLength); }
		if (failure == 3) { args.insert("targetTrack", 99); }
		if (failure == 4) { args.insert("targetContainer", "song"); }
		if (failure == 5) { args.insert("targetContainer", "invalid"); args.insert("targetTrack", 0); }
		if (failure == 6) { args.insert("replace", false); }
		if (failure >= 7)
		{
			args = {{"track", 0}, {"clip", 0}, {"targetContainer", "pattern"}, {"targetTrack", 0}, {"targetClip", 0}, {"replace", true}};
			if (failure == 7) { args.insert("replace", false); } if (failure == 8) { args.remove("targetClip"); }
			if (failure == 9) { args.insert("position", 0); } if (failure == 10) { args.insert("targetClip", 1); }
			if (failure == 11) { args.insert("container", "pattern"); }
		}
		const auto original = sourceTrack->getClip(0); const auto cell = row->getClip(0); const bool modified = song()->isModified();
		QVERIFY(!StudioCapabilities::execute("clip.clone", args).value("ok").toBool());
		QCOMPARE(sourceTrack->numOfClips(), 1); QCOMPARE(sourceTrack->getClip(0), original); assertContent(original);
		QCOMPARE(row->numOfClips(), 1); QCOMPARE(row->getClip(0), cell); QCOMPARE(song()->isModified(), modified);
	}

	void recordingOwnershipPreventsCopyAndReplacement_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("source-note-recording") << 0; QTest::newRow("target-note-recording") << 1;
		QTest::newRow("sample-armed") << 2; QTest::newRow("sample-reserved") << 3;
		QTest::newRow("automation-armed") << 4; QTest::newRow("automation-reserved") << 5;
		QTest::newRow("target-sample-armed") << 6; QTest::newRow("target-automation-reserved") << 7;
		QTest::newRow("other-target-sample-armed") << 8; QTest::newRow("other-target-sample-reserved") << 9;
	}
	void recordingOwnershipPreventsCopyAndReplacement()
	{
		QFETCH(int, failure); const int type = failure < 2 ? 0 : failure < 4 || failure == 6 || failure >= 8 ? 1 : 2;
		auto sourceTrack = Track::create(kind(type), song()); auto source = sourceTrack->createClip(0);
		Track::create(Track::Type::Pattern, song()); auto destination = Track::create(kind(type), store()); auto cell = destination->getClip(0);
		if (failure >= 8)
		{
			Track::create(Track::Type::Pattern, song()); auto other = static_cast<SampleClip*>(destination->getClip(1));
			if (failure == 8) { other->setRecord(true); }
			else { QString error; QVERIFY(other->reserveRecordingCompletion(error)); }
		}
		if (failure < 2) { QVERIFY((failure == 0 ? sourceTrack : destination)->beginNoteRecording(this)); }
		if (failure == 2 || failure == 6) { static_cast<SampleClip*>(failure == 2 ? source : cell)->setRecord(true); }
		if (failure == 3) { QString error; QVERIFY(static_cast<SampleClip*>(source)->reserveRecordingCompletion(error)); }
		if (failure == 4) { static_cast<AutomationClip*>(source)->setRecording(true); }
		if (failure == 5 || failure == 7) { static_cast<AutomationClip*>(failure == 5 ? source : cell)->reserveRecordingTake(32, 1); }
		QVERIFY_EXCEPTION_THROWN(source->copyTo(destination, cell), std::invalid_argument);
		QCOMPARE(destination->numOfClips(), failure >= 8 ? 2 : 1); QCOMPARE(destination->getClip(0), cell); QCOMPARE(sourceTrack->getClip(0), source);
		if (failure < 2) { (failure == 0 ? sourceTrack : destination)->endNoteRecording(this); }
		if (failure == 3) { static_cast<SampleClip*>(source)->discardReservedRecording(); }
		if (failure == 5 || failure == 7) { static_cast<AutomationClip*>(failure == 5 ? source : cell)->discardRecordingTake(); }
		if (failure == 9) { static_cast<SampleClip*>(destination->getClip(1))->discardReservedRecording(); }
	}

	void foreignThreadCannotPublishAClip()
	{
		auto sourceTrack = Track::create(Track::Type::Instrument, song()); auto source = sourceTrack->createClip(0);
		auto destination = Track::create(Track::Type::Instrument, song()); bool rejected = false;
		std::thread worker([&] { try { source->copyTo(destination); } catch (const std::invalid_argument&) { rejected = true; } }); worker.join();
		QVERIFY(rejected); QCOMPARE(destination->numOfClips(), 0);
	}

	void sparsePatternDestinationCannotPublishAnotherColumn()
	{
		auto sourceTrack = Track::create(Track::Type::Instrument, song()); auto source = sourceTrack->createClip(0);
		for (int i = 0; i < 3; ++i) { Track::create(Track::Type::Pattern, song()); }
		auto destination = Track::create(Track::Type::Instrument, store()); auto cell = destination->getClip(0); delete destination->getClip(2);
		QVERIFY_EXCEPTION_THROWN(source->copyTo(destination, cell), std::invalid_argument);
		QCOMPARE(destination->numOfClips(), 2); QCOMPARE(destination->getClip(0), cell); QCOMPARE(sourceTrack->numOfClips(), 1);
	}

	void copiesSurviveNativeXmlHistoryReplayAndRecovery_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<bool>("patternTarget");
		QTest::newRow("midi-song") << 0 << false; QTest::newRow("midi-pattern") << 0 << true;
		QTest::newRow("sample-song") << 1 << false; QTest::newRow("sample-pattern") << 1 << true;
		QTest::newRow("automation-song") << 2 << false; QTest::newRow("automation-pattern") << 2 << true;
	}
	void copiesSurviveNativeXmlHistoryReplayAndRecovery()
	{
		QFETCH(int, type); QFETCH(bool, patternTarget); QTemporaryDir directory;
		const auto owner = [&]() -> Track* { return patternTarget ? store()->tracks().at(0) : song()->tracks().at(2); };
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			Track::create(Track::Type::Instrument, song()); auto source = Track::create(kind(type), song())->createClip(0); fill(source);
			if (patternTarget) { Track::create(Track::Type::Pattern, song()); Track::create(kind(type), store()); }
			else { Track::create(kind(type), song()); }
			QVERIFY(runtime.process(request("fixture", "song.set", {{"tempo", 103}})).value("ok").toBool());
			QJsonObject args{{"track", 1}, {"clip", 0}, {"targetTrack", patternTarget ? 0 : 2}};
			if (patternTarget) { args.insert("targetContainer", "pattern"); args.insert("targetClip", 0); args.insert("replace", true); }
			else { args.insert("position", 384); }
			const auto copyRequest = request("copy", "clip.clone", args); const auto response = runtime.process(copyRequest);
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); assertContent(owner()->getClip(0));
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QCOMPARE(owner()->numOfClips(), patternTarget ? 1 : 0);
			if (patternTarget) { QVERIFY(owner()->getClip(0)->name() != "Copied native state"); }
			QVERIFY(runtime.process(copyRequest).value("replayed").toBool()); QCOMPARE(owner()->numOfClips(), patternTarget ? 1 : 0);
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); assertContent(owner()->getClip(0));
			QCOMPARE(owner()->getClip(0)->startPosition().getTicks(), patternTarget ? 0 : 384);
			const auto file = directory.filePath("copied.mmp"); QVERIFY(song()->saveProjectFile(file)); QVERIFY(song()->loadProject(file));
			assertContent(owner()->getClip(0)); assertContent(song()->tracks().at(1)->getClip(0));
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		assertContent(owner()->getClip(0)); assertContent(song()->tracks().at(1)->getClip(0));
		QCOMPARE(owner()->getClip(0)->startPosition().getTicks(), patternTarget ? 0 : 384);
	}
};
QTEST_GUILESS_MAIN(StudioClipCopyTest)
#include "StudioClipCopyTest.moc"
