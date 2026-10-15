/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TEST_GIG_NATIVE_FIXTURE_H
#define LMMS_TEST_GIG_NATIVE_FIXTURE_H

#include <gig.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <cstdint>
#include <vector>
#include "GigResourceFixture.h"

namespace lmms::test::gigNative
{
inline QByteArray read(const QString& path)
{
	QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
inline bool write(const QString& path, const QByteArray& bytes)
{
	QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}

// The media writer is libgig itself. The small split variant moves its entire
// native wave pool unchanged, then points the one-entry native ptbl at file 1.
// It exercises real companion sample reads without needing a multi-GiB bank.
inline bool create(const QString& path, bool split, QString& error, unsigned bitDepth = 16, unsigned channels = 1, bool alternateProgram = false)
{
	if ((bitDepth != 16 && bitDepth != 24) || (channels != 1 && channels != 2))
	{
		error = "Unsupported native fixture shape"; return false;
	}
	try
	{
		gig::File bank;
		auto sample = bank.AddSample();
		sample->pInfo->Name = "Native constant calibration";
		sample->FormatTag = 1; sample->Channels = channels; sample->BitDepth = bitDepth;
		sample->SamplesPerSecond = 44100; sample->FrameSize = channels * (bitDepth / 8);
		constexpr std::size_t frames = 132300;
		sample->Resize(frames);
		for (int program = 0; program < (alternateProgram ? 2 : 1); ++program)
		{
			auto instrument = bank.AddInstrument();
			instrument->pInfo->Name = program ? "Native alternate bank" : "Native persistent bank";
			// MIDIBank is a derived view; libgig saves the coarse/fine fields.
			instrument->MIDIBankCoarse = 0; instrument->MIDIBankFine = program ? 4 : 2; instrument->MIDIProgram = program ? 11 : 7;
			auto region = instrument->AddRegion(); region->KeyRange.low = 0; region->KeyRange.high = 127;
			auto dimension = region->pDimensionRegions[0];
			if (!dimension) { error = "libgig did not create the fixture dimension region"; return false; }
			dimension->pSample = sample; dimension->PitchTrack = false;
			dimension->EG1PreAttack = 1000; dimension->EG1Attack = .001;
			dimension->EG1Decay1 = .005; dimension->EG1Sustain = 1000;
			dimension->EG1InfiniteSustain = true; dimension->EG1Release = .01;
		}
		bank.Save(path.toUtf8().constData());
		gig::file_offset_t written = 0;
		if (bitDepth == 16)
		{
			std::vector<std::int16_t> pcm(frames * channels);
			for (std::size_t index = 0; index < pcm.size(); ++index) { pcm[index] = channels == 2 && index % 2 ? -16384 : 8192; }
			written = sample->Write(pcm.data(), frames);
		}
		else
		{
			std::vector<std::uint8_t> pcm(frames * channels * 3);
			for (std::size_t index = 0; index < frames * channels; ++index)
			{
				const auto bits = channels == 2 && index % 2 ? 0xc00000u : 0x200000u;
				pcm[index * 3] = bits & 0xff; pcm[index * 3 + 1] = (bits >> 8) & 0xff; pcm[index * 3 + 2] = (bits >> 16) & 0xff;
			}
			written = sample->Write(pcm.data(), frames);
		}
		if (written != frames) { error = "libgig wrote incomplete fixture PCM"; return false; }
	}
	catch (...) { error = "libgig could not write the playable fixture"; return false; }
	if (!split) { error.clear(); return true; }
	const auto source = read(path);
	if (source.size() < 12 || source.left(4) != "RIFF" || source.mid(8, 4) != "DLS ")
	{
		error = "libgig fixture is not a small little-endian RIFF bank"; return false;
	}
	QByteArray chunks, wavePool, identifier; bool table = false;
	for (qint64 offset = 12; offset < source.size();)
	{
		if (source.size() - offset < 8) { error = "libgig fixture chunk header is incomplete"; return false; }
		const auto header = source.mid(offset, 8);
		const auto size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 4));
		const qint64 length = 8 + qint64(size) + (size & 1);
		if (length > source.size() - offset) { error = "libgig fixture chunk is incomplete"; return false; }
		auto body = source.mid(offset + 8, size);
		if (header.left(4) == "LIST" && body.left(4) == "wvpl")
		{
			if (!wavePool.isEmpty()) { error = "libgig fixture has multiple wave pools"; return false; }
			wavePool = source.mid(offset, length);
		}
		else if (header.left(4) == "ptbl")
		{
			if (table || body.size() < 12) { error = "libgig fixture wave table is invalid"; return false; }
			const auto tableHeader = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(body.constData()));
			const auto count = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(body.constData() + 4));
			if (count != 1 || tableHeader < 8 || tableHeader > quint32(body.size() - 4)
				|| (body.size() - tableHeader != 4 && body.size() - tableHeader != 8))
			{
				error = "libgig fixture requires a one-sample wave table"; return false;
			}
			// The 64-bit entry stores the high word first. For small split banks
			// that word is the companion file number, followed by the pool offset.
			body = body.left(tableHeader) + gigResource::uint32(1) + body.right(4);
			chunks += gigResource::chunk("ptbl", body); table = true;
		}
		else
		{
			if (header.left(4) == "dlid") { identifier = body; }
			chunks += source.mid(offset, length);
		}
		offset += length;
	}
	if (!table || wavePool.isEmpty() || identifier.size() != 16)
	{
		error = "libgig fixture lacks its wave pool, wave table or bank identifier"; return false;
	}
	const auto name = QFileInfo(path).completeBaseName() + ".gx01";
	const auto encoded = name.toUtf8();
	if (encoded.size() > 122) { error = "Fixture companion name is too long"; return false; }
	chunks += gigResource::chunk("xfil", gigResource::uint32(1) + encoded + '\0' + ".gx01" + QByteArray(122 - encoded.size(), '\0') + identifier);
	if (!write(QFileInfo(path).absoluteDir().filePath(name), gigResource::riff(gigResource::chunk("dlid", identifier) + wavePool))
		|| !write(path, gigResource::riff(chunks)))
	{
		error = "Could not publish the split native fixture"; return false;
	}
	error.clear(); return true;
}

inline bool invertPcm(QByteArray& bytes, int begin, int end)
{
	for (int offset = begin; offset < end;)
	{
		if (end - offset < 8) { return false; }
		const auto size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + offset + 4));
		if (qint64(size) + 8 + (size & 1) > end - offset) { return false; }
		const auto id = bytes.mid(offset, 4);
		if (id == "LIST")
		{
			if (size < 4 || !invertPcm(bytes, offset + 12, offset + 8 + size)) { return false; }
		}
		else if (id == "data")
		{
			if (size % 2) { return false; }
			for (int index = offset + 8; index < offset + 8 + int(size); index += 2)
			{
				const auto value = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(bytes.constData() + index));
				qToLittleEndian<qint16>(-value, reinterpret_cast<uchar*>(bytes.data() + index));
			}
		}
		offset += 8 + size + (size & 1);
	}
	return true;
}
inline bool invertCompanion(const QString& path)
{
	auto bytes = read(path);
	return bytes.size() >= 12 && bytes.left(4) == "RIFF" && invertPcm(bytes, 12, bytes.size()) && write(path, bytes);
}
} // namespace lmms::test::gigNative
#endif
