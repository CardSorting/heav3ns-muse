#include "../core/NativeSongTestAccess.h"
/* Native GIG patch dialog ownership and modal invalidation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QApplication>
#include <QtTest>
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
#include "GigPlayer.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "PatchesDialog.h"
#include "PluginFactory.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../core/GigNativeFixture.h"
using namespace lmms;
using lmms::gui::PatchesDialog;
namespace lmms
{
class GigPatchDialogTestAccess
{
public:
	static std::pair<int, int> nativeSelection(GigInstrument* native) { QMutexLocker lock(&native->m_synthMutex); return native->m_instrument ? std::pair<int, int>{native->m_instrument->MIDIBank, native->m_instrument->MIDIProgram} : std::pair<int, int>{-1, -1}; }
};
}
namespace
{
class ObservedGig final : public GigInstrument
{
public:
	using GigInstrument::GigInstrument;
	mutable std::function<void()> inspect;
	mutable std::function<void(std::vector<FileResource>&)> changeResult;
	std::vector<FileResource> fileResources() const override { auto result = GigInstrument::fileResources(); const auto callback = inspect; const auto change = changeResult; if (callback) { callback(); } if (change) { change(result); } return result; }
};
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new ObservedGig(this); }
};
QTreeWidget* banks(PatchesDialog* dialog) { return dialog->findChild<QTreeWidget*>("m_bankListView"); }
QTreeWidget* programs(PatchesDialog* dialog) { return dialog->findChild<QTreeWidget*>("m_progListView"); }
QPushButton* ok(PatchesDialog* dialog) { return dialog->findChild<QPushButton*>("m_okButton"); }
bool choose(PatchesDialog* dialog, int bank, int program)
{
	const QPointer<PatchesDialog> owner(dialog); const QPointer<QTreeWidget> bankList(banks(dialog)), programList(programs(dialog)); if (!bankList || !programList) { return false; }
	const auto bankItems = bankList->findItems(QString::number(bank), Qt::MatchExactly, 0); if (bankItems.isEmpty()) { return false; } bankList->setCurrentItem(bankItems.front()); if (!owner || !bankList || !programList) { return false; }
	const auto items = programList->findItems(QString::number(program), Qt::MatchExactly, 0); if (items.isEmpty()) { return false; } programList->setCurrentItem(items.front()); return owner && bankList && programList;
}
bool accept(PatchesDialog* dialog) { return QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection); }
bool reject(PatchesDialog* dialog) { return QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection); }
}
class GigPatchDialogTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration, media;
	QString bank() const { return media.filePath("programs.gig"); }
	ObservedGig* seed(bool loaded = true) { auto track = new NativeTrack; auto native = dynamic_cast<ObservedGig*>(track->loadInstrument("gigplayer")); if (!native) { throw std::runtime_error("Native GIG was not selected"); } if (loaded) { QString error; if (!native->setFileResource("bank", bank(), error)) { throw std::runtime_error(error.toStdString()); } } return native; }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); QVERIFY(media.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QString error; QVERIFY2(test::gigNative::create(bank(), true, error, 16, 1, true), qPrintable(error)); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void cleanup() { for (auto track : Engine::getSong()->tracks()) { if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { if (auto native = dynamic_cast<ObservedGig*>(instrument->instrument())) { native->inspect = {}; native->changeResult = {}; } } } Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void setupUsesTheCurrentNativeSelection_data()
	{ QTest::addColumn<bool>("alternate"); QTest::newRow("first-bank") << false; QTest::newRow("alternate-bank") << true; }
	void setupUsesTheCurrentNativeSelection()
	{
		QFETCH(bool, alternate); auto native = seed(); QString error; if (alternate) { QVERIFY(native->selectProgram(4, 11, error)); } PatchesDialog dialog; QVERIFY(dialog.setup(native, "Native programs")); QCOMPARE(banks(&dialog)->topLevelItemCount(), 2); QCOMPARE(banks(&dialog)->currentItem()->text(0).toInt(), alternate ? 4 : 2); QCOMPARE(programs(&dialog)->currentItem()->text(0).toInt(), alternate ? 11 : 7); QVERIFY(ok(&dialog)->isEnabled());
	}
	void cancellationLeavesNativeModelsAndInstrumentUntouched_data()
	{ QTest::addColumn<int>("route"); QTest::newRow("reject-slot") << 0; QTest::newRow("cancel-button") << 1; QTest::newRow("window-close") << 2; }
	void cancellationLeavesNativeModelsAndInstrumentUntouched()
	{
		QFETCH(int, route); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Cancel")); QVERIFY(choose(&dialog, 4, 11)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7); QCOMPARE(GigPatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{2, 7})); if (route == 0) { QVERIFY(reject(&dialog)); } if (route == 1) { dialog.findChild<QPushButton*>("m_cancelButton")->click(); } if (route == 2) { dialog.show(); dialog.close(); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7); QCOMPARE(GigPatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{2, 7}));
	}
	void acceptCommitsThroughNativeProgramSelection_data()
	{ QTest::addColumn<bool>("button"); QTest::newRow("accept-slot") << false; QTest::newRow("ok-button") << true; }
	void acceptCommitsThroughNativeProgramSelection()
	{
		QFETCH(bool, button); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Commit")); QVERIFY(choose(&dialog, 4, 11)); QSignalSpy changed(native, &GigInstrument::patchChanged); if (button) { ok(&dialog)->click(); } else { QVERIFY(accept(&dialog)); } QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(native->getBank(), 4); QCOMPARE(native->getPatch(), 11); QCOMPARE(GigPatchDialogTestAccess::nativeSelection(native), (std::pair<int, int>{4, 11})); QCOMPARE(changed.count(), 1);
	}
	void emptyAndPendingBanksCannotOpenAnEditableSelection_data()
	{ QTest::addColumn<bool>("pending"); QTest::newRow("empty") << false; QTest::newRow("pending") << true; }
	void emptyAndPendingBanksCannotOpenAnEditableSelection()
	{
		QFETCH(bool, pending); auto native = seed(false); if (pending) { QDomDocument document; auto node = document.createElement("gigplayer"); document.appendChild(node); node.setAttribute("src", media.filePath("missing.gig")); node.setAttribute("unresolved", 1); node.setAttribute("bank", 4); node.setAttribute("patch", 11); native->loadSettings(node); } PatchesDialog dialog; QVERIFY(!dialog.setup(native, "Unavailable")); QVERIFY(accept(&dialog)); QVERIFY(native->programSnapshot().empty()); QCOMPARE(native->fileResources()[0].unresolved, pending);
	}
	void modalLoopRejectsRetiredOrReplacedNativeContext_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("owner") << 0; QTest::newRow("track") << 1; QTest::newRow("project") << 2; QTest::newRow("clear-bank") << 3; QTest::newRow("replace-bank") << 4; QTest::newRow("project-filename") << 5; }
	void modalLoopRejectsRetiredOrReplacedNativeContext()
	{
		QFETCH(int, change); const QPointer<ObservedGig> native(seed()); const QPointer<InstrumentTrack> track(native->instrumentTrack()); PatchesDialog dialog; QVERIFY(dialog.setup(native.data(), "Modal")); QVERIFY(choose(&dialog, 4, 11)); bool called = false; QTimer::singleShot(0, &dialog, [&] { called = true; QString error; if (change == 0) { delete native.data(); } if (change == 1) { delete track.data(); } if (change == 2) { Engine::getSong()->clearProject(); } if (change == 3) { QVERIFY(native->setFileResource("bank", {}, error)); } if (change == 4) { QVERIFY(native->setFileResource("bank", bank(), error)); } if (change == 5) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), media.filePath("changed.mmp")); } }); const auto result = dialog.exec(); QVERIFY(called); QCOMPARE(result, int(QDialog::Rejected)); QVERIFY(accept(&dialog)); if (native) { QVERIFY(native->getBank() != 4 || native->getPatch() != 11); }
	}
	void externalControlEditsInvalidateTheCandidate_data()
	{ QTest::addColumn<bool>("patch"); QTest::newRow("bank") << false; QTest::newRow("program") << true; }
	void externalControlEditsInvalidateTheCandidate()
	{
		QFETCH(bool, patch); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "External edit")); QVERIFY(choose(&dialog, 4, 11)); native->childModel(patch ? "patch" : "bank")->setValue(99); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(patch ? native->getPatch() : native->getBank(), 99);
	}
	void returnedResourceGettersMayRetireNativeActors_data()
	{ QTest::addColumn<bool>("setup"); QTest::addColumn<bool>("track"); QTest::newRow("setup-owner") << true << false; QTest::newRow("setup-track") << true << true; QTest::newRow("commit-owner") << false << false; QTest::newRow("commit-track") << false << true; }
	void returnedResourceGettersMayRetireNativeActors()
	{
		QFETCH(bool, setup); QFETCH(bool, track); const QPointer<ObservedGig> native(seed()); const QPointer<InstrumentTrack> parent(native->instrumentTrack()); PatchesDialog dialog; if (!setup) { QVERIFY(dialog.setup(native.data(), "Returned getter")); QVERIFY(choose(&dialog, 4, 11)); } bool retired = false; native->inspect = [&] { if (!retired) { retired = true; if (track) { delete parent.data(); } else { delete native.data(); } } }; if (setup) { QVERIFY(!dialog.setup(native.data(), "Returned getter")); } else { QVERIFY(accept(&dialog)); } QVERIFY(retired); QVERIFY(!native); QCOMPARE(dialog.result(), int(QDialog::Rejected));
	}
	void commitCallbacksMayRetireTheOwnerOrDialog_data()
	{ QTest::addColumn<bool>("dialogDeath"); QTest::newRow("native-owner") << false; QTest::newRow("dialog-owner") << true; }
	void commitCallbacksMayRetireTheOwnerOrDialog()
	{
		QFETCH(bool, dialogDeath); const QPointer<ObservedGig> native(seed()); const QPointer<InstrumentTrack> track(native->instrumentTrack()); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data(); QVERIFY(active->setup(native.data(), "Retirement")); QVERIFY(choose(active, 4, 11)); bool retired = false; const auto connection = connect(native.data(), &GigInstrument::patchChanged, this, [&] { if (!retired) { retired = true; if (dialogDeath) { delete dialog.data(); } else { delete track.data(); } } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(accept(active)); QVERIFY(retired); if (dialogDeath) { QVERIFY(!dialog); QVERIFY(native); QCOMPARE(native->getBank(), 4); QCOMPARE(native->getPatch(), 11); } else { QVERIFY(!native); QVERIFY(dialog); QCOMPARE(dialog->result(), int(QDialog::Rejected)); delete dialog.data(); }
	}
	void widgetNotificationsCannotResumeAnOldSetup_data()
	{ QTest::addColumn<bool>("dialogDeath"); QTest::newRow("nested-setup") << false; QTest::newRow("dialog-retirement") << true; }
	void widgetNotificationsCannotResumeAnOldSetup()
	{
		QFETCH(bool, dialogDeath); auto first = seed(); auto second = seed(); QString error; QVERIFY(second->selectProgram(4, 11, error)); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data(); bool called = false; connect(banks(active), &QTreeWidget::currentItemChanged, this, [&] { if (!called) { called = true; if (dialogDeath) { delete dialog.data(); } else { QVERIFY(active->setup(second, "New setup")); } } }); QVERIFY(!active->setup(first, "Old setup")); QVERIFY(called); if (dialogDeath) { QVERIFY(!dialog); } else { QVERIFY(dialog); QCOMPARE(banks(active)->currentItem()->text(0).toInt(), 4); QSignalSpy changedFirst(first, &GigInstrument::patchChanged), changedSecond(second, &GigInstrument::patchChanged); QVERIFY(choose(active, 2, 7)); QVERIFY(accept(active)); QCOMPARE(changedFirst.count(), 0); QCOMPARE(changedSecond.count(), 1); delete dialog.data(); }
	}
	void reusedSetupDisconnectsOldNativeInvalidation()
	{
		auto first = seed(); auto second = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(first, "First")); QVERIFY(dialog.setup(second, "Second")); QString error; QVERIFY(first->setFileResource("bank", {}, error)); QVERIFY(choose(&dialog, 4, 11)); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(second->getBank(), 4); QCOMPARE(second->getPatch(), 11);
	}
	void queuedOldNotificationsCannotInvalidateAReusedDialog()
	{
		auto first = seed(); auto second = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(first, "First queued context")); std::thread notification([first] { emit first->fileLoading(); }); notification.join(); QVERIFY(dialog.setup(second, "New context")); QCoreApplication::processEvents(); QVERIFY(choose(&dialog, 4, 11)); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(first->getBank(), 2); QCOMPARE(second->getBank(), 4); QCOMPARE(second->getPatch(), 11);
	}
	void rootChangesAndRateNotificationRejectBeforeCommit_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("routing-generation") << 0; QTest::newRow("filename") << 1; QTest::newRow("rate-notification") << 2; }
	void rootChangesAndRateNotificationRejectBeforeCommit()
	{
		QFETCH(int, change); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Root changes")); QVERIFY(choose(&dialog, 4, 11)); if (change == 0) { Engine::mixer()->clear(); } if (change == 1) { NativeSongTestAccess::setProjectFileName(Engine::getSong(), media.filePath("changed.mmp")); } if (change == 2) { QVERIFY(QMetaObject::invokeMethod(Engine::audioEngine(), "sampleRateChanged", Qt::DirectConnection)); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void getterExceptionsStayInsideTheDialog_data()
	{ QTest::addColumn<bool>("setup"); QTest::addColumn<bool>("standard"); QTest::newRow("setup-standard") << true << true; QTest::newRow("setup-nonstandard") << true << false; QTest::newRow("commit-standard") << false << true; QTest::newRow("commit-nonstandard") << false << false; }
	void getterExceptionsStayInsideTheDialog()
	{
		QFETCH(bool, setup); QFETCH(bool, standard); auto native = seed(); PatchesDialog dialog; if (!setup) { QVERIFY(dialog.setup(native, "Exception")); QVERIFY(choose(&dialog, 4, 11)); } native->inspect = [standard] { if (standard) { throw std::runtime_error("Native dialog getter fixture"); } throw 149; }; if (setup) { QVERIFY(!dialog.setup(native, "Exception")); } else { QVERIFY(accept(&dialog)); } native->inspect = {}; QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void captionInputsAreOwnedAcrossWidgetCallbacks()
	{
		auto native = seed(); PatchesDialog dialog; QString caption = "Original caption"; connect(banks(&dialog), &QTreeWidget::currentItemChanged, this, [&] { caption = "Changed borrowed input"; }); QVERIFY(dialog.setup(native, caption)); QVERIFY(dialog.windowTitle().startsWith("Original caption")); QCOMPARE(caption, QString("Changed borrowed input"));
	}
	void changedReturnedBankMetadataRefusesAStaleCommit_data()
	{ QTest::addColumn<int>("field"); QTest::newRow("path") << 0; QTest::newRow("snapshot") << 1; QTest::newRow("unresolved") << 2; }
	void changedReturnedBankMetadataRefusesAStaleCommit()
	{
		QFETCH(int, field); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Changed bank metadata")); QVERIFY(choose(&dialog, 4, 11)); native->changeResult = [field](std::vector<Plugin::FileResource>& resources) { if (field == 0) { resources[0].path += ".changed"; } if (field == 1) { resources[0].identity.reset(); } if (field == 2) { resources[0].unresolved = true; } }; QVERIFY(accept(&dialog)); native->changeResult = {}; QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void removedWidgetsCannotLeaveAUsableStaleSelection_data()
	{ QTest::addColumn<int>("widget"); QTest::newRow("bank-list") << 0; QTest::newRow("program-list") << 1; QTest::newRow("accept-button") << 2; }
	void removedWidgetsCannotLeaveAUsableStaleSelection()
	{
		QFETCH(int, widget); auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Widget retirement")); QVERIFY(choose(&dialog, 4, 11)); if (widget == 0) { delete banks(&dialog); } if (widget == 1) { delete programs(&dialog); } if (widget == 2) { delete ok(&dialog); } QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void unchangedAcceptanceDoesNotRepublishNativeProgramChanges()
	{
		auto native = seed(); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Unchanged")); QSignalSpy changed(native, &GigInstrument::patchChanged), rejected(&dialog, &QDialog::rejected); QVERIFY(accept(&dialog)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(changed.count(), 0); QString error; QVERIFY(native->setFileResource("bank", {}, error)); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(rejected.count(), 0);
	}
	void unavailableStoredProgramRequiresAnExplicitAcceptance()
	{
		auto native = seed(); native->childModel("bank")->setValue(99); native->childModel("patch")->setValue(99); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Unavailable stored patch")); QCOMPARE(native->getBank(), 99); QCOMPARE(native->getPatch(), 99); QCOMPARE(banks(&dialog)->currentItem()->text(0).toInt(), 2); QCOMPARE(programs(&dialog)->currentItem()->text(0).toInt(), 7); QVERIFY(accept(&dialog)); QCOMPARE(native->getBank(), 2); QCOMPARE(native->getPatch(), 7);
	}
	void retiredBankCleanupCannotResumeOnADeletedDialog()
	{
		auto first = seed(); auto second = seed(); const QPointer<PatchesDialog> dialog(new PatchesDialog); auto active = dialog.data(); QVERIFY(active->setup(first, "Retained old bank")); bool retired = false; { const auto resource = first->fileResources()[0]; const auto snapshot = std::static_pointer_cast<const FileResourceSnapshot>(resource.identity); connect(snapshot.get(), &QObject::destroyed, this, [&] { retired = true; delete dialog.data(); }); } QString error; QVERIFY(first->setFileResource("bank", {}, error)); QVERIFY(!retired); active->setup(second, "Replacement context"); QVERIFY(retired); QVERIFY(!dialog); QCOMPARE(second->getBank(), 2); QCOMPARE(second->getPatch(), 7);
	}
	void actualModuleSelectionPersistsThroughOwnedProjectResources()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("gigplayer").isNull(), "The built native GIG module must be discoverable"); QTemporaryDir source; const auto path = source.filePath("bank.gig"), project = source.filePath("portable.mmp"); QString error; QVERIFY(test::gigNative::create(path, true, error, 16, 1, true)); auto response = StudioCapabilities::execute("track.add", {{"type", "instrument"}, {"instrument", "gigplayer"}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); QVERIFY(StudioCapabilities::execute("instrument.resource.set", {{"track", 0}, {"resource", "bank"}, {"path", path}}).value("ok").toBool()); auto native = static_cast<GigInstrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); PatchesDialog dialog; QVERIFY(dialog.setup(native, "Actual module")); QVERIFY(choose(&dialog, 4, 11)); QVERIFY(accept(&dialog)); response = StudioCapabilities::execute("project.save", {{"path", project}, {"withResources", true}}); QVERIFY2(response.value("ok").toBool(), QJsonDocument(response).toJson().constData()); const auto saved = response.value("result").toObject().value("path").toString(); QVERIFY(!saved.isEmpty()); QVERIFY(QFile::remove(path)); QVERIFY(QFile::remove(source.filePath("bank.gx01"))); QVERIFY(StudioCapabilities::execute("project.load", {{"path", saved}}).value("ok").toBool()); auto restored = static_cast<GigInstrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); PatchesDialog reopened; QVERIFY(reopened.setup(restored, "Reopened module")); QCOMPARE(banks(&reopened)->currentItem()->text(0).toInt(), 4); QCOMPARE(programs(&reopened)->currentItem()->text(0).toInt(), 11);
	}
};
int main(int argc, char** argv) { QApplication application(argc, argv); GigPatchDialogTest test; return QTest::qExec(&test, argc, argv); }
#include "GigPatchDialogTest.moc"
