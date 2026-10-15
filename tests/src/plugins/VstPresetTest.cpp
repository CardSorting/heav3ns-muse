/* Native VST preset codec and callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <array>
#include "VstPresetAccess.h"
#include "VstPresetFile.h"

using namespace lmms;
namespace
{
using Bytes = std::vector<std::uint8_t>;
Bytes fromHex(const QByteArray& hex)
{
	const auto bytes = QByteArray::fromHex(hex);
	return {bytes.begin(), bytes.end()};
}
Bytes programBytes()
{
	// Independently specified big-endian FXP: high-bit plugin ID, two values.
	return fromHex("43636e4b 00000038 4678436b 00000001 fedcba98 00000009 00000002"
		"54657374000000000000000000000000000000000000000000000000 3e800000 3f400000");
}
Bytes chunkBytes()
{
	return fromHex("43636e4b 00000037 46504368 00000001 fedcba98 00000009 00000007"
		"54657374000000000000000000000000000000000000000000000000 00000003 00ff01");
}
Bytes bankBytes(bool opaque)
{
	auto bytes = fromHex(opaque
		? "43636e4b 0000009b 46424368 00000002 fedcba98 00000009 00000001 00000000"
		: "43636e4b 000000d4 4678426b 00000002 fedcba98 00000009 00000001 00000000");
	bytes.insert(bytes.end(), 124, 0);
	const auto payload = opaque ? fromHex("00000003 00ff01") : programBytes();
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	return bytes;
}
void word(Bytes& bytes, std::size_t offset, std::uint32_t value)
{
	for (int shift = 24; shift >= 0; shift -= 8) { bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift); }
}

struct NativeFixture
{
	int selected = 1, writes = 0, selections = 0, chunkReads = 0, chunkWrites = 0;
	bool failRead = false, refuseSelection = false, lastChunkProgram = false;
	std::string liveName = "Unsaved | sound";
	std::array<float, 2> live{.33f, .66f};
	Bytes received;
	VstPresetAccess access()
	{
		VstPresetAccess a;
		a.pluginId = 0xfedcba98; a.pluginVersion = 9; a.programCount = 3; a.parameterCount = 2;
		a.current = [&] { return selected; };
		a.select = [&](int index) {
			++selections;
			if (refuseSelection) { return; }
			selected = index; liveName = "Factory " + std::to_string(index); live = {.1f * index, .2f * index};
		};
		a.name = [&] { return liveName; };
		a.setName = [&](const std::string& name) { ++writes; liveName = name; };
		a.parameter = [&](int index) {
			if (failRead && selected == 2) { throw std::runtime_error("Native capture failed"); }
			return live.at(index);
		};
		a.setParameter = [&](int index, float value) { ++writes; live.at(index) = value; };
		a.chunk = [&](bool program) { ++chunkReads; lastChunkProgram = program; return Bytes{0, 255, 1}; };
		a.setChunk = [&](bool program, const Bytes& bytes) { ++chunkWrites; lastChunkProgram = program; received = bytes; };
		return a;
	}
};
}

class VstPresetTest : public QObject
{
	Q_OBJECT
private slots:
	void goldenFiles_data()
	{
		QTest::addColumn<int>("kind");
		QTest::newRow("parameter-program") << 0; QTest::newRow("chunk-program") << 1;
		QTest::newRow("parameter-bank") << 2; QTest::newRow("chunk-bank") << 3;
	}
	void goldenFiles()
	{
		QFETCH(int, kind);
		const auto bytes = kind == 0 ? programBytes() : kind == 1 ? chunkBytes() : bankBytes(kind == 3);
		const auto preset = decodeVstPreset(bytes);
		QCOMPARE(preset.pluginId, std::uint32_t{0xfedcba98}); QCOMPARE(preset.pluginVersion, std::uint32_t{9});
		QCOMPARE(preset.bank(), kind >= 2); QCOMPARE(preset.opaque(), kind == 1 || kind == 3);
		QVERIFY(encodeVstPreset(preset) == bytes);
		if (kind == 0) { QVERIFY(preset.parameters == std::vector<float>({.25f, .75f})); QCOMPARE(QString::fromStdString(preset.name), QString("Test")); }
		if (kind == 1) { QCOMPARE(preset.programCount, std::uint32_t{7}); }
		// Every short prefix must fail, including a partially present nested record.
		for (std::size_t length = 0; length < bytes.size(); ++length)
		{ QVERIFY_EXCEPTION_THROWN(decodeVstPreset(std::span<const std::uint8_t>(bytes.data(), length)), std::runtime_error); }
		auto zeroSize = bytes; word(zeroSize, 4, 0);
		if (kind == 2) { word(zeroSize, 160, 0); }
		QVERIFY(encodeVstPreset(decodeVstPreset(zeroSize)) == bytes);
	}

	void malformedFiles_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("magic") << 0;
		QTest::newRow("short-size") << 1;
		QTest::newRow("oversized-size") << 2;
		QTest::newRow("kind") << 3;
		QTest::newRow("version") << 4;
		QTest::newRow("parameter-count") << 5;
		QTest::newRow("nan") << 6;
		QTest::newRow("infinity") << 7;
		QTest::newRow("above-one") << 8;
		QTest::newRow("negative") << 9;
		QTest::newRow("trailing") << 10;
		QTest::newRow("chunk-length") << 11;
	}
	void malformedFiles()
	{
		QFETCH(int, change); auto bytes = programBytes();
		switch (change)
		{
			case 0: word(bytes, 0, 0); break;
			case 1: word(bytes, 4, 4); break;
			case 2: word(bytes, 4, 0xffffffff); break;
			case 3: word(bytes, 8, 0); break;
			case 4: word(bytes, 12, 3); break;
			case 5: word(bytes, 24, 0x7fffffff); break;
			case 6: word(bytes, 56, 0x7fc00000); break;
			case 7: word(bytes, 56, 0x7f800000); break;
			case 8: word(bytes, 56, 0x40000000); break;
			case 9: word(bytes, 56, 0xbe800000); break;
			case 10: bytes.push_back(0); break;
			case 11: bytes = chunkBytes(); word(bytes, 56, 0xffffffff); break;
		}
		QVERIFY_EXCEPTION_THROWN(decodeVstPreset(bytes), std::runtime_error);
	}

	void completeBankValidationPrecedesNativeWrites()
	{
		NativeFixture fixture; auto bytes = bankBytes(false); word(bytes, 172, 0x12345678);
		QVERIFY_EXCEPTION_THROWN(applyVstPreset(decodeVstPreset(bytes), fixture.access()), std::runtime_error);
		QCOMPARE(fixture.writes, 0); QCOMPARE(fixture.selections, 0);
		auto bank = decodeVstPreset(bankBytes(false)); bank.programCount = 2; bank.programs.push_back(bank.programs.front());
		bank.programs.back().parameters.pop_back();
		QVERIFY_EXCEPTION_THROWN(applyVstPreset(bank, fixture.access()), std::runtime_error);
		QCOMPARE(fixture.writes, 0); QCOMPARE(fixture.selections, 0);
		bank.programs.back().parameters.push_back(.75f); bank.currentProgram = 1;
		applyVstPreset(bank, fixture.access()); QCOMPARE(fixture.selected, 1); QCOMPARE(fixture.live[0], .25f); QCOMPARE(fixture.live[1], .75f);
	}

	void targetMismatch_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("plugin-id") << 0;
		QTest::newRow("parameters") << 1;
		QTest::newRow("unsupported-chunk") << 2;
		QTest::newRow("program-count") << 3;
		QTest::newRow("current-program") << 4;
		QTest::newRow("empty-chunk") << 5;
	}
	void targetMismatch()
	{
		QFETCH(int, change); NativeFixture fixture; auto target = fixture.access(); auto preset = decodeVstPreset(programBytes());
		if (change == 0) { target.pluginId = 0x12345678; }
		if (change == 1) { target.parameterCount = 1; }
		if (change == 2) { preset = decodeVstPreset(chunkBytes()); }
		if (change == 3) { preset = decodeVstPreset(bankBytes(false)); target.programCount = 0; }
		if (change == 4) { preset = decodeVstPreset(bankBytes(true)); target.usesChunks = true; preset.currentProgram = 3; }
		if (change == 5) { preset = decodeVstPreset(chunkBytes()); target.usesChunks = true; preset.chunk.clear(); }
		QVERIFY_EXCEPTION_THROWN(applyVstPreset(preset, target), std::runtime_error);
		QCOMPARE(fixture.writes, 0); QCOMPARE(fixture.selections, 0); QCOMPARE(fixture.chunkWrites, 0);
	}

	void exportPreservesUnsavedProgramAndRestoresOnFailure()
	{
		NativeFixture fixture; const auto name = fixture.liveName; const auto values = fixture.live;
		const auto program = captureVstPreset(fixture.access(), false);
		QCOMPARE(fixture.writes, 0); QCOMPARE(fixture.selections, 0); QVERIFY(program.parameters == std::vector<float>(values.begin(), values.end()));
		const auto bank = captureVstPreset(fixture.access(), true);
		QCOMPARE(bank.programs.size(), std::size_t{3}); QCOMPARE(bank.currentProgram.value(), 1);
		QVERIFY(bank.programs[1].parameters == program.parameters); QVERIFY(fixture.liveName == name); QVERIFY(fixture.live == values); QCOMPARE(fixture.selected, 1);
		fixture.failRead = true;
		QVERIFY_EXCEPTION_THROWN(captureVstPreset(fixture.access(), true), std::runtime_error);
		QVERIFY(fixture.liveName == name); QVERIFY(fixture.live == values); QCOMPARE(fixture.selected, 1);
		fixture.refuseSelection = true;
		QVERIFY_EXCEPTION_THROWN(captureVstPreset(fixture.access(), true), std::runtime_error);
	}

	void opaqueAndZeroProgramStateUseCorrectNativeCallbacks()
	{
		NativeFixture fixture; auto target = fixture.access(); target.usesChunks = true;
		auto program = captureVstPreset(target, false); QVERIFY(fixture.lastChunkProgram); QCOMPARE(fixture.selections, 0);
		applyVstPreset(program, target); QVERIFY(fixture.lastChunkProgram); QCOMPARE(fixture.chunkWrites, 1); QVERIFY(fixture.received == Bytes({0, 255, 1}));
		auto bank = captureVstPreset(target, true); QVERIFY(!fixture.lastChunkProgram); QCOMPARE(fixture.selections, 0);
		bank.programCount = 1; bank.currentProgram = 2; // Chunk metadata need not enumerate its internal programs.
		applyVstPreset(decodeVstPreset(encodeVstPreset(bank)), target); QVERIFY(!fixture.lastChunkProgram); QCOMPARE(fixture.selected, 2);
		target.usesChunks = false; target.programCount = 0;
		program = captureVstPreset(target, false); QCOMPARE(program.parameters.size(), std::size_t{2});
		bank = captureVstPreset(target, true); QVERIFY(bank.programs.empty()); QVERIFY(!bank.currentProgram);
		applyVstPreset(bank, target);
	}

	void versionOneAndMixedBankRecords()
	{
		auto bytes = bankBytes(false); word(bytes, 12, 1); word(bytes, 28, 0xffffffff);
		const auto oldBank = decodeVstPreset(bytes); QVERIFY(!oldBank.currentProgram);
		auto bank = oldBank; bank.programs.push_back(decodeVstPreset(chunkBytes())); bank.programCount = 2;
		const auto mixed = decodeVstPreset(encodeVstPreset(bank)); QVERIFY(mixed.programs[1].opaque());
		NativeFixture fixture; auto target = fixture.access(); target.usesChunks = true;
		applyVstPreset(mixed, target); QCOMPARE(fixture.selected, 1); QCOMPARE(fixture.chunkWrites, 1);
	}

	void partialBankPreservesTheUnimportedActiveEditBuffer()
	{
		NativeFixture fixture; const auto name = fixture.liveName; const auto values = fixture.live;
		auto bank = decodeVstPreset(bankBytes(false)); bank.version = 1; bank.currentProgram.reset();
		applyVstPreset(bank, fixture.access()); QCOMPARE(fixture.selected, 1);
		QVERIFY(fixture.live == values); QVERIFY(fixture.liveName == name);
	}

	void workerFileReadsAndOpenFailures()
	{
		const auto bytes = programBytes(); FILE* file = std::tmpfile(); QVERIFY(file);
		QCOMPARE(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size()); std::rewind(file);
		QVERIFY(readVstPresetFile(file) == bytes);
		QVERIFY_EXCEPTION_THROWN(readVstPresetFile(nullptr), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(writeVstPresetFile(nullptr, bytes), std::runtime_error);
	}
};

QTEST_GUILESS_MAIN(VstPresetTest)
#include "VstPresetTest.moc"
