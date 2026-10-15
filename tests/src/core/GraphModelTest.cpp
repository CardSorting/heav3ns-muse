/* Native waveform/curve validation and publication. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "GraphModel.h"
#include "NotePlayHandle.h"
#include "Song.h"

using namespace lmms;

namespace
{
QByteArray bytes(const graphModel& graph)
{
	return {reinterpret_cast<const char*>(graph.samples()), graph.length() * static_cast<int>(sizeof(float))};
}
QString encoded(const std::array<float, 4>& values)
{
	return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(values.data()), sizeof(values)).toBase64());
}
}

class GraphModelTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearErrors(); }

	void checkedEditsSnapAndRetainTheHiddenTail()
	{
		graphModel graph(0, 15, 4, nullptr, false, 1); QString error;
		const std::array<float, 4> initial{1, 2, 3, 4}; QVERIFY(graph.setSamplesChecked(initial, error));
		const auto borrowed = graph.samples();
		graph.setLength(2); const std::array<float, 2> edited{7.9f, 15}; QVERIFY(graph.setSamplesChecked(edited, error, true));
		QCOMPARE(graph.samples()[0], 7.f); QCOMPARE(graph.samples()[1], 15.f);
		QCOMPARE(graph.samples(), borrowed);
		graph.setLength(4); QCOMPARE(graph.samples()[2], 3.f); QCOMPARE(graph.samples()[3], 4.f);
		// A self-referencing source remains valid until the new storage is ready.
		QVERIFY(graph.setSamplesChecked({graph.samples(), 4}, error)); QCOMPARE(graph.samples()[0], 7.f);
	}

	void rejectedBulkEditsLeaveAllSamplesAndSignalsUntouched_data()
	{
		QTest::addColumn<int>("kind");
		QTest::newRow("wrong-length") << 0; QTest::newRow("late-nan") << 1;
		QTest::newRow("late-infinity") << 2; QTest::newRow("outside-range") << 3;
	}
	void rejectedBulkEditsLeaveAllSamplesAndSignalsUntouched()
	{
		QFETCH(int, kind); graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSine(); const auto original = bytes(graph);
		QSignalSpy changed(&graph, &graphModel::samplesChanged); QString error;
		std::array<float, 4> input{.5f, .5f, .5f, .5f};
		if (kind == 1) { input[3] = std::numeric_limits<float>::quiet_NaN(); }
		if (kind == 2) { input[3] = std::numeric_limits<float>::infinity(); }
		if (kind == 3) { input[3] = 1.001f; }
		QVERIFY(!graph.setSamplesChecked({input.data(), kind == 0 ? std::size_t(3) : input.size()}, error, true));
		QVERIFY(!error.isEmpty()); QCOMPARE(bytes(graph), original); QCOMPARE(changed.count(), 0);
	}

	void legacyEncodedSamplesKeepFiniteAmplitudeAndWhitespace_data()
	{
		QTest::addColumn<int>("form"); QTest::newRow("canonical") << 0; QTest::newRow("wrapped") << 1; QTest::newRow("unpadded") << 2;
	}
	void legacyEncodedSamplesKeepFiniteAmplitudeAndWhitespace()
	{
		QFETCH(int, form); graphModel graph(0, 1, 4, nullptr); const std::array<float, 4> input{-2, 0, .25f, 2};
		auto text = encoded(input);
		if (form == 1) { text.insert(7, " \t\r\n"); }
		if (form == 2) { text.remove('='); }
		QString error("old error"); QVERIFY2(graph.loadBase64(text, error), qPrintable(error)); QVERIFY(error.isEmpty());
		for (int index = 0; index < 4; ++index) { QCOMPARE(graph.samples()[index], input[index]); }
	}

	void malformedEncodingCannotReadPastDecodedStorage_data()
	{
		QTest::addColumn<QString>("text"); const auto valid = encoded({1, 2, 3, 4});
		QTest::newRow("empty") << QString{}; QTest::newRow("truncated") << valid.left(8);
		QTest::newRow("partial-float") << QString::fromLatin1(QByteArray(15, '\0').toBase64());
		QTest::newRow("extra-float") << QString::fromLatin1(QByteArray(20, '\0').toBase64());
		QTest::newRow("bad-character") << (valid + "!"); QTest::newRow("unicode") << (valid + QChar(0x97f3));
		QTest::newRow("huge") << QString(5000, 'A');
		QTest::newRow("late-nan") << encoded({1, 2, 3, std::numeric_limits<float>::quiet_NaN()});
	}
	void malformedEncodingCannotReadPastDecodedStorage()
	{
		QFETCH(QString, text); graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSquare(); const auto original = bytes(graph);
		QSignalSpy changed(&graph, &graphModel::samplesChanged); QString error;
		QVERIFY(!graph.loadBase64(text, error)); QVERIFY(!error.isEmpty()); QCOMPARE(bytes(graph), original); QCOMPARE(changed.count(), 0);
	}

	void missingAttributesKeepDefaultsAndMalformedAttributesReportNativeErrors()
	{
		graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSquare(); const auto original = bytes(graph);
		QDomDocument document; auto element = document.createElement("fixture");
		QVERIFY(graph.loadSampleAttribute(element, "wave")); QCOMPARE(bytes(graph), original); QVERIFY(!Engine::getSong()->hasErrors());
		element.setAttribute("wave", "AA=="); QVERIFY(!graph.loadSampleAttribute(element, "wave"));
		QVERIFY(Engine::getSong()->errorSummary().contains("fixture/wave")); QCOMPARE(bytes(graph), original);
	}

	void explicitlyShortLegacyTablesZeroFillOnlyWithinTheirCapacity()
	{
		graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSquare(); QString error;
		const float input[]{.5f, -.5f}; const auto text = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(input), sizeof(input)).toBase64());
		QVERIFY(graph.loadBase64(text, error, true)); QCOMPARE(graph.length(), 4);
		QCOMPARE(graph.samples()[0], .5f); QCOMPARE(graph.samples()[1], -.5f); QCOMPARE(graph.samples()[2], 0.f); QCOMPARE(graph.samples()[3], 0.f);
		QVERIFY(!graph.loadBase64(QString::fromLatin1(QByteArray(20, '\0').toBase64()), error, true)); QCOMPARE(graph.samples()[0], .5f);
	}

	void callbacksSeeThePublishedCurveAndCannotReenterIt()
	{
		graphModel graph(-1, 1, 4, nullptr); QString error; bool rejected = false; float observed = 0;
		const auto connection = connect(&graph, &graphModel::samplesChanged, &graph, [&]
		{
			observed = graph.samples()[0]; const std::array<float, 4> replacement{};
			rejected = !graph.setSamplesChecked(replacement, error); graph.clear(); graph.setLength(1); graph.setRange(0, 1);
		});
		const std::array<float, 4> input{.25f, .5f, .75f, 1}; QVERIFY(graph.setSamplesChecked(input, error));
		QVERIFY(rejected); QCOMPARE(observed, .25f); QCOMPARE(graph.length(), 4); QCOMPARE(graph.minValue(), -1.f); QCOMPARE(graph.samples()[3], 1.f);
		disconnect(connection); graph.clear(); QCOMPARE(graph.samples()[3], 0.f);
	}

	void callbackExceptionsReleaseNativeAdmissionAndAudioGuard()
	{
		graphModel graph(-1, 1, 4, nullptr); QString error;
		const auto connection = connect(&graph, &graphModel::samplesChanged, &graph, [] { throw std::runtime_error("fixture"); });
		const std::array<float, 4> input{.5f, .5f, .5f, .5f};
		QVERIFY_EXCEPTION_THROWN(graph.setSamplesChecked(input, error), std::runtime_error); disconnect(connection);
		std::thread probe([] { const auto guard = Engine::audioEngine()->requestChangesGuard(); }); probe.join();
		graph.clear(); QCOMPARE(graph.samples()[0], 0.f); QVERIFY(graph.setSamplesChecked(input, error));
	}

	void foreignThreadCannotPublishOrResizeTheCurve()
	{
		graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSine(); const auto original = bytes(graph); bool accepted = true; QString error;
		std::thread worker([&] { const std::array<float, 4> input{}; accepted = graph.setSamplesChecked(input, error); graph.clear(); graph.setLength(2); }); worker.join();
		QVERIFY(!accepted); QVERIFY(!error.isEmpty()); QCOMPARE(graph.length(), 4); QCOMPARE(bytes(graph), original);
	}

	void singleSampleAndNegativeOffsetsUseBoundedNativeTransforms()
	{
		graphModel single(-1, 1, 1, nullptr); single.setSampleAt(0, .5f); single.smooth(); single.smoothNonCyclic();
		single.shiftPhase(std::numeric_limits<int>::min()); QCOMPARE(single.samples()[0], .5f);
		graphModel graph(-10, 10, 4, nullptr); const float values[]{1, 2, 3, 4}; graph.setSamples(values);
		const float kernel[]{1}; graph.convolve(kernel, 1, -1); QCOMPARE(graph.samples()[0], 2.f); QCOMPARE(graph.samples()[3], 1.f);
		graph.shiftPhase(180); QCOMPARE(graph.samples()[0], 4.f); QCOMPARE(graph.samples()[3], 3.f);
	}

	void invalidDimensionsAndRangesDoNotCorruptAnExistingCurve()
	{
		QVERIFY_EXCEPTION_THROWN(graphModel(-1, 1, 0, nullptr), std::invalid_argument);
		QVERIFY_EXCEPTION_THROWN(graphModel(1, -1, 4, nullptr), std::invalid_argument);
		graphModel graph(-1, 1, 4, nullptr); graph.setWaveToSine(); const auto original = bytes(graph);
		QVERIFY_EXCEPTION_THROWN(graph.setLength(0), std::invalid_argument);
		QVERIFY_EXCEPTION_THROWN(graph.setRange(2, 1), std::invalid_argument); QCOMPARE(graph.length(), 4); QCOMPARE(bytes(graph), original);
	}

	void fileLoadingUsesNativeDecodedAmplitudeAndPreservesOnFailure()
	{
		QTemporaryDir directory; const auto path = directory.filePath("constant.wav"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
		// Four mono PCM16 frames at +0.25, 44100 Hz.
		const auto wav = QByteArray::fromHex("524946462c00000057415645666d7420100000000100010044ac0000885801000200100064617461080000000020002000200020");
		QCOMPARE(file.write(wav), qint64(wav.size())); file.close(); graphModel graph(-1, 1, 8, nullptr); QString error;
		QVERIFY2(graph.loadWaveform(path, error), qPrintable(error));
		for (int index = 0; index < graph.length(); ++index) { QVERIFY(std::abs(graph.samples()[index] - .25f) < .0001f); }
		const auto original = bytes(graph); QVERIFY(!graph.loadWaveform(directory.filePath("missing.wav"), error)); QCOMPARE(bytes(graph), original);
	}
};

QTEST_GUILESS_MAIN(GraphModelTest)
#include "GraphModelTest.moc"
