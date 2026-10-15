/* In-process inference adapters. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioInference.h"

#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace lmms
{
namespace
{

[[noreturn]] void fail(const QString& message)
{
	throw std::runtime_error(message.toStdString());
}

int integer(const QJsonObject& input, const QString& key, int fallback, int min, int max)
{
	if (!input.contains(key)) { return fallback; }
	const auto value = input.value(key);
	const auto n = value.toDouble();
	if (!value.isDouble() || !std::isfinite(n) || n < min || n > max || std::floor(n) != n)
	{
		fail(key + " is outside the supported integer range");
	}
	return static_cast<int>(n);
}

QJsonObject post(const QJsonObject& config, const QJsonObject& body, const StudioInference::Cancelled& cancelled)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
	{
		fail("Inference must run on the application thread");
	}
	const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
	if (bytes.size() > 32 * 1024 * 1024) { fail("Inference request exceeds the 32 MiB budget"); }
	if (cancelled && cancelled()) { fail("Inference cancelled"); }
	if (QUrl(config.value("endpoint").toString()).scheme() == "https")
	{
#if QT_CONFIG(ssl)
		if (!QSslSocket::supportsSsl())
#endif
		{
			fail("HTTPS inference is unavailable: this Qt installation has no usable TLS backend");
		}
	}
	QNetworkRequest request{QUrl(config.value("endpoint").toString())};
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	const auto keyEnv = config.value("keyEnv").toString();
	if (!keyEnv.isEmpty())
	{
		const auto key = qgetenv(keyEnv.toUtf8().constData());
		if (key.isEmpty() || key.contains('\r') || key.contains('\n')) { fail("Configured API credential is unavailable or invalid"); }
		request.setRawHeader("Authorization", "Bearer " + key);
	}
	QNetworkAccessManager network;
	std::unique_ptr<QNetworkReply> reply(network.post(request, bytes));
	reply->setReadBufferSize(65536);
	QByteArray response;
	QEventLoop loop;
	QTimer deadline;
	QTimer cancellation;
	deadline.setSingleShot(true);
	bool timedOut = false;
	bool aborted = false;
	bool oversized = false;
	std::exception_ptr callbackFailure;
	const auto failCallback = [&]
	{
		if (!callbackFailure) { callbackFailure = std::current_exception(); }
		cancellation.stop(); deadline.stop(); reply->abort(); loop.quit();
	};
	const auto collect = [&]
	{
		if (callbackFailure || oversized || timedOut || aborted) { return; }
		try
		{
			const auto remaining = 4 * 1024 * 1024 - response.size();
			const auto block = reply->read(remaining + 1);
			if (block.size() > remaining)
			{
				oversized = true;
				reply->abort();
				return;
			}
			response.append(block);
		}
		catch (...) { failCallback(); }
	};
	QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, collect);
	QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { timedOut = true; reply->abort(); loop.quit(); });
	QObject::connect(&cancellation, &QTimer::timeout, &loop, [&]
	{
		try { if (cancelled && cancelled()) { aborted = true; reply->abort(); loop.quit(); } }
		catch (...) { failCallback(); }
	});
	deadline.start(config.value("timeoutMs").toInt());
	cancellation.start(25);
	if (!reply->isFinished()) { loop.exec(); }
	deadline.stop();
	cancellation.stop();
	collect();
	if (callbackFailure) { std::rethrow_exception(callbackFailure); }
	if (oversized) { fail("Inference response exceeds the 4 MiB budget"); }
	if (timedOut) { fail("Inference request timed out"); }
	if (aborted || (cancelled && cancelled())) { fail("Inference cancelled"); }
	const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
	{
		// Provider bodies and raw network diagnostics may contain credentials or inputs.
		fail(QString("Inference transport failed (HTTP %1, network error %2)").arg(status).arg(static_cast<int>(reply->error())));
	}
	QJsonParseError parse;
	const auto document = QJsonDocument::fromJson(response, &parse);
	if (parse.error != QJsonParseError::NoError || !document.isObject()) { fail("Inference returned invalid JSON"); }
	return document.object();
}

QJsonObject call(const QString& id, const QString& name, const QString& arguments)
{
	static const QRegularExpression functionName("^[A-Za-z0-9_-]{1,64}$");
	// Leave room for the native request envelope within its 1 MiB limit.
	if (id.isEmpty() || id.size() > 128 || !functionName.match(name).hasMatch() || arguments.toUtf8().size() > 512 * 1024)
	{
		fail("Provider returned an invalid or oversized function call");
	}
	QJsonParseError error;
	const auto args = QJsonDocument::fromJson(arguments.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !args.isObject()) { fail("Provider function arguments must be a JSON object"); }
	return {{"id", id}, {"name", name}, {"args", args.object()}};
}

QJsonObject exchange(const QJsonObject& config, const QJsonObject& body,
	const StudioInference::Cancelled& cancelled, const StudioInference::Transport& transport)
{
	if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > 32 * 1024 * 1024)
	{
		fail("Inference request exceeds the 32 MiB budget");
	}
	if (cancelled && cancelled()) { fail("Inference cancelled"); }
	const auto response = transport ? transport(config, body, cancelled) : post(config, body, cancelled);
	if (cancelled && cancelled()) { fail("Inference cancelled"); }
	if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > 4 * 1024 * 1024)
	{
		fail("Inference response exceeds the 4 MiB budget");
	}
	return response;
}

} // namespace

QJsonObject StudioInference::configuration(const QJsonObject& input, bool audio)
{
	for (auto it = input.begin(); it != input.end(); ++it)
	{
		if (it.key() != "protocol" && it.key() != "endpoint" && it.key() != "model" && it.key() != "keyEnv"
			&& it.key() != "timeoutMs" && it.key() != "maxOutputTokens") { fail("Unknown provider configuration field: " + it.key()); }
	}
	const auto protocol = input.value("protocol").toString();
	const auto model = input.value("model").toString();
	const auto url = QUrl(input.value("endpoint").toString(), QUrl::StrictMode);
	if ((protocol != "responses" && protocol != "chat") || (audio && protocol != "chat")
		|| model.trimmed().isEmpty() || model.size() > 256 || !url.isValid() || url.host().isEmpty()
		|| (url.scheme() != "http" && url.scheme() != "https") || !url.userInfo().isEmpty()
		|| url.hasFragment() || url.hasQuery()) { fail("Provider requires an explicit supported protocol, endpoint and model"); }
	const auto keyEnv = input.value("keyEnv").toString();
	static const QRegularExpression envName("^[A-Za-z_][A-Za-z0-9_]{0,127}$");
	if (input.contains("keyEnv") && (!input.value("keyEnv").isString() || (!keyEnv.isEmpty() && !envName.match(keyEnv).hasMatch())))
	{
		fail("keyEnv must name an environment variable; credentials must not be stored in session configuration");
	}
	if (!keyEnv.isEmpty() && url.scheme() == "http"
		&& url.host() != "localhost" && url.host() != "127.0.0.1" && url.host() != "::1")
	{
		fail("Authenticated remote endpoints require HTTPS");
	}
	return {{"protocol", protocol}, {"endpoint", url.toString()}, {"model", model}, {"keyEnv", keyEnv},
		{"timeoutMs", integer(input, "timeoutMs", 120000, 100, 600000)},
		{"maxOutputTokens", integer(input, "maxOutputTokens", 4096, 64, 32768)}};
}

QJsonObject StudioInference::requestBody(const QJsonObject& config, const QJsonArray& input,
	const QJsonArray& tools, const QString& instructions)
{
	QJsonObject body{{"model", config.value("model")}, {"store", false}, {"stream", false}};
	if (config.value("protocol").toString() == "responses")
	{
		body.insert("instructions", instructions);
		body.insert("input", input);
		body.insert("max_output_tokens", config.value("maxOutputTokens"));
		body.insert("include", QJsonArray{"reasoning.encrypted_content"});
	}
	else
	{
		QJsonArray messages{QJsonObject{{"role", "system"}, {"content", instructions}}};
		for (const auto& item : input) { messages.append(item); }
		body.insert("messages", messages);
		body.insert("max_completion_tokens", config.value("maxOutputTokens"));
	}
	if (!tools.isEmpty())
	{
		body.insert("tools", tools);
		body.insert("parallel_tool_calls", false);
	}
	return body;
}

QJsonObject StudioInference::interpret(const QJsonObject& response, const QString& protocol)
{
	QJsonArray calls;
	QJsonArray continuation;
	QString text;
	if (protocol == "responses")
	{
		if (response.value("status").toString() != "completed" || !response.value("output").isArray())
		{
			fail("Provider response is incomplete or unsupported");
		}
		continuation = response.value("output").toArray();
		for (const auto& value : continuation)
		{
			const auto item = value.toObject();
			const auto type = item.value("type").toString();
			if (!value.isObject() || (item.contains("status") && !item.value("status").isNull()
				&& item.value("status").toString() != "completed"))
			{
				fail("Provider returned an invalid or unfinished output item");
			}
			if (type == "function_call")
			{
				calls.append(call(item.value("call_id").toString(), item.value("name").toString(), item.value("arguments").toString()));
			}
			else if (type == "message")
			{
				if (item.value("role").toString() != "assistant" || !item.value("content").isArray())
				{
					fail("Provider returned an invalid assistant message");
				}
				for (const auto& part : item.value("content").toArray())
				{
					const auto content = part.toObject();
					const auto field = content.value("type").toString() == "output_text" ? QString("text")
						: content.value("type").toString() == "refusal" ? QString("refusal") : QString();
					if (field.isEmpty() || !content.value(field).isString()) { fail("Provider returned unsupported message content"); }
					text += content.value(field).toString();
				}
			}
			else if (type != "reasoning") { fail("Provider returned an unsupported output item"); }
		}
	}
	else if (protocol == "chat")
	{
		const auto choices = response.value("choices").toArray();
		if (choices.size() != 1 || !choices.at(0).isObject()) { fail("Provider must return exactly one completion"); }
		const auto choice = choices.at(0).toObject();
		const auto finish = choice.value("finish_reason").toString();
		if (finish != "stop" && finish != "tool_calls") { fail("Provider completion is incomplete or unsupported"); }
		auto message = choice.value("message").toObject();
		if (message.value("role").toString() != "assistant"
			|| (message.contains("content") && !message.value("content").isString() && !message.value("content").isNull())
			|| (message.contains("tool_calls") && !message.value("tool_calls").isArray() && !message.value("tool_calls").isNull()))
		{
			fail("Provider returned an invalid assistant message");
		}
		text = message.value("content").toString();
		if (text.isEmpty()) { text = message.value("refusal").toString(); }
		for (const auto& value : message.value("tool_calls").toArray())
		{
			const auto tool = value.toObject();
			if (tool.value("type").toString() != "function") { fail("Only native function calls are supported"); }
			const auto function = tool.value("function").toObject();
			calls.append(call(tool.value("id").toString(), function.value("name").toString(), function.value("arguments").toString()));
		}
		if ((finish == "tool_calls") != !calls.isEmpty()) { fail("Provider finish reason disagrees with its function calls"); }
		// Retain accepted input fields, including local-provider reasoning text.
		// Generated audio, annotations and output-only fields are not chat inputs.
		for (const auto& key : message.keys())
		{
			if (key != "role" && key != "content" && key != "refusal" && key != "tool_calls" && key != "reasoning_content")
			{
				message.remove(key);
			}
		}
		if (!message.contains("content")) { message.insert("content", QJsonValue::Null); }
		if (message.value("tool_calls").isNull()) { message.remove("tool_calls"); }
		if (message.value("reasoning_content").isNull()) { message.remove("reasoning_content"); }
		if (message.contains("reasoning_content") && !message.value("reasoning_content").isString())
		{
			fail("Provider returned invalid reasoning content");
		}
		continuation.append(message);
	}
	else { fail("Unsupported inference protocol"); }
	if (calls.size() > 64 || (calls.isEmpty() && text.isEmpty())) { fail("Provider returned no usable decision or too many calls"); }
	QSet<QString> ids;
	for (const auto& value : calls)
	{
		const auto id = value.toObject().value("id").toString();
		if (ids.contains(id)) { fail("Provider reused a function-call identifier"); }
		ids.insert(id);
	}
	return {{"text", text}, {"calls", calls}, {"continuation", continuation}, {"usage", response.value("usage")}};
}

QJsonObject StudioInference::generate(const QJsonObject& config, const QJsonArray& input,
	const QJsonArray& tools, const QString& instructions, const Cancelled& cancelled, const Transport& transport)
{
	const auto validated = configuration(config);
	const auto body = requestBody(validated, input, tools, instructions);
	return interpret(exchange(validated, body, cancelled, transport), validated.value("protocol").toString());
}

QJsonObject StudioInference::listen(const QJsonObject& config, const QByteArray& wave,
	const QString& prompt, const Cancelled& cancelled, const Transport& transport)
{
	const auto validated = configuration(config, true);
	if (wave.isEmpty() || wave.size() > 20 * 1024 * 1024) { fail("Listening excerpt exceeds the 20 MiB audio budget"); }
	QJsonArray content{
		QJsonObject{{"type", "text"}, {"text", prompt}},
		QJsonObject{{"type", "input_audio"}, {"input_audio", QJsonObject{{"data", QString::fromLatin1(wave.toBase64())}, {"format", "wav"}}}}
	};
	const QJsonArray input{QJsonObject{{"role", "user"}, {"content", content}}};
	auto body = requestBody(validated, input, {},
		"Evaluate the supplied musical audio excerpt. Describe audible properties, transitions and mix balance. "
		"Separate subjective musical judgments from technical claims, disclose uncertainty, and avoid claiming to have heard anything outside this excerpt.");
	body.insert("modalities", QJsonArray{"text"});
	const auto decision = interpret(exchange(validated, body, cancelled, transport), "chat");
	if (!decision.value("calls").toArray().isEmpty()) { fail("Audio evaluation must return observations, not execution requests"); }
	return {{"kind", "provider audio evaluation"}, {"provider", validated.value("endpoint")},
		{"model", validated.value("model")}, {"judgment", decision.value("text")}, {"usage", decision.value("usage")}};
}

} // namespace lmms
