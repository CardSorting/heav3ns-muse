#include "../core/NativeSongTestAccess.h"
/* Native SoundFont patch dialog ownership and modal invalidation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QApplication>
#include <QtTest>
#include <fluidsynth.h>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Sf2Player.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "PatchesDialog.h"
#include "PluginFactory.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"
#include <QTableView>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
using namespace lmms;
using lmms::gui::PatchesDialog;
namespace lmms
{
class Sf2PatchDialogTestAccess
{
public:
	static std::pair<int, int> nativeSelection(Sf2Instrument* native) { QMutexLocker lock(&native->m_synthMutex); int font = -1, bank = -1, program = -1; if (!native->m_synth || !fluid_synth_get_channel_preset(native->m_synth, native->m_channel) || fluid_synth_get_program(native->m_synth, native->m_channel, &font, &bank, &program) != FLUID_OK || font < 0 || program < 0 || program > 127) { return {-1, -1}; } return {bank, program}; }

};
}
namespace
{
class ObservedSf2 final : public Sf2Instrument
{
public:
	using Sf2Instrument::Sf2Instrument;
	mutable std::function<void()> inspect;
	mutable std::function<void(std::vector<FileResource>&)> changeResult;
	std::vector<FileResource> fileResources() const override { auto result = Sf2Instrument::fileResources(); const auto callback = inspect; const auto change = changeResult; if (callback) { callback(); } if (change) { change(result); } return result; }
};
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new ObservedSf2(this); }
};
QTreeWidget* banks(PatchesDialog* dialog) { return dialog->findChild<QTreeWidget*>("m_bankListView"); }
QTableView* programs(PatchesDialog* dialog) { return dialog->findChild<QTableView*>("m_progListView"); }
QPushButton* ok(PatchesDialog* dialog) { return dialog->findChild<QPushButton*>("m_okButton"); }
bool choose(PatchesDialog* dialog, int bank, int program)
{
	const QPointer<PatchesDialog> owner(dialog); const QPointer<QTreeWidget> bankList(banks(dialog)); const QPointer<QTableView> programList(programs(dialog)); if (!bankList || !programList) { return false; }
	const auto bankItems = bankList->findItems(QString::number(bank), Qt::MatchExactly, 0); if (bankItems.isEmpty()) { return false; } bankList->setCurrentItem(bankItems.front()); if (!owner || !bankList || !programList) { return false; }
	for (int row = 0; row < programList->model()->rowCount(); ++row) { const auto index = programList->model()->index(row, 0); if (index.data().toInt() == program) { programList->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); return owner && bankList && programList; } } return false;
}
int selectedProgram(PatchesDialog* dialog) { return programs(dialog)->currentIndex().sibling(programs(dialog)->currentIndex().row(), 0).data().toInt(); }
bool accept(PatchesDialog* dialog) { return QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection); }
bool reject(PatchesDialog* dialog) { return QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection); }
}
class Sf2PatchDialogTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration, media;
	QString bank() const { return QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"; }
	ObservedSf2* seed(bool loaded = true) { auto track = new NativeTrack; auto native = dynamic_cast<ObservedSf2*>(track->loadInstrument("sf2player")); if (!native) { throw std::runtime_error("Native SoundFont was not selected"); } if (loaded) { QString error; if (!native->setFileResource("soundfont", bank(), error)) { throw std::runtime_error(error.toStdString()); } } return native; }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); QVERIFY(media.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setSF2File(""); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(QFile::exists(bank())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { for (auto track : Engine::getSong()->tracks()) { if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { if (auto native = dynamic_cast<ObservedSf2*>(instrument->instrument())) { native->inspect = {}; native->changeResult = {}; } } } Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void setupUsesTheCurrentNativeSelection_data()
	{ QTest::addColumn<bool>("alternate"); QTest::newRow("first-bank") << false; QTest::newRow("alternate-bank") << true; }
	void setupUsesTheCurrentNativeSelection()
	{
		QFETCH(bool, alternate); auto native = seed(); QString error; if (alternate) { QVERIFY(native->selectProgram(1024, 7, error)); } PatchesDialog dialog; QVERIFY(dialog.setup(native, "Native programs")); QCOMPARE(banks(&dialog)->topLevelItemCount(), 2); QCOMPARE(banks(&dialog)->currentItem()->text(0).toInt(), alternate ? 1024 : 0); QCOMPARE(selectedProgram(&dialog), alternate ? 7 : 0); QVERIFY(ok(&dialog)->isEnabled());
	}
	void cancellationRestoresNativeProgramWithoutChangingModels_data()
	{ QTest::addColumn<int>("route"); QTest::newRow("reject-slot") << 0; QTest::newRow("cancel-button") << 1; QTest::newRow("window-close") << 2; }
	void cancellationRestoresNativeProgramWithoutChangingModels()
	{
		QFETCH(int, route); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Cancel")); QVERIFY(choose(&dialog, 1024, 7)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); if (route == 0) { QVERIFY(reject(&dialog)); } if (route == 1) { dialog.findChild<QPushButton*>("m_cancelButton")->click(); } if (route == 2) { dialog.show(); dialog.close(); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void acceptCommitsThroughNativeProgramSelection_data()
	{ QTest::addColumn<bool>("button"); QTest::newRow("accept-slot") << false; QTest::newRow("ok-button") << true; }
	void acceptCommitsThroughNativeProgramSelection()
	{
		QFETCH(bool, button); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Commit")); QVERIFY(choose(&dialog, 1024, 7)); QSignalSpy changed(native, &Sf2Instrument::patchChanged); if (button) { ok(&dialog)->click(); } else { QVERIFY(accept(&dialog)); } QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); QCOMPARE(changed.count(), 1);
	}
	void emptyAndPendingBanksCannotOpenAnEditableSelection_data()
	{ QTest::addColumn<bool>("pending"); QTest::newRow("empty") << false; QTest::newRow("pending") << true; }
	void emptyAndPendingBanksCannotOpenAnEditableSelection()
	{
		QFETCH(bool, pending); auto native = seed(false); if (pending) { QDomDocument document; auto node = document.createElement("sf2player"); document.appendChild(node); node.setAttribute("src", media.filePath("missing.sf2")); node.setAttribute("unresolved", 1); node.setAttribute("bank", 1024); node.setAttribute("patch", 7); native->loadSettings(node); } PatchesDialog dialog; QVERIFY(!dialog.setup(native, "Unavailable")); QVERIFY(accept(&dialog)); QVERIFY(native->programSnapshot().empty()); QCOMPARE(native->fileResources()[0].unresolved, pending);
	}
	void modalLoopRejectsRetiredOrReplacedNativeContext_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("owner") << 0; QTest::newRow("track") << 1; QTest::newRow("project") << 2; QTest::newRow("clear-bank") << 3; QTest::newRow("replace-bank") << 4; QTest::newRow("project-filename") << 5; }
	void modalLoopRejectsRetiredOrReplacedNativeContext()
	{
		QFETCH(int, change); const QPointer<ObservedSf2> native(seed()); const QPointer<InstrumentTrack> track(native->instrumentTrack()); PatchesDialog dialog; QVERIFY(dialog.setup(native.data(), "Modal")); QVERIFY(choose(&dialog, 1024, 7)); bool called = false; QTimer::singleShot(0, &dialog, [&] { called = true; QString error; if (change == 0) { delete native.data(); } if (change == 1) { delete track.data(); } if (change == 2) { Engine::getSong()->clearProject(); } if (change == 3) { QVERIFY(native->setFileResource("soundfont", {}, error)); } if (change == 4) { QVERIFY(native->setFileResource("soundfont", bank(), error)); } if (change == 5) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), media.filePath("changed.mmp")); } }); const auto result = dialog.exec(); QVERIFY(called); QCOMPARE(result, int(QDialog::Rejected)); QVERIFY(accept(&dialog)); if (native) { QVERIFY(native->getBank() != 1024 || native->getPatch() != 7); }
	}
	void externalControlEditsInvalidateTheCandidate_data()
	{ QTest::addColumn<bool>("patch"); QTest::newRow("bank") << false; QTest::newRow("program") << true; }
	void externalControlEditsInvalidateTheCandidate()
	{
		QFETCH(bool, patch); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "External edit")); QVERIFY(choose(&dialog, 1024, 7)); native->childModel(patch ? "patch" : "bank")->setValue(99); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(patch ? native->getPatch() : native->getBank(), 99);
	}
	void returnedResourceGettersMayRetireNativeActors_data()
	{ QTest::addColumn<bool>("setup"); QTest::addColumn<bool>("track"); QTest::newRow("setup-owner") << true << false; QTest::newRow("setup-track") << true << true; QTest::newRow("commit-owner") << false << false; QTest::newRow("commit-track") << false << true; }
	void returnedResourceGettersMayRetireNativeActors()
	{
		QFETCH(bool, setup); QFETCH(bool, track); const QPointer<ObservedSf2> native(seed()); const QPointer<InstrumentTrack> parent(native->instrumentTrack()); PatchesDialog dialog; if (!setup) { QVERIFY(dialog.setup(native.data(), "Returned getter")); QVERIFY(choose(&dialog, 1024, 7)); } bool retired = false; native->inspect = [&] { if (!retired) { retired = true; if (track) { delete parent.data(); } else { delete native.data(); } } }; if (setup) { QVERIFY(!dialog.setup(native.data(), "Returned getter")); } else { QVERIFY(accept(&dialog)); } QVERIFY(retired); QVERIFY(!native); QCOMPARE(dialog.result(), int(QDialog::Rejected));
	}
	void commitCallbacksMayRetireTheOwnerOrDialog_data()
	{ QTest::addColumn<bool>("dialogDeath"); QTest::newRow("native-owner") << false; QTest::newRow("dialog-owner") << true; }
	void commitCallbacksMayRetireTheOwnerOrDialog()
	{
		QFETCH(bool, dialogDeath); const QPointer<ObservedSf2> native(seed()); const QPointer<InstrumentTrack> track(native->instrumentTrack()); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data(); QVERIFY(active->setup(native.data(), "Retirement")); QVERIFY(choose(active, 1024, 7)); bool retired = false; const auto connection = connect(native.data(), &Sf2Instrument::patchChanged, this, [&] { if (!retired) { retired = true; if (dialogDeath) { delete dialog.data(); } else { delete track.data(); } } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(accept(active)); QVERIFY(retired); if (dialogDeath) { QVERIFY(!dialog); QVERIFY(native); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7); } else { QVERIFY(!native); QVERIFY(dialog); QCOMPARE(dialog->result(), int(QDialog::Rejected)); delete dialog.data(); }
	}
	void widgetNotificationsCannotResumeAnOldSetup_data()
	{ QTest::addColumn<bool>("dialogDeath"); QTest::newRow("nested-setup") << false; QTest::newRow("dialog-retirement") << true; }
	void widgetNotificationsCannotResumeAnOldSetup()
	{
		QFETCH(bool, dialogDeath); auto first = seed(); auto second = seed(); QString error; QVERIFY(second->selectProgram(1024, 7, error)); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data(); bool called = false; connect(banks(active), &QTreeWidget::currentItemChanged, this, [&] { if (!called) { called = true; if (dialogDeath) { QVERIFY(QMetaObject::invokeMethod(active, "reject", Qt::DirectConnection)); active->deleteLater(); } else { QVERIFY(active->setup(second, "New setup")); } } }); QVERIFY(!active->setup(first, "Old setup")); QVERIFY(called); if (dialogDeath) { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!dialog); QCOMPARE(first->getBank(), 0); QCOMPARE(first->getPatch(), 0); QCOMPARE(second->getBank(), 1024); QCOMPARE(second->getPatch(), 7); } else { QVERIFY(dialog); QCOMPARE(banks(active)->currentItem()->text(0).toInt(), 1024); QSignalSpy changedFirst(first, &Sf2Instrument::patchChanged), changedSecond(second, &Sf2Instrument::patchChanged); QVERIFY(choose(active, 0, 0)); QVERIFY(accept(active)); QCOMPARE(changedFirst.count(), 0); QCOMPARE(changedSecond.count(), 1); delete dialog.data(); }
	}
	void reusedSetupDisconnectsOldNativeInvalidation()
	{
		auto first = seed(); auto second = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(first, "First")); QVERIFY(dialog.setup(second, "Second")); QString error; QVERIFY(first->setFileResource("soundfont", {}, error)); QVERIFY(choose(&dialog, 1024, 7)); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(second->getBank(), 1024); QCOMPARE(second->getPatch(), 7);
	}
	void queuedOldNotificationsCannotInvalidateAReusedDialog()
	{
		auto first = seed(); auto second = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(first, "First queued context")); std::thread notification([first] { emit first->fileLoading(); }); notification.join(); QVERIFY(dialog.setup(second, "New context")); QCoreApplication::processEvents(); QVERIFY(choose(&dialog, 1024, 7)); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(first->getBank(), 0); QCOMPARE(second->getBank(), 1024); QCOMPARE(second->getPatch(), 7);
	}
	void rootChangesAndRateNotificationRejectBeforeCommit_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("routing-generation") << 0; QTest::newRow("filename") << 1; QTest::newRow("rate-notification") << 2; }
	void rootChangesAndRateNotificationRejectBeforeCommit()
	{
		QFETCH(int, change); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Root changes")); QVERIFY(choose(&dialog, 1024, 7)); if (change == 0) { Engine::mixer()->clear(); } if (change == 1) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), media.filePath("changed.mmp")); } if (change == 2) { QVERIFY(QMetaObject::invokeMethod(Engine::audioEngine(), "sampleRateChanged", Qt::DirectConnection)); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void getterExceptionsStayInsideTheDialog_data()
	{ QTest::addColumn<bool>("setup"); QTest::addColumn<bool>("standard"); QTest::newRow("setup-standard") << true << true; QTest::newRow("setup-nonstandard") << true << false; QTest::newRow("commit-standard") << false << true; QTest::newRow("commit-nonstandard") << false << false; }
	void getterExceptionsStayInsideTheDialog()
	{
		QFETCH(bool, setup); QFETCH(bool, standard); auto native = seed(); PatchesDialog dialog; if (!setup) { QVERIFY(dialog.setup(native, "Exception")); QVERIFY(choose(&dialog, 1024, 7)); } native->inspect = [standard] { if (standard) { throw std::runtime_error("Native dialog getter fixture"); } throw 149; }; if (setup) { QVERIFY(!dialog.setup(native, "Exception")); } else { QVERIFY(accept(&dialog)); } native->inspect = {}; QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void captionInputsAreOwnedAcrossWidgetCallbacks()
	{
		auto native = seed(); PatchesDialog dialog; QString caption = "Original caption"; connect(banks(&dialog), &QTreeWidget::currentItemChanged, this, [&] { caption = "Changed borrowed input"; }); QVERIFY(dialog.setup(native, caption)); QVERIFY(dialog.windowTitle().startsWith("Original caption")); QCOMPARE(caption, QString("Changed borrowed input"));
	}
	void changedReturnedBankMetadataRefusesAStaleCommit_data()
	{ QTest::addColumn<int>("field"); QTest::newRow("path") << 0; QTest::newRow("snapshot") << 1; QTest::newRow("unresolved") << 2; }
	void changedReturnedBankMetadataRefusesAStaleCommit()
	{
		QFETCH(int, field); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Changed bank metadata")); QVERIFY(choose(&dialog, 1024, 7)); native->changeResult = [field](std::vector<Plugin::FileResource>& resources) { if (field == 0) { resources[0].path += ".changed"; } if (field == 1) { resources[0].identity.reset(); } if (field == 2) { resources[0].unresolved = true; } }; QVERIFY(accept(&dialog)); native->changeResult = {}; QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void removedWidgetsCannotLeaveAUsableStaleSelection_data()
	{ QTest::addColumn<int>("widget"); QTest::newRow("bank-list") << 0; QTest::newRow("program-list") << 1; QTest::newRow("accept-button") << 2; }
	void removedWidgetsCannotLeaveAUsableStaleSelection()
	{
		QFETCH(int, widget); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Widget retirement")); QVERIFY(choose(&dialog, 1024, 7)); if (widget == 0) { delete banks(&dialog); } if (widget == 1) { delete programs(&dialog); } if (widget == 2) { delete ok(&dialog); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void currentSelectionAcceptanceUsesTheNativeCommitPath()
	{
		auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Unchanged")); QSignalSpy changed(native, &Sf2Instrument::patchChanged), rejected(&dialog, &QDialog::rejected); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(changed.count(), 1); QString error; QVERIFY(native->setFileResource("soundfont", {}, error)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(rejected.count(), 0);
	}
	void unavailableStoredProgramRequiresAnExplicitAcceptance()
	{
		auto native = seed(); native->childModel("bank")->setValue(99); native->childModel("patch")->setValue(99); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Unavailable stored patch")); QCOMPARE(native->getBank(), 99); QCOMPARE(native->getPatch(), 99); QCOMPARE(banks(&dialog)->currentItem()->text(0).toInt(), 0); QCOMPARE(selectedProgram(&dialog), 0); QVERIFY(accept(&dialog)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void retiredBankCleanupCannotResumeOnADeletedDialog()
	{
		auto first = seed(); auto second = seed(); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data();
		QVERIFY(active->setup(first, "Retained old bank")); bool retired = false; QMetaObject::Connection retirement;
		{
			const auto resource = first->fileResources()[0]; const auto snapshot = std::static_pointer_cast<const FileResourceSnapshot>(resource.identity);
			retirement = connect(snapshot.get(), &QObject::destroyed, this, [&] { retired = true; delete dialog.data(); });
		}
		const auto disconnect = qScopeGuard([retirement] { QObject::disconnect(retirement); });
		QString error; QVERIFY(first->setFileResource("soundfont", {}, error)); QVERIFY(!retired);
		// Retire the old native synth as well as its lazily unloaded bank.
		first->reloadSynth(); QVERIFY(!retired);
		active->setup(second, "Replacement context"); QVERIFY(retired); QVERIFY(!dialog);
		QCOMPARE(second->getBank(), 0); QCOMPARE(second->getPatch(), 0);
	}
	void previewTokensRefuseAnOlderDialogRestore()
	{
		auto native = seed(); QString error; Sf2Instrument::ProgramPreviewToken first, second; QVERIFY(native->previewProgram(1024, 7, error, first)); QVERIFY(native->previewProgram(0, 0, error, second)); QVERIFY(first != second); QVERIFY(!native->restoreProgramPreview(first, error)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0})); QVERIFY(native->restoreProgramPreview(second, error)); QVERIFY(!native->restoreProgramPreview(second, error)); QCOMPARE(native->getBank(), 0); QCOMPARE(native->getPatch(), 0);
	}
	void nativeChangesSupersedePreviewTokens_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("bank-clear") << 0; QTest::newRow("bank-replace") << 1; QTest::newRow("synth-rebuild") << 2; QTest::newRow("committed-program") << 3; QTest::newRow("bank-control") << 4; QTest::newRow("patch-control") << 5; }
	void nativeChangesSupersedePreviewTokens()
	{
		QFETCH(int, change); auto native = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(native->previewProgram(1024, 7, error, token)); if (change == 0) { QVERIFY(native->setFileResource("soundfont", {}, error)); } if (change == 1) { QVERIFY(native->setFileResource("soundfont", bank(), error)); } if (change == 2) { native->reloadSynth(); } if (change == 3) { QVERIFY(native->selectProgram(1024, 7, error)); } if (change == 4) { native->childModel("bank")->setValue(99); } if (change == 5) { native->childModel("patch")->setValue(99); } QVERIFY(!native->restoreProgramPreview(token, error)); if (change == 4 || change == 5) { QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{-1, -1})); }
	}
	void fileLoadingCanRestoreTheCurrentPreviewWithoutReenteringAResourceChange()
	{
		auto native = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(native->previewProgram(1024, 7, error, token)); bool restored = false; const auto connection = connect(native, &Sf2Instrument::fileLoading, this, [&] { restored = native->restoreProgramPreview(token, error); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(!native->setFileResource("soundfont", media.filePath("absent.sf2"), error)); QVERIFY(restored); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void filteringAndKeyboardSelectionPreserveAuditionAndRestoreHiddenRows()
	{
		auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Search and keys")); QVERIFY(choose(&dialog, 1024, 7)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); const auto filter = dialog.findChild<QLineEdit*>("m_filterEdit"); QVERIFY(filter); filter->setText("does-not-match"); QCOMPARE(programs(&dialog)->model()->rowCount(), 0); QVERIFY(!ok(&dialog)->isEnabled()); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0})); QTest::keyClick(&dialog, Qt::Key_Down); filter->setText("OCTAVE"); QCOMPARE(programs(&dialog)->model()->rowCount(), 1); QTest::keyClick(&dialog, Qt::Key_Up); QCOMPARE(selectedProgram(&dialog), 7); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); QTest::keyClick(&dialog, Qt::Key_Return); QCOMPARE(native->getBank(), 1024); QCOMPARE(native->getPatch(), 7);
	}
	void dialogDestructionRestoresItsLiveAudition()
	{
		auto native = seed(); auto dialog = new PatchesDialog; QVERIFY(dialog->setup(native, "Destroyed preview")); QVERIFY(choose(dialog, 1024, 7)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); delete dialog; QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void olderDialogCancellationDoesNotCancelANewerAudition()
	{
		auto native = seed(); PatchesDialog first, second; QVERIFY(first.setup(native, "First")); QVERIFY(choose(&first, 1024, 7)); QVERIFY(second.setup(native, "Second")); QVERIFY(choose(&second, 1024, 7)); QVERIFY(reject(&first)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{1024, 7})); QVERIFY(reject(&second)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void invalidPreviewRequestsKeepTheLastRestorableToken()
	{
		auto native = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(native->previewProgram(1024, 7, error, token)); const auto prior = token; QVERIFY(!native->previewProgram(111, 99, error, token)); QVERIFY(token == prior); QVERIFY(native->restoreProgramPreview(token, error)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0})); QVERIFY(!native->restoreProgramPreview({}, error));
	}
	void previewOwnershipDoesNotAlterSerializedBankAndProgram()
	{
		auto native = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(native->previewProgram(1024, 7, error, token)); QDomDocument document; auto node = document.createElement("sf2player"); document.appendChild(node); native->saveSettings(document, node); QCOMPARE(node.attribute("bank").toInt(), 0); QCOMPARE(node.attribute("patch").toInt(), 0); QVERIFY(native->restoreProgramPreview(token, error));
	}
	void projectionAndFilterRetirementInvalidatesAudition_data()
	{ QTest::addColumn<int>("part"); QTest::newRow("filter") << 0; QTest::newRow("source-model") << 1; QTest::newRow("proxy-model") << 2; QTest::newRow("selection-model") << 3; QTest::newRow("replaced-view-model") << 4; }
	void projectionAndFilterRetirementInvalidatesAudition()
	{
		QFETCH(int, part); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Projection retirement")); QVERIFY(choose(&dialog, 1024, 7)); auto table = programs(&dialog); auto proxy = qobject_cast<QSortFilterProxyModel*>(table->model()); QVERIFY(proxy); if (part == 0) { delete dialog.findChild<QLineEdit*>("m_filterEdit"); } if (part == 1) { delete proxy->sourceModel(); } if (part == 2) { delete proxy; } if (part == 3) { delete table->selectionModel(); } if (part == 4) { table->setModel(new QStandardItemModel(&dialog)); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void sourceModelNotificationsCheckReturnedNativeOwnership_data()
	{ QTest::addColumn<bool>("track"); QTest::newRow("native-owner") << false; QTest::newRow("native-track") << true; }
	void sourceModelNotificationsCheckReturnedNativeOwnership()
	{
		QFETCH(bool, track); const QPointer<ObservedSf2> native(seed()); const QPointer<InstrumentTrack> parent(native->instrumentTrack()); PatchesDialog dialog; auto proxy = qobject_cast<QSortFilterProxyModel*>(programs(&dialog)->model()); QVERIFY(proxy); bool retired = false; connect(proxy->sourceModel(), &QAbstractItemModel::rowsInserted, this, [&] { if (!retired) { retired = true; if (track) { delete parent.data(); } else { delete native.data(); } } }); QVERIFY(!dialog.setup(native.data(), "Source row callback")); QVERIFY(retired); QVERIFY(!native); QCOMPARE(dialog.result(), int(QDialog::Rejected));
	}
	void admissionReservationsStillAllowCancellingOwnedAudition_data()
	{ QTest::addColumn<bool>("stem"); QTest::newRow("device-reservation") << false; QTest::newRow("stem-selection") << true; }
	void admissionReservationsStillAllowCancellingOwnedAudition()
	{
		QFETCH(bool, stem); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Admission cancellation")); QVERIFY(choose(&dialog, 1024, 7)); if (stem) { TrackRenderScope selection(native->instrumentTrack()); QVERIFY(reject(&dialog)); } else { QVERIFY(Engine::audioEngine()->storeAudioDevice()); const auto restore = qScopeGuard([] { Engine::audioEngine()->restoreAudioDevice(); }); QVERIFY(reject(&dialog)); } QCOMPARE(Sf2PatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{0, 0}));
	}
	void actualModuleSelectionPersistsThroughOwnedProjectResources()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native SoundFont module must be discoverable"); QTemporaryDir source; const auto path = source.filePath("bank.sf2"), project = source.filePath("portable.mmp"); QString error; QVERIFY(QFile::copy(bank(), path)); auto response = StudioCapabilities::execute("track.add", {{"type", "instrument"}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); response = StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "sf2player"}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", path}}).value("ok").toBool()); auto native = static_cast<Sf2Instrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Actual module")); QVERIFY(choose(&dialog, 1024, 7)); QVERIFY(accept(&dialog)); response = StudioCapabilities::execute("project.save", {{"path", project}, {"withResources", true}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); const auto saved = response.value("result").toObject().value("path").toString(); QVERIFY(!saved.isEmpty()); QVERIFY(QFile::remove(path)); QVERIFY(StudioCapabilities::execute("project.load", {{"path", saved}}).value("ok").toBool()); auto restored = static_cast<Sf2Instrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); PatchesDialog reopened; QVERIFY(reopened.setup(restored, "Reopened module")); QCOMPARE(banks(&reopened)->currentItem()->text(0).toInt(), 1024); QCOMPARE(selectedProgram(&reopened), 7);
	}
};
int main(int argc, char** argv) { QApplication application(argc, argv); Sf2PatchDialogTest test; return QTest::qExec(&test, argc, argv); }
#include "Sf2PatchDialogTest.moc"
