/* Native pattern queries and loop dispatch; no PCM claim.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <limits>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
// Observe native pattern dispatch coordinates without loading an instrument or
// interpreting a returned dispatch flag as generated PCM.
class ObservedInstrument : public InstrumentTrack
{
public:
	explicit ObservedInstrument(TrackContainer* owner) : InstrumentTrack(owner) {}
	int calls = 0;
	int tick = -1;
	bool play(const TimePos& start, f_cnt_t, f_cnt_t, int) override
	{ ++calls; tick = start.getTicks(); return true; }
};
struct Fixture { PatternTrack* pattern; InstrumentTrack* target; ObservedInstrument* midi; Track* automation; };
Fixture seed(int columns = 2)
{
	Fixture result{};
	for (int index = 0; index < columns; ++index)
	{
		auto pattern = new PatternTrack(Engine::getSong()); if (!index) { result.pattern = pattern; }
	}
	result.target = new InstrumentTrack(Engine::getSong()); result.midi = new ObservedInstrument(Engine::patternStore());
	result.midi->createClipsForPattern(columns - 1);
	result.automation = Track::create(Track::Type::Automation, Engine::patternStore());
	for (int index = 0; index < columns; ++index)
	{
		auto cell = result.midi->getClips().at(index); cell->setAutoResize(false); cell->changeLength(192);
		auto curve = static_cast<AutomationClip*>(result.automation->getClips().at(index));
		curve->addObject(result.target->volumeModel()); curve->setProgressionType(AutomationClip::ProgressionType::Linear);
		curve->putValue(0, 10 + index, false, true); curve->putValue(96, 106 + index, false, true);
	}
	return result;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
}
class PatternQueryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{ Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void absentColumnsDoNotCreateCells_data()
	{
		QTest::addColumn<int>("column"); QTest::newRow("negative") << -1;
		QTest::newRow("minimum") << std::numeric_limits<int>::min(); QTest::newRow("past-end") << 2;
		QTest::newRow("maximum") << std::numeric_limits<int>::max();
	}
	void absentColumnsDoNotCreateCells()
	{
		QFETCH(int, column); const auto f = seed(); const auto clips = f.midi->getClips();
		QSignalSpy added(f.midi, &Track::clipAdded); auto store = Engine::patternStore();
		QCOMPARE(store->lengthOfPattern(column), 1); QVERIFY(store->automatedValuesAt(0, column).empty());
		QVERIFY(!store->play(0, 256, 0, column)); QCOMPARE(f.midi->calls, 0);
		QCOMPARE(f.midi->getClips(), clips); QCOMPARE(f.automation->numOfClips(), 2); QCOMPARE(added.count(), 0);
	}
	void sparseColumnsRemainSparseAcrossQueriesAndDispatch()
	{
		const auto f = seed(); delete f.midi->getClips().at(1); delete f.automation->getClips().at(1);
		auto store = Engine::patternStore(); QSignalSpy midiAdded(f.midi, &Track::clipAdded), autoAdded(f.automation, &Track::clipAdded);
		QCOMPARE(store->lengthOfPattern(1), 1); QVERIFY(store->automatedValuesAt(24, 1).empty());
		QVERIFY(store->TrackContainer::automatedValuesAt(24, 1).empty());
		QVERIFY(!store->play(24, 256, 0, 1)); QCOMPARE(f.midi->calls, 0);
		QCOMPARE(f.midi->numOfClips(), 1); QCOMPARE(f.automation->numOfClips(), 1);
		QCOMPARE(midiAdded.count(), 0); QCOMPARE(autoAdded.count(), 0);
	}
	void negativeAutomationTimeRefusesWhileNativePrerollIsRetained()
	{
		const auto f = seed(); auto store = Engine::patternStore();
		QVERIFY(store->automatedValuesAt(-1, 0).empty()); QVERIFY(store->play(-1, 256, 0, 0));
		QCOMPARE(f.midi->calls, 1); QCOMPARE(f.midi->tick, -1); QCOMPARE(f.midi->numOfClips(), 2);
	}
	void selectedColumnUsesLocalTimeAndNativeRowPrecedence()
	{
		const auto f = seed(); auto later = Track::create(Track::Type::Automation, Engine::patternStore());
		auto first = f.automation->getClips().at(1); first->movePosition(576);
		auto curve = static_cast<AutomationClip*>(later->getClips().at(1)); curve->movePosition(0);
		QVERIFY(curve->addObject(f.target->volumeModel())); curve->putValue(0, 90, false, true);
		QCOMPARE(Engine::patternStore()->automatedValuesAt(24, 1).value(f.target->volumeModel()), 90.f);
		QVERIFY(Engine::patternStore()->moveTrack(later, 0));
		QCOMPARE(Engine::patternStore()->automatedValuesAt(24, 1).value(f.target->volumeModel()), 35.f);
	}
	void automationReadDoesNotApplyValuesToItsTarget()
	{
		const auto f = seed(); f.target->volumeModel()->setValue(70); QSignalSpy changed(f.target->volumeModel(), &AutomatableModel::dataChanged);
		QCOMPARE(Engine::patternStore()->automatedValuesAt(24, 0).value(f.target->volumeModel()), 34.f);
		QCOMPARE(f.target->volumeModel()->storedValue(), 70.f); QCOMPARE(changed.count(), 0);
	}
	void localTimeClampRetainsNativePatternEndValue()
	{
		const auto f = seed(); QCOMPARE(Engine::patternStore()->automatedValuesAt(300, 0).value(f.target->volumeModel()), 106.f);
	}
	void nativeLengthRoundingDoesNotMutateCellsOrOverflow_data()
	{
		QTest::addColumn<int>("length"); QTest::addColumn<int>("bars");
		QTest::newRow("negative-native-length") << -1 << 1; QTest::newRow("empty") << 0 << 1;
		QTest::newRow("partial") << 1 << 1; QTest::newRow("whole-bar") << 192 << 1;
		QTest::newRow("next-bar") << 193 << 2;
		QTest::newRow("maximum-length") << std::numeric_limits<int>::max() << 11184811;
	}
	void nativeLengthRoundingDoesNotMutateCellsOrOverflow()
	{
		QFETCH(int, length); QFETCH(int, bars); const auto f = seed(); QCOMPARE(TimePos::ticksPerBar(), 192);
		auto clip = f.midi->getClips().at(0); clip->changeLength(length); QSignalSpy changed(clip, &Clip::lengthChanged);
		QCOMPARE(Engine::patternStore()->lengthOfPattern(0), bars); QCOMPARE(clip->length().getTicks(), length);
		QCOMPARE(changed.count(), 0); QCOMPARE(f.midi->numOfClips(), 2);
	}
	void loopDispatchWrapsIntoItsNativeColumn_data()
	{
		QTest::addColumn<int>("time"); QTest::addColumn<int>("expected");
		QTest::newRow("start") << 0 << 0; QTest::newRow("second-loop") << 216 << 24;
		QTest::newRow("maximum-tick") << std::numeric_limits<int>::max() << std::numeric_limits<int>::max() % 192;
	}
	void loopDispatchWrapsIntoItsNativeColumn()
	{
		QFETCH(int, time); QFETCH(int, expected); const auto f = seed();
		QVERIFY(Engine::patternStore()->play(time, 256, 0, 1)); QCOMPARE(f.midi->calls, 1); QCOMPARE(f.midi->tick, expected);
	}
	void songDispatchAndAutomationAgreeForExtremeSignedOffsets_data()
	{
		QTest::addColumn<int>("offset"); QTest::newRow("minimum") << std::numeric_limits<int>::min();
		QTest::newRow("maximum") << std::numeric_limits<int>::max(); QTest::newRow("advance") << -216;
		QTest::newRow("delay") << 216; QTest::newRow("zero") << 0;
	}
	void songDispatchAndAutomationAgreeForExtremeSignedOffsets()
	{
		QFETCH(int, offset); const auto f = seed(); auto clip = f.pattern->createClip(0); clip->setAutoResize(false);
		clip->changeLength(192); clip->setStartTimeOffset(offset);
		const int expected = static_cast<int>(((24LL - offset) % 192 + 192) % 192);
		QVERIFY(f.pattern->play(24, 256, 0)); QCOMPARE(f.midi->tick, expected);
		const auto native = static_cast<AutomationClip*>(f.automation->getClips().at(0));
		QCOMPARE(Engine::getSong()->automatedValuesAt(24).value(f.target->volumeModel()), native->valueAt(expected));
	}
	void songDispatchPreservesNativeSignedPreroll()
	{
		const auto f = seed(); auto clip = f.pattern->createClip(1); clip->setAutoResize(false); clip->changeLength(192);
		const auto frames = static_cast<f_cnt_t>(Engine::framesPerTick() * 2);
		QVERIFY(f.pattern->play(0, frames, 0)); QCOMPARE(f.midi->tick, -1); QCOMPARE(f.midi->calls, 1);
	}
	void invalidMeterAndUnrepresentableLoopRefuseNativeDispatch_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("zero-meter") << 0; QTest::newRow("negative-meter") << 1;
		QTest::newRow("rounded-span-overflow") << 2;
	}
	void invalidMeterAndUnrepresentableLoopRefuseNativeDispatch()
	{
		QFETCH(int, failure); const auto f = seed(); auto clip = f.pattern->createClip(0); clip->setAutoResize(false); clip->changeLength(192);
		const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		if (failure < 2) { TimePos::setTicksPerBar(failure == 0 ? 0 : -1); }
		else { f.midi->getClips().at(0)->changeLength(std::numeric_limits<int>::max()); }
		auto store = Engine::patternStore();
		QCOMPARE(store->lengthOfPattern(0), failure < 2 ? 0 : static_cast<int>((static_cast<qint64>(std::numeric_limits<int>::max()) + meter - 1) / meter));
		QVERIFY(!store->play(24, 256, 0, 0)); QVERIFY(store->automatedValuesAt(24, 0).empty());
		QVERIFY(!f.pattern->play(24, 256, 0)); QVERIFY(Engine::getSong()->automatedValuesAt(24).empty());
		QCOMPARE(f.midi->calls, 0);
	}
	void emptyProjectRetainsMetadataFallbackWithoutPlayback()
	{
		auto store = Engine::patternStore(); QCOMPARE(store->lengthOfPattern(0), 1);
		QVERIFY(!store->play(0, 256, 0, 0)); QVERIFY(store->automatedValuesAt(0, 0).empty());
		QCOMPARE(store->countTracks(), 0);
	}
	void nativeProjectRoundTripRetainsQueryResults()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); const auto f = seed();
		const auto before = Engine::patternStore()->automatedValuesAt(24, 1).value(f.target->volumeModel());
		const auto path = directory.filePath("patterns.mmp");
		QVERIFY(runtime.process(request("save", "project.save", {{"path", path}})).value("ok").toBool());
		QVERIFY(runtime.process(request("load", "project.load", {{"path", path}})).value("ok").toBool());
		auto target = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(2))->volumeModel();
		QCOMPARE(Engine::patternStore()->automatedValuesAt(24, 1).value(target), before);
		QCOMPARE(Engine::patternStore()->tracks().at(0)->numOfClips(), 2);
	}
};
QTEST_GUILESS_MAIN(PatternQueryTest)
#include "PatternQueryTest.moc"
