/* Native MIDI export and independent Standard MIDI File decoding.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiExport.h"
#include "MidiFile.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"

using namespace lmms;

namespace
{
void require(bool condition) { if (!condition) { throw std::runtime_error("Malformed exported Standard MIDI File"); } }
struct Event
{
	std::uint64_t tick;
	int status, first = 0, second = 0;
	QByteArray payload;
};
struct Decoded
{
	int division;
	std::vector<std::vector<Event>> tracks;
};
// This reader does not reuse MidiFile's encoding helpers. It enforces chunk
// extents, four-byte VLQs, data-byte bounds and one terminal EOT per track.
class Reader
{
	QByteArray m_bytes;
public:
	explicit Reader(QByteArray bytes) : m_bytes(std::move(bytes)), limit(static_cast<int>(m_bytes.size())) { }
	int pos = 0, limit;
	std::uint8_t byte() { require(pos < limit); return static_cast<std::uint8_t>(m_bytes.at(pos++)); }
	std::uint32_t be(int count)
	{
		std::uint32_t value = 0;
		while (count--) { value = (value << 8) | byte(); }
		return value;
	}
	std::uint32_t vlq()
	{
		std::uint32_t value = 0;
		for (int i = 0; i < 4; ++i)
		{
			const auto next = byte(); value = (value << 7) | (next & 0x7f);
			if (!(next & 0x80)) { return value; }
		}
		throw std::runtime_error("Exported MIDI uses a VLQ longer than four bytes");
	}
	QByteArray bytes(std::uint32_t count)
	{
		require(count <= static_cast<std::uint32_t>(limit - pos));
		const auto result = m_bytes.mid(pos, static_cast<int>(count)); pos += static_cast<int>(count); return result;
	}
};

Decoded decode(const QByteArray& bytes)
{
	Reader reader(bytes);
	require(reader.bytes(4) == "MThd" && reader.be(4) == 6 && reader.be(2) == 1);
	const auto count = reader.be(2); require(count > 0);
	Decoded result{static_cast<int>(reader.be(2)), {}}; require(result.division > 0 && result.division < 0x8000);
	for (std::uint32_t track = 0; track < count; ++track)
	{
		require(reader.bytes(4) == "MTrk");
		const auto size = reader.be(4); require(size <= static_cast<std::uint32_t>(reader.limit - reader.pos));
		const auto fileEnd = reader.limit; reader.limit = reader.pos + static_cast<int>(size);
		std::vector<Event> events; std::uint64_t tick = 0; bool ended = false;
		while (reader.pos < reader.limit)
		{
			require(!ended); tick += reader.vlq(); Event event{tick, reader.byte()};
			if (event.status == 0xff)
			{
				event.first = reader.byte(); event.payload = reader.bytes(reader.vlq());
				if (event.first == 0x2f) { require(event.payload.isEmpty()); ended = true; }
			}
			else
			{
				require(event.status >= 0x80 && event.status < 0xf0);
				event.first = reader.byte(); require(event.first < 128);
				if ((event.status & 0xf0) != 0xc0 && (event.status & 0xf0) != 0xd0)
				{ event.second = reader.byte(); require(event.second < 128); }
			}
			events.push_back(event);
		}
		require(ended); result.tracks.push_back(std::move(events)); reader.limit = fileEnd;
	}
	require(reader.pos == reader.limit); return result;
}

QByteArray section(const auto& bytes) { return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())); }
Decoded decodeTrack(MidiFile::Track& track)
{
	MidiFile::Header header(1); header.writeToBuffer(); track.writeToBuffer();
	return decode(section(header.m_buffer) + section(track.m_buffer));
}
std::vector<Event> messages(const Decoded& file, int kind, std::size_t track = 0)
{
	std::vector<Event> result;
	for (const auto& event : file.tracks.at(track)) { if ((event.status & 0xf0) == kind) { result.push_back(event); } }
	return result;
}
QVector<quint64> times(const Decoded& file, int kind = 0x90, std::size_t track = 0)
{
	QVector<quint64> result;
	for (const auto& event : messages(file, kind, track)) { result.push_back(event.tick); }
	return result;
}
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
	return std::filesystem::path(path.toStdWString());
#else
	return std::filesystem::path(path.toStdString());
#endif
}
QByteArray read(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read MIDI test output"); }
	return file.readAll();
}
bool write(const QString& path, const QByteArray& bytes)
{
	QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
}

class MidiExportTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	Song* song() const { return Engine::getSong(); }
	PatternStore* store() const { return Engine::patternStore(); }
	InstrumentTrack* instrument(TrackContainer* container = nullptr)
	{ return static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, container ? container : song())); }
	PatternTrack* pattern() { return static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song())); }
	MidiClip* notes(InstrumentTrack* track, int position = 0)
	{
		auto clip = static_cast<MidiClip*>(track->createClip(position)); clip->clearNotes(); clip->setAutoResize(false); return clip;
	}
	MidiClip* cell(InstrumentTrack* row, int index = 0)
	{
		auto clip = static_cast<MidiClip*>(row->getClip(index)); clip->clearNotes(); clip->setAutoResize(false); return clip;
	}
	PatternClip* placement(PatternTrack* track, int position, int length, int offset = 0)
	{
		auto clip = static_cast<PatternClip*>(track->createClip(position));
		clip->setAutoResize(false); clip->changeLength(length); clip->setStartTimeOffset(offset); return clip;
	}
	bool exportTo(MidiExport& exporter, const QString& path, int tempo = 120, int pitch = 0)
	{ return exporter.tryExport(song()->tracks(), store()->tracks(), tempo, pitch, nativePath(path)); }
	Decoded exported(int pitch = 0)
	{
		MidiExport exporter; const auto path = m_configuration.filePath("notes.mid");
		if (!exportTo(exporter, path, 120, pitch)) { throw std::runtime_error("Native exporter returned failure"); }
		return decode(read(path));
	}
	void denseSong(int count = 20000)
	{
		auto clip = notes(instrument());
		for (int i = 0; i < count; ++i) { clip->addNote(Note(2, i * 3, 60), false); }
		clip->changeLength(count * 3);
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256");
		NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		song()->clearProject(); song()->clearErrors();
		song()->getTimeSigModel().setNumerator(4); song()->getTimeSigModel().setDenominator(4);
	}
	void cleanup() { song()->stop(); song()->clearProject(); }

	void denseTrackRetainsEveryNoteAndRelease()
	{
		denseSong(); const auto file = exported();
		QVERIFY(read(m_configuration.filePath("notes.mid")).size() > 150000);
		QCOMPARE(file.division, 48); QCOMPARE(file.tracks.size(), std::size_t(1));
		const auto on = messages(file, 0x90), off = messages(file, 0x80);
		QCOMPARE(on.size(), std::size_t(20000)); QCOMPARE(off.size(), on.size());
		for (std::size_t i = 0; i < on.size(); ++i)
		{
			QCOMPARE(on[i].tick, i * 3); QCOMPARE(off[i].tick, i * 3 + 2);
			QCOMPARE(on[i].first, 60); QCOMPARE(off[i].first, on[i].first); QCOMPARE(on[i].second, 64);
		}
		QCOMPARE(file.tracks[0].back().tick, std::uint64_t(59999));
	}

	void writerIsRepeatableAndOrdersSimultaneousEvents()
	{
		MidiFile::Track track; track.setChannel(2); track.addPatch({257, 43}, 0); track.addTempo(123, 0); track.addName("Order", 0);
		track.addNote(60, 80, 0, .25); track.setChannel(3); track.addNote(60, 81, .25, .25);
		const auto file = decodeTrack(track); const auto first = section(track.m_buffer);
		track.writeToBuffer(); QCOMPARE(section(track.m_buffer), first);
		const auto& events = file.tracks.front();
		QCOMPARE(events[0].status, 0xff); QCOMPARE(events[0].first, 3);
		QCOMPARE(events[1].first, 0x51); QCOMPARE(events[1].payload.toHex(), QByteArray("07717d"));
		QCOMPARE(events[2].status, 0xb2); QCOMPARE(events[2].first, 0); QCOMPARE(events[2].second, 2);
		QCOMPARE(events[3].status, 0xb2); QCOMPARE(events[3].first, 32); QCOMPARE(events[3].second, 1);
		QCOMPARE(events[4].status, 0xc2); QCOMPARE(events[4].first, 43);
		QCOMPARE(events[5].status, 0x92); QCOMPARE(events[5].tick, std::uint64_t(0));
		QCOMPARE(events[6].status, 0x82); QCOMPARE(events[6].tick, std::uint64_t(12));
		QCOMPARE(events[7].status, 0x93); QCOMPARE(events[7].tick, std::uint64_t(12));
		QCOMPARE(events[8].status, 0x83); QCOMPARE(events[8].tick, std::uint64_t(24));
		MidiFile::Header header(1); header.writeToBuffer(); const auto bytes = section(header.m_buffer);
		header.writeToBuffer(); QCOMPARE(section(header.m_buffer), bytes);
	}

	void longGapsUseBoundedVariableLengthQuantities_data()
	{
		QTest::addColumn<int>("tick"); QTest::newRow("largest-delta") << 0x0fffffff;
		QTest::newRow("split-delta") << 0x10000000; QTest::newRow("native-limit") << 0x7ffffffe;
	}
	void longGapsUseBoundedVariableLengthQuantities()
	{
		QFETCH(int, tick); MidiFile::Track track; track.addNote(60, 100, tick / 48., 1. / 48.);
		const auto file = decodeTrack(track);
		QCOMPARE(times(file), QVector<quint64>{static_cast<quint64>(tick)});
		QCOMPARE(times(file, 0x80), QVector<quint64>{static_cast<quint64>(tick) + 1});
	}

	void invalidWriterValuesAreRejected_data()
	{
		QTest::addColumn<int>("kind");
		const char* names[] = {"channel", "key", "velocity", "zero-duration", "negative-duration", "nan-duration",
			"negative-time", "nan-time", "end-overflow", "zero-tempo", "too-slow-tempo", "program", "bank",
			"no-tracks", "too-many-tracks", "zero-division", "smpte-division"};
		for (int i = 0; i < 17; ++i) { QTest::newRow(names[i]) << i; }
	}
	void invalidWriterValuesAreRejected()
	{
		QFETCH(int, kind); MidiFile::Track track; const double nan = std::numeric_limits<double>::quiet_NaN();
		const auto invalid = [&]
		{
			switch (kind)
			{
				case 0: track.setChannel(16); break;
				case 1: track.addNote(128, 100, 0, 1); break;
				case 2: track.addNote(60, 128, 0, 1); break;
				case 3: track.addNote(60, 100, 0, 0); break;
				case 4: track.addNote(60, 100, 0, -1); break;
				case 5: track.addNote(60, 100, 0, nan); break;
				case 6: track.addNote(60, 100, -1, 1); break;
				case 7: track.addNote(60, 100, nan, 1); break;
				case 8: track.addNote(60, 100, std::numeric_limits<tick_t>::max() / 48., 1); break;
				case 9: track.addTempo(0, 0); break;
				case 10: track.addTempo(1, 0); break;
				case 11: track.addPatch({0, 128}, 0); break;
				case 12: track.addPatch({16384, 0}, 0); break;
				case 13: { MidiFile::Header header(0); header.writeToBuffer(); break; }
				case 14: { MidiFile::Header header(65536); header.writeToBuffer(); break; }
				case 15: { MidiFile::Header header(1, 0); header.writeToBuffer(); break; }
				case 16: { MidiFile::Header header(1, 0x8000); header.writeToBuffer(); break; }
			}
		};
		QVERIFY_EXCEPTION_THROWN(invalid(), std::out_of_range);
		track.addNote(60, 80, 0, 1); QCOMPARE(messages(decodeTrack(track), 0x90).size(), std::size_t(1));
	}

	void emptySongExportsValidConductor()
	{
		const auto file = exported(); QCOMPARE(file.tracks.size(), std::size_t(1)); QCOMPARE(file.division, 48);
		QVERIFY(times(file).isEmpty()); QCOMPARE(file.tracks[0].size(), std::size_t(3));
		QCOMPARE(file.tracks[0][0].payload, QByteArray("LMMS"));
		QCOMPARE(file.tracks[0][1].payload.toHex(), QByteArray("07a120"));
	}

	void pitchAndVelocityUseNativeTrackModels_data()
	{
		QTest::addColumn<int>("base"); QTest::addColumn<int>("master"); QTest::addColumn<bool>("useMaster");
		QTest::addColumn<int>("key"); QTest::addColumn<int>("volume"); QTest::addColumn<float>("trackVolume");
		QTest::addColumn<int>("pitch"); QTest::addColumn<int>("velocity");
		QTest::newRow("default-unloaded") << 69 << 0 << true << 60 << 100 << 100.f << 60 << 64;
		QTest::newRow("low-pitch") << 127 << -12 << true << 0 << 100 << 100.f << 0 << 64;
		QTest::newRow("high-pitch") << 0 << 12 << true << 127 << 100 << 100.f << 127 << 64;
		QTest::newRow("high-volume") << 69 << 0 << true << 60 << 200 << 200.f << 60 << 127;
		QTest::newRow("track-volume") << 69 << 0 << true << 60 << 100 << 25.f << 60 << 16;
		QTest::newRow("fractional-volume") << 69 << 0 << true << 60 << 100 << 37.5f << 60 << 24;
		QTest::newRow("master-disabled") << 69 << 12 << false << 60 << 100 << 100.f << 60 << 64;
		QTest::newRow("silent-note") << 69 << 0 << true << 60 << 0 << 100.f << 60 << 0;
	}
	void pitchAndVelocityUseNativeTrackModels()
	{
		QFETCH(int, base); QFETCH(int, master); QFETCH(bool, useMaster); QFETCH(int, key); QFETCH(int, volume);
		QFETCH(float, trackVolume); QFETCH(int, pitch); QFETCH(int, velocity);
		auto track = instrument(); QVERIFY(!track->instrument()); track->baseNoteModel()->setValue(base);
		track->volumeModel()->setValue(trackVolume); track->useMasterPitchModel()->setValue(useMaster);
		auto clip = notes(track); clip->addNote(Note(37, 5, key, volume), false); clip->changeLength(192);
		const auto file = exported(master); const auto on = messages(file, 0x90);
		if (velocity == 0) { QVERIFY(on.empty()); QVERIFY(messages(file, 0x80).empty()); }
		else
		{
			QCOMPARE(on.size(), std::size_t(1)); QCOMPARE(on[0].first, pitch); QCOMPARE(on[0].second, velocity);
			QCOMPARE(on[0].tick, std::uint64_t(5)); QCOMPARE(times(file, 0x80), QVector<quint64>{42});
		}
		QCOMPARE(clip->notes().front()->key(), key); QCOMPARE(clip->notes().front()->length().getTicks(), 37);
	}

	void songClipTrimsUseSignedNativeOffsets_data()
	{
		QTest::addColumn<int>("offset"); QTest::addColumn<QVector<quint64>>("onsets");
		QTest::newRow("trim-leading") << -12 << QVector<quint64>{96, 124};
		QTest::newRow("trim-trailing") << 0 << QVector<quint64>{96, 136};
		QTest::newRow("delay") << 12 << QVector<quint64>{108};
	}
	void songClipTrimsUseSignedNativeOffsets()
	{
		QFETCH(int, offset); QFETCH(QVector<quint64>, onsets); auto clip = notes(instrument(), 96);
		clip->addNote(Note(60, 0, 60), false); clip->addNote(Note(60, 40, 64), false);
		clip->addNote(Note(12, 80, 67), false); clip->addNote(Note(0, 20, 70), false);
		clip->changeLength(48); clip->setStartTimeOffset(offset); const auto file = exported();
		QCOMPARE(times(file), onsets); QCOMPARE(times(file, 0x80), QVector<quint64>(onsets.size(), 144));
		QCOMPARE(clip->notes().size(), std::size_t(4)); QCOMPARE(clip->startTimeOffset().getTicks(), offset);
	}

	void mutedMaterialDoesNotExportNotes_data()
	{
		QTest::addColumn<int>("kind");
		QTest::newRow("song-track") << 0; QTest::newRow("song-clip") << 1;
		QTest::newRow("pattern-track") << 2; QTest::newRow("pattern-placement") << 3;
		QTest::newRow("pattern-row") << 4; QTest::newRow("pattern-cell") << 5;
	}
	void mutedMaterialDoesNotExportNotes()
	{
		QFETCH(int, kind); MidiClip* clip = nullptr;
		if (kind < 2)
		{
			auto track = instrument(); clip = notes(track);
			if (kind == 0) { track->setMuted(true); } else { clip->setMuted(true); }
		}
		else
		{
			auto column = pattern(); auto row = instrument(store()); clip = cell(row);
			auto arranged = placement(column, 0, 384);
			if (kind == 2) { column->setMuted(true); } else if (kind == 3) { arranged->setMuted(true); }
			else if (kind == 4) { row->setMuted(true); } else { clip->setMuted(true); }
		}
		clip->addNote(Note(12, 0, 60), false); clip->changeLength(192);
		const auto file = exported(); QVERIFY(times(file).isEmpty()); QVERIFY(times(file, 0x80).isEmpty());
	}

	void patternIndicesAbove255KeepTheirOwnContent()
	{
		PatternTrack* last = nullptr;
		for (int i = 0; i < 257; ++i) { last = pattern(); }
		auto row = instrument(store()); auto first = cell(row); first->addNote(Note(12, 0, 36), false); first->changeLength(192);
		auto selected = cell(row, 256); selected->addNote(Note(12, 0, 72), false); selected->changeLength(192);
		placement(last, 384, 384); const auto file = exported();
		QCOMPARE(times(file), (QVector<quint64>{384, 576}));
		for (const auto& event : messages(file, 0x90)) { QCOMPARE(event.first, 72); }
		QCOMPARE(row->numOfClips(), 257);
	}

	void patternPeriodUsesEveryNativeRowAndCurrentMeter_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator"); QTest::addColumn<int>("period");
		QTest::newRow("four-four") << 4 << 4 << 576; QTest::newRow("three-four") << 3 << 4 << 432;
		QTest::newRow("seven-eight") << 7 << 8 << 504;
	}
	void patternPeriodUsesEveryNativeRowAndCurrentMeter()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); QFETCH(int, period);
		song()->getTimeSigModel().setNumerator(numerator); song()->getTimeSigModel().setDenominator(denominator);
		auto column = pattern(); auto row = instrument(store()); auto longRow = instrument(store());
		auto shortCell = cell(row); shortCell->addNote(Note(12, 12, 60), false); shortCell->changeLength(TimePos::ticksPerBar());
		cell(longRow)->changeLength(TimePos::ticksPerBar() * 2 + 1); longRow->setMuted(true);
		placement(column, 0, period * 2); const auto file = exported();
		QCOMPARE(times(file), (QVector<quint64>{12, static_cast<quint64>(period) + 12}));
		QVERIFY(times(file, 0x90, 1).isEmpty()); QCOMPARE(file.division, 48);
	}

	void patternOffsetsPreserveLoopPhase_data()
	{
		QTest::addColumn<int>("offset"); QTest::addColumn<QVector<quint64>>("onsets");
		QTest::newRow("zero") << 0 << QVector<quint64>{384, 456, 576, 648};
		QTest::newRow("advance") << -24 << QVector<quint64>{432, 552, 624, 744};
		QTest::newRow("delay") << 24 << QVector<quint64>{408, 480, 600, 672};
		QTest::newRow("advance-over-loop") << -216 << QVector<quint64>{432, 552, 624, 744};
		QTest::newRow("delay-over-loop") << 216 << QVector<quint64>{408, 480, 600, 672};
	}
	void patternOffsetsPreserveLoopPhase()
	{
		QFETCH(int, offset); QFETCH(QVector<quint64>, onsets); auto column = pattern(); auto clip = cell(instrument(store()));
		clip->addNote(Note(12, 0, 60), false); clip->addNote(Note(12, 72, 64), false); clip->changeLength(192);
		placement(column, 384, 384, offset); QCOMPARE(times(exported()), onsets);
	}

	void overlappingPlacementsResumeEarlierPhase_data()
	{
		QTest::addColumn<int>("start"); QTest::addColumn<int>("length"); QTest::addColumn<QVector<quint64>>("onsets");
		QTest::newRow("nested") << 96 << 192 << QVector<quint64>{0, 48, 120, 168, 384, 432};
		QTest::newRow("extends") << 96 << 672 << QVector<quint64>{0, 48, 120, 168, 312, 360, 504, 552, 696, 744};
		QTest::newRow("equal-start") << 0 << 192 << QVector<quint64>{24, 72, 192, 240, 384, 432};
	}

	void patternCellOffsetsKeepNativeLocalOnsets_data()
	{
		QTest::addColumn<int>("offset"); QTest::addColumn<QVector<quint64>>("onsets"); QTest::addColumn<QVector<quint64>>("releases");
		QTest::newRow("zero") << 0 << QVector<quint64>{0} << QVector<quint64>{36};
		QTest::newRow("advanced-cell-retrigger") << -12 << QVector<quint64>{0, 12} << QVector<quint64>{48, 48};
		QTest::newRow("positive-cell-crop") << 12 << QVector<quint64>{0} << QVector<quint64>{24};
	}
	void patternCellOffsetsKeepNativeLocalOnsets()
	{
		QFETCH(int, offset); QFETCH(QVector<quint64>, onsets); QFETCH(QVector<quint64>, releases);
		auto column = pattern(); auto clip = cell(instrument(store())); clip->addNote(Note(60, 0, 60), false);
		clip->changeLength(36); clip->setStartTimeOffset(offset); placement(column, 0, 192);
		const auto file = exported(); QCOMPARE(times(file), onsets); QCOMPARE(times(file, 0x80), releases);
	}

	void trimmedStepNotesRestartOnlyWhileOverlappingTheBoundary()
	{
		auto clip = notes(instrument(), 96);
		Note crossing(24, 0, 60); crossing.setType(Note::Type::Step); clip->addNote(crossing, false);
		Note ended(12, 0, 64); ended.setType(Note::Type::Step); clip->addNote(ended, false);
		clip->changeLength(48); clip->setStartTimeOffset(-12);
		const auto file = exported(); QCOMPARE(times(file), QVector<quint64>{96});
		QCOMPARE(times(file, 0x80), QVector<quint64>{144}); QCOMPARE(messages(file, 0x90)[0].first, 60);
	}
	void overlappingPlacementsResumeEarlierPhase()
	{
		QFETCH(int, start); QFETCH(int, length); QFETCH(QVector<quint64>, onsets);
		auto column = pattern(); auto clip = cell(instrument(store()));
		clip->addNote(Note(180, 0, 60), false); clip->addNote(Note(12, 48, 64), false); clip->changeLength(192);
		placement(column, 0, 576); placement(column, start, length, 24); const auto file = exported();
		QCOMPARE(times(file), onsets);
		// A regular note already triggered by a placement can ring beyond its end.
		if (start == 96 && length == 192) { QVERIFY(times(file, 0x80).contains(300)); }
	}

	void stepDurationsAreBoundedAndDisabledNotesStaySilent_data()
	{ QTest::addColumn<bool>("patternMode"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void stepDurationsAreBoundedAndDisabledNotesStaySilent()
	{
		QFETCH(bool, patternMode); MidiClip* clip;
		if (patternMode) { auto column = pattern(); clip = cell(instrument(store())); placement(column, 0, 192); }
		else { clip = notes(instrument()); }
		const std::tuple<int, int, int> steps[] = {{0, 60, 1}, {48, 62, 1}, {48, 64, 1}, {72, 65, 0}, {180, 69, 1}};
		for (const auto [pos, key, length] : steps)
		{
			Note note(length, pos, key); note.setType(Note::Type::Step); clip->addNote(note, false);
		}
		clip->addNote(Note(12, 96, 66, 0), false); clip->addNote(Note(12, 144, 67), false); clip->changeLength(192);
		const auto file = exported(); QCOMPARE(times(file), (QVector<quint64>{0, 48, 48, 144, 180}));
		QCOMPARE(times(file, 0x80), (QVector<quint64>{48, 144, 144, 156, 192}));
		QCOMPARE(clip->notes().size(), std::size_t(7));
	}

	void repeatedExportsResetStateAndPreserveUnicode()
	{
		auto column = pattern(); auto patternCell = cell(instrument(store())); patternCell->addNote(Note(12, 0, 72), false);
		patternCell->changeLength(192); placement(column, 0, 384);
		auto track = instrument(); track->setName(QStringLiteral("\u97f3\u697d"));
		auto clip = notes(track); clip->addNote(Note(12, 1, 60), false); clip->changeLength(192);
		MidiExport exporter; const auto path = m_configuration.filePath(QStringLiteral("\u97f3\u697d.mid"));
		QVERIFY(exportTo(exporter, path)); const auto first = read(path);
		QVERIFY(exportTo(exporter, path)); QCOMPARE(read(path), first);
		QCOMPARE(decode(first).tracks[0][0].payload, track->name().toUtf8());
		QVERIFY(!exportTo(exporter, path, 0)); QCOMPARE(read(path), first);
		song()->clearProject(); clip = notes(instrument()); clip->addNote(Note(12, 0, 65), false); clip->changeLength(192);
		QVERIFY(exportTo(exporter, path)); MidiExport fresh;
		const auto comparison = m_configuration.filePath("fresh.mid"); QVERIFY(exportTo(fresh, comparison));
		QCOMPARE(read(path), read(comparison)); QCOMPARE(messages(decode(read(path)), 0x90)[0].status, 0x90);
	}

	void cancellationNeverPublishesAPartialFile_data()
	{
		QTest::addColumn<int>("percent"); QTest::newRow("initial") << 0; QTest::newRow("reading") << 25;
		QTest::newRow("encoding") << 75; QTest::newRow("near-commit") << 99;
	}
	void cancellationNeverPublishesAPartialFile()
	{
		QFETCH(int, percent); denseSong(); MidiExport exporter; const auto path = m_configuration.filePath("cancelled.mid");
		int total = 0;
		{ FileResourceIO::Scope work([&] { ++total; return false; }); QVERIFY(exportTo(exporter, path)); }
		QVERIFY(total > 20); const QByteArray sentinel("existing destination"); QVERIFY(write(path, sentinel));
		int checks = 0; const int stop = std::max(1, total * percent / 100);
		{ FileResourceIO::Scope work([&] { return ++checks >= stop; }); QVERIFY(!exportTo(exporter, path)); }
		QCOMPARE(read(path), sentinel); QVERIFY(exportTo(exporter, path));
		QCOMPARE(messages(decode(read(path)), 0x90).size(), std::size_t(20000));
	}

	void exportSuppressesEventReentryAndRejectsForeignThreads()
	{
		auto clip = notes(instrument()); clip->addNote(Note(12, 0, 60), false); clip->changeLength(192);
		MidiExport exporter; const auto path = m_configuration.filePath("owner.mid"); bool changed = false;
		QTimer::singleShot(0, clip, [&] { clip->addNote(Note(12, 24, 64), false); changed = true; });
		{ FileResourceIO::Scope work([] { return false; }, true); QVERIFY(exportTo(exporter, path)); }
		QVERIFY(!changed); QCOMPARE(messages(decode(read(path)), 0x90).size(), std::size_t(1));
		QCoreApplication::processEvents(); QVERIFY(changed);
		const auto previous = read(path); bool result = true;
		std::thread foreign([&] { result = exportTo(exporter, path); }); foreign.join();
		QVERIFY(!result); QCOMPARE(read(path), previous);
	}

	void sparsePatternsAndInvalidDestinationsDoNotMutateTheProject()
	{
		pattern(); auto second = pattern(); auto row = instrument(store());
		auto clip = cell(row); clip->addNote(Note(12, 0, 60), false); clip->changeLength(192);
		delete row->getClips().back(); QCOMPARE(row->numOfClips(), 1); placement(second, 0, 384);
		QVERIFY(times(exported()).isEmpty()); QCOMPARE(row->numOfClips(), 1);
		MidiExport exporter; QVERIFY(!exportTo(exporter, m_configuration.filePath("absent-directory/out.mid")));
		QCOMPARE(row->numOfClips(), 1);
	}
};

QTEST_GUILESS_MAIN(MidiExportTest)
#include "MidiExportTest.moc"
