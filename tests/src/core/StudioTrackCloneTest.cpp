/* Native track duplication, identity and failure cleanup tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QJsonArray>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

class CloneFaultSampleTrack : public SampleTrack
{
public:
	using SampleTrack::SampleTrack;
	int creationFault = 0;
	int serializationFault = 0;
	bool inspectResourceScope = false;
	Clip* createClip(const TimePos& pos) override
	{
		if (creationFault == 1) { throw std::runtime_error("fixture before cell construction"); }
		auto clip = SampleTrack::createClip(pos);
		if (creationFault == 2) { throw std::runtime_error("fixture after cell construction"); }
		return clip;
	}
	void saveTrackSpecificSettings(QDomDocument& doc, QDomElement& element, bool presetMode) override
	{
		SampleTrack::saveTrackSpecificSettings(doc, element, presetMode);
		if (inspectResourceScope)
		{
			QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		}
		if (serializationFault == 1) { throw std::runtime_error("fixture serialization exception"); }
		if (serializationFault == 2) { DataFile::recordSerializationError(doc, "fixture serialization rejection"); }
	}
};

class CloneFaultContainer : public TrackContainer
{
public:
	bool fail = true;
	void updateAfterTrackAdd() override { if (fail) { throw std::runtime_error("fixture after track construction"); } }
};

class StudioTrackCloneTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	Song* song() const { return Engine::getSong(); }
	PatternStore* store() const { return Engine::patternStore(); }
	FloatModel* target() const { return static_cast<InstrumentTrack*>(song()->tracks().at(0))->volumeModel(); }
	Track::Type type(int kind) const { return kind == 0 ? Track::Type::Instrument : kind == 1 ? Track::Type::Sample : Track::Type::Automation; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	void fill(Clip* clip, int column)
	{
		if (auto midi = dynamic_cast<MidiClip*>(clip))
		{
			midi->clearNotes(); Note note(27, 11, 60 + column, 82, -19); note.createDetuning();
			note.detuning()->automationClip()->putValue(0, column + .25f, false); midi->addNote(note, false);
		}
		if (auto sample = dynamic_cast<SampleClip*>(clip))
		{
			sample->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(256, SampleFrame(.125f * (column + 1), -.25f)), 24000));
			QVERIFY(sample->setSampleWindow(7, 200)); sample->sample().setAmplification(.625f); sample->sample().setReversed(true);
		}
		if (auto automation = dynamic_cast<AutomationClip*>(clip))
		{
			QVERIFY(automation->addObject(target())); automation->putValues(0, 20 + column, 30 + column, false, true);
			automation->putValue(48, 70 + column, false, true); automation->setProgressionType(AutomationClip::ProgressionType::Linear);
		}
		clip->setName(QString("Cell %1").arg(column)); clip->setAutoResize(false); clip->changeLength(97 + column);
		clip->setStartTimeOffset(-7 - column); clip->setColor(QColor("#6ca83e"));
	}
	void assertContent(Clip* clip, int column)
	{
		QCOMPARE(clip->name(), QString("Cell %1").arg(column)); QCOMPARE(clip->length().getTicks(), 97 + column);
		QCOMPARE(clip->startTimeOffset().getTicks(), -7 - column); QVERIFY(!clip->getAutoResize());
		QVERIFY(clip->color()); QCOMPARE(*clip->color(), QColor("#6ca83e"));
		if (auto midi = dynamic_cast<MidiClip*>(clip))
		{
			QCOMPARE(midi->notes().size(), std::size_t(1)); const auto note = midi->notes().front();
			QCOMPARE(note->key(), 60 + column); QCOMPARE(note->pos().getTicks(), 11); QCOMPARE(note->length().getTicks(), 27);
			QVERIFY(note->detuning());
			QCOMPARE(note->detuning()->automationClip()->valueAt(0), column + .25f);
			QCOMPARE(midi->instrumentTrack(), static_cast<InstrumentTrack*>(clip->getTrack()));
		}
		if (auto sample = dynamic_cast<SampleClip*>(clip))
		{
			QCOMPARE(sample->sample().sampleSize(), std::size_t(256)); QCOMPARE(sample->sample().sampleRate(), 24000);
			QCOMPARE(sample->sample().startFrame(), 7); QCOMPARE(sample->sample().endFrame(), 200);
			QCOMPARE(sample->sample().data()[31][0], .125f * (column + 1)); QCOMPARE(sample->sample().amplification(), .625f);
			QVERIFY(sample->sample().reversed()); QVERIFY(!sample->isPlaying());
		}
		if (auto automation = dynamic_cast<AutomationClip*>(clip))
		{
			QCOMPARE(automation->objects().size(), std::size_t(1)); QCOMPARE(automation->objects().front().data(), target());
			QCOMPARE(automation->getTimeMap().value(0).getInValue(), 20.f + column); QCOMPARE(automation->valueAt(48), 70.f + column);
		}
	}
	void patterns()
	{
		Track::create(Track::Type::Instrument, song());
		for (int i = 0; i < 3; ++i)
		{
			auto track = Track::create(Track::Type::Pattern, song()); track->setName(QString("Pattern %1").arg(i));
			auto marker = track->createClip(192 * (i + 1)); marker->setAutoResize(false); marker->changeLength(384); marker->setStartTimeOffset(-24);
		}
		for (int kind = 0; kind < 3; ++kind)
		{
			auto row = Track::create(type(kind), store()); for (int column = 0; column < 3; ++column) { fill(row->getClip(column), column); }
		}
		store()->setCurrentPattern(0);
	}
	void assertPatternCopy(int source)
	{
		QCOMPARE(store()->numOfPatterns(), 4); QCOMPARE(store()->currentPattern(), 3);
		for (auto row : store()->tracks())
		{
			QCOMPARE(row->numOfClips(), 4); assertContent(row->getClip(3), source);
			for (int i = 0; i < 4; ++i) { QCOMPARE(row->getClip(i)->startPosition().getTicks(), i * TimePos::ticksPerBar()); }
		}
		auto copy = PatternTrack::findPatternTrack(3); QVERIFY(copy); QCOMPARE(copy->name(), QString("Clone of Pattern %1").arg(source));
		QCOMPARE(copy->numOfClips(), 1); QCOMPARE(static_cast<PatternClip*>(copy->getClip(0))->patternIndex(), 3);
		QCOMPARE(copy->getClip(0)->startPosition().getTicks(), 192 * (source + 1)); QCOMPARE(copy->getClip(0)->startTimeOffset().getTicks(), -24);
	}
	void assertModelLockReleased()
	{
		// A leaked recursive model lock would otherwise pass another owner-thread edit.
		auto probe = std::async(std::launch::async, [] { const auto guard = Engine::audioEngine()->requestChangesGuard(); return true; });
		QVERIFY(probe.wait_for(std::chrono::seconds(1)) == std::future_status::ready); QVERIFY(probe.get());
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { song()->clearProject(); song()->clearErrors(); }
	void cleanup() { song()->stop(); song()->clearProject(); song()->getSaveOptions().setDefaultOptions(); }

	void songAndPatternRowsRetainNativeClipsAndSettings_data()
	{
		QTest::addColumn<int>("kind"); QTest::addColumn<bool>("patternRow");
		QTest::newRow("song-midi") << 0 << false; QTest::newRow("song-sample") << 1 << false; QTest::newRow("song-automation") << 2 << false;
		QTest::newRow("pattern-midi") << 0 << true; QTest::newRow("pattern-sample") << 1 << true; QTest::newRow("pattern-automation") << 2 << true;
	}
	void songAndPatternRowsRetainNativeClipsAndSettings()
	{
		QFETCH(int, kind); QFETCH(bool, patternRow); Track::create(Track::Type::Instrument, song());
		if (patternRow) { for (int i = 0; i < 3; ++i) { Track::create(Track::Type::Pattern, song()); } }
		auto owner = patternRow ? static_cast<TrackContainer*>(store()) : song(); auto source = Track::create(type(kind), owner);
		source->setName("Native duplicate"); source->setMuted(true); source->setMutedBeforeSolo(true); source->setHeight(93); source->setColor(QColor("#cc8423"));
		for (int i = 0; i < 3; ++i) { fill(patternRow ? source->getClip(i) : source->createClip(192 * i), i); }
		const auto original = source->getClips(); auto copy = source->clone(); QVERIFY2(copy, qPrintable(song()->errorSummary()));
		QCOMPARE(copy->trackContainer(), owner); QCOMPARE(copy->numOfClips(), 3); QVERIFY(copy->isMuted()); QVERIFY(copy->isMutedBeforeSolo());
		QCOMPARE(copy->getHeight(), 93); QCOMPARE(copy->name(), source->name()); QVERIFY(copy->color() == source->color());
		for (int i = 0; i < 3; ++i)
		{
			QCOMPARE(source->getClip(i), original.at(i)); QVERIFY(copy->getClip(i) != original.at(i)); assertContent(copy->getClip(i), i);
			if (kind == 1) { QCOMPARE(static_cast<SampleClip*>(copy->getClip(i))->sample().buffer().get(), static_cast<SampleClip*>(original.at(i))->sample().buffer().get()); }
		}
		QTemporaryDir directory; const auto path = directory.filePath("track-copy.mmp"); QVERIFY(song()->saveProjectFile(path)); QVERIFY(song()->loadProject(path));
		owner = patternRow ? static_cast<TrackContainer*>(store()) : song(); auto restored = owner->tracks().back();
		QCOMPARE(restored->numOfClips(), 3); for (int i = 0; i < 3; ++i) { assertContent(restored->getClip(i), i); }
	}

	void patternCloningCopiesEveryOwnedColumn_data()
	{
		QTest::addColumn<int>("source"); QTest::addColumn<bool>("oddMeter");
		QTest::newRow("first") << 0 << false; QTest::newRow("middle") << 1 << false; QTest::newRow("last") << 2 << false;
		QTest::newRow("first-seven-eight") << 0 << true; QTest::newRow("middle-seven-eight") << 1 << true; QTest::newRow("last-seven-eight") << 2 << true;
	}
	void patternCloningCopiesEveryOwnedColumn()
	{
		QFETCH(int, source); QFETCH(bool, oddMeter); if (oddMeter) { song()->getTimeSigModel().setNumerator(7); song()->getTimeSigModel().setDenominator(8); }
		patterns(); std::vector<Clip*> original;
		for (auto row : store()->tracks()) { original.insert(original.end(), row->getClips().begin(), row->getClips().end()); }
		auto copy = PatternTrack::findPatternTrack(source)->clone(); QVERIFY2(copy, qPrintable(song()->errorSummary())); assertPatternCopy(source);
		int index = 0; for (auto row : store()->tracks()) { for (int i = 0; i < 3; ++i) { QCOMPARE(row->getClip(i), original.at(index++)); } }
		auto notes = static_cast<MidiClip*>(store()->tracks().at(0)->getClip(3)); auto prior = static_cast<MidiClip*>(store()->tracks().at(0)->getClip(source));
		QVERIFY(notes->notes().front()->detuning() != prior->notes().front()->detuning());
		notes->notes().front()->setKey(41); QCOMPARE(prior->notes().front()->key(), 60 + source);
		auto samples = store()->tracks().at(1); QCOMPARE(static_cast<SampleClip*>(samples->getClip(3))->sample().buffer().get(), static_cast<SampleClip*>(samples->getClip(source))->sample().buffer().get());
	}

	void trackCopyUsesResidentAudioWhenTheOriginalPathIsAbsent()
	{
		auto source = Track::create(Track::Type::Sample, song()); auto clip = static_cast<SampleClip*>(source->createClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(93, SampleFrame(.25f)), 32000, m_configuration.filePath("missing-original.wav")));
		auto copy = source->clone(); QVERIFY2(copy, qPrintable(song()->errorSummary()));
		QCOMPARE(static_cast<SampleClip*>(copy->getClip(0))->sample().buffer().get(), clip->sample().buffer().get());
		QVERIFY(!song()->hasErrors()); delete source; QCOMPARE(static_cast<SampleClip*>(copy->getClip(0))->sample().data()[92][0], .25f);
	}

	void duplicateParameterIdsAndAutomationRemainDistinctAfterReload_data()
	{
		QTest::addColumn<bool>("loaded"); QTest::newRow("fresh-identities") << false; QTest::newRow("loaded-identities") << true;
	}
	void duplicateParameterIdsAndAutomationRemainDistinctAfterReload()
	{
		QFETCH(bool, loaded); auto source = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song())); source->volumeModel()->setValue(47);
		auto curves = Track::create(Track::Type::Automation, song()); auto first = static_cast<AutomationClip*>(curves->createClip(0));
		QVERIFY(first->addObject(source->volumeModel())); first->putValue(0, 23, false);
		QTemporaryDir directory; const auto path = directory.filePath("ids.mmp");
		if (loaded) { QVERIFY(song()->saveProjectFile(path)); QVERIFY(song()->loadProject(path)); source = static_cast<InstrumentTrack*>(song()->tracks().at(0)); curves = song()->tracks().at(1); }
		const auto sourceId = source->volumeModel()->id(); auto copy = static_cast<InstrumentTrack*>(source->clone()); QVERIFY(copy);
		QCOMPARE(source->volumeModel()->id(), sourceId); QVERIFY(ProjectJournal::idToSave(copy->volumeModel()->id()) != ProjectJournal::idToSave(sourceId));
		QCOMPARE(copy->volumeModel()->value(), 47.f); auto second = static_cast<AutomationClip*>(curves->createClip(384));
		QVERIFY(second->addObject(copy->volumeModel())); second->putValue(0, 71, false);
		QVERIFY(song()->saveProjectFile(path)); QVERIFY(song()->loadProject(path)); QVERIFY2(!song()->hasErrors(), qPrintable(song()->errorSummary()));
		source = static_cast<InstrumentTrack*>(song()->tracks().at(0)); copy = static_cast<InstrumentTrack*>(song()->tracks().at(2)); curves = song()->tracks().at(1);
		QCOMPARE(static_cast<AutomationClip*>(curves->getClip(0))->objects().front().data(), source->volumeModel());
		QCOMPARE(static_cast<AutomationClip*>(curves->getClip(1))->objects().front().data(), copy->volumeModel());
	}

	void midiControllerAndPortSettingsAreNativeCopies()
	{
		auto source = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song()));
		source->midiPort()->setInputChannel(5); source->midiPort()->setOutputChannel(11);
		QVERIFY(StudioCapabilities::execute("controller.midi.connect", {{"parameter", static_cast<int>(source->volumeModel()->id())}, {"inputController", 74}, {"inputChannel", 3}}).value("ok").toBool());
		auto copy = static_cast<InstrumentTrack*>(source->clone()); QVERIFY2(copy, qPrintable(song()->errorSummary()));
		QCOMPARE(copy->midiPort()->inputChannel(), 5); QCOMPARE(copy->midiPort()->outputChannel(), 11);
		const auto original = dynamic_cast<MidiController*>(source->volumeModel()->controllerConnection()->getController());
		const auto controller = dynamic_cast<MidiController*>(copy->volumeModel()->controllerConnection()->getController());
		QVERIFY(original && controller); QVERIFY(original != controller); QCOMPARE(controller->midiPort()->inputController(), 74);
		QCOMPARE(controller->midiPort()->inputChannel(), 3); QVERIFY(song()->controllers().empty());
	}

	void recordingInAnyPatternColumnPreventsStructuralCopy_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("note-reserved") << 0; QTest::newRow("sample-armed") << 1; QTest::newRow("last-row-automation-reserved") << 2;
		QTest::newRow("sample-armed-in-another-column") << 3;
	}
	void recordingInAnyPatternColumnPreventsStructuralCopy()
	{
		QFETCH(int, failure); patterns(); std::vector<Clip*> original; for (auto row : store()->tracks()) { original.insert(original.end(), row->getClips().begin(), row->getClips().end()); }
		auto row = store()->tracks().at(failure == 3 ? 1 : failure); auto cell = row->getClip(failure == 3 ? 2 : 1);
		if (failure == 0) { QVERIFY(row->beginNoteRecording(this)); }
		if (failure == 1 || failure == 3) { static_cast<SampleClip*>(cell)->setRecord(true); }
		if (failure == 2) { static_cast<AutomationClip*>(cell)->reserveRecordingTake(64, 1); }
		QVERIFY(!PatternTrack::findPatternTrack(1)->clone()); QCOMPARE(song()->tracks().size(), std::size_t(4)); QCOMPARE(store()->currentPattern(), 0);
		QVERIFY(!PatternTrack::findPatternTrack(3)); int index = 0;
		for (auto existing : store()->tracks()) { QCOMPARE(existing->numOfClips(), 3); for (auto clip : existing->getClips()) { QCOMPARE(clip, original.at(index++)); } }
		assertModelLockReleased();
		if (failure == 0) { row->endNoteRecording(this); } if (failure == 1 || failure == 3) { static_cast<SampleClip*>(cell)->setRecord(false); }
		if (failure == 2) { static_cast<AutomationClip*>(cell)->discardRecordingTake(); }
		song()->clearErrors(); QVERIFY(PatternTrack::findPatternTrack(1)->clone()); assertPatternCopy(1);
	}

	void sparsePatternRowsAreRejectedWithoutRepair()
	{
		patterns(); auto row = store()->tracks().at(0); delete row->getClip(2); const auto retained = row->getClip(0);
		QVERIFY(!PatternTrack::findPatternTrack(0)->clone()); QVERIFY(!row->clone());
		QCOMPARE(row->numOfClips(), 2); QCOMPARE(row->getClip(0), retained); QCOMPARE(song()->tracks().size(), std::size_t(4)); QCOMPARE(store()->tracks().size(), std::size_t(3));
	}

	void cancellingPartialCopiesDoesNotPumpEventsOrRetainTheDuplicate_data()
	{
		QTest::addColumn<bool>("pattern"); QTest::newRow("sample-track") << false; QTest::newRow("pattern-column") << true;
	}
	void cancellingPartialCopiesDoesNotPumpEventsOrRetainTheDuplicate()
	{
		QFETCH(bool, pattern); Track* source;
		if (pattern) { patterns(); source = PatternTrack::findPatternTrack(1); }
		else
		{
			auto sample = new CloneFaultSampleTrack(song()); sample->inspectResourceScope = true; source = sample;
			for (int i = 0; i < 3; ++i) { fill(source->createClip(192 * i), i); }
		}
		const auto count = song()->tracks().size(); bool eventPolled = false, cancelled = false; QTimer timer; timer.setSingleShot(true);
		connect(&timer, &QTimer::timeout, &timer, [&] { eventPolled = true; }); timer.start(0);
		{
			FileResourceIO::Scope scope([&] {
				if (pattern) { cancelled = cancelled || (store()->numOfPatterns() == 4 && store()->tracks().at(0)->getClip(3)->name() == "Cell 1"); }
				else { cancelled = cancelled || (song()->tracks().size() > count && song()->tracks().back()->numOfClips() > 0); }
				return cancelled;
			}, true);
			QVERIFY(!source->clone());
		}
		QVERIFY(cancelled); QVERIFY(!eventPolled); QCOMPARE(song()->tracks().size(), count); assertModelLockReleased();
		if (pattern) { QCOMPARE(store()->currentPattern(), 0); for (auto row : store()->tracks()) { QCOMPARE(row->numOfClips(), 3); } }
		{
			FileResourceIO::Scope resumed({}, true); QString error;
			QVERIFY(FileResourceIO::check(error)); QVERIFY(eventPolled);
		}
		song()->clearErrors(); QVERIFY(source->clone());
	}

	void serializationFailuresDoNotCreateTracksOrLeakTheModelLock_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("exception") << 1; QTest::newRow("document-error") << 2;
	}
	void serializationFailuresDoNotCreateTracksOrLeakTheModelLock()
	{
		QFETCH(int, failure); auto source = new CloneFaultSampleTrack(song()); source->createClip(0); source->serializationFault = failure;
		QVERIFY(!source->clone()); QCOMPARE(song()->tracks().size(), std::size_t(1)); QVERIFY(song()->hasErrors()); assertModelLockReleased();
		song()->clearErrors(); source->serializationFault = 0; QVERIFY(source->clone());
	}

	void failedPatternConstructionRemovesMapEntriesAndPartialCells_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("before-cell") << 1; QTest::newRow("after-cell") << 2;
	}
	void failedPatternConstructionRemovesMapEntriesAndPartialCells()
	{
		QFETCH(int, failure); patterns(); auto fault = new CloneFaultSampleTrack(store()); fault->createClipsForPattern(2); fault->creationFault = failure;
		QVERIFY_EXCEPTION_THROWN(Track::create(Track::Type::Pattern, song()), std::runtime_error);
		QCOMPARE(song()->tracks().size(), std::size_t(4)); QCOMPARE(store()->numOfPatterns(), 3); QCOMPARE(store()->currentPattern(), 0);
		QVERIFY(!PatternTrack::findPatternTrack(3)); for (auto row : store()->tracks()) { QCOMPARE(row->numOfClips(), 3); }
		assertModelLockReleased(); fault->creationFault = 0; auto next = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song()));
		QCOMPARE(next->patternIndex(), 3); QCOMPARE(store()->numOfPatterns(), 4);
	}

	void failedTrackFactoriesRemoveConstructedTracks_data()
	{
		QTest::addColumn<bool>("restoring"); QTest::newRow("post-construction-hook") << false; QTest::newRow("restoration-exception") << true;
	}
	void failedTrackFactoriesRemoveConstructedTracks()
	{
		QFETCH(bool, restoring); CloneFaultContainer owner; owner.setType(TrackContainer::Type::Song);
		if (restoring)
		{
			owner.fail = false;
			connect(&owner, &TrackContainer::trackAdded, &owner, [](Track* track) {
				connect(track->getMutedModel(), &BoolModel::dataChanged, track, [] { throw std::runtime_error("fixture restoration failure"); });
			});
			QDomDocument doc; auto state = doc.createElement("track"); state.setAttribute("type", static_cast<int>(Track::Type::Sample)); state.setAttribute("muted", 1);
			QVERIFY_EXCEPTION_THROWN(Track::create(state, &owner), std::runtime_error);
		}
		else { QVERIFY_EXCEPTION_THROWN(Track::create(Track::Type::Sample, &owner), std::runtime_error); }
		QVERIFY(owner.tracks().empty()); assertModelLockReleased();
	}

	void identityScopeRestoresNormalLoadingAfterNestedExceptions()
	{
		FloatModel original(1, 0, 100), copy(2, 0, 100); original.setScaleLogarithmic(true);
		QDomDocument doc; auto root = doc.createElement("state"); original.saveSettings(doc, root, "parameter"); const auto id = copy.id();
		{
			JournallingObject::KeepIdentityScope outer; copy.loadSettings(root, "parameter"); QCOMPARE(copy.id(), id);
			try { JournallingObject::KeepIdentityScope inner; throw std::runtime_error("nested copy failure"); } catch (const std::runtime_error&) { }
			copy.loadSettings(root, "parameter"); QCOMPARE(copy.id(), id);
		}
		copy.loadSettings(root, "parameter"); QCOMPARE(copy.id(), ProjectJournal::idToSave(original.id()));
	}

	void allocatingNewIdsDoesNotAliasLoadedOrReservedIds()
	{
		ProjectJournal journal; for (int id = 0; id < 32768; ++id) { journal.reallocID(id, nullptr); }
		const auto allocated = journal.allocID(nullptr); QVERIFY(ProjectJournal::idToSave(allocated) >= 32768);
	}

	void nativeClonesSurviveHistoryReplayAndRecovery_data()
	{
		QTest::addColumn<bool>("pattern"); QTest::newRow("sample-track") << false; QTest::newRow("complete-pattern") << true;
	}
	void nativeClonesSurviveHistoryReplayAndRecovery()
	{
		QFETCH(bool, pattern); QTemporaryDir directory; const auto verify = [&] {
			if (pattern) { assertPatternCopy(1); }
			else { QCOMPARE(song()->tracks().size(), std::size_t(3)); assertContent(song()->tracks().at(2)->getClip(0), 1); }
		};
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			if (pattern) { patterns(); } else { Track::create(Track::Type::Instrument, song()); fill(Track::create(Track::Type::Sample, song())->createClip(384), 1); }
			QVERIFY(runtime.process(request("fixture", "song.set", {{"tempo", 103}})).value("ok").toBool()); const int before = song()->tracks().size();
			const auto cloning = request("clone", "track.clone", {{"track", pattern ? 2 : 1}}); auto response = runtime.process(cloning);
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); verify();
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(song()->tracks().size(), static_cast<std::size_t>(before));
			QVERIFY(runtime.process(cloning).value("replayed").toBool()); QCOMPARE(song()->tracks().size(), static_cast<std::size_t>(before));
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); verify();
			const auto saved = directory.filePath("clone.mmp"); QVERIFY(song()->saveProjectFile(saved)); QVERIFY(song()->loadProject(saved)); verify();
		}
		StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error)); verify();
	}
};
QTEST_GUILESS_MAIN(StudioTrackCloneTest)
#include "StudioTrackCloneTest.moc"
