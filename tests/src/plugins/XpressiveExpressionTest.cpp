/* Actual native ExprTk preview path. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <cmath>
#include "ExprSynth.h"
#include "FileResourceIO.h"

using namespace lmms;

class XpressiveExpressionTest : public QObject
{
	Q_OBJECT
private slots:
	void waveformPreviewUsesNativeTimeRateAndIntegration()
	{
		XpressiveExpressionContext context; context.length = context.sampleRate = 64;
		const auto time = previewXpressiveExpression("t*srate", context); QVERIFY2(time.valid, qPrintable(time.error)); QCOMPARE(time.preview.size(), std::size_t(64));
		for (int index = 0; index < 64; ++index) { QCOMPARE(time.preview[index], static_cast<float>(index)); }
		const auto integral = previewXpressiveExpression("integrate(1)", context); QVERIFY2(integral.valid, qPrintable(integral.error));
		for (int index = 0; index < 64; ++index) { QVERIFY(std::abs(integral.preview[index] - index / 64.f) < .00001f); }
		const auto sine = previewXpressiveExpression("sinew(t)", context); QVERIFY(sine.valid);
		QVERIFY(std::abs(sine.preview[0]) < .00001f); QVERIFY(std::abs(sine.preview[16] - 1) < .00001f); QVERIFY(std::abs(sine.preview[48] + 1) < .00001f);
	}

	void outputSymbolsUseTheNativeNotePreviewContext()
	{
		graphModel wave(-1, 1, 4, nullptr); const float samples[]{.25f, .5f, .75f, 1}; wave.setSamples(samples);
		XpressiveExpressionContext context; context.output = true; context.length = context.sampleRate = 16; context.waves = {&wave, &wave, &wave};
		context.frequency = 10; context.key = 5; context.baseNote = 69; context.volume = .5f; context.tempo = 120; context.parameters = {.25f, .5f, .75f};
		const auto result = previewXpressiveExpression("f+key+bnote+v+tempo+A1+A2+A3+rel+trel+W1(0)+W2(0.25)+W3(0.5)", context);
		QVERIFY2(result.valid, qPrintable(result.error)); for (const auto value : result.preview) { QCOMPARE(value, 207.5f); }
	}

	void waveformsCannotUseOutputOnlySymbols_data()
	{
		QTest::addColumn<QString>("source");
		QTest::newRow("frequency") << QString("f"); QTest::newRow("release") << QString("rel");
		QTest::newRow("parameter") << QString("A1"); QTest::newRow("wave-reference") << QString("W1(t)");
	}
	void waveformsCannotUseOutputOnlySymbols()
	{
		QFETCH(QString, source); const auto result = previewXpressiveExpression(source, {}); QVERIFY(!result.valid); QVERIFY(!result.error.isEmpty()); QVERIFY(result.preview.empty());
	}

	void parserErrorsAndUnsupportedSourceAreReported_data()
	{
		QTest::addColumn<QString>("source");
		QTest::newRow("incomplete") << QString("sinew("); QTest::newRow("unknown-symbol") << QString("missing_symbol+t");
		QTest::newRow("assignment") << QString("t:=1"); QTest::newRow("control-flow") << QString("while(1){1}");
		QTest::newRow("unicode") << QString(QChar(0x97f3)); QTest::newRow("nul") << QString::fromLatin1("t\0+1", 4);
		QTest::newRow("oversized") << QString(65537, '1');
	}
	void parserErrorsAndUnsupportedSourceAreReported()
	{
		QFETCH(QString, source); const auto result = previewXpressiveExpression(source, {}); QVERIFY(!result.valid); QVERIFY(!result.error.isEmpty());
		QVERIFY(result.preview.empty());
	}

	void nonfiniteWaveSamplesKeepNativeZeroReplacementButOutputsAreRejected()
	{
		XpressiveExpressionContext context; context.length = context.sampleRate = 64;
		const auto wave = previewXpressiveExpression("sqrt(-1)", context); QVERIFY(wave.valid); QCOMPARE(wave.nonfiniteSamples, 64);
		QVERIFY(std::all_of(wave.preview.begin(), wave.preview.end(), [](float value) { return value == 0; }));
		graphModel graph(-1, 1, 4, nullptr); context.output = true; context.waves = {&graph, &graph, &graph};
		const auto output = previewXpressiveExpression("sqrt(-1)", context); QVERIFY(!output.valid); QCOMPARE(output.nonfiniteSamples, 64); QVERIFY(!output.error.isEmpty());
		const auto badPhase = previewXpressiveExpression("W1(sqrt(-1))", context); QVERIFY(!badPhase.valid); QCOMPARE(badPhase.nonfiniteSamples, 64);
	}

	void emptyWaveSourceRetainsSamplesAndEmptyOutputIsSilent()
	{
		graphModel wave(-1, 1, 4, nullptr); wave.setWaveToSquare(); XpressiveExpressionContext context;
		context.length = context.sampleRate = 4; context.currentWave = &wave;
		const auto result = previewXpressiveExpression(" \n\t", context); QVERIFY(result.valid);
		for (int index = 0; index < 4; ++index) { QCOMPARE(result.preview[index], wave.samples()[index]); }
		context.output = true; const auto output = previewXpressiveExpression("", context); QVERIFY(output.valid);
		QVERIFY(std::all_of(output.preview.begin(), output.preview.end(), [](float value) { return value == 0; }));
	}

	void validationPreservesRandomStateAndSuccessfulGenerationCanAdvanceIt()
	{
		XpressiveExpressionContext context; context.length = context.sampleRate = 64;
		const auto first = previewXpressiveExpression("randv(t*srate)", context); const auto second = previewXpressiveExpression("randv(t*srate)", context);
		QVERIFY(first.valid && second.valid); QVERIFY(first.preview == second.preview);
		context.consumeRandom = true; const auto generated = previewXpressiveExpression("randv(t*srate)", context);
		QVERIFY(generated.valid); QVERIFY(generated.preview == first.preview);
		context.consumeRandom = false; const auto next = previewXpressiveExpression("randv(t*srate)", context); QVERIFY(next.valid); QVERIFY(next.preview != first.preview);
	}

	void cancellationDiscardsPreviewAndDoesNotAdvanceRandomState()
	{
		XpressiveExpressionContext context; context.length = context.sampleRate = 256;
		const auto before = previewXpressiveExpression("randv(t*srate)", context); QVERIFY(before.valid);
		Plugin::ExpressionResult cancelled; int checks = 0;
		{
			FileResourceIO::Scope scope([&] { return ++checks >= 7; }); context.consumeRandom = true;
			cancelled = previewXpressiveExpression("randv(t*srate)", context);
		}
		QVERIFY(!cancelled.valid); QVERIFY(cancelled.preview.empty()); QVERIFY(!cancelled.error.isEmpty());
		context.consumeRandom = false; const auto after = previewXpressiveExpression("randv(t*srate)", context); QVERIFY(after.valid); QVERIFY(before.preview == after.preview);
	}

	void nativeRandomAndWaveLookupBoundsAvoidInvalidConversions()
	{
		QCOMPARE(positiveFraction(1e30f), 0.f); QCOMPARE(positiveFraction(-1e30f), 0.f);
		QCOMPARE(positiveFraction(-.25f), .75f); QCOMPARE(positiveFraction(-std::numeric_limits<float>::denorm_min()), 0.f);
		XpressiveExpressionContext context; context.length = context.sampleRate = 4;
		const auto random = previewXpressiveExpression("randsv(1e30,1e30)", context); QVERIFY(random.valid);
		QVERIFY(std::all_of(random.preview.begin(), random.preview.end(), [](float value) { return value == 0; }));
		graphModel wave(-1, 1, 4, nullptr); const float samples[]{.25f, .5f, .75f, 1}; wave.setSamples(samples);
		context.output = true; context.waves = {&wave, &wave, &wave}; context.interpolate = {false, true, false};
		const auto phase = previewXpressiveExpression("W1(-1)+W2(-1)+W3(1e30)", context); QVERIFY(phase.valid);
		for (const auto value : phase.preview) { QCOMPARE(value, .75f); }
	}

	void invalidContextIsRejectedBeforeHistoryAllocation()
	{
		XpressiveExpressionContext context; context.sampleRate = std::numeric_limits<int>::max();
		const auto invalid = previewXpressiveExpression("last(1)", context); QVERIFY(!invalid.valid); QVERIFY(invalid.preview.empty());
	}
};

QTEST_GUILESS_MAIN(XpressiveExpressionTest)
#include "XpressiveExpressionTest.moc"
