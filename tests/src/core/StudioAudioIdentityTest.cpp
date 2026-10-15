/* Audio evaluation identity across native rendering and revision. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {}) { return {{"id", id}, {"name", name}, {"args", args}}; }
QByteArray bytes(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QString hash(const QByteArray& value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); }
QJsonObject manifest(const QString& directory) { return QJsonDocument::fromJson(read(QDir(directory).filePath("session.json"))).object(); }
QJsonObject job(const QString& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
bool writeWave(const QString& path, float value = 0.25f, int rate = 8000, int frames = 24000)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
	SF_INFO info{}; info.samplerate = rate; info.channels = 1; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!audio) { return false; }
	const std::vector<float> samples(frames, value);
	const auto written = sf_writef_float(audio, samples.data(), frames); const auto closed = sf_close(audio);
	return written == frames && closed == 0 && file.flush();
}
bool replace(const QString& path, const QByteArray& data)
{ QSaveFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit(); }
bool arrange(const QString& path)
{
	return StudioCapabilities::execute("track.add", {{"type", "sample"}, {"name", "Native audio fixture"}}).value("ok").toBool()
		&& StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool()
		&& StudioCapabilities::execute("sample.load", {{"track", 0}, {"clip", 0}, {"path", path}}).value("ok").toBool();
}
QJsonObject provider(const QString& role, const QString& protocol = "chat")
{ return {{"role", role}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", role + "-audio-identity-fixture"}}}}; }
QJsonObject judgment()
{ return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{{"role", "assistant"}, {"content", "Scripted fixture: evaluate the level and revise the gain. This is not musical listening evidence."}}}}}}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto arguments = QString::fromUtf8(bytes(item.value("args").toObject()));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", arguments}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", arguments}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject outputFor(const QJsonObject& body, const QString& protocol, const QString& id)
{
	for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
	{
		const auto item = value.toObject();
		if (item.value(protocol == "chat" ? "tool_call_id" : "call_id").toString() == id
			&& (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output"))
		{ return QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
	}
	return {};
}
void evaluators() { QTest::addColumn<bool>("listen"); QTest::newRow("analysis") << false; QTest::newRow("excerpt") << true; }
}

class StudioAudioIdentityTest : public QObject
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
	void cleanup() { Engine::getSong()->clearProject(); }

	void publishedNativeAudioBindsMeasurementAndExcerpt_data()
	{ QTest::addColumn<int>("depth"); for (int depth : {16, 24, 32}) { QTest::newRow(qPrintable(QString::number(depth))) << depth; } }
	void publishedNativeAudioBindsMeasurementAndExcerpt()
	{
		QFETCH(int, depth); QTemporaryDir directory; const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav");
		QVERIFY(writeWave(source, 0.25f, 44100, 44100)); QVERIFY(arrange(source));
		const auto render = StudioCapabilities::execute("audio.render", {{"path", path}, {"bitDepth", depth}, {"timeoutMs", 30000}});
		QVERIFY2(render.value("ok").toBool(), qPrintable(render.value("error").toString())); const auto result = render.value("result").toObject(); const auto encoded = read(path);
		QVERIFY(!encoded.isEmpty()); QCOMPARE(result.value("sha256").toString(), hash(encoded)); QCOMPARE(result.value("bytes").toDouble(), double(encoded.size()));
		const auto response = StudioCapabilities::execute("audio.analyze", {{"path", path}, {"sourceSha256", result.value("sha256")}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto measured = response.value("result").toObject();
		QCOMPARE(measured.value("sourceSha256"), result.value("sha256")); QCOMPARE(measured.value("sourceBytes"), result.value("bytes"));
		QCOMPARE(measured.value("sampleRate").toInt(), 44100); QVERIFY(!measured.value("perceptualListening").toBool());
		QVERIFY(!measured.value("channels").toArray().isEmpty()); QVERIFY(measured.value("channels").toArray().first().toObject().value("peak").toDouble() > 0.01);
		const auto excerpt = StudioAudio::excerpt({{"path", path}, {"sourceSha256", result.value("sha256")}, {"durationSeconds", 0.5}});
		QCOMPARE(excerpt.observation.value("sourceSha256"), result.value("sha256")); QCOMPARE(excerpt.observation.value("sourceBytes"), result.value("bytes"));
		QCOMPARE(excerpt.observation.value("excerptSha256").toString(), hash(excerpt.wave)); QCOMPARE(read(path), encoded);
	}

	void acceptedAudioSurvivesOriginalMutationDuringDecode_data()
	{
		QTest::addColumn<bool>("listen"); QTest::addColumn<QString>("mutation");
		for (bool listen : {false, true}) for (const auto mutation : {QString("overwrite"), QString("replace"), QString("remove")})
		{ QTest::newRow(qPrintable(QString::number(listen) + mutation)) << listen << mutation; }
	}
	void acceptedAudioSurvivesOriginalMutationDuringDecode()
	{
		QFETCH(bool, listen); QFETCH(QString, mutation); QTemporaryDir directory; const auto path = directory.filePath("source.wav"), other = directory.filePath("other.wav");
		QVERIFY(writeWave(path)); QVERIFY(writeWave(other, 0.75f)); const auto original = read(path), replacement = read(other); QCOMPARE(original.size(), replacement.size());
		const QJsonObject args{{"path", path}, {"sourceSha256", hash(original)}, {"durationSeconds", 3.0}};
		QJsonObject analysisArgs = args; analysisArgs.remove("durationSeconds");
		const auto baseline = listen ? StudioAudio::excerpt(args) : StudioAudio::Excerpt{{}, StudioAudio::analyze(analysisArgs)};
		bool captureSeen = false, changed = false, changedSuccessfully = false; int checksSinceFileWork = 0;
		// File work resets the marker. Two consecutive decoding callbacks locate a
		// point after acceptance without relying on a file size or fixed check count.
		FileResourceIO::Scope capture([&] { captureSeen = true; checksSinceFileWork = 0; return false; });
		const auto cancelled = [&] {
			if (captureSeen && ++checksSinceFileWork >= 2 && !changed)
			{
				changed = true;
				if (mutation == "remove") { changedSuccessfully = QFile::remove(path); }
				else if (mutation == "replace") { changedSuccessfully = replace(path, replacement); }
				else { QFile file(path); changedSuccessfully = file.open(QIODevice::ReadWrite) && file.write(replacement) == replacement.size() && file.flush(); }
			}
			return false;
		};
		const auto actual = listen ? StudioAudio::excerpt(args, cancelled) : StudioAudio::Excerpt{{}, StudioAudio::analyze(analysisArgs, cancelled)};
		QVERIFY(captureSeen && changed && changedSuccessfully); QCOMPARE(actual.observation, baseline.observation); QCOMPARE(actual.wave, baseline.wave);
		QCOMPARE(actual.observation.value("sourceSha256").toString(), hash(original)); QCOMPARE(actual.observation.value("sourceBytes").toDouble(), double(original.size()));
		if (mutation == "remove") { QVERIFY(!QFile::exists(path)); }
		else { QCOMPARE(read(path), replacement); QVERIFY(hash(replacement) != actual.observation.value("sourceSha256").toString()); }
	}

	void invalidDigestsAndDurationBudgetsFailExplicitly_data() { evaluators(); }
	void invalidDigestsAndDurationBudgetsFailExplicitly()
	{
		QFETCH(bool, listen); QTemporaryDir directory; const auto path = directory.filePath("source.wav"); QVERIFY(writeWave(path)); const auto original = read(path); const auto digest = hash(original);
		int fileChecks = 0; FileResourceIO::Scope capture([&] { ++fileChecks; return false; });
		const QJsonArray invalid{QJsonValue::Null, false, 2, QJsonObject{}, QJsonArray{}, "", QString(63, 'a'), QString(65, 'a'), QString(64, 'A'), QString(64, 'z'), digest + "\n"};
		for (const auto& value : invalid)
		{
			const QJsonObject args{{"path", path}, {"sourceSha256", value}}; const int before = fileChecks;
			if (listen) { QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt(args), std::invalid_argument); }
			else { QVERIFY_EXCEPTION_THROWN(StudioAudio::analyze(args), std::invalid_argument); }
			QCOMPARE(fileChecks, before);
			// Dispatch has its own cooperative admission polls. Verify the wire
			// error separately from the direct decoder's no-I/O validation check.
			if (!listen) { const auto response = StudioCapabilities::execute("audio.analyze", args); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("sourceSha256")); }
		}
		const QJsonObject mismatch{{"path", path}, {"sourceSha256", QString(64, '0')}};
		if (listen) { QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt(mismatch), std::runtime_error); }
		else { const auto response = StudioCapabilities::execute("audio.analyze", mismatch); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("differs from sourceSha256")); }
		QVERIFY(fileChecks > 0);
		if (listen)
		{
			QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt({{"path", path}, {"durationSeconds", 121}}), std::invalid_argument);
			QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt({{"path", path}, {"offsetSeconds", 4}}), std::invalid_argument);
		}
		else
		{
			const int before = fileChecks;
			QVERIFY_EXCEPTION_THROWN(StudioAudio::analyze({{"path", path}, {"maximumSeconds", 1.5}}), std::invalid_argument); QCOMPARE(fileChecks, before);
			QVERIFY_EXCEPTION_THROWN(StudioAudio::analyze({{"path", path}, {"maximumSeconds", 1}}), std::invalid_argument); QCOMPARE(fileChecks, before);
			bool changed = false, replacementWritten = false;
			{
				FileResourceIO::Scope replaceAfterPreflight([&] {
					if (!changed) { changed = true; replacementWritten = writeWave(path, 0.25f, 8000, 40000); } return false;
				});
				QVERIFY_EXCEPTION_THROWN(StudioAudio::analyze({{"path", path}, {"maximumSeconds", 4}}), std::invalid_argument);
			}
			QVERIFY(changed && replacementWritten); QVERIFY(fileChecks > before); QVERIFY(replace(path, original));
		}
		QCOMPARE(hash(read(path)), digest);
	}

	void captureAndDecodeCancellationLeaveSourceUsable_data()
	{
		QTest::addColumn<bool>("listen"); QTest::addColumn<bool>("duringCapture");
		for (bool listen : {false, true}) for (bool capture : {false, true})
		{ QTest::newRow(qPrintable(QString::number(listen) + QString::number(capture))) << listen << capture; }
	}
	void captureAndDecodeCancellationLeaveSourceUsable()
	{
		QFETCH(bool, listen); QFETCH(bool, duringCapture); QTemporaryDir directory; const auto path = directory.filePath("source.wav");
		QVERIFY(writeWave(path)); const auto original = read(path); const QJsonObject args{{"path", path}, {"sourceSha256", hash(original)}};
		int fileChecks = 0, checksSinceFileWork = 0; bool stopped = false;
		{
			FileResourceIO::Scope capture([&] { checksSinceFileWork = 0; ++fileChecks; return stopped = duringCapture && fileChecks >= 4; });
			const auto cancelled = [&] { return stopped = !duringCapture && fileChecks > 0 && ++checksSinceFileWork >= 2; };
			if (listen) { QVERIFY_EXCEPTION_THROWN(StudioAudio::excerpt(args, cancelled), std::runtime_error); }
			else { QVERIFY_EXCEPTION_THROWN(StudioAudio::analyze(args, cancelled), std::runtime_error); }
		}
		QVERIFY(stopped); QVERIFY(fileChecks > 0); QCOMPARE(read(path), original);
		if (listen) { QCOMPARE(StudioAudio::excerpt(args).observation.value("sourceSha256").toString(), hash(original)); }
		else { QCOMPARE(StudioAudio::analyze(args).value("sourceSha256").toString(), hash(original)); }
	}

	void receiptsRemainHistoricalAndFreshRequestsCheckChangedAudio_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-process") << false; QTest::newRow("reopen") << true; }
	void receiptsRemainHistoricalAndFreshRequestsCheckChangedAudio()
	{
		QFETCH(bool, restart); QTemporaryDir directory; const auto session = directory.filePath("session"), path = directory.filePath("source.wav");
		QVERIFY(writeWave(path)); const auto original = read(path); const auto digest = hash(original); int submissions = 0; bool waveformMatches = false;
		const auto expected = StudioAudio::excerpt({{"path", path}, {"sourceSha256", digest}});
		const auto transport = [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			++submissions; const auto content = body.value("messages").toArray().last().toObject().value("content").toArray();
			waveformMatches = content.size() == 2 && content.at(0).toObject().value("text").toString().contains(digest)
				&& QByteArray::fromBase64(content.at(1).toObject().value("input_audio").toObject().value("data").toString().toLatin1()) == expected.wave;
			return judgment();
		};
		auto studio = std::make_unique<StudioRuntime>(session, transport); QString error; QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("provider", "session.provider.set", provider("audio"))).value("ok").toBool());
		const auto measureRequest = request("measure", "audio.analyze", {{"path", path}, {"sourceSha256", digest}});
		const auto listenRequest = request("listen", "audio.listen", {{"path", path}, {"sourceSha256", digest}, {"prompt", "Evaluate the accepted audio"}});
		const auto measured = studio->process(measureRequest), heard = studio->process(listenRequest);
		QVERIFY(measured.value("ok").toBool()); QVERIFY(heard.value("ok").toBool()); QVERIFY(waveformMatches); QCOMPARE(submissions, 1);
		if (restart) { studio.reset(); studio = std::make_unique<StudioRuntime>(session, transport); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto before = manifest(session); QVERIFY(writeWave(path, 0.75f)); QVERIFY(hash(read(path)) != digest);
		for (const auto& previous : {measureRequest, listenRequest})
		{
			auto fresh = previous; fresh.insert("id", "fresh-" + previous.value("id").toString()); const auto response = studio->process(fresh);
			QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("differs from sourceSha256")); QVERIFY(!response.contains("result"));
		}
		QCOMPARE(submissions, 1); QVERIFY(QFile::remove(path));
		auto measuredAgain = studio->process(measureRequest), heardAgain = studio->process(listenRequest);
		QVERIFY(measuredAgain.take("replayed").toBool()); QVERIFY(heardAgain.take("replayed").toBool()); QCOMPARE(measuredAgain, measured); QCOMPARE(heardAgain, heard); QCOMPARE(submissions, 1);
		QVERIFY(!QFile::exists(path)); const auto after = manifest(session); QCOMPARE(after.value("revision"), before.value("revision")); QCOMPARE(after.value("history"), before.value("history"));
	}

	void agentRendersEvaluatesRevisesAndPreservesNativeWork_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("restart");
		for (const auto protocol : {QString("chat"), QString("responses")}) for (bool restart : {false, true})
		{ QTest::newRow(qPrintable(protocol + QString::number(restart))) << protocol << restart; }
	}
	void agentRendersEvaluatesRevisesAndPreservesNativeWork()
	{
		QFETCH(QString, protocol); QFETCH(bool, restart); QTemporaryDir directory; const auto session = directory.filePath("session");
		const auto source = directory.filePath("source.wav"), path = directory.filePath("render.wav"), project = directory.filePath("editable.mmp");
		QVERIFY(writeWave(source, 0.25f, 44100, 44100)); int round = 0, submissions = 0; bool receivedIdentity = true, evaluatedBoth = false, reopened = false;
		QString firstDigest, secondDigest; QJsonObject firstMeasurement, secondMeasurement;
		const auto transport = [&](const QJsonObject& configuration, const QJsonObject& body, const StudioInference::Cancelled&) {
			if (configuration.value("model").toString() == "audio-audio-identity-fixture")
			{
				++submissions; const auto digest = submissions == 1 ? firstDigest : secondDigest;
				const auto content = body.value("messages").toArray().last().toObject().value("content").toArray();
				const auto expected = StudioAudio::excerpt({{"path", path}, {"sourceSha256", digest}, {"durationSeconds", 0.5}});
				receivedIdentity = receivedIdentity && content.size() == 2 && content.at(0).toObject().value("text").toString().contains(digest)
					&& QByteArray::fromBase64(content.at(1).toObject().value("input_audio").toObject().value("data").toString().toLatin1()) == expected.wave;
				return judgment();
			}
			++round;
			if (round == 1) { return decision(protocol, {
				call("plan", "agent_plan_set", {{"goal", "Arrange the supplied audio, evaluate its level and preserve a quieter revision"}, {"steps", QJsonArray{"Arrange", "Render and evaluate", "Reduce gain", "Render and evaluate again", "Save"}}, {"decisions", QJsonArray{}}}),
				call("tempo", "song_set", {{"tempo", 120}}), call("track", "track_add", {{"type", "sample"}}), call("clip", "clip_add", {{"track", 0}, {"position", 0}}),
				call("load", "sample_load", {{"track", 0}, {"clip", 0}, {"path", source}})}); }
			if (round == 2 || round == 5) { return decision(protocol, {call(round == 2 ? "render-a" : "render-b", "audio_render", {{"path", path}, {"bitDepth", 24}, {"timeoutMs", 30000}})}); }
			if (round == 3 || round == 6)
			{
				const auto suffix = round == 3 ? QString("a") : QString("b"); const auto result = outputFor(body, protocol, "render-" + suffix).value("result").toObject();
				auto& digest = round == 3 ? firstDigest : secondDigest; digest = result.value("sha256").toString();
				QJsonArray calls{call("measure-" + suffix, "audio_analyze", {{"path", path}, {"sourceSha256", digest}}),
					call("listen-" + suffix, "audio_listen", {{"path", path}, {"sourceSha256", digest}, {"durationSeconds", 0.5}, {"prompt", "Evaluate the level of this exact render"}})};
				if (round == 6) { calls.append(call("preserve", "project_save", {{"path", project}})); } return decision(protocol, calls);
			}
			if (round == 4)
			{
				firstMeasurement = outputFor(body, protocol, "measure-a").value("result").toObject();
				receivedIdentity = receivedIdentity && outputFor(body, protocol, "listen-a").value("result").toObject().value("excerpt").toObject().value("sourceSha256").toString() == firstDigest;
				return decision(protocol, {call("revise", "sample_edit", {{"track", 0}, {"clip", 0}, {"gain", 0.5}})});
			}
			secondMeasurement = outputFor(body, protocol, "measure-b").value("result").toObject();
			evaluatedBoth = round == 7 && outputFor(body, protocol, "listen-b").value("result").toObject().value("excerpt").toObject().value("sourceSha256").toString() == secondDigest
				&& outputFor(body, protocol, "preserve").value("ok").toBool();
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Preserved the measured quieter native revision; provider judgments are scripted test fixtures"},
				{"evidence", QJsonArray{"render-a", "measure-a", "listen-a", "revise", "render-b", "measure-b", "listen-b", "preserve"}}})});
		};
		auto studio = std::make_unique<StudioRuntime>(session, transport); QString error; QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("reasoning", "session.provider.set", provider("reasoning", protocol))).value("ok").toBool());
		QVERIFY(studio->process(request("audio", "session.provider.set", provider("audio"))).value("ok").toBool());
		QVERIFY(studio->process(request("chat", "session.chat", {{"text", "Arrange the supplied source, evaluate it, reduce the level by half, evaluate again, and save the editable project."}})).value("ok").toBool());
		for (int step = 0; step < 40 && StudioAgent::active(job(session)); ++step)
		{
			studio->advanceAgent();
			if (restart && !reopened && job(session).value("operations").toInt() == 6)
			{ studio.reset(); studio = std::make_unique<StudioRuntime>(session, transport); QVERIFY2(studio->open(error), qPrintable(error)); reopened = true; }
		}
		QCOMPARE(job(session).value("status").toString(), QString("complete")); QCOMPARE(round, 7); QCOMPARE(submissions, 2); QCOMPARE(reopened, restart); QVERIFY(receivedIdentity && evaluatedBoth);
		QCOMPARE(firstDigest.size(), 64); QCOMPARE(secondDigest.size(), 64); QVERIFY(firstDigest != secondDigest); QCOMPARE(hash(read(path)), secondDigest);
		QCOMPARE(firstMeasurement.value("sourceSha256").toString(), firstDigest); QCOMPARE(secondMeasurement.value("sourceSha256").toString(), secondDigest);
		QVERIFY(!firstMeasurement.value("channels").toArray().isEmpty()); QVERIFY(!secondMeasurement.value("channels").toArray().isEmpty());
		const auto firstRms = firstMeasurement.value("channels").toArray().first().toObject().value("rms").toDouble();
		const auto secondRms = secondMeasurement.value("channels").toArray().first().toObject().value("rms").toDouble();
		QVERIFY(firstRms > 0.01); QVERIFY(secondRms > firstRms * 0.45 && secondRms < firstRms * 0.55); QVERIFY(QFile::exists(project));
		const auto mismatch = studio->process(request("stale-render", "audio.analyze", {{"path", path}, {"sourceSha256", firstDigest}})); QVERIFY(!mismatch.value("ok").toBool());
		studio.reset(); StudioRuntime recovered(session, transport); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); const auto clip = dynamic_cast<SampleClip*>(Engine::getSong()->tracks().front()->getClip(0)); QVERIFY(clip);
		QCOMPARE(clip->sample().amplification(), 0.5f); QCOMPARE(job(session).value("status").toString(), QString("complete")); QCOMPARE(submissions, 2);
	}
};

QTEST_GUILESS_MAIN(StudioAudioIdentityTest)
#include "StudioAudioIdentityTest.moc"
