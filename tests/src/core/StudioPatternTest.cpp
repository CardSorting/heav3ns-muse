/* Native pattern coordinates, deletion, automation and persistence.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <array>
#include <algorithm>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

class StudioPatternTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	std::vector<PatternTrack*> m_patterns;
	std::array<Track*, 3> m_rows{};
	PatternStore* store() const { return Engine::patternStore(); }
	Song* song() const { return Engine::getSong(); }
	FloatModel* target() const { return static_cast<InstrumentTrack*>(store()->tracks().at(0))->volumeModel(); }
	void meter(int numerator, int denominator)
	{
		song()->getTimeSigModel().setNumerator(numerator); song()->getTimeSigModel().setDenominator(denominator);
	}
	void fixture(int count = 3)
	{
		m_patterns.clear();
		for (int i = 0; i < count; ++i)
		{
			auto pattern = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song()));
			pattern->setName(QString("Pattern %1").arg(i)); m_patterns.push_back(pattern);
		}
		m_rows[0] = Track::create(Track::Type::Instrument, store());
		m_rows[1] = Track::create(Track::Type::Automation, store());
		m_rows[2] = Track::create(Track::Type::Sample, store());
		for (int i = 0; i < count; ++i)
		{
			auto midi = static_cast<MidiClip*>(m_rows[0]->getClip(i)); midi->clearNotes();
			midi->addNote(Note(TimePos(12), TimePos(0), 48 + i), false);
			auto curve = static_cast<AutomationClip*>(m_rows[1]->getClip(i)); QVERIFY(curve->addObject(target()));
			curve->setProgressionType(AutomationClip::ProgressionType::Linear);
			curve->putValue(0, 0, false, true); curve->putValue(192, 192, false, true);
		}
	}
	void assertColumns()
	{
		for (auto row : store()->tracks())
		{
			QCOMPARE(row->numOfClips(), store()->numOfPatterns());
			for (int i = 0; i < row->numOfClips(); ++i) { QCOMPARE(row->getClip(i)->startPosition().getTicks(), i * TimePos::ticksPerBar()); }
		}
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const { return {{"id", id}, {"op", op}, {"args", args}}; }
	QJsonArray content() const
	{
		QJsonArray result;
		for (auto track : song()->tracks())
		{
			auto pattern = static_cast<PatternTrack*>(track); auto midi = static_cast<MidiClip*>(store()->tracks().at(0)->getClip(pattern->patternIndex()));
			result.append(QJsonObject{{"name", pattern->name()}, {"note", midi->notes().front()->key()}});
		}
		return result;
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

	void everyPatternEvaluatesAutomationInLocalTime_data()
	{
		QTest::addColumn<int>("column"); QTest::addColumn<bool>("oddMeter");
		QTest::newRow("first") << 0 << false; QTest::newRow("second") << 1 << false; QTest::newRow("sixth") << 5 << false;
		QTest::newRow("first-seven-eight") << 0 << true; QTest::newRow("second-seven-eight") << 1 << true; QTest::newRow("sixth-seven-eight") << 5 << true;
	}
	void everyPatternEvaluatesAutomationInLocalTime()
	{
		QFETCH(int, column); QFETCH(bool, oddMeter); if (oddMeter) { meter(7, 8); } fixture(6);
		QCOMPARE(store()->automatedValuesAt(0, column).value(target()), 0.f);
		QCOMPARE(store()->automatedValuesAt(24, column).value(target()), 24.f);
		QCOMPARE(store()->automatedValuesAt(72, column).value(target()), 72.f);
		auto curve = static_cast<AutomationClip*>(m_rows[1]->getClip(column)); curve->setStartTimeOffset(TimePos(-12));
		QCOMPARE(store()->automatedValuesAt(24, column).value(target()), 36.f);
	}

	void nativePlaybackUsesTheSelectedPatternsCurrentTick()
	{
		fixture(); auto curve = static_cast<AutomationClip*>(m_rows[1]->getClip(2)); curve->clear();
		curve->setProgressionType(AutomationClip::ProgressionType::Discrete); curve->putValue(0, 20, false, true); curve->putValue(48, 80, false, true);
		store()->setCurrentPattern(2); song()->playPattern(); song()->setPlayPos(24, Song::PlayMode::Pattern);
		Engine::audioEngine()->renderNextPeriod(); QCOMPARE(target()->value(), 20.f);
		song()->setPlayPos(60, Song::PlayMode::Pattern); Engine::audioEngine()->renderNextPeriod(); QCOMPARE(target()->value(), 80.f);
		QVERIFY(song()->moveTrack(m_patterns[2], 0)); QCOMPARE(store()->currentPattern(), 0);
		song()->setPlayPos(24, Song::PlayMode::Pattern); Engine::audioEngine()->renderNextPeriod(); QCOMPARE(target()->value(), 20.f);
	}

	void songPatternAutomationMatchesSignedPlaybackOffsets_data()
	{
		QTest::addColumn<int>("offset"); QTest::newRow("zero") << 0; QTest::newRow("advance") << -24;
		QTest::newRow("delay") << 24; QTest::newRow("advance-over-loop") << -216; QTest::newRow("delay-over-loop") << 216;
	}
	void songPatternAutomationMatchesSignedPlaybackOffsets()
	{
		QFETCH(int, offset); fixture(); auto clip = static_cast<PatternClip*>(m_patterns[1]->createClip(TimePos(384)));
		clip->setAutoResize(false); clip->changeLength(TimePos(384)); clip->setStartTimeOffset(TimePos(offset));
		for (int tick : {0, 12, 96, 204})
		{
			const auto expected = static_cast<float>(((tick - offset) % 192 + 192) % 192);
			QCOMPARE(song()->automatedValuesAt(384 + tick).value(target()), expected);
		}
	}

	void meterChangesRepositionEveryNativeColumn_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator");
		QTest::newRow("three-four") << 3 << 4; QTest::newRow("seven-four") << 7 << 4;
		QTest::newRow("seven-eight") << 7 << 8; QTest::newRow("five-sixteen") << 5 << 16;
	}
	void meterChangesRepositionEveryNativeColumn()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); fixture(6);
		std::array<std::vector<QPointer<Clip>>, 3> retained;
		for (int row = 0; row < 3; ++row) { for (auto clip : m_rows[row]->getClips()) { retained[row].push_back(clip); } }
		store()->setCurrentPattern(4); meter(numerator, denominator); assertColumns(); QCOMPARE(store()->currentPattern(), 4);
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 6; ++column) { QCOMPARE(m_rows[row]->getClip(column), retained[row][column].data()); }
		}
		QCOMPARE(static_cast<MidiClip*>(m_rows[0]->getClip(5))->notes().front()->key(), 53);
		QCOMPARE(store()->automatedValuesAt(24, 5).value(target()), 24.f);
	}

	void deletingPatternsKeepsContentAndCanonicalColumns_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator"); QTest::addColumn<int>("removed"); QTest::addColumn<int>("selected");
		QTest::newRow("default-middle") << 4 << 4 << 4 << 8; QTest::newRow("short-bar-late-column") << 3 << 4 << 5 << 8;
		QTest::newRow("wide-bar-prior-columns") << 7 << 4 << 3 << 1; QTest::newRow("seven-eight-late-column") << 7 << 8 << 8 << 9;
		QTest::newRow("selected-first") << 3 << 4 << 0 << 0; QTest::newRow("selected-last") << 7 << 4 << 9 << 9;
	}
	void deletingPatternsKeepsContentAndCanonicalColumns()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); QFETCH(int, removed); QFETCH(int, selected); meter(numerator, denominator); fixture(10);
		std::array<std::vector<QPointer<Clip>>, 3> retained;
		for (int row = 0; row < 3; ++row) { for (auto clip : m_rows[row]->getClips()) { retained[row].push_back(clip); } }
		store()->setCurrentPattern(selected); delete m_patterns[removed]; assertColumns(); QCOMPARE(store()->numOfPatterns(), 9);
		QCOMPARE(store()->currentPattern(), removed <= selected ? std::max(0, selected - 1) : selected);
		for (int row = 0; row < 3; ++row)
		{
			QVERIFY(!retained[row][removed]);
			for (int i = 0; i < 9; ++i) { QCOMPARE(m_rows[row]->getClip(i), retained[row][i < removed ? i : i + 1].data()); }
		}
		for (int i = 0; i < 9; ++i)
		{
			QCOMPARE(static_cast<MidiClip*>(m_rows[0]->getClip(i))->notes().front()->key(), 48 + (i < removed ? i : i + 1));
			QCOMPARE(PatternTrack::findPatternTrack(i)->name(), QString("Pattern %1").arg(i < removed ? i : i + 1));
		}
	}

	void removingSparsePatternDoesNotCreateReplacementClips()
	{
		fixture(); delete m_rows[2]->getClip(2); QSignalSpy added(m_rows[2], &Track::clipAdded);
		delete m_patterns[2]; QCOMPARE(added.count(), 0); assertColumns(); QCOMPARE(store()->numOfPatterns(), 2);
	}

	void changeNotificationsUseColumnOwnershipInsteadOfPosition()
	{
		meter(7, 8); fixture(); QSignalSpy first(m_patterns[0], &Model::dataChanged), second(m_patterns[1], &Model::dataChanged), third(m_patterns[2], &Model::dataChanged);
		QSignalSpy updated(store(), &PatternStore::trackUpdated);
		auto clip = m_rows[0]->getClip(1); clip->movePosition(TimePos(999));
		first.clear(); second.clear(); third.clear(); updated.clear(); store()->updatePatternTrack(clip);
		QCOMPARE(first.count(), 0); QCOMPARE(second.count(), 1); QCOMPARE(third.count(), 0); QCOMPARE(updated.count(), 1);
		store()->updatePatternTrack(nullptr); QCOMPARE(updated.count(), 1);
		auto foreign = Track::create(Track::Type::Instrument, song())->createClip(TimePos(0));
		store()->updatePatternTrack(foreign); QCOMPARE(updated.count(), 1);
	}

	void nativeProjectSelectionRestoresAfterAllPatternsExist_data()
	{
		QTest::addColumn<QString>("attribute"); QTest::addColumn<int>("expected");
		QTest::newRow("first") << QString("0") << 0; QTest::newRow("middle") << QString("1") << 1;
		QTest::newRow("last") << QString("2") << 2; QTest::newRow("legacy-absent") << QString() << 2;
		QTest::newRow("malformed") << QString("unknown") << 0; QTest::newRow("out-of-range") << QString("12") << 0;
	}
	void nativeProjectSelectionRestoresAfterAllPatternsExist()
	{
		QFETCH(QString, attribute); QFETCH(int, expected); fixture(); store()->setCurrentPattern(1); QTemporaryDir directory;
		const auto path = directory.filePath("patterns.mmp"); QVERIFY(song()->saveProjectFile(path));
		QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QDomDocument doc; QVERIFY(doc.setContent(file.readAll())); file.close();
		auto head = doc.documentElement().firstChildElement("head"); QCOMPARE(head.attribute("currentpattern"), QString("1"));
		if (attribute.isNull()) { head.removeAttribute("currentpattern"); } else { head.setAttribute("currentpattern", attribute); }
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); const auto bytes = doc.toByteArray(); QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size())); file.close();
		QVERIFY(song()->loadProject(path)); QCOMPARE(store()->currentPattern(), expected); assertColumns();
		QCOMPARE(store()->automatedValuesAt(24, 2).value(target()), 24.f);
	}

	void selectionAndPatternContentSurviveHistoryAndRecovery()
	{
		QTemporaryDir session; QJsonArray finalContent;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); meter(7, 8); fixture(4); store()->setCurrentPattern(2);
			QVERIFY(runtime.process(request("fixture", "song.set", {{"tempo", 101}})).value("ok").toBool()); const auto before = content();
			const auto move = request("move", "track.edit", {{"track", 0}, {"to", 3}});
			QVERIFY(runtime.process(move).value("ok").toBool()); QCOMPARE(store()->currentPattern(), 1); const auto moved = content();
			QVERIFY(runtime.process(request("remove", "track.remove", {{"track", 3}})).value("ok").toBool()); QCOMPARE(store()->currentPattern(), 1); finalContent = content(); assertColumns();
			QVERIFY(runtime.process(request("undo-remove", "session.undo")).value("ok").toBool()); QCOMPARE(content(), moved); QCOMPARE(store()->currentPattern(), 1);
			QVERIFY(runtime.process(request("undo-move", "session.undo")).value("ok").toBool()); QCOMPARE(content(), before); QCOMPARE(store()->currentPattern(), 2);
			QVERIFY(runtime.process(move).value("replayed").toBool()); QCOMPARE(store()->currentPattern(), 2);
			QVERIFY(runtime.process(request("redo-move", "session.redo")).value("ok").toBool()); QCOMPARE(store()->currentPattern(), 1);
			QVERIFY(runtime.process(request("redo-remove", "session.redo")).value("ok").toBool()); QCOMPARE(content(), finalContent); assertColumns();
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(content(), finalContent); QCOMPARE(store()->currentPattern(), 1); assertColumns();
		QCOMPARE(store()->automatedValuesAt(24, 2).value(target()), 24.f);
	}
};
QTEST_GUILESS_MAIN(StudioPatternTest)
#include "StudioPatternTest.moc"
