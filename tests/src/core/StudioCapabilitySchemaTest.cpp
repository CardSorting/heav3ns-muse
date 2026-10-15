/* Published native capability schemas, recursive preflight and catalog coverage.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <QSet>
#include <QTimer>
#include <limits>
#include <stdexcept>
#include "Engine.h"
#include "FileResourceIO.h"
#include "StudioCapabilities.h"
using namespace lmms;
namespace
{
QJsonValue example(const QJsonObject& schema)
{
	const auto type = schema.value("type").toString();
	if (schema.contains("enum")) { return schema.value("enum").toArray().at(0); }
	if (schema.contains("default")) { return schema.value("default"); }
	if (type == "object") { QJsonObject result; const auto fields = schema.value("properties").toObject(); for (auto field = fields.begin(); field != fields.end(); ++field) { result.insert(field.key(), example(field.value().toObject())); } return result; }
	if (type == "array") { QJsonArray result; for (int index = 0; index < schema.value("minItems").toInt(); ++index) { result.append(example(schema.value("items").toObject())); } return result; }
	if (type == "string") { if (schema.contains("pattern")) { return QString(64, 'a'); } return QString(std::max(1, schema.value("minLength").toInt()), 'x'); }
	if (type == "boolean") { return false; }
	if (type == "integer" || type == "number") { return schema.contains("minimum") ? schema.value("minimum") : QJsonValue(0); }
	throw std::runtime_error("Catalog contains a schema type the fixture cannot represent");
}
}
class StudioCapabilitySchemaTest : public QObject
{
	Q_OBJECT
private slots:
	void allPublishedSchemasAcceptTheirRepresentativeArgumentsWithoutEngine()
	{
		QVERIFY(!Engine::getSong()); QVERIFY(!Engine::audioEngine()); const auto catalog = StudioCapabilities::schema(); QCOMPARE(catalog.size(), 165); QSet<QString> names;
		for (const auto& value : catalog) { const auto entry = value.toObject(); const auto name = entry.value("name").toString(); QVERIFY(!names.contains(name)); names.insert(name); const auto schema = entry.value("inputSchema").toObject(); QCOMPARE(schema.value("type").toString(), QString("object")); QVERIFY(!schema.value("additionalProperties").toBool()); const auto properties = schema.value("properties").toObject(); for (const auto key : schema.value("required").toArray()) { QVERIFY(properties.contains(key.toString())); } const auto args = example(schema).toObject(); QString error; QVERIFY2(StudioCapabilities::validateArguments(name, args, error), qPrintable(name + ": " + error)); QVERIFY(error.isEmpty()); }
	}
	void topLevelTypesRequiredFieldsAndUnknownOperations_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("missing") << 0; QTest::newRow("unknown-field") << 1; QTest::newRow("wrong-bool") << 2; QTest::newRow("wrong-array") << 3; QTest::newRow("fractional-index") << 4; QTest::newRow("null") << 5; QTest::newRow("unknown-operation") << 6; QTest::newRow("wrong-object") << 7; QTest::newRow("nonfinite-number") << 8; }
	void topLevelTypesRequiredFieldsAndUnknownOperations()
	{
		QFETCH(int, failure); QString operation = "note.add"; QJsonObject args{{"track", 0}, {"clip", 0}, {"position", 0}, {"length", 24}, {"key", 60}};
		if (failure == 0) { args.remove("track"); } if (failure == 1) { args.insert("unknown", true); } if (failure == 2) { operation = "step.set"; args = {{"track", 0}, {"clip", 0}, {"step", 0}, {"enabled", 1}}; } if (failure == 3) { operation = "note.selection.remove"; args = {{"track", 0}, {"clip", 0}, {"notes", 0}}; } if (failure == 4) { args.insert("track", .5); } if (failure == 5) { operation = "song.set"; args = {{"tempo", QJsonValue::Null}}; } if (failure == 6) { operation = "unknown.operation"; args = {}; } if (failure == 7) { operation = "effect.add"; args = {{"plugin", "missing"}, {"attributes", "string"}}; } if (failure == 8) { operation = "sample.edit"; args = {{"track", 0}, {"clip", 0}, {"gain", std::numeric_limits<double>::infinity()}}; } QString error; QVERIFY(!StudioCapabilities::validateArguments(operation, args, error)); QVERIFY(!error.isEmpty());
	}
	void declaredEnumsAndNumericBounds_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("tempo-low") << 0; QTest::newRow("tempo-high") << 1; QTest::newRow("master-volume") << 2; QTest::newRow("master-pitch") << 3; QTest::newRow("track-type") << 4; QTest::newRow("container") << 5; QTest::newRow("mode") << 6; QTest::newRow("bit-depth") << 7; QTest::newRow("sample-rate") << 8; QTest::newRow("compression") << 9; }
	void declaredEnumsAndNumericBounds()
	{
		QFETCH(int, failure); QString operation = "song.set"; QJsonObject args; if (failure == 0) { args.insert("tempo", 9); } if (failure == 1) { args.insert("tempo", 1000); } if (failure == 2) { args.insert("masterVolume", 201); } if (failure == 3) { args.insert("masterPitch", 13); } if (failure == 4) { operation = "track.add"; args = {{"type", "unsupported"}}; } if (failure == 5) { operation = "note.clear"; args = {{"container", "unsupported"}, {"track", 0}, {"clip", 0}}; } if (failure == 6) { operation = "note.selection.quantize"; args = {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{0}}, {"grid", 12}, {"mode", "unsupported"}}; } if (failure >= 7) { operation = "audio.render"; args = {{"path", "fixture.wav"}}; args.insert(failure == 7 ? "bitDepth" : failure == 8 ? "sampleRate" : "compressionLevel", failure == 7 ? 8 : failure == 8 ? 123 : 2); } QString error; QVERIFY(!StudioCapabilities::validateArguments(operation, args, error)); QVERIFY(!error.isEmpty());
	}
	void nestedObjectsArraysAndAttributeMaps_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("late-required") << 0; QTest::newRow("late-unknown") << 1; QTest::newRow("late-type") << 2; QTest::newRow("clip-copy-required") << 3; QTest::newRow("nested-container") << 4; QTest::newRow("attribute-string") << 5; QTest::newRow("graph-number") << 6; QTest::newRow("slice-range") << 7; }
	void nestedObjectsArraysAndAttributeMaps()
	{
		QFETCH(int, failure); QString operation = "note.batch.add"; QJsonObject note{{"position", 0}, {"length", 24}, {"key", 60}}, last = note; if (failure == 0) { last.remove("key"); } if (failure == 1) { last.insert("unknown", true); } if (failure == 2) { last.insert("key", "60"); } QJsonObject args{{"track", 0}, {"clip", 0}, {"items", QJsonArray{note, last}}}; if (failure == 3 || failure == 4) { operation = "clip.selection.copy"; args = {{"items", QJsonArray{failure == 3 ? QJsonObject{{"clip", 0}} : QJsonObject{{"track", 0}, {"clip", 0}, {"container", "unsupported"}}}}}; } if (failure == 5) { operation = "effect.add"; args = {{"plugin", "missing"}, {"attributes", QJsonObject{{"vendor", 1}}}}; } if (failure == 6) { operation = "graph.set"; args = {{"graph", "wave"}, {"values", QJsonArray{0, "bad"}}}; } if (failure == 7) { operation = "instrument.slices.edit"; args = {{"track", 0}, {"points", QJsonArray{0, 1.1}}}; } QString error; QVERIFY(!StudioCapabilities::validateArguments(operation, args, error)); QVERIFY(error.contains("args.")); if (failure < 3) { QVERIFY(error.contains("items[1]")); }
	}
	void indexArraysEnforceDeclaredLengthAndDistinctNativeIndices_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("duplicate") << 0; QTest::newRow("signed-zero") << 1; QTest::newRow("negative") << 2; QTest::newRow("empty") << 3; QTest::newRow("too-many") << 4; }
	void indexArraysEnforceDeclaredLengthAndDistinctNativeIndices()
	{
		QFETCH(int, failure); QJsonArray notes{0, 1}; if (failure == 0) { notes = {0, 0}; } if (failure == 1) { notes = {0, -0.}; } if (failure == 2) { notes = {-1}; } if (failure == 3) { notes = {}; } if (failure == 4) { notes = {}; for (int index = 0; index < 4097; ++index) { notes.append(index); } } QString error; for (const auto operation : {QString("note.selection.remove"), QString("note.selection.edit")}) { QVERIFY(!StudioCapabilities::validateArguments(operation, {{"track", 0}, {"clip", 0}, {"notes", notes}}, error)); }
	}
	void validSpecializedSchemasKeepTheirNativeShapes_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("fractional-dynamics") << 0; QTest::newRow("digest") << 1; QTest::newRow("slices") << 2; QTest::newRow("string-attributes") << 3; QTest::newRow("unicode-name") << 4; QTest::newRow("full-signed-knife") << 5; QTest::newRow("zero-length-bound") << 6; }
	void validSpecializedSchemasKeepTheirNativeShapes()
	{
		QFETCH(int, mode); QString operation; QJsonObject args; if (mode == 0) { operation = "note.selection.edit"; args = {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{0}}, {"volume", 110.9}, {"panning", -20.9}}; } if (mode == 1) { operation = "audio.analyze"; args = {{"path", "fixture.wav"}, {"sourceSha256", QString(64, 'a')}}; } if (mode == 2) { operation = "instrument.slices.edit"; args = {{"track", 0}, {"points", QJsonArray{0, .5, 1}}}; } if (mode == 3) { operation = "effect.add"; args = {{"plugin", "missing"}, {"attributes", QJsonObject{{"vendor", ""}}}}; } if (mode == 4) { operation = "track.add"; args = {{"type", "instrument"}, {"name", QString::fromUtf8("\xf0\x9f\x8e\xb5")}}; } if (mode == 5) { operation = "note.selection.knife"; args = {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{0}}, {"fromPosition", std::numeric_limits<int>::min()}, {"fromKey", std::numeric_limits<int>::min()}, {"toPosition", std::numeric_limits<int>::max()}, {"toKey", std::numeric_limits<int>::max()}, {"keepLongest", true}}; } if (mode == 6) { operation = "note.selection.length"; args = {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{0}}, {"mode", "maximum"}, {"length", 0}}; } QString error; QVERIFY2(StudioCapabilities::validateArguments(operation, args, error), qPrintable(error));
	}
	void validationPreservesArgumentsAndDoesNotInsertDefaults()
	{
		const QJsonObject item{{"position", 0}, {"length", 24}, {"key", 60}}; const QJsonObject args{{"track", 0}, {"clip", 0}, {"items", QJsonArray{item, item}}}, before = args; QString error = "old error"; QVERIFY(StudioCapabilities::validateArguments("note.batch.add", args, error)); QCOMPARE(args, before); QVERIFY(error.isEmpty()); QVERIFY(!args.value("items").toArray()[0].toObject().contains("volume"));
	}
	void digestPatternsAndLengthArePreflighted_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("short") << 0; QTest::newRow("uppercase") << 1; QTest::newRow("non-hex") << 2; }
	void digestPatternsAndLengthArePreflighted()
	{
		QFETCH(int, failure); const QString digest(failure == 0 ? 63 : 64, failure == 0 ? 'a' : failure == 1 ? 'A' : 'z'); QString error; QVERIFY(!StudioCapabilities::validateArguments("audio.analyze", {{"path", "fixture.wav"}, {"sourceSha256", digest}}, error)); QVERIFY(error.contains("args.sourceSha256"));
	}
	void largePreflightHonorsCancellationWithoutPumpingEvents()
	{
		QJsonArray notes; for (int index = 0; index < 4096; ++index) { notes.append(index); } bool timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); int checks = 0; QString error; FileResourceIO::Scope outer({}, true); QVERIFY(!StudioCapabilities::validateArguments("note.selection.edit", {{"track", 0}, {"clip", 0}, {"notes", notes}}, error, [&] { return ++checks >= 7; })); QVERIFY(checks >= 7); QVERIFY(error.contains("cancelled")); QVERIFY(!timer);
	}
	void modelDependentAndCrossFieldAdmissionStillBelongsToNativeOperations()
	{
		QString error; QVERIFY(StudioCapabilities::validateArguments("note.clear", {{"track", 999999}, {"clip", 999999}}, error)); QVERIFY(StudioCapabilities::validateArguments("timeline.set", {{"begin", 100}, {"end", 99}, {"enabled", true}}, error)); QVERIFY(!Engine::getSong());
	}
};
QTEST_GUILESS_MAIN(StudioCapabilitySchemaTest)
#include "StudioCapabilitySchemaTest.moc"
