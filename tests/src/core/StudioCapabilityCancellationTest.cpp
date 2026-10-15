/* Cooperative ordinary-capability cancellation and runtime restoration.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "ModelValueNotificationTestUtils.h"
#include "NativeSongTestAccess.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Lfo, nullptr, "Native capability cancellation fixture") { }
	int kind = 0, reads = 0;
	float currentValue(int) override
	{
		++reads;
		if (kind == 2) { throw std::runtime_error("Native getter fixture"); }
		if (kind == 3) { throw 347; }
		return kind == 1 ? std::numeric_limits<float>::quiet_NaN() : .25f;
	}
};
[[noreturn]] void raise(bool nonstandard)
{ if (nonstandard) { throw 349; } throw std::runtime_error("Native capability cancellation origin fixture"); }
QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
{ return {{"id", id}, {"op", operation}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject selector(AutomatableModel* model) { return {{"parameter", int(model->id())}, {"choices", false}}; }
void seed()
{
	auto song = Engine::getSong(); song->setMasterVolume(100); auto track = new SampleTrack(song); track->setName("Retained native sample");
	auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); std::vector<SampleFrame> pcm(1024);
	for (auto& frame : pcm) { frame[0] = .125f; frame[1] = -.0625f; }
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
}
SampleClip* retainedClip()
{
	auto song = Engine::getSong(); if (song->tracks().size() != 1) { return nullptr; }
	auto track = dynamic_cast<SampleTrack*>(song->tracks().front());
	return track && track->numOfClips() == 1 ? dynamic_cast<SampleClip*>(track->getClips().front()) : nullptr;
}
}
class StudioCapabilityCancellationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	void replay(StudioRuntime& runtime, const QJsonObject& command, const QJsonObject& receipt)
	{ auto response = runtime.process(command); QVERIFY(response.take("replayed").toBool()); QCOMPARE(response, receipt); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void directInspectionKeepsItsCancellationSourceAfterPreflight_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("finite") << 0; QTest::newRow("nonfinite") << 1;
		QTest::newRow("standard-getter") << 2; QTest::newRow("nonstandard-getter") << 3;
	}
	void directInspectionKeepsItsCancellationSourceAfterPreflight()
	{
		QFETCH(int, kind); Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input));
		const auto cleanupBinding = qScopeGuard([&] { input.kind = 0; bool changed = false; QString error; model.disconnectController(changed, error); });
		input.kind = kind; input.reads = 0; bool eventRan = false; QObject timerOwner; QTimer::singleShot(0, &timerOwner, [&] { eventRan = true; });
		// Deliberately no outer FileResourceIO scope: execute owns this token.
		const auto response = StudioCapabilities::execute("parameter.inspect", selector(&model), [&] { return input.reads > 0; });
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(response.value("error").toString().contains("cancelled"));
		QCOMPARE(input.reads, 1); QVERIFY(!eventRan); QVERIFY(model.controllerConnection());
		QString error; QVERIFY(FileResourceIO::check(error)); input.kind = 0;
		QVERIFY(StudioCapabilities::execute("parameter.inspect", selector(&model)).value("ok").toBool());
	}

	void callbackExceptionsAfterTheGetterAreWholeInspectionFailures_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void callbackExceptionsAfterTheGetterAreWholeInspectionFailures()
	{
		QFETCH(bool, nonstandard); Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input)); input.reads = 0;
		const auto cleanupBinding = qScopeGuard([&] { bool changed = false; QString error; model.disconnectController(changed, error); });
		bool entered = false; const auto response = StudioCapabilities::execute("parameter.inspect", selector(&model), [&] {
			if (input.reads) { entered = true; raise(nonstandard); } return false;
		});
		QVERIFY(entered); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains(nonstandard ? "nonstandard" : "origin fixture")); QString error; QVERIFY(FileResourceIO::check(error));
	}

	void lateDirectCancellationDoesNotAcknowledgeAppliedNativeEdits_data()
	{ QTest::addColumn<bool>("dirty"); QTest::newRow("value-notification") << false; QTest::newRow("dirty-notification") << true; }
	void lateDirectCancellationDoesNotAcknowledgeAppliedNativeEdits()
	{
		QFETCH(bool, dirty); auto song = Engine::getSong(); song->setTempo(91); song->setMasterVolume(100); NativeSongTestAccess::setModified(song, false); bool cancelled = false;
		const auto connection = dirty ? connect(song, &Song::modified, this, [&] { cancelled = true; }, Qt::DirectConnection)
			: connect(song, &Song::tempoChanged, this, [&](bpm_t tempo) { if (tempo == 123) { cancelled = true; } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("song.set", {{"tempo", 123}, {"masterVolume", 90}}, [&] { return cancelled; });
		QVERIFY(cancelled); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains("cancelled"));
		// The adapter does not promise rollback. Runtime owns that boundary.
		QCOMPARE(song->getTempo(), bpm_t(123)); QCOMPARE(song->masterVolume(), 90);
		QString error; QVERIFY(FileResourceIO::check(error));
	}

	void nativeFailureKeepsItsOriginWhenCancellationAlsoArrives_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void nativeFailureKeepsItsOriginWhenCancellationAlsoArrives()
	{
		QFETCH(bool, nonstandard); auto song = Engine::getSong(); song->setTempo(91); bool cancelled = false;
		ModelValueNotificationProbe fault(&song->tempoModel(), [&] { cancelled = true; raise(nonstandard); }, true);
		const auto response = StudioCapabilities::execute("song.set", {{"tempo", 123}}, [&] { return cancelled; }); fault.reset();
		QVERIFY(cancelled); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains(nonstandard ? "nonstandard" : "origin fixture")); QVERIFY(!response.value("error").toString().contains("cancelled"));
		QString error; QVERIFY(FileResourceIO::check(error)); song->setTempo(97); QCOMPARE(song->getTempo(), bpm_t(97));
	}

	void nestedScopeRetainsTheOuterSourceAndDoesNotLeakTheInnerOne()
	{
		Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input)); input.reads = 0;
		const auto cleanupBinding = qScopeGuard([&] { bool changed = false; QString error; model.disconnectController(changed, error); });
		bool outerStop = false; int outerChecks = 0; FileResourceIO::Scope outer([&] { ++outerChecks; return outerStop; });
		const auto response = StudioCapabilities::execute("parameter.inspect", selector(&model), [&] { return input.reads > 0; });
		QVERIFY(!response.value("ok").toBool()); QVERIFY(outerChecks > 0); QString error; QVERIFY(FileResourceIO::check(error));
		QVERIFY(StudioCapabilities::execute("parameter.inspect", selector(&model)).value("ok").toBool()); outerStop = true;
		QVERIFY(!FileResourceIO::check(error)); const auto before = input.reads;
		QVERIFY(!StudioCapabilities::execute("parameter.inspect", selector(&model), [] { return false; }).value("ok").toBool()); QCOMPARE(input.reads, before);
		outerStop = false; QVERIFY(FileResourceIO::check(error));
	}

	void aProjectReplacedByTheFinalProbeCannotPublishAnOldResult()
	{
		auto song = Engine::getSong(); song->setTempo(91); NativeSongTestAccess::setModified(song, false); bool published = false, replaced = false;
		const auto connection = connect(song, &Song::modified, this, [&] { published = true; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto generation = song->replacementGeneration();
		const auto response = StudioCapabilities::execute("song.set", {{"tempo", 123}}, [&] {
			if (published && !replaced) { replaced = true; song->clearProject(); song->setTempo(77); } return false;
		});
		QVERIFY(replaced); QVERIFY(song->replacementGeneration() != generation); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains("project retired or changed")); QCOMPARE(song->getTempo(), bpm_t(77));
	}

	void ordinaryPublicationDoesNotPumpAnOuterEventPollingScope()
	{
		bool eventRan = false; QObject timerOwner; QTimer::singleShot(0, &timerOwner, [&] { eventRan = true; });
		{
			FileResourceIO::Scope outer({}, true);
			const auto response = StudioCapabilities::execute("studio.capabilities", {});
			QVERIFY(response.value("ok").toBool()); QVERIFY(!eventRan);
		}
		QCoreApplication::processEvents(); QVERIFY(eventRan);
	}

	void runtimeCancellationRestoresNativeContentAndDurableReceipts_data()
	{ QTest::addColumn<bool>("dirty"); QTest::newRow("value-notification") << false; QTest::newRow("dirty-notification") << true; }
	void runtimeCancellationRestoresNativeContentAndDurableReceipts()
	{
		QFETCH(bool, dirty); QTemporaryDir directory; const auto command = request("cancelled-edit", "song.set", {{"tempo", 123}, {"masterVolume", 90}}); QJsonObject receipt;
		QString pcm;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); seed();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool()); const auto before = manifest(directory);
			QVERIFY(retainedClip()); pcm = retainedClip()->sample().toBase64(); QVERIFY(!pcm.isEmpty()); const QPointer<SampleClip> old(retainedClip());
			auto song = Engine::getSong(); NativeSongTestAccess::setModified(song, false); bool cancelled = false;
			const auto connection = dirty ? connect(song, &Song::modified, this, [&] { cancelled = true; }, Qt::DirectConnection)
				: connect(song, &Song::tempoChanged, this, [&](bpm_t tempo) { if (tempo == 123) { cancelled = true; } }, Qt::DirectConnection);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			{ FileResourceIO::Scope outer([&] { return cancelled; }); receipt = runtime.process(command); }
			QVERIFY(cancelled); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!receipt.contains("result")); QVERIFY(receipt.value("error").toString().contains("cancelled"));
			QVERIFY(!old); QVERIFY(!runtime.stopping()); QCOMPARE(song->getTempo(), bpm_t(91)); QCOMPARE(song->masterVolume(), 100);
			QVERIFY(retainedClip()); QCOMPARE(retainedClip()->sample().toBase64(), pcm); QCOMPARE(retainedClip()->length().getTicks(), 192);
			const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1); replay(runtime, command, receipt); QObject::disconnect(connection);
			QVERIFY(runtime.process(request("revise", "song.set", {{"tempo", 97}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QString error; QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt);
		QCOMPARE(Engine::getSong()->getTempo(), bpm_t(97)); QCOMPARE(Engine::getSong()->masterVolume(), 100); QVERIFY(retainedClip()); QCOMPARE(retainedClip()->sample().toBase64(), pcm);
	}
};
QTEST_GUILESS_MAIN(StudioCapabilityCancellationTest)
#include "StudioCapabilityCancellationTest.moc"
