/* Native track order, pattern ownership and recovery tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

class StudioTrackMoveTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	std::array<PatternTrack*, 3> m_patterns{};
	std::array<Track*, 3> m_rows{};
	std::array<std::array<QPointer<Clip>, 3>, 3> m_clips{};
	InstrumentTrack* m_instrument = nullptr;
	TrackContainer* song() const { return Engine::getSong(); }
	PatternStore* store() const { return Engine::patternStore(); }
	void advanceTick()
	{
		const int periods = static_cast<int>(std::ceil(Engine::framesPerTick() / Engine::audioEngine()->framesPerPeriod())) + 1;
		for (int i = 0; i < periods; ++i) { Engine::audioEngine()->renderNextPeriod(); }
	}
	QStringList names(TrackContainer* owner) const
	{
		QStringList result; for (auto track : owner->tracks()) { result.append(track->name()); } return result;
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	QJsonObject manifest(const QTemporaryDir& directory) const
	{
		QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
	void fixture()
	{
		for (int i = 0; i < 3; ++i)
		{
			m_patterns[i] = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song())); m_patterns[i]->setName(QString("Pattern %1").arg(i));
			m_patterns[i]->createClip(TimePos(i * 2 * TimePos::ticksPerBar()));
			if (i < 2) { Track::create(Track::Type::Instrument, song())->setName(QString("Plain %1").arg(i)); }
		}
		m_rows[0] = Track::create(Track::Type::Instrument, store()); m_rows[0]->setName("Notes");
		m_rows[1] = Track::create(Track::Type::Sample, store()); m_rows[1]->setName("Samples");
		m_rows[2] = Track::create(Track::Type::Automation, store()); m_rows[2]->setName("Curves");
		m_instrument = static_cast<InstrumentTrack*>(m_rows[0]);
		for (int i = 0; i < 3; ++i)
		{
			for (int row = 0; row < 3; ++row) { m_clips[row][i] = m_rows[row]->getClip(i); }
			auto notes = static_cast<MidiClip*>(m_rows[0]->getClip(i)); notes->clearNotes();
			notes->addNote(Note(TimePos(48), TimePos(0), 60 + i, 80 + i, i * 10), false);
			auto sample = static_cast<SampleClip*>(m_rows[1]->getClip(i)); sample->setAutoResize(false);
			sample->changeLength(TimePos(96 + i)); sample->setStartTimeOffset(TimePos(12 + i));
			auto curve = static_cast<AutomationClip*>(m_rows[2]->getClip(i)); QVERIFY(curve->addObject(m_instrument->volumeModel()));
			curve->putValue(0, 20.f * (i + 1), false, true);
		}
		store()->setCurrentPattern(1);
	}
	void assertOwnership()
	{
		int column = 0;
		for (auto track : song()->tracks())
		{
			const auto pattern = dynamic_cast<PatternTrack*>(track); if (!pattern) { continue; }
			QCOMPARE(pattern->patternIndex(), column++); QCOMPARE(static_cast<PatternClip*>(pattern->getClip(0))->patternIndex(), pattern->patternIndex());
			const auto found = std::find(m_patterns.begin(), m_patterns.end(), pattern); QVERIFY(found != m_patterns.end());
			const auto original = static_cast<int>(found - m_patterns.begin()); const int index = pattern->patternIndex();
			for (int row = 0; row < 3; ++row)
			{
				QVERIFY(m_clips[row][original]); QCOMPARE(m_rows[row]->getClip(index), m_clips[row][original].data());
				QCOMPARE(m_rows[row]->getClip(index)->startPosition().getTicks(), index * TimePos::ticksPerBar());
			}
			const auto notes = static_cast<MidiClip*>(m_rows[0]->getClip(index)); QCOMPARE(notes->notes().size(), std::size_t(1));
			QCOMPARE(notes->notes().front()->key(), 60 + original);
			const auto sample = static_cast<SampleClip*>(m_rows[1]->getClip(index)); QCOMPARE(sample->length().getTicks(), 96 + original);
			QCOMPARE(sample->startTimeOffset().getTicks(), 12 + original);
			const auto curve = static_cast<AutomationClip*>(m_rows[2]->getClip(index)); QCOMPARE(curve->valueAt(0), 20.f * (original + 1));
			QCOMPARE(curve->objects().front().data(), m_instrument->volumeModel());
		}
	}
	QJsonObject semanticState() const
	{
		QJsonArray patterns;
		Track* notes = nullptr; Track* curves = nullptr;
		for (auto row : store()->tracks()) { if (row->name() == "Notes") { notes = row; } if (row->name() == "Curves") { curves = row; } }
		for (auto track : song()->tracks())
		{
			if (auto pattern = dynamic_cast<PatternTrack*>(track))
			{
				const int index = pattern->patternIndex(); const auto midi = static_cast<MidiClip*>(notes->getClip(index));
				const auto curve = static_cast<AutomationClip*>(curves->getClip(index));
				patterns.append(QJsonObject{{"name", pattern->name()}, {"column", index}, {"key", midi->notes().front()->key()}, {"value", curve->valueAt(0)}});
			}
		}
		return {{"song", QJsonArray::fromStringList(names(song()))}, {"rows", QJsonArray::fromStringList(names(store()))}, {"patterns", patterns}};
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void ordinaryRowsMoveWithinEitherContainer_data()
	{
		QTest::addColumn<bool>("patternRows"); QTest::addColumn<bool>("backward");
		QTest::newRow("song-forward") << false << false; QTest::newRow("song-backward") << false << true;
		QTest::newRow("pattern-forward") << true << false; QTest::newRow("pattern-backward") << true << true;
	}
	void ordinaryRowsMoveWithinEitherContainer()
	{
		QFETCH(bool, patternRows); QFETCH(bool, backward); fixture();
		auto owner = patternRows ? static_cast<TrackContainer*>(store()) : song();
		const int from = patternRows ? (backward ? 2 : 0) : (backward ? 3 : 1); const int to = backward ? 0 : static_cast<int>(owner->tracks().size()) - 1;
		const auto retained = owner->tracks().at(from); retained->setMuted(true); auto expected = names(owner); expected.move(from, to);
		const auto renamed = "Moved " + retained->name(); expected[to] = renamed;
		QSignalSpy moved(owner, &TrackContainer::trackMoved);
		const auto response = StudioCapabilities::execute("track.edit", {{"container", patternRows ? "pattern" : "song"}, {"track", from}, {"to", to}, {"name", renamed}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(names(owner), expected);
		QCOMPARE(owner->tracks().at(to), retained); QVERIFY(retained->isMuted()); QCOMPARE(moved.count(), 1); QCOMPARE(store()->currentPattern(), 1);
		assertOwnership();
	}

	void patternMovesRotateEveryCrossedColumn_data()
	{
		QTest::addColumn<int>("from"); QTest::addColumn<int>("to"); QTest::addColumn<bool>("oddMeter");
		QTest::newRow("first-last") << 0 << 4 << false; QTest::newRow("last-first") << 4 << 0 << false;
		QTest::newRow("middle-first") << 2 << 0 << false; QTest::newRow("middle-last") << 2 << 4 << false;
		QTest::newRow("ends-at-plain-row") << 0 << 3 << false; QTest::newRow("starts-after-plain-row") << 4 << 1 << false;
		QTest::newRow("seven-eight-forward") << 0 << 4 << true; QTest::newRow("seven-eight-backward") << 4 << 0 << true;
	}
	void patternMovesRotateEveryCrossedColumn()
	{
		QFETCH(int, from); QFETCH(int, to); QFETCH(bool, oddMeter);
		if (oddMeter) { Engine::getSong()->getTimeSigModel().setNumerator(7); Engine::getSong()->getTimeSigModel().setDenominator(8); }
		fixture(); auto expected = names(song()); expected.move(from, to); const auto selected = m_patterns[1];
		const auto retained = song()->tracks().at(from); QSignalSpy moved(song(), &TrackContainer::trackMoved);
		QVERIFY(song()->moveTrack(retained, to)); QCOMPARE(names(song()), expected); QCOMPARE(moved.count(), 1); assertOwnership();
		QCOMPARE(PatternTrack::findPatternTrack(store()->currentPattern()), selected); QVERIFY(Engine::getSong()->isModified());
		const auto inspected = StudioCapabilities::inspect().value("tracks").toArray();
		QCOMPARE(inspected.at(to).toObject().value("patternIndex").toInt(), static_cast<PatternTrack*>(retained)->patternIndex());
		QVERIFY(song()->moveTrack(retained, to)); QCOMPARE(moved.count(), 1);
	}

	void invalidNativeMovesDoNotChangeOrder_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("negative") << 0; QTest::newRow("past-end") << 1;
		QTest::newRow("null") << 2; QTest::newRow("foreign-container") << 3;
	}
	void invalidNativeMovesDoNotChangeOrder()
	{
		QFETCH(int, failure); fixture(); const auto before = semanticState(); const bool modified = Engine::getSong()->isModified();
		QSignalSpy moved(song(), &TrackContainer::trackMoved);
		auto target = failure == 2 ? nullptr : failure == 3 ? m_rows[0] : static_cast<Track*>(m_patterns[0]);
		QVERIFY(!song()->moveTrack(target, failure == 0 ? -1 : failure == 1 ? 5 : 1));
		QCOMPARE(semanticState(), before); QCOMPARE(moved.count(), 0); QCOMPARE(Engine::getSong()->isModified(), modified); assertOwnership();
	}

	void capabilityRejectsInvalidSelectorsBeforeMutation_data()
	{
		QTest::addColumn<QJsonObject>("arguments");
		QTest::newRow("negative") << QJsonObject{{"track", 0}, {"to", -1}};
		QTest::newRow("past-end") << QJsonObject{{"track", 0}, {"to", 5}};
		QTest::newRow("fractional") << QJsonObject{{"track", 0}, {"to", 1.5}};
		QTest::newRow("invalid-combined-name") << QJsonObject{{"track", 0}, {"to", 4}, {"name", ""}};
		QTest::newRow("invalid-container") << QJsonObject{{"container", "unknown"}, {"track", 0}, {"to", 1}};
		QTest::newRow("invalid-track") << QJsonObject{{"track", 9}, {"to", 1}};
	}
	void capabilityRejectsInvalidSelectorsBeforeMutation()
	{
		QFETCH(QJsonObject, arguments); fixture(); const auto before = semanticState();
		QVERIFY(!StudioCapabilities::execute("track.edit", arguments).value("ok").toBool()); QCOMPARE(semanticState(), before); assertOwnership();
	}

	void sparsePatternStoreCannotPartiallyReorder()
	{
		fixture(); delete m_rows[1]->getClip(2); const auto before = semanticState(); const auto order = names(song());
		QSignalSpy moved(song(), &TrackContainer::trackMoved); QVERIFY(!song()->moveTrack(m_patterns[0], 4));
		QCOMPARE(names(song()), order); QCOMPARE(semanticState(), before); QCOMPARE(moved.count(), 0); QCOMPARE(m_rows[1]->numOfClips(), 2);
		m_rows[1]->createClipsForPattern(2); // Restore the intentionally malformed fixture for native teardown.
	}

	void reentryAndForeignThreadsCannotInterleaveMoves()
	{
		fixture(); bool threadResult = true;
		std::thread foreign([&] { threadResult = song()->moveTrack(m_patterns[0], 4); }); foreign.join(); QVERIFY(!threadResult);
		bool attempted = false, nestedResult = true; int observedCount = 0; QObject callbacks;
		QObject::connect(m_rows[0]->getClip(0), &Clip::positionChanged, &callbacks, [&] {
			attempted = true; nestedResult = song()->moveTrack(m_patterns[2], 0);
		});
		QObject::connect(song(), &TrackContainer::trackMoved, &callbacks, [&] { observedCount = song()->countTracks(); });
		QVERIFY(song()->moveTrack(m_patterns[0], 4)); QVERIFY(attempted); QVERIFY(!nestedResult); QCOMPARE(observedCount, 5); assertOwnership();
	}

	void selectedPatternPlaybackRetainsItsAutomation_data()
	{
		QTest::addColumn<int>("selected"); QTest::newRow("moving") << 0; QTest::newRow("crossed-middle") << 1; QTest::newRow("crossed-last") << 2;
	}
	void selectedPatternPlaybackRetainsItsAutomation()
	{
		QFETCH(int, selected); fixture(); store()->setCurrentPattern(selected); Engine::getSong()->playPattern();
		advanceTick(); QCOMPARE(m_instrument->volumeModel()->value(), 20.f * (selected + 1));
		QVERIFY(song()->moveTrack(m_patterns[0], 4)); QCOMPARE(PatternTrack::findPatternTrack(store()->currentPattern()), m_patterns[selected]);
		advanceTick(); QCOMPARE(m_instrument->volumeModel()->value(), 20.f * (selected + 1)); assertOwnership();
	}

	void patternRowOrderControlsNativeAutomationPrecedence()
	{
		fixture(); auto later = Track::create(Track::Type::Automation, store());
		for (int i = 0; i < 3; ++i)
		{
			auto curve = static_cast<AutomationClip*>(later->getClip(i)); QVERIFY(curve->addObject(m_instrument->volumeModel()));
			curve->putValue(0, 90, false, true);
		}
		store()->setCurrentPattern(0); Engine::getSong()->playPattern(); advanceTick();
		QCOMPARE(m_instrument->volumeModel()->value(), 90.f);
		QVERIFY(store()->moveTrack(m_rows[2], 3)); advanceTick();
		QCOMPARE(m_instrument->volumeModel()->value(), 20.f);
		QCOMPARE(static_cast<AutomationClip*>(later->getClip(0))->valueAt(0), 90.f); assertOwnership();
	}

	void nativeHistoryAndProjectRoundTripsKeepReorderedContent_data()
	{
		QTest::addColumn<bool>("patternRows"); QTest::newRow("song-pattern-columns") << false; QTest::newRow("pattern-track-rows") << true;
	}
	void nativeHistoryAndProjectRoundTripsKeepReorderedContent()
	{
		QFETCH(bool, patternRows); QTemporaryDir session; QJsonObject movedState, move;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); fixture();
			QVERIFY(runtime.process(request("fixture", "song.set", {{"tempo", 101}})).value("ok").toBool()); const auto before = semanticState();
			move = request("move", "track.edit", {{"container", patternRows ? "pattern" : "song"}, {"track", 0}, {"to", patternRows ? 2 : 4}});
			QVERIFY(runtime.process(move).value("ok").toBool()); movedState = semanticState(); QVERIFY(movedState != before);
			QVERIFY(runtime.process(move).value("replayed").toBool()); QCOMPARE(semanticState(), movedState);
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(semanticState(), before);
			QVERIFY(runtime.process(move).value("replayed").toBool()); QCOMPARE(semanticState(), before);
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(semanticState(), movedState);
			const auto history = manifest(session); QVERIFY(!history.isEmpty());
			QVERIFY(!runtime.process(request("bad", "track.edit", {{"track", 0}, {"to", 99}})).value("ok").toBool()); QCOMPARE(semanticState(), movedState);
			QCOMPARE(manifest(session).value("history"), history.value("history")); QCOMPARE(manifest(session).value("cursor"), history.value("cursor"));
			const auto path = session.filePath("reordered.mmp");
			QVERIFY(runtime.process(request("save", "project.save", {{"path", path}})).value("ok").toBool());
			QVERIFY(runtime.process(request("load", "project.load", {{"path", path}})).value("ok").toBool()); QCOMPARE(semanticState(), movedState);
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(semanticState(), movedState);
		QVERIFY(recovered.process(move).value("replayed").toBool()); QCOMPARE(semanticState(), movedState);
	}
};
QTEST_GUILESS_MAIN(StudioTrackMoveTest)
#include "StudioTrackMoveTest.moc"
