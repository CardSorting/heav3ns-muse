/* Provider protocol and real loopback transport tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QHostAddress>
#include <QJsonDocument>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <stdexcept>

#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioInference.h"

using namespace lmms;

namespace
{

QJsonObject configuration(const QString& protocol = "chat")
{
	return StudioInference::configuration({{"protocol", protocol},
		{"endpoint", "http://127.0.0.1:12345/test"}, {"model", "test-fixture"}, {"timeoutMs", 1000}});
}

QJsonObject chatText(const QString& text)
{
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"},
		{"message", QJsonObject{{"role", "assistant"}, {"content", text}}}}}}};
}

QJsonObject chatCall(const QJsonArray& calls)
{
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"},
		{"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}}}}}}};
}

QJsonObject tool(const QString& id, const QString& arguments)
{
	return {{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", "song_set"}, {"arguments", arguments}}}};
}

// Only test instrumentation: the production transport remains QNetworkAccessManager.
struct Loopback
{
	QTcpServer server;
	QJsonObject request;
	QByteArray response = QJsonDocument(chatText("fixture response")).toJson(QJsonDocument::Compact);
	QByteArray status = "200 OK";
	bool hang = false;
	int received = 0;
	int disconnected = 0;

	Loopback()
	{
		QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
			while (auto socket = server.nextPendingConnection())
			{
				QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				QObject::connect(socket, &QTcpSocket::disconnected, &server, [&] { ++disconnected; });
				QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
					if (socket->property("answered").toBool()) { socket->readAll(); return; }
					auto bytes = socket->property("request").toByteArray() + socket->readAll();
					socket->setProperty("request", bytes);
					const int headers = bytes.indexOf("\r\n\r\n");
					if (headers < 0) { return; }
					int length = -1;
					for (const auto& line : bytes.left(headers).split('\n'))
					{
						if (line.toLower().startsWith("content-length:")) { length = line.mid(15).trimmed().toInt(); }
					}
					if (length < 0 || bytes.size() < headers + 4 + length) { return; }
					socket->setProperty("answered", true);
					request = QJsonDocument::fromJson(bytes.mid(headers + 4, length)).object();
					++received;
					if (hang) { return; }
					socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
						+ QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
					socket->disconnectFromHost();
				});
			}
		});
	}

	~Loopback()
	{
		// Disconnect member-capturing callbacks before the fixture's counters
		// and payloads retire, including when an assertion exits a test early.
		QObject::disconnect(&server, nullptr, &server, nullptr);
		for (auto socket : server.findChildren<QTcpSocket*>()) { QObject::disconnect(socket, nullptr, &server, nullptr); socket->abort(); }
		server.close();
	}

	QJsonObject config() const
	{
		auto result = configuration();
		result.insert("endpoint", QString("http://127.0.0.1:%1/test").arg(server.serverPort()));
		return result;
	}
};

} // namespace

class StudioInferenceTest : public QObject
{
	Q_OBJECT
private slots:
	void explicitConfigurationAndSecretReferences()
	{
		QVERIFY_EXCEPTION_THROWN(StudioInference::configuration({}), std::runtime_error);
		auto config = configuration();
		config.insert("apiKey", "fixture-secret");
		QVERIFY_EXCEPTION_THROWN(StudioInference::configuration(config), std::runtime_error);
		config.remove("apiKey");
		config.insert("keyEnv", "LMMS_TEST_CREDENTIAL");
		QCOMPARE(StudioInference::configuration(config).value("keyEnv").toString(), QString("LMMS_TEST_CREDENTIAL"));
		config.insert("endpoint", "http://example.com/chat");
		QVERIFY_EXCEPTION_THROWN(StudioInference::configuration(config), std::runtime_error);
		config.insert("endpoint", "https://example.com/chat?key=fixture-secret");
		QVERIFY_EXCEPTION_THROWN(StudioInference::configuration(config), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(StudioInference::configuration(configuration("responses"), true), std::runtime_error);
	}

	void toolDeclarationsAndRequestShapes()
	{
		const auto chat = StudioAgent::tools("chat");
		const auto responses = StudioAgent::tools("responses");
		QCOMPARE(chat.size(), std::min<int>(StudioAgent::schemas().size() + 1, StudioAgent::ToolDeclarationLimit));
		QCOMPARE(responses.size(), chat.size());
		QSet<QString> declared;
		for (int i = 0; i < chat.size(); ++i)
		{
			const auto function = chat.at(i).toObject().value("function").toObject();
			QVERIFY(!function.value("strict").toBool(true));
			const auto name = function.value("name").toString();
			QVERIFY(!declared.contains(name)); declared.insert(name);
			QVERIFY(name == "studio_execute" || !StudioAgent::operation(name).isEmpty());
			QCOMPARE(function.value("parameters"), responses.at(i).toObject().value("parameters"));
			if (name == "audio_analyze" || name == "audio_listen")
			{
				const auto parameters = function.value("parameters").toObject();
				const auto digest = parameters.value("properties").toObject().value("sourceSha256").toObject();
				QCOMPARE(digest.value("type").toString(), QString("string")); QCOMPARE(digest.value("minLength").toInt(), 64); QCOMPARE(digest.value("maxLength").toInt(), 64);
				QVERIFY(!parameters.value("required").toArray().contains("sourceSha256"));
			}
		}
		QVERIFY(declared.contains("studio_execute")); QVERIFY(declared.contains("studio_capabilities"));
		QVERIFY(declared.contains("audio_render")); QVERIFY(declared.contains("audio_record")); QVERIFY(declared.contains("audio_analyze"));
		const QJsonObject inner{{"native", QJsonArray{1, QJsonObject{{"preserved", true}}}}};
		for (const auto& value : StudioCapabilities::schema())
		{
			const auto op = value.toObject().value("name").toString();
			QJsonObject args{{"op", op}, {"args", inner}};
			QCOMPARE(StudioAgent::operation("studio_execute", args), op); QCOMPARE(args, inner);
		}
		const auto schemas = StudioAgent::schemas();
		for (int index = StudioCapabilities::schema().size(); index < schemas.size(); ++index)
		{
			auto name = schemas.at(index).toObject().value("name").toString();
			QVERIFY(declared.contains(name.replace('.', '_')));
		}
		QVERIFY(StudioAgent::operation("arbitrary_code").isEmpty());
		QVERIFY_EXCEPTION_THROWN(StudioAgent::tools("unknown"), std::invalid_argument);
		const QJsonArray input{QJsonObject{{"role", "user"}, {"content", "Compose"}}};
		const auto body = StudioInference::requestBody(configuration("responses"), input, responses, "instructions");
		QCOMPARE(body.value("input").toArray(), input);
		QVERIFY(!body.value("store").toBool(true));
		QVERIFY(body.value("include").toArray().contains("reasoning.encrypted_content"));
		QCOMPARE(StudioInference::requestBody(configuration(), input, chat, "instructions")
			.value("messages").toArray().at(0).toObject().value("role").toString(), QString("system"));
	}

	void malformedCatalogCallsCannotResolveOtherRuntimeOperations_data()
	{
		QTest::addColumn<QJsonObject>("args");
		QTest::newRow("missing-operation") << QJsonObject{{"args", QJsonObject{}}};
		QTest::newRow("missing-arguments") << QJsonObject{{"op", "song.set"}};
		QTest::newRow("non-string-operation") << QJsonObject{{"op", 42}, {"args", QJsonObject{}}};
		QTest::newRow("array-arguments") << QJsonObject{{"op", "song.set"}, {"args", QJsonArray{}}};
		QTest::newRow("extra-field") << QJsonObject{{"op", "song.set"}, {"args", QJsonObject{}}, {"expectedRevision", 0}};
		QTest::newRow("unknown-operation") << QJsonObject{{"op", "shell.execute"}, {"args", QJsonObject{}}};
		QTest::newRow("configuration-operation") << QJsonObject{{"op", "session.provider.set"}, {"args", QJsonObject{}}};
		QTest::newRow("agent-control") << QJsonObject{{"op", "agent.finish"}, {"args", QJsonObject{}}};
		QTest::newRow("recursive-router") << QJsonObject{{"op", "studio.execute"}, {"args", QJsonObject{}}};
		QTest::newRow("flattened-name") << QJsonObject{{"op", "song_set"}, {"args", QJsonObject{}}};
	}
	void malformedCatalogCallsCannotResolveOtherRuntimeOperations()
	{
		QFETCH(QJsonObject, args); const auto original = args;
		QVERIFY(StudioAgent::operation("studio_execute", args).isEmpty()); QCOMPARE(args, original);
		QJsonObject direct{{"tempo", 99}};
		QCOMPARE(StudioAgent::operation("song_set", direct), QString("song.set")); QCOMPARE(direct.value("tempo").toInt(), 99);
	}

	void encryptedReasoningSurvivesContinuation()
	{
		const QJsonObject reasoning{{"type", "reasoning"}, {"id", "rs_fixture"},
			{"summary", QJsonArray{}}, {"encrypted_content", "fixture-encrypted-state"}};
		const QJsonObject function{{"type", "function_call"}, {"call_id", "call_fixture"},
			{"name", "song_set"}, {"arguments", "{\"tempo\":99}"}, {"status", "completed"}};
		const QJsonArray output{reasoning, function};
		const auto decision = StudioInference::interpret({{"status", "completed"}, {"output", output}}, "responses");
		QCOMPARE(decision.value("continuation").toArray(), output);
		QCOMPARE(decision.value("calls").toArray().at(0).toObject().value("args").toObject().value("tempo").toInt(), 99);
	}

	void malformedCallsAreRejected()
	{
		QVERIFY_EXCEPTION_THROWN(StudioInference::interpret(chatCall({tool("bad", "[]")}), "chat"), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(StudioInference::interpret(chatCall({tool("large", "{\"value\":\"" + QString(512 * 1024, 'x') + "\"}")}), "chat"), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(StudioInference::interpret(chatCall({tool("same", "{}"), tool("same", "{}")}), "chat"), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(StudioInference::interpret({{"status", "incomplete"}, {"output", QJsonArray{}}}, "responses"), std::runtime_error);
		auto bad = chatText("text");
		auto choices = bad.value("choices").toArray();
		auto choice = choices.at(0).toObject();
		auto message = choice.value("message").toObject();
		message.insert("tool_calls", "malformed");
		choice.insert("message", message); choices[0] = choice; bad.insert("choices", choices);
		QVERIFY_EXCEPTION_THROWN(StudioInference::interpret(bad, "chat"), std::runtime_error);
	}

	void assistantContinuationRemovesOutputOnlyFields()
	{
		auto response = chatCall({tool("call", "{}")});
		auto choices = response.value("choices").toArray();
		auto choice = choices.at(0).toObject();
		auto message = choice.value("message").toObject();
		message.remove("content");
		message.insert("annotations", QJsonArray{});
		message.insert("audio", QJsonObject{{"data", "fixture-output-audio"}});
		message.insert("reasoning_content", "local-provider-continuation");
		choice.insert("message", message); choices[0] = choice; response.insert("choices", choices);
		const auto continuation = StudioInference::interpret(response, "chat").value("continuation").toArray().at(0).toObject();
		QVERIFY(continuation.value("content").isNull());
		QVERIFY(!continuation.contains("audio"));
		QVERIFY(!continuation.contains("annotations"));
		QCOMPARE(continuation.value("reasoning_content").toString(), QString("local-provider-continuation"));
	}

	void realHttpCarriesNativeTools()
	{
		Loopback fixture;
		QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
		const auto decision = StudioInference::generate(fixture.config(),
			{QJsonObject{{"role", "user"}, {"content", "Plan a composition"}}}, StudioAgent::tools("chat"), "native instructions");
		QCOMPARE(decision.value("text").toString(), QString("fixture response"));
		QCOMPARE(fixture.received, 1);
		QCOMPARE(fixture.request.value("tools").toArray(), StudioAgent::tools("chat"));
		QVERIFY(!fixture.request.value("parallel_tool_calls").toBool(true));
	}

	void realHttpCancellationExceptionsReturnOutsideQtAndCanRetry_data()
	{
		QTest::addColumn<bool>("audio"); QTest::addColumn<bool>("nonstandard");
		for (bool audio : {false, true}) for (bool nonstandard : {false, true})
		{ QTest::newRow(qPrintable(QString::number(audio) + QString::number(nonstandard))) << audio << nonstandard; }
	}
	void realHttpCancellationExceptionsReturnOutsideQtAndCanRetry()
	{
		QFETCH(bool, audio); QFETCH(bool, nonstandard); Loopback fixture;
		QVERIFY(fixture.server.listen(QHostAddress::LocalHost)); fixture.hang = true; int callbacks = 0; bool caught = false;
		const auto cancelled = [&]() -> bool {
			if (fixture.received > 0) { ++callbacks; if (nonstandard) { throw 223; } throw std::runtime_error("Inference cancellation callback fixture"); }
			return false;
		};
		try
		{
			// Payload is transport fixture data, not evidence of provider listening.
			if (audio) { StudioInference::listen(fixture.config(), QByteArray("fixture audio bytes"), "fixture", cancelled); }
			else { StudioInference::generate(fixture.config(), {}, {}, "fixture", cancelled); }
		}
		catch (const std::runtime_error& error) { QVERIFY(!nonstandard); QCOMPARE(QString::fromUtf8(error.what()), QString("Inference cancellation callback fixture")); caught = true; }
		catch (int error) { QVERIFY(nonstandard); QCOMPARE(error, 223); caught = true; }
		QVERIFY(caught); QCOMPARE(callbacks, 1); QCOMPARE(fixture.received, 1);
		QTRY_VERIFY_WITH_TIMEOUT(fixture.disconnected >= 1, 3000);
		fixture.hang = false;
		const auto response = audio ? StudioInference::listen(fixture.config(), QByteArray("fixture audio bytes"), "retry")
			: StudioInference::generate(fixture.config(), {}, {}, "retry");
		QCOMPARE(response.value(audio ? "judgment" : "text").toString(), QString("fixture response")); QCOMPARE(fixture.received, 2);
	}

	void realHttpTimeoutCancellationAndBodyLimits()
	{
		Loopback fixture;
		QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
		fixture.hang = true;
		auto config = fixture.config(); config.insert("timeoutMs", 100);
		QVERIFY_EXCEPTION_THROWN(StudioInference::generate(config, {}, {}, "instructions"), std::runtime_error);
		bool cancelled = false;
		QTimer::singleShot(25, &fixture.server, [&] { cancelled = true; });
		QVERIFY_EXCEPTION_THROWN(StudioInference::generate(fixture.config(), {}, {}, "instructions", [&] { return cancelled; }), std::runtime_error);
		QVERIFY(cancelled);
		fixture.hang = false;
		fixture.response = QByteArray(4 * 1024 * 1024 + 1, 'x');
		QVERIFY_EXCEPTION_THROWN(StudioInference::generate(fixture.config(), {}, {}, "instructions"), std::runtime_error);
		fixture.status = "500 Internal Server Error";
		fixture.response = "fixture-private-error-body";
		try { StudioInference::generate(fixture.config(), {}, {}, "instructions"); QFAIL("HTTP failure was accepted"); }
		catch (const std::runtime_error& error) { QVERIFY(!QString::fromUtf8(error.what()).contains("fixture-private-error-body")); }
	}
};

QTEST_GUILESS_MAIN(StudioInferenceTest)
#include "StudioInferenceTest.moc"
