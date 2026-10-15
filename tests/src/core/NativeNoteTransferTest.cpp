/* Native note transfer, closed inline graphs and clip history.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <limits>
#include <thread>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "NativeNoteTransfer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
using namespace lmms;
namespace
{
MidiClip* empty() { return static_cast<MidiClip*>((new InstrumentTrack(Engine::getSong()))->createClip(0)); }
MidiClip* source(bool linked = true)
{
	auto clip = empty(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, 24 + index * 48, 60 + index, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
	if (linked) { clip->notes().at(0)->detuning()->linkToModel(clip->notes().at(1)->detuning().get()); clip->notes().at(0)->detuning()->automationClip()->mutedModel()->linkToModel(clip->notes().at(1)->detuning()->automationClip()->soloModel()); } return clip;
}
bool graph(const NoteVector& notes) { return notes.size() == 2 && notes[0]->detuning()->isLinkedToModel(notes[1]->detuning().get()) && notes[0]->detuning()->automationClip()->mutedModel()->isLinkedToModel(notes[1]->detuning()->automationClip()->soloModel()); }
}
class NativeNoteTransferTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal(); Engine::getSong()->clearProject(); }

	void directCopyRetainsNativePointsAndClosedGraphs()
	{
		auto original = source(), target = empty(); NoteVector inserted; QString error; QVERIFY2(NativeNoteTransfer::copy(original, original->notes(), 24, target, 192, inserted, error), qPrintable(error)); QVERIFY(graph(inserted)); QCOMPARE(inserted[0]->pos().getTicks(), 192); QCOMPARE(inserted[1]->pos().getTicks(), 240); QCOMPARE(inserted[0]->key(), 60); QCOMPARE(inserted[0]->volume(), volume_t{85}); QCOMPARE(inserted[0]->panning(), panning_t{-20}); QCOMPARE(target->notes().front(), inserted.front()); QVERIFY(inserted.front()->detuning() != original->notes().front()->detuning()); QVERIFY(!inserted.front()->detuning()->isLinkedToModel(original->notes().front()->detuning().get())); QCOMPARE(inserted.front()->detuning()->automationClip()->valueAt(24), original->notes().front()->detuning()->automationClip()->valueAt(24)); QCOMPARE(inserted.front()->detuning()->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(inserted.front()->detuning().get()));
	}
	void clipboardAndLegacyNotesRetainBarRelativePlacement_data()
	{ QTest::addColumn<int>("variant"); QTest::newRow("modern-graph") << 0; QTest::newRow("legacy-note-list") << 1; QTest::newRow("legacy-key-fields") << 2; }
	void clipboardAndLegacyNotesRetainBarRelativePlacement()
	{
		QFETCH(int, variant); auto original = source(variant == 0), target = empty(); DataFile document(DataFile::Type::ClipboardData); QString error; QVERIFY(NativeNoteTransfer::save(original, original->notes(), 0, document, error));
		if (variant) { document.content().firstChildElement("note-list").removeAttribute("version"); document.content().removeChild(document.content().firstChildElement("nativeLinkedControls")); for (auto note = document.content().firstChildElement("note-list").firstChildElement(); !note.isNull(); note = note.nextSiblingElement()) { note.removeChild(note.firstChildElement("inlinecontrol")); if (variant == 2) { const auto key = note.attribute("key").toInt(); note.removeAttribute("key"); note.setAttribute("tone", key % KeysPerOctave); note.setAttribute("oct", key / KeysPerOctave); } } }
		NoteVector inserted; QVERIFY2(NativeNoteTransfer::load(document, target, 192, inserted, error), qPrintable(error)); QCOMPARE(inserted.size(), std::size_t{2}); QCOMPARE(inserted[0]->pos().getTicks(), 216); QCOMPARE(inserted[1]->pos().getTicks(), 264); QCOMPARE(inserted[0]->key(), 60); if (!variant) { QVERIFY(graph(inserted)); } QCOMPARE(inserted.front()->detuning()->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(inserted.front()->detuning().get()));
	}
	void invalidSelectionsRefuseWithoutChangingSource_data()
	{ QTest::addColumn<int>("invalid"); QTest::newRow("empty") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-origin") << 3; QTest::newRow("late-origin") << 4; QTest::newRow("nonempty-document") << 5; QTest::newRow("open-linked-group") << 6; }
	void invalidSelectionsRefuseWithoutChangingSource()
	{
		QFETCH(int, invalid); auto original = source(), other = source(false); NoteVector selected = original->notes(); int origin = 0; DataFile document(DataFile::Type::ClipboardData);
		if (invalid == 0) { selected.clear(); } else if (invalid == 1) { selected.push_back(selected.front()); } else if (invalid == 2) { selected.push_back(other->notes().front()); } else if (invalid == 3) { origin = -1; } else if (invalid == 4) { origin = 25; } else if (invalid == 5) { document.content().appendChild(document.createElement("old")); } else { selected.pop_back(); }
		QString error; QVERIFY(!NativeNoteTransfer::save(original, selected, origin, document, error)); QVERIFY(!error.isEmpty()); QCOMPARE(original->notes().size(), std::size_t{2}); QVERIFY(graph(original->notes()));
	}
	void malformedDocumentsPreserveDestinationAndSelection_data()
	{ QTest::addColumn<int>("invalid"); QTest::newRow("version") << 0; QTest::newRow("key") << 1; QTest::newRow("volume") << 2; QTest::newRow("negative-position") << 3; QTest::newRow("endpoint") << 4; QTest::newRow("missing-graph") << 5; QTest::newRow("unexpected-child") << 6; QTest::newRow("duplicate-list") << 7; QTest::newRow("note-type") << 8; QTest::newRow("wrong-document-type") << 9; QTest::newRow("legacy-key-overflow") << 10; QTest::newRow("unowned-controller-factory") << 11; }
	void malformedDocumentsPreserveDestinationAndSelection()
	{
		QFETCH(int, invalid); auto original = source(), target = empty(); auto retained = target->addNote(Note(48, 0, 64), false); retained->setSelected(true); DataFile document(DataFile::Type::ClipboardData); QString error; QVERIFY(NativeNoteTransfer::save(original, original->notes(), 0, document, error)); auto list = document.content().firstChildElement("note-list"), first = list.firstChildElement();
		if (invalid == 0) { list.setAttribute("version", 99); } else if (invalid == 1) { first.setAttribute("key", NumKeys); } else if (invalid == 2) { first.setAttribute("vol", "nan"); } else if (invalid == 3) { first.setAttribute("pos", -1); } else if (invalid == 4) { first.setAttribute("pos", MaxSongLength); } else if (invalid == 5) { document.content().removeChild(document.content().firstChildElement("nativeLinkedControls")); } else if (invalid == 6) { list.appendChild(document.createElement("other")); } else if (invalid == 7) { document.content().appendChild(list.cloneNode()); } else if (invalid == 8) { first.setAttribute("type", 9); } else if (invalid == 10) { first.setAttribute("oct", std::numeric_limits<int>::max()); }
		if (invalid == 11) { auto connection = document.createElement("connection"), binding = document.createElement("value"), controller = document.createElement("lfocontroller"); controller.setAttribute("type", 1); binding.appendChild(controller); connection.appendChild(binding); first.firstChildElement("inlinecontrol").appendChild(connection); }
		NoteVector inserted; if (invalid == 9) { DataFile wrong(DataFile::Type::SongProject); QVERIFY(!NativeNoteTransfer::load(wrong, target, 192, inserted, error)); } else { QVERIFY(!NativeNoteTransfer::load(document, target, 192, inserted, error)); } QVERIFY(inserted.empty()); QCOMPARE(target->notes().size(), std::size_t{1}); QCOMPARE(target->notes().front(), retained); QVERIFY(retained->selected());
	}
	void nativeClipUndoRedoRestoresInlineGroupsOnTheSameClip()
	{
		auto original = source(), target = source(); const QPointer<MidiClip> same(target); auto journal = Engine::projectJournal(); journal->setJournalling(true); NoteVector inserted; QString error; QVERIFY2(NativeNoteTransfer::copy(original, original->notes(), 24, target, 192, inserted, error), qPrintable(error)); QCOMPARE(target->notes().size(), std::size_t{4}); journal->undo(); QCOMPARE(same.data(), target); QCOMPARE(target->notes().size(), std::size_t{2}); QVERIFY(graph(target->notes())); journal->redo(); QCOMPARE(same.data(), target); QCOMPARE(target->notes().size(), std::size_t{4}); NoteVector copied{target->notes().at(2), target->notes().at(3)}; QVERIFY(graph(copied)); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void enabledHistoryRefusesIncompleteDestinationCheckpoint()
	{
		auto original = source(), target = empty(); QVERIFY(target->mutedModel()->linkToModel(target->instrumentTrack()->volumeModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); NoteVector inserted; QString error; QVERIFY(!NativeNoteTransfer::copy(original, original->notes(), 24, target, 192, inserted, error)); QVERIFY(inserted.empty()); QVERIFY(target->notes().empty()); QVERIFY(target->mutedModel()->isLinkedToModel(target->instrumentTrack()->volumeModel())); QVERIFY(!journal->canUndo());
	}
	void postInsertionFailureRetainsAnUndoableNativeEdit()
	{
		auto original = source(), target = empty(); auto journal = Engine::projectJournal(); journal->setJournalling(true); const auto connection = QObject::connect(target, &MidiClip::dataChanged, this, [] { throw std::runtime_error("fixture insertion notification"); }, Qt::DirectConnection); NoteVector inserted; QString error; QVERIFY(!NativeNoteTransfer::copy(original, original->notes(), 24, target, 192, inserted, error)); QVERIFY(inserted.empty()); QCOMPARE(target->notes().size(), std::size_t{2}); QVERIFY(graph(target->notes())); QVERIFY(journal->canUndo()); QObject::disconnect(connection); journal->undo(); QVERIFY(target->notes().empty()); QVERIFY(journal->isJournalling());
	}
	void cancellationOrDestinationRetirementPublishesNoResults_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("cancel") << false; QTest::newRow("retire-destination") << true; }
	void cancellationOrDestinationRetirementPublishesNoResults()
	{
		QFETCH(bool, retire); auto original = source(); auto target = empty(); const QPointer<MidiClip> weak(target); DataFile document(DataFile::Type::ClipboardData); QString error; QVERIFY(NativeNoteTransfer::save(original, original->notes(), 0, document, error)); NoteVector inserted; bool timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); bool done = false; FileResourceIO::Scope cancel([&] { if (done) { return !retire; } done = true; if (retire) { delete target; return false; } return true; }, true); QVERIFY(!NativeNoteTransfer::load(document, target, 192, inserted, error)); QVERIFY(inserted.empty()); QVERIFY(!timer); if (retire) { QVERIFY(!weak); } else { QVERIFY(target->notes().empty()); }
	}
	void sourceRetirementIsDetectedBeforeSerialization()
	{
		auto original = source(); NoteVector selected = original->notes(); DataFile document(DataFile::Type::ClipboardData); bool done = false; FileResourceIO::Scope work([&] { if (!done) { done = true; original->removeNote(selected.front()); } return false; }); QString error; QVERIFY(!NativeNoteTransfer::save(original, original->notes(), 0, document, error)); QVERIFY(done); QCOMPARE(original->notes().size(), std::size_t{1});
	}
	void foreignThreadCannotSerializeNativeNotes()
	{
		auto original = source(); bool accepted = true; QString error; std::thread worker([&] { DataFile document(DataFile::Type::ClipboardData); accepted = NativeNoteTransfer::save(original, original->notes(), 0, document, error); }); worker.join(); QVERIFY(!accepted); QVERIFY(!error.isEmpty()); QVERIFY(graph(original->notes()));
	}
	void largeSelectionRetainsOneDestinationGroup()
	{
		auto original = empty(), target = empty(); for (int index = 0; index < 1050; ++index) { auto note = original->addNote(Note(48, index * 48, 60), false); note->createDetuning(); if (index) { QVERIFY(original->notes().front()->detuning()->linkToModel(note->detuning().get())); } } NoteVector inserted; QString error; QVERIFY2(NativeNoteTransfer::copy(original, original->notes(), 0, target, 192, inserted, error), qPrintable(error)); QCOMPARE(inserted.size(), std::size_t{1050}); QCOMPARE(inserted.front()->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(inserted.back()->pos().getTicks(), 192 + 1049 * 48);
	}
	void invalidClipJournalHeadersRefuseBeforeReplacingNotes_data()
	{ QTest::addColumn<int>("invalid"); QTest::newRow("version") << 0; QTest::newRow("receiver") << 1; QTest::newRow("type") << 2; QTest::newRow("missing-body") << 3; }
	void invalidClipJournalHeadersRefuseBeforeReplacingNotes()
	{
		QFETCH(int, invalid); auto original = source(); DataFile document(DataFile::Type::JournalData); auto state = original->saveJournalState(document, document.content()); QVERIFY(!state.isNull()); if (invalid == 0) { state.setAttribute("version", 99); } else if (invalid == 1) { state.setAttribute("receiver", 0); } else if (invalid == 2) { state.setAttribute("type", "sampleclip"); } else { state.removeChild(state.firstChildElement(original->nodeName())); } const auto retained = original->notes().front(); QString error; QVERIFY(!original->restoreJournalState(state, error)); QCOMPARE(original->notes().front(), retained); QVERIFY(graph(original->notes()));
	}
};
QTEST_GUILESS_MAIN(NativeNoteTransferTest)
#include "NativeNoteTransferTest.moc"
