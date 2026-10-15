/* Native MIDI input framing, model ownership and cancellation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiImport.h"
#include "MidiImportData.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

namespace lmms
{
namespace
{
QByteArray raw(std::initializer_list<int> values)
{ QByteArray bytes; for (auto value : values) { bytes.append(static_cast<char>(value)); } return bytes; }
QByteArray integer(std::uint32_t value, int count, bool little = false)
{
	QByteArray bytes;
	for (int i = 0; i < count; ++i) { bytes.append(static_cast<char>(value >> (8 * (little ? i : count - 1 - i)))); }
	return bytes;
}
QByteArray vlq(std::uint32_t value)
{
	QByteArray bytes(1, static_cast<char>(value & 0x7f));
	while ((value >>= 7) != 0) { bytes.prepend(static_cast<char>((value & 0x7f) | 0x80)); }
	return bytes;
}
QByteArray fixtureEvent(std::uint32_t delta, std::initializer_list<int> bytes) { return vlq(delta) + raw(bytes); }
QByteArray eot() { return raw({0, 0xff, 0x2f, 0}); }
QByteArray chunk(const char* id, const QByteArray& bytes, bool riff = false)
{ return QByteArray(id, 4) + integer(bytes.size(), 4, riff) + bytes + ((riff && (bytes.size() & 1)) ? QByteArray(1, '\0') : QByteArray{}); }
QByteArray smf(const std::vector<QByteArray>& tracks, int division = 48)
{
	QByteArray result = "MThd"; result += integer(6, 4) + integer(tracks.size() == 1 ? 0 : 1, 2)
		+ integer(tracks.size(), 2) + integer(division, 2);
	for (const auto& track : tracks) { result += chunk("MTrk", track); }
	return result;
}
QByteArray riff(const QByteArray& midi)
{
	const auto payload = QByteArray("RMID") + chunk("JUNK", "odd", true) + chunk("data", midi, true) + chunk("JUNK", "last", true);
	return QByteArray("RIFF") + integer(payload.size(), 4, true) + payload;
}
QByteArray melody(int key = 60)
{ return fixtureEvent(0, {0x90, key, 64}) + fixtureEvent(48, {0x80, key, 0}) + eot(); }
QByteArray dense(int count)
{
	QByteArray track;
	for (int i = 0; i < count; ++i) { track += fixtureEvent(i ? 1 : 0, {0x90, 60, 64}) + fixtureEvent(2, {0x80, 60, 0}); }
	return smf({track + eot()});
}
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
class Container final : public TrackContainer
{
public:
	Container() { setType(Type::Song); }
	QString nodeName() const override { return "midi-import-fixture-container"; }
};
}

class MidiImportTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	Song* song() const { return Engine::getSong(); }
	QString path() const { return m_configuration.filePath(QStringLiteral("\u97f3\u697d.mid")); }
	bool import(const QByteArray& bytes, TrackContainer* container = nullptr, int pattern = -1)
	{
		if (!write(path(), bytes)) { throw std::runtime_error("Cannot write MIDI import fixture"); }
		MidiImport importer(path()); return importer.tryImport(container ? container : song(), pattern);
	}
	QByteArray admitted(const QByteArray& bytes)
	{
		if (!write(path(), bytes)) { throw std::runtime_error("Cannot write MIDI import fixture"); }
		QFile input(path()); if (!input.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot open MIDI fixture"); }
		return readMidiImportData(input);
	}
	std::vector<InstrumentTrack*> instruments(TrackContainer* container = nullptr) const
	{
		std::vector<InstrumentTrack*> result;
		for (auto track : (container ? container : song())->tracks()) { if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { result.push_back(instrument); } }
		return result;
	}
	std::vector<AutomationClip*> curves(AutomatableModel* model, TrackContainer* container = nullptr) const
	{
		std::vector<AutomationClip*> result;
		for (auto track : (container ? container : song())->tracks())
		{
			for (auto item : track->getClips())
			{
				auto clip = dynamic_cast<AutomationClip*>(item); if (!clip) { continue; }
				for (const auto& target : clip->objects()) { if (target == model) { result.push_back(clip); break; } }
			}
		}
		return result;
	}
	InstrumentTrack* existing()
	{
		auto track = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song())); track->setName("Existing composition");
		auto clip = static_cast<MidiClip*>(track->createClip(192)); clip->clearNotes(); clip->addNote(Note(37, 5, 72), false); return track;
	}
	void verifyExisting(InstrumentTrack* track)
	{
		QVERIFY(std::find(song()->tracks().begin(), song()->tracks().end(), track) != song()->tracks().end());
		QCOMPARE(track->name(), QString("Existing composition")); QCOMPARE(track->numOfClips(), 1);
		auto clip = static_cast<MidiClip*>(track->getClips()[0]); QCOMPARE(clip->startPosition().getTicks(), 192);
		QCOMPARE(clip->notes().size(), std::size_t(1)); QCOMPARE(clip->notes()[0]->key(), 72);
		QCOMPARE(clip->notes()[0]->pos().getTicks(), 5); QCOMPARE(clip->notes()[0]->length().getTicks(), 37);
	}
	PatternStore* store() const { return Engine::patternStore(); }
	InstrumentTrack* patternFixture(int count)
	{
		for (int i = 0; i < count; ++i) { Track::create(Track::Type::Pattern, song()); }
		auto row = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, store()));
		row->setName("Existing pattern row");
		for (auto item : row->getClips())
		{
			auto cell = static_cast<MidiClip*>(item); cell->clearNotes(); cell->addNote(Note(19, 3, 72), false);
		}
		return row;
	}
	void verifyColumns()
	{
		for (auto row : store()->tracks())
		{
			QCOMPARE(row->numOfClips(), store()->numOfPatterns());
			for (int column = 0; column < row->numOfClips(); ++column)
			{ QCOMPARE(row->getClips()[column]->startPosition().getTicks(), column * TimePos::ticksPerBar()); }
		}
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const
	{ return {{"id", id}, {"op", op}, {"args", args}}; }
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		song()->clearProject(); song()->clearErrors();
		song()->getTimeSigModel().setNumerator(4); song()->getTimeSigModel().setDenominator(4);
	}
	void cleanup() { song()->stop(); song()->clearProject(); }

	void validFramingPreservesTheExactSmfPayload_data()
	{
		QTest::addColumn<QByteArray>("input"); QTest::addColumn<QByteArray>("expected");
		const auto plain = smf({melody()}); QTest::newRow("plain") << plain << plain;
		const auto odd = smf({fixtureEvent(0, {0xff, 3, 1, 'X'}) + melody()});
		QTest::newRow("riff-odd-payload-and-neighbor-chunks") << riff(odd) << odd;
		const auto running = smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(0, {64, 80})
			+ fixtureEvent(48, {0x80, 60, 0}) + fixtureEvent(0, {64, 0}) + eot()});
		QTest::newRow("running-status") << running << running;
		const auto sysex = smf({fixtureEvent(0, {0xf0, 2, 0x7e, 0xf7}) + fixtureEvent(0, {0xf7, 1, 0xf8}) + melody()});
		QTest::newRow("sysex-and-escape") << sysex << sysex;
		auto extended = plain; extended.replace(4, 4, integer(8, 4)); extended.insert(14, "ab"); extended.insert(16, chunk("JUNK", "alien"));
		QTest::newRow("extended-header-and-unknown-chunk") << extended << extended;
		const auto smpte = smf({melody()}, 0xe728); QTest::newRow("smpte-division") << smpte << smpte;
	}
	void validFramingPreservesTheExactSmfPayload()
	{ QFETCH(QByteArray, input); QFETCH(QByteArray, expected); QCOMPARE(admitted(input), expected); QVERIFY(song()->isEmpty()); }

	void malformedRecognizedInputCreatesNoNativeTracks_data()
	{
		QTest::addColumn<QByteArray>("bytes"); const auto plain = smf({melody()});
		QTest::newRow("truncated-header") << QByteArray("MThd");
		auto wrong = plain; wrong.replace(4, 4, integer(5, 4)); QTest::newRow("short-header") << wrong;
		wrong = plain; wrong.replace(10, 2, integer(2, 2)); QTest::newRow("format-zero-many-tracks") << wrong;
		wrong = plain; wrong.replace(8, 2, integer(1, 2)); wrong.replace(10, 2, integer(2, 2)); QTest::newRow("missing-track") << wrong;
		wrong = plain + chunk("MTrk", eot()); QTest::newRow("extra-track") << wrong;
		wrong = plain; wrong.replace(12, 2, integer(0, 2)); QTest::newRow("zero-division") << wrong;
		wrong = plain; wrong.replace(12, 2, integer(0x8001, 2)); QTest::newRow("invalid-smpte") << wrong;
		wrong = plain; wrong.replace(18, 4, integer(0xffffffff, 4)); QTest::newRow("oversized-track") << wrong;
		QTest::newRow("missing-eot") << smf({fixtureEvent(0, {0x90, 60, 64})});
		QTest::newRow("events-after-eot") << smf({eot() + melody()});
		QTest::newRow("long-vlq") << smf({raw({0x81, 0x80, 0x80, 0x80, 0, 0x90, 60, 64}) + eot()});
		QTest::newRow("truncated-note") << smf({raw({0, 0x90, 60})});
		QTest::newRow("status-in-data") << smf({fixtureEvent(0, {0x90, 60, 0x80}) + eot()});
		QTest::newRow("orphan-running-status") << smf({fixtureEvent(0, {60, 64}) + eot()});
		QTest::newRow("running-status-after-meta") << smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(1, {0xff, 1, 0}) + fixtureEvent(1, {60, 0}) + eot()});
		QTest::newRow("oversized-meta") << smf({fixtureEvent(0, {0xff, 1, 127}) + eot()});
		QTest::newRow("zero-tempo") << smf({fixtureEvent(0, {0xff, 0x51, 3, 0, 0, 0}) + melody()});
		QTest::newRow("bad-time-signature") << smf({fixtureEvent(0, {0xff, 0x58, 4, 0, 2, 24, 8}) + melody()});
		QTest::newRow("unrepresentable-denominator") << smf({fixtureEvent(0, {0xff, 0x58, 4, 4, 255, 24, 8}) + melody()});
		wrong = riff(plain); wrong.chop(1); QTest::newRow("riff-truncated") << wrong;
		QTest::newRow("riff-no-data") << (QByteArray("RIFF") + integer(4, 4, true) + "RMID");
		const auto duplicate = QByteArray("RMID") + chunk("data", plain, true) + chunk("data", plain, true);
		QTest::newRow("riff-duplicate-data") << (QByteArray("RIFF") + integer(duplicate.size(), 4, true) + duplicate);
		QTest::newRow("riff-non-midi-data") << riff("garbage");
	}
	void malformedRecognizedInputCreatesNoNativeTracks()
	{
		QFETCH(QByteArray, bytes); auto prior = existing();
		QVERIFY_EXCEPTION_THROWN(admitted(bytes), std::runtime_error);
		QVERIFY(!import(bytes)); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior); QVERIFY(song()->hasErrors());
	}

	void unrelatedFormatsRemainAvailableToOtherFilters()
	{
		QVERIFY(admitted("FLhd other format").isEmpty());
		QVERIFY(admitted(QByteArray("RIFF") + integer(4, 4, true) + "WAVE").isEmpty());
		QVERIFY(!import("unrelated")); QVERIFY(!song()->hasErrors()); QVERIFY(song()->isEmpty());
	}

	void plainRiffAndRunningStatusCreateTheSameEditableNotes_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("plain") << 0; QTest::newRow("riff") << 1; QTest::newRow("running") << 2; }
	void plainRiffAndRunningStatusCreateTheSameEditableNotes()
	{
		QFETCH(int, kind);
		const auto track = fixtureEvent(0, {0x90, 60, 64}) + (kind == 2 ? fixtureEvent(0, {64, 80}) : fixtureEvent(0, {0x90, 64, 80}))
			+ fixtureEvent(48, {0x80, 60, 0}) + (kind == 2 ? fixtureEvent(0, {64, 0}) : fixtureEvent(0, {0x80, 64, 0})) + eot();
		const auto midi = smf({track}); QVERIFY(import(kind == 1 ? riff(midi) : midi)); const auto tracks = instruments();
		QCOMPARE(tracks.size(), std::size_t(1)); QCOMPARE(tracks[0]->numOfClips(), 1);
		const auto clip = static_cast<MidiClip*>(tracks[0]->getClips()[0]); QCOMPARE(clip->notes().size(), std::size_t(2));
		for (const auto note : clip->notes()) { QCOMPARE(note->pos().getTicks(), 0); QCOMPARE(note->length().getTicks(), 48); }
		QVERIFY(std::any_of(clip->notes().begin(), clip->notes().end(), [](const auto note) { return note->key() == 60; }));
	}

	void controllerAutomationOwnsItsSourceChannel_data()
	{ QTest::addColumn<bool>("separateTracks"); QTest::newRow("two-channels-one-track") << false; QTest::newRow("same-channel-two-tracks") << true; }
	void controllerAutomationOwnsItsSourceChannel()
	{
		QFETCH(bool, separateTracks); QByteArray midi;
		if (separateTracks)
		{
			midi = smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(12, {0xb0, 7, 20}) + fixtureEvent(84, {0x80, 60, 0}) + eot(),
				fixtureEvent(0, {0x90, 64, 64}) + fixtureEvent(12, {0xb0, 7, 100}) + fixtureEvent(84, {0x80, 64, 0}) + eot()});
		}
		else
		{
			midi = smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(0, {0x91, 64, 64}) + fixtureEvent(12, {0xb0, 7, 20})
				+ fixtureEvent(0, {0xb1, 7, 100}) + fixtureEvent(84, {0x80, 60, 0}) + fixtureEvent(0, {0x81, 64, 0}) + eot()});
		}
		QVERIFY(import(midi)); const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(2));
		for (auto track : tracks)
		{
			const auto clip = static_cast<MidiClip*>(track->getClips()[0]); const int key = clip->notes()[0]->key();
			const auto bound = curves(track->volumeModel()); QCOMPARE(bound.size(), std::size_t(1));
			QCOMPARE(bound[0]->objects().size(), std::size_t(1));
			const float expected = (key == 60 ? 20.f : 100.f) * 100.f / 127.f;
			QVERIFY(std::abs(bound[0]->valueAt(12) - expected) < .001f);
		}
		QVERIFY(curves(tracks[0]->volumeModel())[0] != curves(tracks[1]->volumeModel())[0]);
	}

	void programAutomationIsSeparateAcrossSourceTracks()
	{
#ifndef LMMS_HAVE_FLUIDSYNTH
		QSKIP("Requires the native SoundFont instrument");
#else
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("Requires the built sf2player plugin"); }
		QVERIFY(import(smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(12, {0xc0, 10}) + fixtureEvent(36, {0x80, 60, 0}) + eot(),
			fixtureEvent(0, {0x90, 64, 64}) + fixtureEvent(12, {0xc0, 20}) + fixtureEvent(36, {0x80, 64, 0}) + eot()})));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(2));
		for (auto track : tracks)
		{
			QVERIFY(track->instrument()); auto model = track->instrument()->childModel("patch"); QVERIFY(model);
			const auto bound = curves(model); QCOMPARE(bound.size(), std::size_t(1));
			const auto clip = static_cast<MidiClip*>(track->getClips()[0]);
			QCOMPARE(bound[0]->valueAt(12), clip->notes()[0]->key() == 60 ? 10.f : 20.f);
		}
#endif
	}

	void bankBytesShareOneCurveForTheCombinedNativeBank()
	{
#ifndef LMMS_HAVE_FLUIDSYNTH
		QSKIP("Requires the native SoundFont instrument");
#else
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("Requires the built sf2player plugin"); }
		QVERIFY(import(smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(12, {0xb0, 0, 1}) + fixtureEvent(12, {0xb0, 32, 2})
			+ fixtureEvent(12, {0xb0, 0, 3}) + fixtureEvent(12, {0x80, 60, 0}) + eot()})));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(1)); QVERIFY(tracks[0]->instrument());
		auto bank = tracks[0]->instrument()->childModel("bank"); QVERIFY(bank); const auto bound = curves(bank);
		QCOMPARE(bound.size(), std::size_t(1)); QCOMPARE(bound[0]->valueAt(12), 128.f);
		QCOMPARE(bound[0]->valueAt(24), 130.f); QCOMPARE(bound[0]->valueAt(36), 386.f);
#endif
	}

	void pitchBendUsesTheNativeSemitoneRange()
	{
		QVERIFY(import(smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(12, {0xe0, 0, 0})
			+ fixtureEvent(12, {0xe0, 0, 64}) + fixtureEvent(24, {0x80, 60, 0}) + eot()})));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(1)); QCOMPARE(tracks[0]->pitchRangeModel()->value(), 2);
		const auto bound = curves(tracks[0]->pitchModel()); QCOMPARE(bound.size(), std::size_t(1));
		QCOMPARE(bound[0]->valueAt(12), -200.f); QCOMPARE(bound[0]->valueAt(24), 0.f);
	}

	void eventEndpointsRoundInNativeTicks_data()
	{
		QTest::addColumn<int>("division"); QTest::addColumn<int>("start"); QTest::addColumn<int>("duration");
		QTest::addColumn<int>("nativeStart"); QTest::addColumn<int>("nativeDuration");
		QTest::newRow("native") << 48 << 1 << 37 << 1 << 37;
		QTest::newRow("half-tick") << 96 << 1 << 2 << 1 << 1;
		QTest::newRow("high-resolution") << 192 << 5 << 6 << 1 << 2;
		QTest::newRow("collapsed-short-note") << 192 << 0 << 1 << 0 << 1;
	}
	void eventEndpointsRoundInNativeTicks()
	{
		QFETCH(int, division); QFETCH(int, start); QFETCH(int, duration); QFETCH(int, nativeStart); QFETCH(int, nativeDuration);
		QVERIFY(import(smf({fixtureEvent(start, {0x90, 60, 31}) + fixtureEvent(duration, {0x80, 60, 0}) + eot()}, division)));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(1));
		auto clip = static_cast<MidiClip*>(tracks[0]->getClips()[0]); const auto note = clip->notes()[0];
		QCOMPARE(clip->startPosition().getTicks() + note->pos().getTicks(), nativeStart);
		QCOMPARE(note->length().getTicks(), nativeDuration); QCOMPARE(note->getVolume(), 49);
	}

	void tempoAndMeterEventsIgnoreTheEditorQuantization()
	{
		const auto previous = AutomationClip::quantization(); const auto restore = qScopeGuard([&] { AutomationClip::setQuantization(previous); });
		AutomationClip::setQuantization(12);
		QVERIFY(import(smf({fixtureEvent(0, {0xff, 0x51, 3, 7, 0xa1, 0x20}) + fixtureEvent(7, {0xff, 0x51, 3, 9, 0x27, 0xc0})
			+ fixtureEvent(4, {0xff, 0x58, 4, 3, 2, 24, 8}) + melody()})));
		const auto tempo = curves(&song()->tempoModel()); QCOMPARE(tempo.size(), std::size_t(1)); QVERIFY(tempo[0]->getTimeMap().contains(7));
		QVERIFY(std::abs(tempo[0]->valueAt(7) - 100.f) < .001f);
		const auto numerator = curves(&song()->getTimeSigModel().numeratorModel()); QCOMPARE(numerator.size(), std::size_t(1));
		QVERIFY(numerator[0]->getTimeMap().contains(11)); QCOMPARE(numerator[0]->valueAt(11), 3.f);
	}

	void sustainedNotesKeepOverlappingMaterialInOneClip()
	{
		QVERIFY(import(smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(96, {0x90, 64, 64}) + fixtureEvent(12, {0x80, 64, 0})
			+ fixtureEvent(292, {0x90, 67, 64}) + fixtureEvent(12, {0x80, 67, 0}) + fixtureEvent(188, {0x80, 60, 0}) + eot()})));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(1)); QCOMPARE(tracks[0]->numOfClips(), 1);
		auto clip = static_cast<MidiClip*>(tracks[0]->getClips()[0]); QCOMPARE(clip->notes().size(), std::size_t(3));
		QCOMPARE(clip->notes()[0]->length().getTicks(), 600); QCOMPARE(clip->notes()[2]->pos().getTicks(), 400);
	}

	void unrepresentableTimelineRollsBackOnlyTheImport()
	{
		auto prior = existing(); const auto bytes = smf({fixtureEvent(0x0fffffff, {0x90, 60, 64}) + fixtureEvent(1, {0x80, 60, 0}) + eot()}, 1);
		QCOMPARE(admitted(bytes), bytes); QVERIFY(!import(bytes)); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior);
	}

	void cancellationRemovesPartialNativeTracksAndAllowsRetry_data()
	{
		QTest::addColumn<int>("phase"); QTest::addColumn<bool>("patternDestination");
		QTest::newRow("before-parse") << 0 << false; QTest::newRow("global-automation") << 1 << false;
		QTest::newRow("partial-notes") << 2 << false; QTest::newRow("pattern-rows-and-created-column") << 2 << true;
	}
	void cancellationRemovesPartialNativeTracksAndAllowsRetry()
	{
		QFETCH(int, phase); QFETCH(bool, patternDestination); QPointer<InstrumentTrack> prior(existing());
		const auto bytes = dense(1024); QVERIFY(write(path(), bytes)); MidiImport importer(path()); bool observedPartial = false;
		auto destination = patternDestination ? static_cast<TrackContainer*>(Engine::patternStore()) : song();
		{
			FileResourceIO::Scope work([&]
			{
				if (phase == 0) { return true; }
				if (phase == 1) { return song()->tracks().size() > 1; }
				for (auto track : destination->tracks())
				{
					if (track == prior.data()) { continue; }
					for (auto item : track->getClips())
					{
						if (auto clip = dynamic_cast<MidiClip*>(item); clip && clip->notes().size() >= 64)
						{ observedPartial = true; return true; }
					}
				}
				return false;
			});
			QVERIFY(!importer.tryImport(destination));
		}
		if (phase == 2) { QVERIFY(observedPartial); }
		QVERIFY(prior); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior.data());
		QVERIFY(Engine::patternStore()->isEmpty()); QVERIFY(importer.tryImport(song()));
		const auto tracks = instruments(); QCOMPARE(tracks.size(), std::size_t(2));
		std::size_t notes = 0;
		for (auto track : tracks) { if (track != prior.data()) { for (auto item : track->getClips()) { notes += static_cast<MidiClip*>(item)->notes().size(); } } }
		QCOMPARE(notes, std::size_t(1024)); verifyExisting(prior.data());
	}

	void framedReadsAndValidationRemainCancellable_data()
	{ QTest::addColumn<int>("percent"); QTest::newRow("reading") << 10; QTest::newRow("event-scan") << 70; QTest::newRow("final-check") << 99; }
	void framedReadsAndValidationRemainCancellable()
	{
		QFETCH(int, percent); const auto bytes = dense(20000); int total = 0;
		{ FileResourceIO::Scope work([&] { ++total; return false; }); QCOMPARE(admitted(bytes), bytes); }
		int checks = 0; const int stop = std::max(1, total * percent / 100);
		{ FileResourceIO::Scope work([&] { return ++checks >= stop; }); QVERIFY_EXCEPTION_THROWN(admitted(bytes), std::runtime_error); }
		QVERIFY(song()->isEmpty());
	}

	void nativeConstructionSuppressesReentryAndRefusesForeignThreads()
	{
		QVERIFY(write(path(), smf({melody()}))); MidiImport importer(path()); bool ran = false;
		QTimer::singleShot(0, this, [&] { ran = true; });
		{ FileResourceIO::Scope work([] { return false; }, true); QVERIFY(importer.tryImport(song())); }
		QVERIFY(!ran); QCoreApplication::processEvents(); QVERIFY(ran);
		const auto count = song()->tracks().size(); bool result = true;
		std::thread foreign([&] { result = importer.tryImport(song()); }); foreign.join();
		QVERIFY(!result); QCOMPARE(song()->tracks().size(), count);
	}

	void patternImportUsesOneLocalCellPerChannel_data()
	{
		QTest::addColumn<int>("column"); QTest::addColumn<int>("columns"); QTest::addColumn<bool>("oddMeter"); QTest::addColumn<bool>("explicitSelection");
		QTest::newRow("empty-store") << 0 << 0 << false << false;
		QTest::newRow("first") << 0 << 3 << false << false;
		QTest::newRow("second") << 1 << 3 << false << false;
		QTest::newRow("above-byte-range") << 256 << 258 << false << true;
		QTest::newRow("seven-eight") << 5 << 7 << true << false;
		QTest::newRow("explicit-unselected-column") << 2 << 4 << false << true;
	}
	void patternImportUsesOneLocalCellPerChannel()
	{
		QFETCH(int, column); QFETCH(int, columns); QFETCH(bool, oddMeter); QFETCH(bool, explicitSelection);
		if (oddMeter) { song()->getTimeSigModel().setNumerator(7); song()->getTimeSigModel().setDenominator(8); }
		auto priorSong = existing(); auto priorRow = columns ? patternFixture(columns) : nullptr;
		const auto priorCells = priorRow ? priorRow->getClips() : Track::clipVector{};
		const auto priorSongTracks = song()->tracks();
		const int selected = explicitSelection ? 1 : column; if (columns) { store()->setCurrentPattern(selected); }
		const auto midi = smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(24, {0x80, 60, 0}) + fixtureEvent(576, {0x90, 67, 90})
			+ fixtureEvent(24, {0x80, 67, 0}) + fixtureEvent(276, {0xff, 0x2f, 0}),
			fixtureEvent(0, {0xb1, 7, 20}) + fixtureEvent(12, {0x91, 64, 80}) + fixtureEvent(5, {0xb1, 7, 90})
			+ fixtureEvent(19, {0x81, 64, 0}) + fixtureEvent(464, {0xb1, 7, 10}) + eot(),
			fixtureEvent(0, {0xff, 0x51, 3, 7, 0xa1, 0x20}) + fixtureEvent(400, {0xff, 0x51, 3, 9, 0x27, 0xc0})
			+ fixtureEvent(20, {0xff, 0x58, 4, 3, 2, 24, 8}) + eot()});
		QVERIFY(import(midi, store(), explicitSelection ? column : -1)); verifyColumns(); verifyExisting(priorSong);
		QCOMPARE(store()->numOfPatterns(), std::max(1, columns)); QCOMPARE(store()->currentPattern(), columns ? selected : 0);
		QCOMPARE(song()->tracks().size(), priorSongTracks.size() + (columns ? 0 : 1));
		for (std::size_t i = 0; i < priorSongTracks.size(); ++i) { QCOMPARE(song()->tracks()[i], priorSongTracks[i]); }
		if (priorRow)
		{
			QCOMPARE(priorRow->getClips(), priorCells);
			for (auto item : priorCells)
			{
				auto cell = static_cast<MidiClip*>(item); QCOMPARE(cell->notes().size(), std::size_t(1));
				QCOMPARE(cell->notes()[0]->key(), 72); QCOMPARE(cell->notes()[0]->pos().getTicks(), 3); QCOMPARE(cell->notes()[0]->length().getTicks(), 19);
				QCOMPARE(cell->length().getTicks(), TimePos::ticksPerBar());
			}
		}
		const int expectedLength = oddMeter ? 1008 : 960;
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(priorRow ? 3 : 2));
		for (auto row : rows)
		{
			if (row == priorRow) { continue; }
			auto cell = static_cast<MidiClip*>(row->getClips()[column]); QVERIFY(!cell->getAutoResize());
			QCOMPARE(cell->length().getTicks(), expectedLength); QCOMPARE(cell->startTimeOffset().getTicks(), 0);
			QVERIFY(!cell->notes().empty());
			if (cell->notes()[0]->key() == 60)
			{
				QCOMPARE(cell->notes().size(), std::size_t(2)); QCOMPARE(cell->notes()[0]->pos().getTicks(), 0);
				QCOMPARE(cell->notes()[1]->pos().getTicks(), 600); QCOMPARE(cell->notes()[1]->length().getTicks(), 24);
			}
			else
			{
				QCOMPARE(cell->notes().size(), std::size_t(1)); QCOMPARE(cell->notes()[0]->key(), 64); QCOMPARE(cell->notes()[0]->pos().getTicks(), 12);
				const auto bound = curves(row->volumeModel(), store()); QCOMPARE(bound.size(), std::size_t(1));
				QCOMPARE(bound[0]->objects().size(), std::size_t(1)); QCOMPARE(bound[0]->objects()[0].data(), static_cast<AutomatableModel*>(row->volumeModel()));
				QCOMPARE(bound[0]->startPosition().getTicks(), column * TimePos::ticksPerBar());
				for (const auto point : {std::pair<int, int>{0, 20}, {17, 90}, {500, 10}})
				{ QVERIFY(std::abs(store()->automatedValuesAt(point.first, column).value(row->volumeModel()) - point.second * 100.f / 127.f) < .001f); }
			}
		}
		for (auto row : store()->tracks())
		{
			if (row == priorRow) { continue; }
			for (int other = 0; other < row->numOfClips(); ++other)
			{
				if (other == column) { continue; }
				if (auto cell = dynamic_cast<MidiClip*>(row->getClips()[other])) { QVERIFY(cell->notes().empty()); }
				if (auto cell = dynamic_cast<AutomationClip*>(row->getClips()[other])) { QVERIFY(cell->objects().empty()); QVERIFY(!cell->hasAutomation()); }
			}
		}
		QCOMPARE(store()->lengthOfPattern(column) * TimePos::ticksPerBar(), expectedLength);
		QCOMPARE(store()->automatedValuesAt(400, column).value(&song()->tempoModel()), 100.f);
		QCOMPARE(store()->automatedValuesAt(420, column).value(&song()->getTimeSigModel().numeratorModel()), 3.f);
		QVERIFY(curves(&song()->tempoModel()).empty());
	}

	void automationOnlyPatternsRetainTheirWholePeriod_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("tempo-only") << 0;
		QTest::newRow("controller-without-notes") << 1; QTest::newRow("empty-sequence-with-duration") << 2;
	}
	void automationOnlyPatternsRetainTheirWholePeriod()
	{
		QFETCH(int, kind); auto prior = patternFixture(3); store()->setCurrentPattern(2);
		if (kind == 2) { prior->getClips()[2]->setAutoResize(false); prior->getClips()[2]->changeLength(1536); }
		QByteArray content;
		if (kind == 0) { content = fixtureEvent(600, {0xff, 0x51, 3, 9, 0x27, 0xc0}) + fixtureEvent(168, {0xff, 0x2f, 0}); }
		else if (kind == 1) { content = fixtureEvent(0, {0xb0, 7, 20}) + fixtureEvent(600, {0xb0, 7, 90}) + fixtureEvent(168, {0xff, 0x2f, 0}); }
		else { content = fixtureEvent(768, {0xff, 0x2f, 0}); }
		QVERIFY(import(smf({content}), store())); verifyColumns();
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(2));
		auto cell = static_cast<MidiClip*>(rows[1]->getClips()[2]); QVERIFY(cell->notes().empty()); QVERIFY(!cell->getAutoResize());
		QCOMPARE(cell->length().getTicks(), 768); QCOMPARE(store()->lengthOfPattern(2), kind == 2 ? 8 : 4);
		const auto tempo = curves(&song()->tempoModel(), store()); QCOMPARE(tempo.size(), std::size_t(1));
		QCOMPARE(tempo[0]->length().getTicks(), kind == 2 ? 1536 : 768);
		if (kind == 0) { QCOMPARE(store()->automatedValuesAt(600, 2).value(&song()->tempoModel()), 100.f); }
		if (kind == 1) { QVERIFY(std::abs(store()->automatedValuesAt(600, 2).value(rows[1]->volumeModel()) - 90.f * 100 / 127) < .001f); }
		else { QVERIFY(rows[1]->instrument() == nullptr); QCOMPARE(rows[1]->name(), QString("MIDI sequence length")); }
	}

	void patternBankAndProgramResetAtTheLocalOrigin()
	{
#ifndef LMMS_HAVE_FLUIDSYNTH
		QSKIP("Requires the native SoundFont instrument");
#else
		if (getPluginFactory()->pluginInfo("sf2player").isNull()) { QSKIP("Requires the built sf2player plugin"); }
		patternFixture(3); store()->setCurrentPattern(2);
		QVERIFY(import(smf({fixtureEvent(0, {0xc0, 10}) + fixtureEvent(0, {0xb0, 0, 1}) + fixtureEvent(0, {0xb0, 32, 2})
			+ fixtureEvent(500, {0xc0, 20}) + fixtureEvent(0, {0xb0, 32, 3}) + eot()}), store())); verifyColumns();
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(2)); QVERIFY(rows[1]->instrument());
		auto patch = rows[1]->instrument()->childModel("patch"); auto bank = rows[1]->instrument()->childModel("bank"); QVERIFY(patch); QVERIFY(bank);
		QCOMPARE(curves(patch, store()).size(), std::size_t(1)); QCOMPARE(curves(bank, store()).size(), std::size_t(1));
		QCOMPARE(store()->automatedValuesAt(0, 2).value(patch), 10.f); QCOMPARE(store()->automatedValuesAt(500, 2).value(patch), 20.f);
		QCOMPARE(store()->automatedValuesAt(0, 2).value(bank), 130.f); QCOMPARE(store()->automatedValuesAt(500, 2).value(bank), 131.f);
#endif
	}

	void invalidPatternDestinationsDoNotTouchExistingModels_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("song-with-column") << 0; QTest::newRow("negative-column") << 1;
		QTest::newRow("beyond-last-column") << 2; QTest::newRow("empty-store-nonzero-column") << 3; QTest::newRow("sparse-existing-row") << 4;
	}
	void invalidPatternDestinationsDoNotTouchExistingModels()
	{
		QFETCH(int, kind); auto prior = existing(); InstrumentTrack* row = kind == 3 ? nullptr : patternFixture(3);
		if (kind == 4) { delete row->getClips().back(); }
		const auto songTracks = song()->tracks(), patternTracks = store()->tracks(); const int cells = row ? row->numOfClips() : 0;
		const int column = kind == 0 ? 0 : kind == 1 ? -2 : kind == 2 ? 3 : kind == 3 ? 1 : 2;
		QVERIFY(!import(smf({melody()}), kind == 0 ? static_cast<TrackContainer*>(song()) : store(), column));
		if (kind != 4)
		{
			const auto result = StudioCapabilities::execute("project.import", {{"path", path()}, {"container", kind == 0 ? "song" : "pattern"}, {"pattern", column}});
			QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("pattern"));
		}
		QCOMPARE(song()->tracks(), songTracks); QCOMPARE(store()->tracks(), patternTracks); verifyExisting(prior);
		if (row) { QCOMPARE(row->numOfClips(), cells); }
	}

	void cancelledImportPreservesExistingPatternCellsAndCanRetry()
	{
		auto prior = patternFixture(4); store()->setCurrentPattern(1); const auto cells = prior->getClips();
		QVERIFY(write(path(), dense(1024))); MidiImport importer(path()); bool partial = false;
		{
			FileResourceIO::Scope work([&]
			{
				for (auto row : instruments(store()))
				{
					if (row != prior && static_cast<MidiClip*>(row->getClips()[3])->notes().size() >= 64) { partial = true; return true; }
				}
				return false;
			});
			QVERIFY(!importer.tryImport(store(), 3));
		}
		QVERIFY(partial); QCOMPARE(store()->tracks().size(), std::size_t(1)); QCOMPARE(prior->getClips(), cells);
		QCOMPARE(song()->tracks().size(), std::size_t(4)); QCOMPARE(store()->currentPattern(), 1); verifyColumns();
		QVERIFY(importer.tryImport(store(), 3)); verifyColumns(); QCOMPARE(store()->currentPattern(), 1);
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(2));
		QCOMPARE(static_cast<MidiClip*>(rows[1]->getClips()[3])->notes().size(), std::size_t(1024));
		for (auto item : cells) { QCOMPARE(static_cast<MidiClip*>(item)->notes().size(), std::size_t(1)); }
	}

	void importedPatternNotesAndBindingsSurviveNativeProjectReload()
	{
		patternFixture(4); store()->setCurrentPattern(1);
		QVERIFY(import(smf({fixtureEvent(0, {0xb0, 7, 20}) + fixtureEvent(500, {0xb0, 7, 90})
			+ fixtureEvent(100, {0x90, 60, 64}) + fixtureEvent(24, {0x80, 60, 0}) + fixtureEvent(276, {0xff, 0x2f, 0})}), store(), 3));
		const auto saved = m_configuration.filePath("imported-pattern.mmp"); QVERIFY(song()->saveProjectFile(saved));
		QVERIFY(song()->loadProject(saved)); verifyColumns(); QCOMPARE(store()->currentPattern(), 1);
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(2));
		auto cell = static_cast<MidiClip*>(rows[1]->getClips()[3]); QCOMPARE(cell->notes().size(), std::size_t(1));
		QCOMPARE(cell->notes()[0]->pos().getTicks(), 600); QCOMPARE(cell->notes()[0]->length().getTicks(), 24);
		QCOMPARE(cell->length().getTicks(), 960); QVERIFY(!cell->getAutoResize());
		QCOMPARE(curves(rows[1]->volumeModel(), store()).size(), std::size_t(1));
		QVERIFY(std::abs(store()->automatedValuesAt(500, 3).value(rows[1]->volumeModel()) - 90.f * 100 / 127) < .001f);
	}

	void importSelectorsTrackNativePatternIdentity()
	{
		patternFixture(3); store()->setCurrentPattern(1);
		const QJsonObject explicitArgs{{"container", "pattern"}, {"pattern", 1}}, currentArgs{{"container", "pattern"}};
		const auto explicitChoice = StudioCapabilities::selection("project.import", explicitArgs);
		const auto currentChoice = StudioCapabilities::selection("project.import", currentArgs);
		store()->setCurrentPattern(2);
		QVERIFY(!explicitChoice.changed(StudioCapabilities::selection("project.import", explicitArgs)));
		QVERIFY(currentChoice.changed(StudioCapabilities::selection("project.import", currentArgs)));
		delete PatternTrack::findPatternTrack(0);
		QVERIFY(explicitChoice.changed(StudioCapabilities::selection("project.import", explicitArgs)));
		for (const auto item : StudioCapabilities::schema())
		{
			if (item.toObject().value("name").toString() != "project.import") { continue; }
			const auto properties = item.toObject().value("inputSchema").toObject().value("properties").toObject();
			QCOMPARE(properties.value("container").toObject().value("enum").toArray(), (QJsonArray{"song", "pattern"}));
			QCOMPARE(properties.value("pattern").toObject().value("type").toString(), QString("integer")); return;
		}
		QFAIL("Native import capability is absent");
	}

	void structuredPatternImportSurvivesUndoReplayAndRecovery()
	{
		if (getPluginFactory()->pluginInfo("midiimport").isNull()) { QSKIP("Requires the built native MIDI import plugin"); }
		QTemporaryDir session; QVERIFY(session.isValid()); QVERIFY(write(path(), smf({melody()})));
		const auto command = request("import-pattern", "project.import", {{"path", path()}, {"container", "pattern"}, {"pattern", 2}, {"summary", true}});
		std::size_t importedRows = 0;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); patternFixture(3); store()->setCurrentPattern(1);
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 101}})).value("ok").toBool());
			const auto result = runtime.process(command); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
			QCOMPARE(result.value("result").toObject().value("inspection").toString(), QString("summary"));
			importedRows = store()->tracks().size(); QVERIFY(importedRows > 1); verifyColumns(); QCOMPARE(store()->currentPattern(), 1);
			QVERIFY(runtime.process(command).value("replayed").toBool()); QCOMPARE(store()->tracks().size(), importedRows);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			QCOMPARE(store()->tracks().size(), std::size_t(1)); QCOMPARE(store()->currentPattern(), 1); verifyColumns();
			QVERIFY(runtime.process(command).value("replayed").toBool()); QCOMPARE(store()->tracks().size(), std::size_t(1));
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
			QCOMPARE(store()->tracks().size(), importedRows); verifyColumns();
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(store()->tracks().size(), importedRows); QCOMPARE(store()->currentPattern(), 1); verifyColumns();
		const auto rows = instruments(store()); QCOMPARE(rows.size(), std::size_t(2));
		QCOMPARE(static_cast<MidiClip*>(rows[1]->getClips()[2])->notes().size(), std::size_t(1));
	}

	void genericControllerImportsActivateTheNativeRack_data()
	{
		QTest::addColumn<int>("controller"); QTest::addColumn<bool>("patternDestination");
		QTest::newRow("song-modulation") << 1 << false; QTest::newRow("song-sustain") << 64 << false;
		QTest::newRow("song-expression-zero") << 11 << false; QTest::newRow("pattern-modulation") << 1 << true;
		QTest::newRow("pattern-sustain") << 64 << true; QTest::newRow("pattern-expression-zero") << 11 << true;
	}
	void genericControllerImportsActivateTheNativeRack()
	{
		QFETCH(int, controller); QFETCH(bool, patternDestination);
		if (patternDestination) { patternFixture(3); store()->setCurrentPattern(2); }
		auto destination = patternDestination ? static_cast<TrackContainer*>(store()) : song();
		QVERIFY(import(smf({fixtureEvent(0, {0xb0, controller, 0}) + fixtureEvent(0, {0x90, 60, 64})
			+ fixtureEvent(17, {0xb0, controller, 100}) + fixtureEvent(79, {0x80, 60, 0}) + eot()}), destination));
		const auto rows = instruments(destination); auto row = rows.back(); QVERIFY(row->midiCCEnabledModel()->value());
		QCOMPARE(row->midiCCModel(controller)->value(), 0.f); const auto bound = curves(row->midiCCModel(controller), destination);
		QCOMPARE(bound.size(), std::size_t(1)); QCOMPARE(bound[0]->valueAt(0), 0.f); QCOMPARE(bound[0]->valueAt(17), 100.f);
		QDomDocument document; auto root = document.createElement("fixture"); document.appendChild(root); row->saveState(document, root);
		QCOMPARE(root.firstChildElement("track").firstChildElement("instrumenttrack").attribute("activecc"), QString::number(controller));
		if (patternDestination) { verifyColumns(); }
	}

	void nativeControllerDefaultsPrecedeTheFirstImportedChange_data()
	{
		QTest::addColumn<int>("controller"); QTest::addColumn<float>("initial");
		QTest::newRow("expression") << 11 << 127.f; QTest::newRow("expression-lsb") << 43 << 127.f;
		QTest::newRow("balance") << 8 << 64.f; QTest::newRow("sound-controller") << 74 << 64.f;
		QTest::newRow("nrpn-null") << 99 << 127.f;
	}
	void nativeControllerDefaultsPrecedeTheFirstImportedChange()
	{
		QFETCH(int, controller); QFETCH(float, initial);
		QVERIFY(import(smf({fixtureEvent(0, {0x90, 60, 64}) + fixtureEvent(12, {0xb0, controller, 20}) + fixtureEvent(84, {0x80, 60, 0}) + eot()})));
		const auto rows = instruments(); QCOMPARE(rows.size(), std::size_t(1)); auto row = rows[0];
		QVERIFY(row->midiCCEnabledModel()->value()); QCOMPARE(row->midiCCModel(controller)->value(), initial);
		const auto bound = curves(row->midiCCModel(controller)); QCOMPARE(bound.size(), std::size_t(1));
		QCOMPARE(bound[0]->valueAt(0), initial); QCOMPARE(bound[0]->valueAt(12), 20.f);
	}

	void mappedTrackControlsDoNotActivateTheGenericRack_data()
	{
		QTest::addColumn<QByteArray>("updates"); QTest::newRow("volume") << fixtureEvent(0, {0xb0, 7, 100});
		QTest::newRow("pan") << fixtureEvent(0, {0xb0, 10, 64}); QTest::newRow("bank-msb") << fixtureEvent(0, {0xb0, 0, 0});
		QTest::newRow("bank-lsb") << fixtureEvent(0, {0xb0, 32, 0}); QTest::newRow("bend") << fixtureEvent(0, {0xe0, 0, 64});
		QTest::newRow("notes-only") << QByteArray{};
	}
	void mappedTrackControlsDoNotActivateTheGenericRack()
	{
		QFETCH(QByteArray, updates); QVERIFY(import(smf({updates + melody()}))); const auto rows = instruments();
		QCOMPARE(rows.size(), std::size_t(1)); QVERIFY(!rows[0]->midiCCEnabledModel()->value());
	}

	void genericControllerActivationSurvivesProjectReload_data()
	{ QTest::addColumn<bool>("patternDestination"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void genericControllerActivationSurvivesProjectReload()
	{
		QFETCH(bool, patternDestination); if (patternDestination) { patternFixture(3); store()->setCurrentPattern(2); }
		auto destination = patternDestination ? static_cast<TrackContainer*>(store()) : song();
		QVERIFY(import(smf({fixtureEvent(0, {0xb0, 64, 0}) + fixtureEvent(17, {0xb0, 64, 127}) + melody()}), destination));
		const auto saved = m_configuration.filePath("generic-cc.mmp"); QVERIFY(song()->saveProjectFile(saved)); QVERIFY(song()->loadProject(saved));
		const auto rows = instruments(destination); auto row = rows.back(); QVERIFY(row->midiCCEnabledModel()->value());
		QCOMPARE(row->midiCCModel(64)->value(), 0.f); QCOMPARE(curves(row->midiCCModel(64), destination).size(), std::size_t(1));
		QDomDocument document; auto root = document.createElement("fixture"); document.appendChild(root); row->saveState(document, root);
		QCOMPARE(root.firstChildElement("track").firstChildElement("instrumenttrack").attribute("activecc"), QString("64"));
		if (patternDestination) { verifyColumns(); }
	}
	void directImportRestoresThePriorJournalPolicy_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("disabled") << false; QTest::newRow("enabled") << true; }
	void directImportRestoresThePriorJournalPolicy()
	{
		QFETCH(bool, enabled); const bool previous = Engine::projectJournal()->isJournalling(); const auto restore = qScopeGuard([&] { Engine::projectJournal()->setJournalling(previous); }); Engine::projectJournal()->setJournalling(enabled);
		QVERIFY(import(smf({melody()}))); QCOMPARE(Engine::projectJournal()->isJournalling(), enabled); QCOMPARE(instruments().size(), std::size_t(1));
	}
	void retiredDestinationDoesNotInvalidateRollbackContainers_data()
	{ QTest::addColumn<bool>("late"); QTest::newRow("before-local-file") << false; QTest::newRow("partial-native-notes") << true; }
	void retiredDestinationDoesNotInvalidateRollbackContainers()
	{
		QFETCH(bool, late); auto prior = existing(); const QPointer<Container> destination(new Container); bool retired = false; QVERIFY(write(path(), dense(1024))); MidiImport importer(path());
		{ FileResourceIO::Scope work([&] { if (retired) { return false; } bool ready = !late; if (late && destination) { for (auto row : instruments(destination.data())) { for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { ready = true; } } } } if (ready) { retired = true; delete destination.data(); } return false; }); QVERIFY(!importer.tryImport(destination.data())); }
		QVERIFY(retired); QVERIFY(!destination); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior); QVERIFY(importer.tryImport(song()));
	}
	void retiredCreatedActorsStopImportBeforeReuse_data()
	{
		QTest::addColumn<int>("kind"); QTest::addColumn<bool>("patternDestination"); QTest::newRow("song-track") << 0 << false; QTest::newRow("song-clip") << 1 << false; QTest::newRow("song-instrument") << 2 << false; QTest::newRow("pattern-track") << 0 << true; QTest::newRow("pattern-cell") << 1 << true; QTest::newRow("pattern-instrument") << 2 << true;
	}
	void retiredCreatedActorsStopImportBeforeReuse()
	{
		QFETCH(int, kind); QFETCH(bool, patternDestination); auto prior = existing(); auto destination = patternDestination ? static_cast<TrackContainer*>(store()) : song(); bool retired = false; QVERIFY(write(path(), dense(1024))); MidiImport importer(path());
		{ FileResourceIO::Scope work([&] { if (retired) { return false; } for (auto row : instruments(destination)) { if (row == prior) { continue; } for (auto item : row->getClips()) { auto cell = dynamic_cast<MidiClip*>(item); if (!cell || cell->notes().size() < 64) { continue; } retired = true; if (kind == 0) { delete row; } else if (kind == 1) { delete cell; } else { delete row->instrument(); } return false; } } return false; }); QVERIFY(!importer.tryImport(destination)); }
		QVERIFY(retired); QCOMPARE(song()->tracks().size(), std::size_t(1)); QVERIFY(store()->tracks().empty()); verifyExisting(prior); QVERIFY(importer.tryImport(song()));
	}
	void laterMetadataNotificationCannotRetireAnEarlierTrack()
	{
		auto prior = existing(); QPointer<Track> numerator; bool retired = false; std::vector<QMetaObject::Connection> rows;
		const auto connection = connect(song(), &TrackContainer::trackAdded, this, [&](Track* row) { rows.push_back(connect(row, &Model::dataChanged, this, [&, row] { if (retired) { return; } if (row->name() == "MIDI Time Signature Numerator") { numerator = row; } else if (row->name() == "MIDI Time Signature Denominator" && numerator) { retired = true; delete numerator.data(); } })); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); for (const auto& row : rows) { QObject::disconnect(row); } });
		QVERIFY(!import(smf({melody()}))); QVERIFY(retired); QVERIFY(!numerator); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior);
	}
	void callbackOwnedTracksSurviveCancelledImportCleanup()
	{
		auto prior = existing(); QPointer<Track> outsider; QVERIFY(write(path(), dense(1024))); MidiImport importer(path());
		{ FileResourceIO::Scope work([&] { if (outsider) { return true; } for (auto row : instruments()) { if (row == prior) { continue; } for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { outsider = new AutomationTrack(song()); outsider->setName("Callback owned"); return true; } } } return false; }); QVERIFY(!importer.tryImport(song())); }
		QVERIFY(outsider); QCOMPARE(song()->tracks().size(), std::size_t(2)); QCOMPARE(outsider->name(), QString("Callback owned")); verifyExisting(prior);
	}
	void directEntryRefusesSameOtherAndNestedProjectWork()
	{
		QVERIFY(write(path(), smf({melody()}))); MidiImport importer(path()), other(path()); bool invoked = false, sameRefused = false, otherRefused = false, saveRefused = false, creationRefused = false;
		{ FileResourceIO::Scope work([&] { if (invoked) { return false; } invoked = true; sameRefused = !importer.tryImport(song()); otherRefused = !other.tryImport(song()); saveRefused = !song()->saveProjectFile(m_configuration.filePath("nested-import.mmp")); try { song()->createEmptyProject(); } catch (const std::logic_error&) { creationRefused = true; } return false; }); QVERIFY(importer.tryImport(song())); }
		QVERIFY(invoked && sameRefused && otherRefused && saveRefused && creationRefused); QVERIFY(other.tryImport(song()));
	}
	void importerRetirementDoesNotInvalidateTheLocalInput()
	{
		auto prior = existing(); QVERIFY(write(path(), dense(1024))); QPointer<MidiImport> importer(new MidiImport(path())); int checks = 0, deaths = 0; connect(importer.data(), &QObject::destroyed, this, [&] { ++deaths; });
		{ FileResourceIO::Scope work([&] { if (++checks == 3) { delete importer.data(); } return false; }); auto active = importer.data(); QVERIFY(!active->tryImport(song())); }
		QVERIFY(!importer); QCOMPARE(deaths, 1); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior);
	}
	void splitSourceMutationsAreRefusedUsingOwnedNoteCopies_data()
	{ QTest::addColumn<bool>("remove"); QTest::newRow("remove-source-note") << true; QTest::newRow("edit-source-note") << false; }
	void splitSourceMutationsAreRefusedUsingOwnedNoteCopies()
	{
		QFETCH(bool, remove); auto prior = existing(); QVERIFY(write(path(), dense(1024))); MidiImport importer(path()); bool changed = false;
		{ FileResourceIO::Scope work([&] { if (changed) { return false; } for (auto row : instruments()) { if (row == prior || row->numOfClips() < 2) { continue; } auto source = static_cast<MidiClip*>(row->getClips()[0]); auto destination = static_cast<MidiClip*>(row->getClips()[1]); if (source->notes().size() >= 512 && !destination->notes().empty()) { changed = true; if (remove) { source->removeNote(source->notes().front()); } else { source->notes().front()->setKey(72); } return false; } } return false; }); QVERIFY(!importer.tryImport(song())); }
		QVERIFY(changed); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior); QVERIFY(importer.tryImport(song()));
	}
	void workExceptionsCleanOnlyOwnedTracksAndAllowRetry_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void workExceptionsCleanOnlyOwnedTracksAndAllowRetry()
	{
		QFETCH(bool, nonstandard); auto prior = existing(); QVERIFY(write(path(), dense(1024))); MidiImport importer(path()); bool raised = false; const bool journalling = Engine::projectJournal()->isJournalling();
		{ FileResourceIO::Scope work([&]() -> bool { if (!raised) { for (auto row : instruments()) { if (row == prior) { continue; } for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { raised = true; if (nonstandard) { throw 31; } throw std::runtime_error("fixture MIDI work failure"); } } } } return false; }); QVERIFY(!importer.tryImport(song())); }
		QVERIFY(raised); QCOMPARE(Engine::projectJournal()->isJournalling(), journalling); QCOMPARE(song()->tracks().size(), std::size_t(1)); verifyExisting(prior); QVERIFY(importer.tryImport(song()));
	}
	void callbackCreatedColumnIsRetainedAndRefusedAsAnEmptyDestination()
	{
		auto prior = existing(); QPointer<PatternTrack> outsider; QVERIFY(write(path(), smf({melody()}))); MidiImport importer(path());
		{ FileResourceIO::Scope work([&] { if (!outsider) { outsider = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song())); } return false; }); QVERIFY(!importer.tryImport(store())); }
		QVERIFY(outsider); QCOMPARE(song()->tracks().size(), std::size_t(2)); QCOMPARE(store()->numOfPatterns(), 1); QVERIFY(store()->tracks().empty()); verifyExisting(prior); QVERIFY(importer.tryImport(store(), 0)); QCOMPARE(PatternTrack::findPatternTrack(0), outsider.data()); verifyColumns();
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::MidiImportTest)
#include "MidiImportTest.moc"
