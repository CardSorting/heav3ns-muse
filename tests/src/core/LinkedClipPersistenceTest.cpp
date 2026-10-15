/* Native clip control serialization, copied rings and retained content.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "NativeLinkedControls.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
Track::Type kind(int index)
{ return index == 0 ? Track::Type::Instrument : index == 1 ? Track::Type::Sample : index == 2 ? Track::Type::Automation : Track::Type::Pattern; }
void kinds()
{
	QTest::addColumn<int>("type"); QTest::newRow("midi") << 0; QTest::newRow("sample") << 1;
	QTest::newRow("automation") << 2; QTest::newRow("pattern-reference") << 3;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
bool unequalGroup(Clip* clip)
{
	if (!clip->mutedModel()->linkToModel(clip->soloModel())) { return false; }
	QDomDocument document; auto element = document.createElement("controls"); document.appendChild(element); QString error;
	{
		NativeLinkedControls::SaveScope scope(document, NativeLinkedControls::clipModels(clip));
		clip->mutedModel()->saveSettings(document, element, "muted"); clip->soloModel()->saveSettings(document, element, "solo");
		if (!scope.finish(element, error)) { return false; }
	}
	element.firstChildElement("muted").setAttribute("value", 1); element.firstChildElement("solo").setAttribute("value", 0);
	JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope scope(document);
	clip->mutedModel()->loadSettings(element, "muted"); clip->soloModel()->loadSettings(element, "solo"); return scope.finish(element, error);
}
}

class LinkedClipPersistenceTest : public QObject
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void trackedNativeCopiesPreserveClosedRingsAndContent_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<bool>("legacyClone");
		QTest::newRow("midi-copy") << 0 << false; QTest::newRow("midi-clone") << 0 << true;
		QTest::newRow("sample-copy") << 1 << false; QTest::newRow("sample-clone") << 1 << true;
		QTest::newRow("automation-copy") << 2 << false; QTest::newRow("automation-clone") << 2 << true;
		QTest::newRow("pattern-copy") << 3 << false; QTest::newRow("pattern-clone") << 3 << true;
	}
	void trackedNativeCopiesPreserveClosedRingsAndContent()
	{
		QFETCH(int, type); QFETCH(bool, legacyClone); auto track = Track::create(kind(type), Engine::getSong()); auto source = track->createClip(0); QVERIFY(unequalGroup(source));
		if (auto midi = dynamic_cast<MidiClip*>(source)) { midi->clearNotes(); midi->addNote(Note(48, 0, 63, 88, -19), false); }
		if (auto sample = dynamic_cast<SampleClip*>(source)) { sample->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(1024), 24000)); sample->sample().setAmplification(.375f); }
		if (auto curve = dynamic_cast<AutomationClip*>(source)) { curve->putValues(0, 12, 17, false, true); curve->putValue(96, 70, false); }
		source->setName("Independent linked clip"); source->setAutoResize(false); source->changeLength(192); source->setStartTimeOffset(-13);
		const auto id = source->mutedModel()->id(); auto destination = legacyClone ? track : Track::create(kind(type), Engine::getSong());
		auto copy = legacyClone ? source->clone() : source->copyTo(destination); QVERIFY2(copy, qPrintable(Engine::getSong()->errorSummary()));
		QVERIFY(copy->mutedModel()->isLinkedToModel(copy->soloModel())); QVERIFY(!copy->mutedModel()->isLinkedToModel(source->mutedModel()));
		QCOMPARE(copy->mutedModel()->storedValue(), 1.f); QCOMPARE(copy->soloModel()->storedValue(), 0.f); QVERIFY(copy->mutedModel()->id() != id); QCOMPARE(source->mutedModel()->id(), id);
		QCOMPARE(copy->name(), source->name()); QCOMPARE(copy->length().getTicks(), 192); QCOMPARE(copy->startTimeOffset().getTicks(), -13);
		if (auto midi = dynamic_cast<MidiClip*>(copy)) { QCOMPARE(midi->notes().size(), std::size_t{1}); QCOMPARE(midi->notes().front()->key(), 63); }
		if (auto sample = dynamic_cast<SampleClip*>(copy)) { QCOMPARE(sample->sample().buffer().get(), static_cast<SampleClip*>(source)->sample().buffer().get()); QCOMPARE(sample->sample().amplification(), .375f); }
		if (auto curve = dynamic_cast<AutomationClip*>(copy)) { QCOMPARE(curve->getTimeMap().value(0).getOutValue(), 17.f); curve->putValue(96, 90, false); QCOMPARE(static_cast<AutomationClip*>(source)->valueAt(96), 70.f); }
		copy->setMuted(false); QCOMPARE(copy->soloModel()->storedValue(), 0.f); QCOMPARE(source->mutedModel()->storedValue(), 1.f);
	}

	void nativeProjectSerializersRestoreEachClipKind_data() { kinds(); }
	void nativeProjectSerializersRestoreEachClipKind()
	{
		QFETCH(int, type); QTemporaryDir directory; auto song = Engine::getSong(); auto track = Track::create(kind(type), song); auto source = track->createClip(0); QVERIFY(unequalGroup(source));
		if (auto midi = dynamic_cast<MidiClip*>(source)) { midi->clearNotes(); midi->addNote(Note(48, 0, 63), false); }
		const auto path = directory.filePath("clip-links.mmp"); QVERIFY2(song->saveProjectFile(path), qPrintable(song->errorSummary()));
		DataFile document(path); const auto saved = document.elementsByTagName(source->nodeName()).at(0).toElement(); const auto muteKey = type == 2 ? QString("mute") : QString("muted");
		QCOMPARE(saved.attribute(muteKey).toInt(), 1); QCOMPARE(saved.attribute("solo").toInt(), 0);
		QCOMPARE(saved.firstChildElement(muteKey).attribute("metadata").toInt(), 1); QCOMPARE(saved.firstChildElement("solo").attribute("metadata").toInt(), 1);
		QVERIFY(song->loadProject(path)); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY2(!song->hasErrors(), qPrintable(song->errorSummary()));
		auto restored = song->tracks().at(0)->getClip(0); QVERIFY(restored->mutedModel()->isLinkedToModel(restored->soloModel()));
		QCOMPARE(restored->mutedModel()->storedValue(), 1.f); QCOMPARE(restored->soloModel()->storedValue(), 0.f);
		if (auto midi = dynamic_cast<MidiClip*>(restored)) { QCOMPARE(midi->notes().size(), std::size_t{1}); QCOMPARE(midi->notes().front()->key(), 63); }
	}

	void wholeProjectRestoresCrossClipAndTrackRings()
	{
		QTemporaryDir directory; auto song = Engine::getSong(); auto track = new InstrumentTrack(song); auto first = track->createClip(0), second = track->createClip(192);
		QVERIFY(first->mutedModel()->linkToModel(second->soloModel())); QVERIFY(first->mutedModel()->linkToModel(track->volumeModel()));
		const auto path = directory.filePath("cross.mmp"); QVERIFY(song->saveProjectFile(path)); QVERIFY(song->loadProject(path)); Engine::audioEngine()->audioDev()->stopProcessing();
		track = static_cast<InstrumentTrack*>(song->tracks().at(0)); first = track->getClip(0); second = track->getClip(1);
		QVERIFY(first->mutedModel()->isLinkedToModel(second->soloModel())); QVERIFY(first->mutedModel()->isLinkedToModel(track->volumeModel())); QVERIFY(!song->hasErrors());
	}

	void boundaryGroupsRefuseNativeAndStructuredCopy_data()
	{
		QTest::addColumn<int>("entry"); QTest::newRow("copy") << 0; QTest::newRow("clone") << 1; QTest::newRow("split") << 2;
		QTest::newRow("state-copy") << 3; QTest::newRow("structured") << 4;
	}
	void boundaryGroupsRefuseNativeAndStructuredCopy()
	{
		QFETCH(int, entry); auto track = new InstrumentTrack(Engine::getSong()); auto source = track->createClip(0), target = track->createClip(192); source->changeLength(192);
		QVERIFY(source->mutedModel()->linkToModel(track->volumeModel())); const auto count = track->numOfClips();
		if (entry == 0) { QVERIFY_EXCEPTION_THROWN(source->copyTo(track), std::runtime_error); }
		if (entry == 1) { QVERIFY(!source->clone()); }
		if (entry == 2) { QVERIFY(!source->splitAt(96)); QCOMPARE(source->length().getTicks(), 192); }
		if (entry == 3) { QVERIFY_EXCEPTION_THROWN(Clip::copyStateTo(source, target), std::runtime_error); }
		if (entry == 4) { QVERIFY(!StudioCapabilities::execute("clip.clone", {{"track", 0}, {"clip", 0}, {"position", 384}}).value("ok").toBool()); }
		QCOMPARE(track->numOfClips(), count); QCOMPARE(track->getClip(1), target); QVERIFY(source->mutedModel()->isLinkedToModel(track->volumeModel()));
	}

	void omittedOwnedMembersRefuseTrackAndClipCopiesBeforeConstruction()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = track->createClip(0); FloatModel omitted(1, 0, 1, 1, source); QVERIFY(source->mutedModel()->linkToModel(&omitted));
		const auto count = Engine::getSong()->tracks().size(); QVERIFY_EXCEPTION_THROWN(source->copyTo(track), std::runtime_error); QCOMPARE(track->numOfClips(), 1);
		QVERIFY(!track->clone()); QCOMPARE(Engine::getSong()->tracks().size(), count); QVERIFY(source->mutedModel()->isLinkedToModel(&omitted));
	}

	void patternReplacementCopiesBeforeRetiringTheOldCell()
	{
		Track::create(Track::Type::Pattern, Engine::getSong()); Track::create(Track::Type::Pattern, Engine::getSong()); auto row = Track::create(Track::Type::Instrument, Engine::patternStore());
		auto source = row->getClip(0); auto old = QPointer<Clip>(row->getClip(1)); QVERIFY(unequalGroup(source));
		QVERIFY(old->mutedModel()->linkToModel(row->getMutedModel()));
		auto copy = source->copyTo(row, old.data()); QVERIFY(!old); QCOMPARE(row->numOfClips(), 2); QCOMPARE(row->getClip(0), source); QCOMPARE(row->getClip(1), copy);
		QVERIFY(copy->mutedModel()->isLinkedToModel(copy->soloModel())); QVERIFY(!copy->mutedModel()->isLinkedToModel(row->getMutedModel())); QVERIFY(!row->getMutedModel()->isLinked());
	}

	void trackAndColumnClonesPreserveClosedClipGroups_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("song-track") << 0; QTest::newRow("pattern-row") << 1; QTest::newRow("pattern-column") << 2; }
	void trackAndColumnClonesPreserveClosedClipGroups()
	{
		QFETCH(int, mode); auto song = Engine::getSong(); Track* owner; Track* row;
		if (!mode) { owner = row = Track::create(Track::Type::Instrument, song); row->createClip(0); }
		else { owner = Track::create(Track::Type::Pattern, song); row = Track::create(Track::Type::Instrument, Engine::patternStore()); if (mode == 1) { owner = row; } }
		auto source = row->getClip(0); QVERIFY(unequalGroup(source)); auto cloned = owner->clone(); QVERIFY2(cloned, qPrintable(song->errorSummary()));
		auto copy = mode == 2 ? row->getClip(1) : cloned->getClip(0); QVERIFY(copy->mutedModel()->isLinkedToModel(copy->soloModel()));
		QVERIFY(!copy->mutedModel()->isLinkedToModel(source->mutedModel())); QCOMPARE(copy->mutedModel()->storedValue(), 1.f); QCOMPARE(copy->soloModel()->storedValue(), 0.f);
	}

	void ownedMidiControllerCopiesIndependentlyAndStateReplacementRetiresIt()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = track->createClip(0); QVERIFY(unequalGroup(source));
		auto midi = new MidiController(nullptr); midi->midiPort()->setInputController(74); midi->midiPort()->setInputChannel(16);
		source->mutedModel()->setControllerConnection(new ControllerConnection(midi)); auto copy = source->copyTo(track);
		const auto copied = dynamic_cast<MidiController*>(copy->mutedModel()->controllerConnection()->getController()); QVERIFY(copied); QVERIFY(copied != midi);
		QCOMPARE(copied->midiPort()->inputController(), 74); QCOMPARE(copied->midiPort()->inputChannel(), 16); QVERIFY(copy->mutedModel()->isLinkedToModel(copy->soloModel()));
		const QPointer<MidiController> old(copied); auto unlinked = track->createClip(384); Clip::copyStateTo(unlinked, copy); QVERIFY(!old); QVERIFY(!copy->mutedModel()->controllerConnection());
	}

	void stateCopiesKeepDestinationIdentityPositionAndDeclaredGroups_data() { kinds(); }
	void stateCopiesKeepDestinationIdentityPositionAndDeclaredGroups()
	{
		QFETCH(int, type); auto track = Track::create(kind(type), Engine::getSong()); auto source = track->createClip(0), target = track->createClip(384); QVERIFY(unequalGroup(source));
		const auto id = target->mutedModel()->id(); Clip::copyStateTo(source, target); QCOMPARE(target->mutedModel()->id(), id); QCOMPARE(target->startPosition().getTicks(), 384);
		QVERIFY(target->mutedModel()->isLinkedToModel(target->soloModel())); QVERIFY(!target->mutedModel()->isLinkedToModel(source->mutedModel()));
		source->mutedModel()->unlink(); Clip::copyStateTo(source, target); QVERIFY(!target->mutedModel()->isLinked()); QVERIFY(!target->soloModel()->isLinked()); QCOMPARE(track->numOfClips(), 2);
	}

	void inlineNoteDetuningKeepsItsNativeConstructorCopyPath()
	{
		Note source(48, 0, 63); source.createDetuning(); source.detuning()->automationClip()->putValue(0, .5f, false); source.detuning()->automationClip()->putValue(24, -.75f, false);
		std::unique_ptr<Note> copy(source.clone()); QVERIFY(copy->detuning()); QVERIFY(copy->detuning() != source.detuning());
		QCOMPARE(copy->detuning()->automationClip()->valueAt(24), -.75f); copy->detuning()->automationClip()->putValue(24, 1.f, false); QCOMPARE(source.detuning()->automationClip()->valueAt(24), -.75f);
	}

	void runtimeHistoryPreservesClipGroupsAndRollsBackRefusedBoundaryCopies()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); auto track = new InstrumentTrack(Engine::getSong()); QVERIFY(unequalGroup(track->createClip(0)));
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("copy", "clip.clone", {{"track", 0}, {"clip", 0}, {"position", 384}})).value("ok").toBool());
		QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().at(0)->numOfClips(), 1);
		QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto source = track->getClip(0), copy = track->getClip(1); QVERIFY(copy->mutedModel()->isLinkedToModel(copy->soloModel())); QVERIFY(!copy->mutedModel()->isLinkedToModel(source->mutedModel()));
		QVERIFY(source->mutedModel()->linkToModel(copy->mutedModel()));
		QVERIFY(runtime.process(request("linked-baseline", "song.set", {{"tempo", 124}})).value("ok").toBool());
		QVERIFY(!runtime.process(request("bad-copy", "clip.clone", {{"track", 0}, {"clip", 0}, {"position", 768}})).value("ok").toBool());
		track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QCOMPARE(track->numOfClips(), 2); QVERIFY(track->getClip(0)->mutedModel()->isLinkedToModel(track->getClip(1)->mutedModel()));
		QVERIFY(!runtime.stopping()); QVERIFY(!Engine::getSong()->hasErrors());
	}
};
QTEST_GUILESS_MAIN(LinkedClipPersistenceTest)
#include "LinkedClipPersistenceTest.moc"
