/* Native asset lookup, repair and preserved history. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NativeSongTestAccess.h"
#include "NotePlayHandle.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject search(QJsonObject args) { return StudioCapabilities::execute("asset.search", args); }
QJsonObject found(QJsonObject args) { const auto response = search(args); if (!response.value("ok").toBool()) { throw std::runtime_error(QJsonDocument(response).toJson().constData()); } return response.value("result").toObject(); }
bool write(const QString& path, const QByteArray& bytes = "candidate") { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray little(quint32 value, int count) { QByteArray out; for (int i = 0; i < count; ++i) { out.append(static_cast<char>(value >> (8 * i))); } return out; }
bool wave(const QString& path) { QByteArray pcm(128, '\0'); pcm[1] = 0x20; return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm); }
QJsonObject manifest(const QTemporaryDir& root) { QFile file(root.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}
class StudioAssetsTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void rootsReflectNativeAliasesWithoutCreatingContent()
	{
		const auto directory = ConfigManager::inst()->workingDir(); const auto before = QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot); const auto response = StudioCapabilities::execute("asset.roots", {}); QVERIFY(response.value("ok").toBool()); const auto roots = response.value("result").toObject().value("roots").toArray(); QCOMPARE(roots.size(), 14); bool user = false, local = false;
		for (const auto& item : roots) { const auto row = item.toObject(); if (row.value("id") == QJsonValue("usersample")) { user = true; QCOMPARE(row.value("path").toString(), QDir::cleanPath(ConfigManager::inst()->userSamplesDir())); } if (row.value("id") == QJsonValue("local")) { local = true; QVERIFY(!row.value("resolved").toBool()); QCOMPARE(row.value("status").toString(), QString("unresolvedBase")); } } QVERIFY(user && local); QCOMPARE(QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
	}
	void aliasesAbsoluteAndPrefixedDirectoriesUseNativeResolution_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("absolute") << 0; QTest::newRow("alias") << 1; QTest::newRow("prefixed") << 2; }
	void aliasesAbsoluteAndPrefixedDirectoriesUseNativeResolution()
	{
		QFETCH(int, kind); QTemporaryDir root; const auto old = ConfigManager::inst()->workingDir(); const auto restore = qScopeGuard([&] { ConfigManager::inst()->setWorkingDir(old); }); ConfigManager::inst()->setWorkingDir(root.filePath("workspace")); const auto path = kind == 0 ? root.path() : ConfigManager::inst()->userSamplesDir() + (kind == 2 ? "sub" : ""); QVERIFY(QDir().mkpath(path)); const auto file = QDir(path).filePath("repair.take.wav"); QVERIFY(write(file)); const auto page = found({{"root", kind == 0 ? path : kind == 1 ? QString("usersample") : QString("usersample:sub")}, {"query", "repair"}, {"extensions", QJsonArray{"wav"}}}); QCOMPARE(page.value("total").toInt(), 1); QCOMPARE(page.value("matches").toArray()[0].toObject().value("path").toString(), file);
	}
	void filenameQueriesUsePhrasesUnicodeAndLiteralTokens_data()
	{ QTest::addColumn<QString>("query"); QTest::addColumn<int>("expected"); QTest::newRow("phrase") << QString("\"Warm Bass\"") << 1; QTest::newRow("tokens") << QString("BASS warm") << 1; QTest::newRow("literal") << QString("[1]") << 1; QTest::newRow("unicode") << QString::fromUtf8("\"W\xc3\xa4rm\" Bass") << 1; }
	void filenameQueriesUsePhrasesUnicodeAndLiteralTokens()
	{
		QFETCH(QString, query); QFETCH(int, expected); QTemporaryDir root; QVERIFY(write(root.filePath("Warm Bass.take.WAV"))); QVERIFY(write(root.filePath("[1].take.wav"))); QVERIFY(write(root.filePath(QString::fromUtf8("W\xc3\xa4rm Bass.take.wav")))); QVERIFY(write(root.filePath("Warm Bass.txt"))); const auto page = found({{"root", root.path()}, {"query", query}, {"extensions", QJsonArray{".WAV"}}}); QCOMPARE(page.value("total").toInt(), expected); QVERIFY(page.value("scanComplete").toBool());
	}
	void emptyQueriesBrowseSelectedKinds_data()
	{ QTest::addColumn<QString>("kind"); QTest::addColumn<int>("count"); QTest::newRow("files") << QString("files") << 1; QTest::newRow("directories") << QString("directories") << 1; QTest::newRow("all") << QString("all") << 2; }
	void emptyQueriesBrowseSelectedKinds()
	{
		QFETCH(QString, kind); QFETCH(int, count); QTemporaryDir root; QVERIFY(write(root.filePath("one.wav"))); QVERIFY(QDir().mkdir(root.filePath("folder"))); const auto page = found({{"root", root.path()}, {"query", ""}, {"kind", kind}, {"recursive", false}}); QCOMPARE(page.value("total").toInt(), count);
	}
	void observationTokensStayStableAcrossPagesAndTextModes()
	{
		QTemporaryDir root; for (int i = 0; i < 7; ++i) { QVERIFY(write(root.filePath(QString::number(i) + ".wav"))); } QString token; int offset = 0; do { QJsonObject args{{"root", root.path()}, {"offset", offset}, {"limit", 2}, {"names", offset % 4 == 0}}; if (!token.isEmpty()) { args.insert("searchGeneration", token); } const auto page = found(args); if (token.isEmpty()) { token = page.value("searchGeneration").toString(); } QCOMPARE(page.value("searchGeneration").toString(), token); QCOMPARE(page.value("total").toInt(), 7); QCOMPARE(page.value("entriesObserved").toInt(), 7); const auto rows = page.value("matches").toArray(); QVERIFY(!rows.isEmpty()); for (const auto& row : rows) { QCOMPARE(row.toObject().value("index").toInt(), offset++); } QCOMPARE(page.value("nextOffset").toInt(), offset); if (!page.value("hasMore").toBool()) { break; } } while (offset < 7); QCOMPARE(offset, 7);
	}
	void entryLimitsDoNotPretendThereAreNoOtherCandidates()
	{
		QTemporaryDir root; for (int i = 0; i < 5; ++i) { QVERIFY(write(root.filePath(QString::number(i) + ".wav"))); } const auto page = found({{"root", root.path()}, {"query", "unmatched"}, {"maximumEntries", 1}}); QCOMPARE(page.value("entriesObserved").toInt(), 1); QCOMPARE(page.value("total").toInt(), 0); QVERIFY(!page.value("hasMore").toBool()); QVERIFY(page.value("entryLimitReached").toBool()); QVERIFY(!page.value("scanComplete").toBool());
	}
	void metadataChangesRefuseOldObservationTokens_data()
	{ QTest::addColumn<bool>("added"); QTest::newRow("new-entry") << true; QTest::newRow("changed-size") << false; }
	void metadataChangesRefuseOldObservationTokens()
	{
		QFETCH(bool, added); QTemporaryDir root; const auto first = root.filePath("one.wav"); QVERIFY(write(first)); const auto page = found({{"root", root.path()}}); if (added) { QVERIFY(write(root.filePath("two.wav"))); } else { QVERIFY(write(first, "different bytes and size")); } QVERIFY(!search({{"root", root.path()}, {"searchGeneration", page.value("searchGeneration")}}).value("ok").toBool()); QVERIFY(search({{"root", root.path()}}).value("ok").toBool());
	}
	void malformedLookupArgumentsAreRefused_data()
	{ QTest::addColumn<QJsonObject>("args"); QTest::newRow("missing-root") << QJsonObject{}; QTest::newRow("unknown-root") << QJsonObject{{"root", "unknown-native-root"}}; QTest::newRow("bad-kind") << QJsonObject{{"root", "usersample"}, {"kind", "everything"}}; QTest::newRow("bad-flag") << QJsonObject{{"root", "usersample"}, {"recursive", "true"}}; QTest::newRow("extension-type") << QJsonObject{{"root", "usersample"}, {"extensions", "wav"}}; QTest::newRow("extension-empty") << QJsonObject{{"root", "usersample"}, {"extensions", QJsonArray{}}}; QTest::newRow("extension-pattern") << QJsonObject{{"root", "usersample"}, {"extensions", QJsonArray{"*.wa?"}}}; QTest::newRow("long-query") << QJsonObject{{"root", "usersample"}, {"query", QString(4097, 'x')}}; QTest::newRow("negative-offset") << QJsonObject{{"root", "usersample"}, {"offset", -1}}; QTest::newRow("zero-limit") << QJsonObject{{"root", "usersample"}, {"limit", 0}}; QTest::newRow("zero-budget") << QJsonObject{{"root", "usersample"}, {"maximumEntries", 0}}; QTest::newRow("huge-budget") << QJsonObject{{"root", "usersample"}, {"maximumEntries", 1000001}}; QTest::newRow("token") << QJsonObject{{"root", "usersample"}, {"searchGeneration", "old"}}; QTest::newRow("unknown") << QJsonObject{{"root", "usersample"}, {"unexpected", true}}; }
	void malformedLookupArgumentsAreRefused() { QFETCH(QJsonObject, args); QVERIFY(!search(args).value("ok").toBool()); }
	void scansStartOutsideModelOwnershipAndLeaveMusicalStateAlone()
	{
		QTemporaryDir root; QVERIFY(write(root.filePath("one.wav"))); NativeSongTestAccess::setModified(Engine::getSong(), false); const auto diagnostics = Engine::getSong()->nativeErrors(); { const auto guard = Engine::audioEngine()->requestChangesGuard(); QVERIFY(!search({{"root", root.path()}}).value("ok").toBool()); } QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks()); QVERIFY(search({{"root", root.path()}}).value("ok").toBool()); QVERIFY(!Engine::getSong()->isModified()); QCOMPARE(Engine::getSong()->nativeErrors(), diagnostics);
	}
	void nativeProjectAndBaseChangesCannotPublishMixedSearches_data()
	{ QTest::addColumn<bool>("project"); QTest::newRow("project-replaced") << true; QTest::newRow("base-path-changed") << false; }
	void nativeProjectAndBaseChangesCannotPublishMixedSearches()
	{
		QFETCH(bool, project); QTemporaryDir root; for (int i = 0; i < 32; ++i) { QVERIFY(write(root.filePath(QString::number(i) + ".wav"))); } const auto old = ConfigManager::inst()->sf2Dir(); const auto restore = qScopeGuard([&] { ConfigManager::inst()->setSF2Dir(old); }); bool changed = false; int checks = 0;
		const auto response = StudioCapabilities::execute("asset.search", {{"root", root.path()}}, [&] { if (++checks == 20) { changed = true; if (project) { Engine::getSong()->createEmptyProject(); } else { ConfigManager::inst()->setSF2Dir(root.filePath("another-base")); } } return false; }); QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); QVERIFY(search({{"root", root.path()}}).value("ok").toBool());
	}
	void cancellationAndExceptionsReleaseSearchState_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndExceptionsReleaseSearchState()
	{
		QFETCH(int, kind); QTemporaryDir root; for (int i = 0; i < 16; ++i) { QVERIFY(write(root.filePath(QString::number(i) + ".wav"))); } int checks = 0; const auto response = StudioCapabilities::execute("asset.search", {{"root", root.path()}}, [&] { if (++checks < 12) { return false; } if (kind == 1) { throw std::runtime_error("Fixture lookup failure"); } if (kind == 2) { throw 89; } return true; }); QVERIFY(checks >= 12); QVERIFY(!response.value("ok").toBool()); QVERIFY(search({{"root", root.path()}}).value("ok").toBool()); QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks());
	}
	void pendingQtWorkStaysOutsideTheFilesystemWalk()
	{
		QTemporaryDir root; QVERIFY(write(root.filePath("one.wav"))); bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(search({{"root", root.path()}}).value("ok").toBool()); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void unavailableDirectoriesAreNeverCreatedByLookup()
	{
		QTemporaryDir root; const auto missing = root.filePath("not-created"); QVERIFY(!search({{"root", missing}}).value("ok").toBool()); QVERIFY(!QFileInfo::exists(missing)); const auto file = root.filePath("file.wav"); QVERIFY(write(file)); QVERIFY(!search({{"root", file}}).value("ok").toBool());
	}
	void oversizedConfiguredRootsHaveAReadOnlyPreviewRoute()
	{
		const auto old = ConfigManager::inst()->sf2Dir(); const auto restore = qScopeGuard([&] { ConfigManager::inst()->setSF2Dir(old); }); ConfigManager::inst()->setSF2Dir(QString(600000, 'x')); QVERIFY(!StudioCapabilities::execute("asset.roots", {}).value("ok").toBool()); const auto response = StudioCapabilities::execute("asset.roots", {{"names", false}}); QVERIFY(response.value("ok").toBool()); bool found = false; for (const auto& value : response.value("result").toObject().value("roots").toArray()) { const auto row = value.toObject(); if (row.value("id") == QJsonValue("usersoundfont")) { found = true; QCOMPARE(row.value("pathPreview").toString().size(), 256); QVERIFY(row.value("pathTruncated").toBool()); } } QVERIFY(found);
	}
	void lookupReceiptsAreHistoricalAndDoNotAppendProjectHistory()
	{
		QTemporaryDir root, session; const auto file = root.filePath("one.wav"); QVERIFY(write(file)); QString error; StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); const auto before = manifest(session); const auto query = request("lookup", "asset.search", {{"root", root.path()}}); const auto first = runtime.process(query); QVERIFY(first.value("ok").toBool()); QCOMPARE(first.value("result").toObject().value("total").toInt(), 1); QVERIFY(QFile::remove(file)); auto replay = runtime.process(query); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, first); const auto fresh = runtime.process(request("fresh", "asset.search", {{"root", root.path()}})); QVERIFY(fresh.value("ok").toBool()); QCOMPARE(fresh.value("result").toObject().value("total").toInt(), 0); QCOMPARE(manifest(session).value("history"), before.value("history")); QCOMPARE(manifest(session).value("revision"), before.value("revision"));
	}
	void scriptedAgentsDiscoverFindRepairAndPreserveMissingSamples_data()
	{ QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("disappears"); QTest::newRow("chat") << QString("chat") << false; QTest::newRow("responses") << QString("responses") << false; QTest::newRow("chat-disappearing-candidate") << QString("chat") << true; QTest::newRow("responses-disappearing-candidate") << QString("responses") << true; }
	void scriptedAgentsDiscoverFindRepairAndPreserveMissingSamples()
	{
		QFETCH(QString, protocol); QFETCH(bool, disappears); QTemporaryDir source, session; const auto library = ConfigManager::inst()->userSamplesDir(); QVERIFY(QDir().mkpath(library)); const auto candidate = QDir(library).filePath("repair.take.wav"); QVERIFY(wave(candidate)); const auto remove = qScopeGuard([&] { QFile::remove(candidate); }); const auto lost = source.filePath("lost/repair.take.wav"), project = source.filePath("missing.mmp"); auto clip = static_cast<SampleClip*>(Track::create(Track::Type::Sample, Engine::getSong())->createClip(0)); QDomDocument settings; auto element = settings.createElement("sampleclip"); settings.appendChild(element); element.setAttribute("src", lost); element.setAttribute("unresolved", 1); element.setAttribute("len", 311); element.setAttribute("pos", 192); element.setAttribute("off", -13); element.setAttribute("autoresize", 0); element.setAttribute("sample_start", 17); element.setAttribute("sample_end", 44); element.setAttribute("sample_rate", 44100); element.setAttribute("sample_gain", .375); clip->loadSettings(element); QVERIFY(Engine::getSong()->saveProjectFile(project));
		QString error, rootAlias, selected; QJsonObject target; int round = 0; bool failedRepair = false, lookupObserved = false; QJsonArray evidence;
		{
			StudioRuntime runtime(session.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
				QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } }
				const auto call = [&](const QString& id, const QString& name, const QJsonObject& args) { if (!(disappears && id == "repair")) { evidence.append(id); } return decision(protocol, id, name, args); }; ++round;
				if (round == 1) { return call("resources", "project_inspect", {{"section", "resources"}}); }
				if (round == 2) { if (!output.value("ok").toBool()) { throw std::runtime_error("Dependency discovery failed"); } const auto row = output.value("result").toObject().value("resources").toArray()[0].toObject(); if (!row.value("unresolved").toBool()) { throw std::runtime_error("Expected a retained missing source"); } const auto owner = row.value("owner").toObject(); target = {{"container", owner.value("container")}, {"track", owner.value("track")}, {"clip", owner.value("clip")}}; return call("roots", "asset_roots", {}); }
				if (round == 3) { for (const auto& value : output.value("result").toObject().value("roots").toArray()) { const auto row = value.toObject(); if (row.value("id") == QJsonValue("usersample")) { rootAlias = row.value("id").toString(); } } if (rootAlias.isEmpty()) { throw std::runtime_error("Native sample root was not discovered"); } return call("find", "asset_search", {{"root", rootAlias}, {"query", "\"repair.take\""}, {"extensions", QJsonArray{"wav"}}, {"recursive", false}}); }
				if (round == 4 || (disappears && round == 7)) { if (!output.value("ok").toBool()) { throw std::runtime_error("Native asset lookup failed"); } const auto page = output.value("result").toObject(); if (!page.value("scanComplete").toBool() || page.value("total").toInt() != 1) { throw std::runtime_error("Expected one candidate in the scoped native lookup"); } lookupObserved = true; selected = page.value("matches").toArray()[0].toObject().value("path").toString(); if (selected.isEmpty()) { throw std::runtime_error("Candidate path was not returned"); } if (disappears && round == 4 && !QFile::remove(selected)) { throw std::runtime_error("Could not retire fixture candidate"); } auto args = target; args.insert("path", selected); return call(round == 4 ? "repair" : "retry-repair", "sample_load", args); }
				if (disappears && round == 5) { if (output.value("ok").toBool()) { throw std::runtime_error("A removed candidate was accepted"); } failedRepair = true; return call("rediscover", "project_inspect", {{"section", "resources"}}); }
				if (disappears && round == 6) { const auto row = output.value("result").toObject().value("resources").toArray()[0].toObject(); if (!row.value("unresolved").toBool() || !wave(candidate)) { throw std::runtime_error("Native rollback did not retain missing state"); } const auto owner = row.value("owner").toObject(); target = {{"container", owner.value("container")}, {"track", owner.value("track")}, {"clip", owner.value("clip")}}; return call("find-again", "asset_search", {{"root", rootAlias}, {"query", "\"repair.take\""}, {"extensions", QJsonArray{"wav"}}, {"recursive", false}}); }
				if (round == (disappears ? 8 : 5)) { if (!output.value("ok").toBool()) { throw std::runtime_error("Checked repair failed"); } return call("verify", "project_inspect", {{"section", "resources"}}); }
				if (!output.value("ok").toBool() || output.value("result").toObject().value("declaredUnresolvedCount").toInt(-1) != 0) { throw std::runtime_error("Repaired native resource was not observed"); }
				return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Located and repaired the native sample reference; audio evaluation was not performed."}, {"evidence", evidence}});
			});
			QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}, {"summary", true}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-asset-repair-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Find repair.take.wav in the native sample library and repair the missing source, preserving its arrangement."}})).value("ok").toBool()); for (int i = 0; i < 64; ++i) { runtime.advanceAgent(); const auto status = manifest(session).value("agent").toObject().value("job").toObject().value("status").toString(); if (status == "complete" || status == "failed" || status == "paused" || status == "cancelled") { break; } } QVERIFY(lookupObserved); QCOMPARE(failedRepair, disappears); const auto completedJob = manifest(session).value("agent").toObject().value("job").toObject(); QVERIFY2(completedJob.value("status") == QJsonValue("complete"), qPrintable(completedJob.value("lastError").toString())); QVERIFY(!runtime.stopping()); auto current = static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0)); QVERIFY(!current->sampleUnresolved()); QCOMPARE(current->sample().startFrame(), 17); QCOMPARE(current->sample().endFrame(), 44); QCOMPARE(current->sample().amplification(), .375f); QCOMPARE(current->startPosition().getTicks(), 192); QCOMPARE(current->length().getTicks(), 311); QCOMPARE(current->startTimeOffset().getTicks(), -13); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0))->sampleUnresolved()); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		QVERIFY(QFile::remove(candidate)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); auto current = static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0)); QVERIFY(!current->sampleUnresolved()); QCOMPARE(current->sample().sampleSize(), std::size_t(64)); QCOMPARE(current->sample().startFrame(), 17); QCOMPARE(current->sample().endFrame(), 44); QCOMPARE(current->length().getTicks(), 311);
	}
};
QTEST_GUILESS_MAIN(StudioAssetsTest)
#include "StudioAssetsTest.moc"
