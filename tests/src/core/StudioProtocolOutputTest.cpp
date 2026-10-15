/* Native protocol stream tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <QJsonObject>
#include <QProcess>
#include <cstdio>
#include <cstring>

#include "StudioStdio.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace lmms;

namespace
{
bool nativeDiagnostic()
{
	constexpr char message[] = "native output handle diagnostic\n";
#ifdef Q_OS_WIN
	DWORD written = 0;
	return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, nullptr) && written == sizeof(message) - 1;
#else
	return ::write(STDOUT_FILENO, message, sizeof(message) - 1) == sizeof(message) - 1;
#endif
}

bool privateOutputCannotBeInherited(std::FILE* stream)
{
#ifdef Q_OS_WIN
	DWORD flags = HANDLE_FLAG_INHERIT;
	return GetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))), &flags) && !(flags & HANDLE_FLAG_INHERIT);
#else
	const int flags = ::fcntl(fileno(stream), F_GETFD);
	return flags >= 0 && (flags & FD_CLOEXEC);
#endif
}

int protocolChild(bool restore)
{
	{
		StudioProtocolOutput protocol(restore); QString error;
		if (!protocol.open(error)) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 2; }
		const bool repeatedRejected = !protocol.open(error);
		std::puts("native stdio diagnostic"); std::fflush(stdout);
		if (!nativeDiagnostic()) { return 3; }
		QProcess child; child.setProcessChannelMode(QProcess::ForwardedChannels);
		child.start(QCoreApplication::applicationFilePath(), {"--diagnostic-child"});
		if (!child.waitForFinished(5000) || child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) { return 4; }
		const auto message = QJsonDocument(QJsonObject{{"event", "protocol"}, {"repeatRejected", repeatedRejected},
			{"nonInheritable", privateOutputCannotBeInherited(protocol.stream())}}).toJson(QJsonDocument::Compact) + '\n';
		if (std::fwrite(message.constData(), 1, message.size(), protocol.stream()) != static_cast<std::size_t>(message.size()) || std::fflush(protocol.stream()) != 0) { return 5; }
	}
	std::puts("{\"event\":\"restored\"}"); std::fflush(stdout);
	return 0;
}
}

class StudioProtocolOutputTest : public QObject
{
	Q_OBJECT
private slots:
#ifndef Q_OS_WIN
	void inputEofAndInvalidDescriptors_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("pipe-eof") << QString("pipe");
		QTest::newRow("null-device-eof") << QString("null"); QTest::newRow("invalid-descriptor") << QString("invalid");
	}
	void inputEofAndInvalidDescriptors()
	{
		QFETCH(QString, kind); QProcess child;
		if (kind == "null") { child.setStandardInputFile(QProcess::nullDevice()); }
		child.start(QCoreApplication::applicationFilePath(), {kind == "invalid" ? "--invalid-input-child" : "--eof-input-child"});
		child.closeWriteChannel(); QVERIFY(child.waitForFinished(5000));
		QCOMPARE(child.exitStatus(), QProcess::NormalExit); QCOMPARE(child.exitCode(), 0);
	}
#endif
	void nativeAndChildDiagnosticsCannotEnterReservedProtocolOutput_data()
	{
		QTest::addColumn<bool>("restore");
		QTest::newRow("scoped-restore") << true;
		QTest::newRow("process-teardown-diagnostics") << false;
	}

	void nativeAndChildDiagnosticsCannotEnterReservedProtocolOutput()
	{
		QFETCH(bool, restore);
		QProcess child; child.start(QCoreApplication::applicationFilePath(), {restore ? "--protocol-child" : "--protocol-child-retain"});
		QVERIFY(child.waitForFinished(10000)); QCOMPARE(child.exitStatus(), QProcess::NormalExit); QCOMPARE(child.exitCode(), 0);
		const auto lines = child.readAllStandardOutput().trimmed().split('\n'); QCOMPARE(lines.size(), restore ? 2 : 1);
		QJsonParseError error; const auto protocol = QJsonDocument::fromJson(lines.at(0), &error).object(); QCOMPARE(error.error, QJsonParseError::NoError);
		QCOMPARE(protocol.value("event").toString(), QString("protocol")); QVERIFY(protocol.value("repeatRejected").toBool()); QVERIFY(protocol.value("nonInheritable").toBool());
		if (restore) { QCOMPARE(QJsonDocument::fromJson(lines.at(1)).object().value("event").toString(), QString("restored")); }
		const auto diagnostic = child.readAllStandardError(); QVERIFY(diagnostic.contains("native stdio diagnostic"));
		QVERIFY(diagnostic.contains("native output handle diagnostic")); QVERIFY(diagnostic.contains("inherited child stdout diagnostic"));
		QCOMPARE(diagnostic.count("native output handle diagnostic"), 2);
		if (!restore) { QVERIFY(diagnostic.contains("{\"event\":\"restored\"}")); }
	}
};

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	if (argc == 2 && std::strcmp(argv[1], "--diagnostic-child") == 0)
	{
		std::puts("inherited child stdout diagnostic"); std::fflush(stdout); return nativeDiagnostic() ? 0 : 6;
	}
	if (argc == 2 && std::strcmp(argv[1], "--protocol-child") == 0) { return protocolChild(true); }
	if (argc == 2 && std::strcmp(argv[1], "--protocol-child-retain") == 0) { return protocolChild(false); }
#ifndef Q_OS_WIN
	if (argc == 2 && (std::strcmp(argv[1], "--eof-input-child") == 0 || std::strcmp(argv[1], "--invalid-input-child") == 0))
	{
		const bool invalid = std::strcmp(argv[1], "--invalid-input-child") == 0;
		if (invalid) { ::close(STDIN_FILENO); }
		StudioStdio input; QElapsedTimer elapsed; elapsed.start();
		while (!input.ended() && elapsed.elapsed() < 1000)
		{ if (!input.readAvailable().isEmpty()) { return 7; } if (!input.ended()) { QThread::msleep(1); } }
		return input.ended() && input.error().isEmpty() != invalid && input.readAvailable().isEmpty() ? 0 : 8;
	}
#endif
	StudioProtocolOutputTest tests; return QTest::qExec(&tests, argc, argv);
}

#include "StudioProtocolOutputTest.moc"
