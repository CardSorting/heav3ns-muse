/*
 * MidiImport.cpp - support for importing MIDI files
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */


#include <QDir>
#include <QFile>
#include <QDebug>
#include <QMessageBox>
#include <QProgressDialog>

#include <sstream>
#include <memory>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <functional>
#include <string_view>
#include <utility>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>

#include "AudioEngine.h"
#include "FileResourceIO.h"
#include "MidiImportData.h"

#include "MidiImport.h"
#include "TrackContainer.h"
#include "InstrumentTrack.h"
#include "AutomationTrack.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "MidiClip.h"
#include "MidiPatch.h"
#include "Instrument.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "TimePos.h"
#include "Song.h"
#include "PatternStore.h"
#include "PatternTrack.h"

#include "plugin_export.h"

#include "portsmf/include/allegro.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT midiimport_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"MIDI Import",
	QT_TRANSLATE_NOOP("PluginBrowser", "Filter for importing MIDI files into LMMS"),
	"Tobias Doerffel <tobydox/at/users/dot/sf/dot/net>",
	0x0100,
	Plugin::Type::ImportFilter,
	nullptr,
	nullptr,
	nullptr,
};

PLUGIN_EXPORT Plugin* lmms_plugin_main(Model*, void* data)
{
	return new MidiImport(QString::fromUtf8(static_cast<const char*>(data)));
}

}


MidiImport::MidiImport(const QString& file) :
	ImportFilter(file, &midiimport_plugin_descriptor)
{}


namespace
{
tick_t importTick(double beats, std::int64_t columnStart = 0)
{
	const double ticks = std::round(beats * TicksPerQuarterNote);
	// Native clip/automation lengths round up to bars and can add a trailing
	// bar. Keep that arithmetic representable as well as the event itself.
	const auto maximum = std::numeric_limits<tick_t>::max() - 2 * std::int64_t(TimePos::ticksPerBar()) - columnStart;
	if (!std::isfinite(ticks) || beats < 0 || ticks > maximum)
	{ throw std::out_of_range("MIDI time exceeds the native arrangement range"); }
	return static_cast<tick_t>(ticks);
}

float importValue(double value)
{
	if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
	{ throw std::out_of_range("MIDI automation value is not finite or representable"); }
	return static_cast<float>(value);
}
}

// Only the tracks created through this state belong to import rollback. Callback
// additions and tracks adopted elsewhere remain owned by their native callers.
class MidiImportState
{
public:
	MidiImportState(std::function<void()> context, std::function<void()> poll, int pattern)
		: m_context(std::move(context)), m_poll(std::move(poll)), m_song(Engine::getSong()), m_generation(m_song->replacementGeneration()), m_pattern(pattern),
		  m_emptyColumn(pattern >= 0 && Engine::patternStore()->numOfPatterns() == 0), m_column(pattern >= 0 && !m_emptyColumn ? PatternTrack::findPatternTrack(pattern) : nullptr) { }
	~MidiImportState()
	{
		if (m_committed) { return; }
		const FileResourceIO::Suspension recovery; const FileResourceIO::EventSuspension events;
		for (auto it = m_tracks.rbegin(); it != m_tracks.rend(); ++it)
		{
			if (!m_song || Engine::getSong() != m_song.data() || m_song->replacementGeneration() != m_generation) { break; }
			if (it->track && it->owner && it->track->parent() == it->owner.data() && it->track->trackContainer() == it->owner.data()
				&& std::find(it->owner->tracks().begin(), it->owner->tracks().end(), it->track.data()) != it->owner->tracks().end()) { delete it->track.data(); }
		}
	}
	void context() const
	{
		m_context();
		if (m_pattern >= 0)
		{
			if (m_emptyColumn && !m_buildingColumn && Engine::patternStore()->numOfPatterns() != 0) { throw std::runtime_error("MIDI empty pattern destination changed before native column creation"); }
			if (!m_emptyColumn && (!m_column || PatternTrack::findPatternTrack(m_pattern) != m_column.data())) { throw std::runtime_error("MIDI selected pattern column changed"); }
		}
		for (const auto& entry : m_tracks)
		{
			if (!entry.track || !entry.owner || entry.track->parent() != entry.owner.data() || entry.track->trackContainer() != entry.owner.data()
				|| std::find(entry.owner->tracks().begin(), entry.owner->tracks().end(), entry.track.data()) == entry.owner->tracks().end())
			{ throw std::runtime_error("MIDI import created track retired or detached"); }
		}
		for (const auto& entry : m_clips)
		{
			if (!entry.clip || !entry.track || entry.clip->getTrack() != entry.track.data()
				|| std::find(entry.track->getClips().begin(), entry.track->getClips().end(), entry.clip.data()) == entry.track->getClips().end())
			{ throw std::runtime_error("MIDI import clip retired or detached"); }
		}
		for (const auto& entry : m_instruments)
		{ if (!entry.track || !entry.instrument || entry.track->instrument() != entry.instrument.data()) { throw std::runtime_error("MIDI import selected instrument changed"); } }
		for (const auto& model : m_models) { if (!model) { throw std::runtime_error("MIDI import target model retired"); } }
	}
	void poll() const { context(); m_poll(); context(); }
	template<class Function> void apply(Function&& function) const { context(); function(); context(); }
	void commit() { poll(); m_committed = true; }
	Track* track(Track::Type type, TrackContainer* owner)
	{
		context();
		if (owner == Engine::patternStore() && Engine::patternStore()->numOfPatterns() == 0)
		{
			m_buildingColumn = true; const QPointer<PatternTrack> created(dynamic_cast<PatternTrack*>(track(Track::Type::Pattern, Engine::getSong())));
			if (!created || Engine::patternStore()->numOfPatterns() != 1 || PatternTrack::findPatternTrack(0) != created.data()) { throw std::runtime_error("MIDI native initial pattern column changed during construction"); }
			m_column = created; m_emptyColumn = false; m_buildingColumn = false; context();
		}
		const QPointer<TrackContainer> container(owner); const QPointer<Track> candidate(Track::create(type, owner)); bool registered = false;
		const auto cleanup = qScopeGuard([&] { if (!registered && candidate && container && candidate->trackContainer() == container.data()) { delete candidate.data(); } });
		if (!candidate) { throw std::runtime_error("MIDI import track could not be created"); }
		m_tracks.push_back({candidate, container}); registered = true; context();
		for (auto clip : candidate->getClips()) { watch(clip); } context(); return candidate.data();
	}
	Clip* watch(Clip* clip)
	{
		if (!clip) { throw std::runtime_error("MIDI import clip could not be created"); }
		if (std::none_of(m_clips.begin(), m_clips.end(), [clip](const auto& entry) { return entry.clip.data() == clip; })) { m_clips.push_back({clip, clip->getTrack()}); }
		context(); return clip;
	}
	Clip* clip(Track* track, TimePos position) { context(); return watch(track->createClip(position)); }
	Clip* cell(Track* track, int pattern)
	{
		context(); if (!track || pattern < 0 || pattern >= track->numOfClips()) { throw std::runtime_error("MIDI import pattern cell is unavailable"); }
		return watch(track->getClips().at(pattern));
	}
	void forget(Clip* clip) { m_clips.erase(std::remove_if(m_clips.begin(), m_clips.end(), [clip](const auto& entry) { return entry.clip.data() == clip; }), m_clips.end()); }
	Instrument* instrument(InstrumentTrack* track, const QString& name)
	{
		context(); m_instruments.erase(std::remove_if(m_instruments.begin(), m_instruments.end(), [track](const auto& entry) { return entry.track.data() == track; }), m_instruments.end());
		const QPointer<Instrument> result(track->loadInstrument(name)); context();
		if (!result || track->instrument() != result.data()) { throw std::runtime_error("MIDI import instrument did not survive loading"); }
		m_instruments.push_back({track, result}); context(); return result.data();
	}
	AutomatableModel* model(AutomatableModel* model)
	{
		if (!model) { throw std::runtime_error("MIDI import target model is unavailable"); }
		if (std::none_of(m_models.begin(), m_models.end(), [model](const auto& entry) { return entry.data() == model; })) { m_models.emplace_back(model); }
		context(); return model;
	}
	AutomatableModel* model(Instrument* instrument, std::string_view name)
	{ context(); const QPointer<AutomatableModel> result(instrument->childModel(name)); context(); return model(result.data()); }
private:
	struct TrackEntry { QPointer<Track> track; QPointer<TrackContainer> owner; };
	struct ClipEntry { QPointer<Clip> clip; QPointer<Track> track; };
	struct InstrumentEntry { QPointer<InstrumentTrack> track; QPointer<Instrument> instrument; };
	std::function<void()> m_context, m_poll;
	QPointer<Song> m_song;
	std::uint64_t m_generation;
	std::vector<TrackEntry> m_tracks;
	std::vector<ClipEntry> m_clips;
	std::vector<InstrumentEntry> m_instruments;
	std::vector<QPointer<AutomatableModel>> m_models;
	bool m_committed = false;
	int m_pattern;
	bool m_emptyColumn, m_buildingColumn = false;
	QPointer<PatternTrack> m_column;
};

bool MidiImport::tryImport(TrackContainer* tc)
{ return tryImport(tc, -1); }

bool MidiImport::tryImport(TrackContainer* tc, int pattern)
{
	if (!tc || !Engine::getSong() || !Engine::audioEngine()
		|| QThread::currentThread() != Engine::getSong()->thread() || tc->thread() != Engine::getSong()->thread()) { return false; }
	const bool inPattern = tc == Engine::patternStore();
	if (inPattern)
	{
		const auto count = Engine::patternStore()->numOfPatterns();
		if (pattern == -1) { pattern = count ? Engine::patternStore()->currentPattern() : 0; }
		if (pattern < 0 || pattern >= std::max(1, count)) { return false; }
	}
	else if (pattern != -1) { return false; }
	const QString requested(file().fileName()); const QPointer<Song> owner(Engine::getSong());
	try
	{
		WorkScope work(this, tc, pattern); MidiImportState state([&] { work.checkContext(); }, [&] { work.check(); }, pattern);
		const FileResourceIO::EventSuspension noReentry;
		state.poll(); QFile input(requested); if (!openFile(input)) { return false; } state.context();
		const auto bytes = readMidiImportData(input); state.context();
		if (bytes.isEmpty()) { return false; }
#ifdef LMMS_HAVE_FLUIDSYNTH
		if (gui::getGUI() != nullptr && ConfigManager::inst()->sf2File().isEmpty())
		{
			QMessageBox::information(gui::getGUI()->mainWindow(),
				tr("Setup incomplete"),
				tr("You have not set up a default soundfont in "
					"the settings dialog (Edit->Settings). "
					"Therefore no sound will be played back after "
					"importing this MIDI file. You should download "
					"a General MIDI soundfont, specify it in "
					"settings dialog and try again."));
		}
#else
		if (gui::getGUI() != nullptr)
		{
			QMessageBox::information(gui::getGUI()->mainWindow(),
				tr("Setup incomplete"),
				tr("You did not compile LMMS with support for "
					"SoundFont2 player, which is used to add default "
					"sound to imported MIDI files. "
					"Therefore no sound will be played back after "
					"importing this MIDI file."));
		}
#endif

		state.context();
		if (inPattern)
		{
			std::vector<QPointer<Track>> rows; for (auto row : tc->tracks()) { rows.emplace_back(row); }
			for (const auto& row : rows)
			{
				state.poll();
				if (!row || row->numOfClips() != Engine::patternStore()->numOfPatterns())
				{ throw std::runtime_error("MIDI import requires complete native pattern columns"); }
			}
		}
		if (!readSMF(tc, bytes, pattern, state)) { return false; }
		state.commit(); return true;
	}
	catch (const std::exception& error)
	{
		const auto message = tr("MIDI import failed: %1").arg(QString::fromUtf8(error.what()));
		if (owner && Engine::getSong() == owner.data()) { owner->collectError(message); } qWarning().noquote() << message;
	}
	catch (...) { if (owner && Engine::getSong() == owner.data()) { owner->collectError(tr("MIDI import failed with an unknown native exception")); } }
	return false;
}


class smfMidiCC
{

public:
	QPointer<AutomationTrack> at;
	QPointer<AutomationClip> ap;
	TimePos lastPos = 0;
	bool inPattern = false;

	smfMidiCC& create(TrackContainer* tc, QString tn, int pattern, MidiImportState& state)
	{
		if (!at)
		{
			state.poll();
			at = dynamic_cast<AutomationTrack*>(state.track(Track::Type::Automation, tc));
			inPattern = pattern >= 0;
			if (inPattern) { ap = dynamic_cast<AutomationClip*>(state.cell(at.data(), pattern)); }
		}
		if (!at) { throw std::runtime_error("MIDI automation track is unavailable"); }
		if (tn != "") { state.apply([&] { at->setName(tn); }); }
		return *this;
	}


	smfMidiCC& putValue(TimePos time, AutomatableModel* objModel, float value, MidiImportState& state)
	{
		state.context(); const QPointer<AutomatableModel> target(state.model(objModel));
		if (!inPattern && (!ap || time > lastPos + DefaultTicksPerBar))
		{
			TimePos pPos = TimePos(time.getBar(), 0);
			ap = dynamic_cast<AutomationClip*>(state.clip(at.data(), pPos));
			if (!ap) { throw std::runtime_error("MIDI automation clip is unavailable"); }
			state.apply([&] { ap->addObject(target.data()); });
		}
		else if (inPattern && ap && ap->objects().empty()) { state.apply([&] { ap->addObject(target.data()); }); }
		if (!ap) { throw std::runtime_error("MIDI automation clip is unavailable"); }

		lastPos = time;
		if (!inPattern) { time = time - ap->startPosition(); }
		state.apply([&] { ap->putValue(time, value, false); });
		state.apply([&] { ap->changeLength(TimePos(time.getBar() + 1, 0)); });
		return *this;
	}
};



class smfMidiChannel
{

public:
	QPointer<InstrumentTrack> it;
	QPointer<MidiClip> p;
	QPointer<Instrument> it_inst;
	bool isSF2 = false;
	bool hasNotes = false;
	bool hasGenericCC = false;
	QString trackName;
	MidiPatch currentPatch;

	smfMidiChannel* create(TrackContainer* tc, QString tn, int pattern, MidiImportState& state)
	{
		if (!it) {
			state.poll();
			it = dynamic_cast<InstrumentTrack*>(state.track(Track::Type::Instrument, tc));
			if (!it) { throw std::runtime_error("MIDI instrument track is unavailable"); }

#ifdef LMMS_HAVE_FLUIDSYNTH
			it_inst = state.instrument(it.data(), "sf2player");

			if (it_inst && QString::fromUtf8(it_inst->descriptor()->name) == "sf2player"
				&& it_inst->childModel("bank") && it_inst->childModel("patch"))
			{
				isSF2 = true;
				state.apply([&] { it_inst->loadFile(ConfigManager::inst()->sf2File()); });

				const QPointer<AutomatableModel> bank(state.model(it_inst.data(), "bank"));
				state.apply([&] { bank->setValue(0); });
				currentPatch.bank = 0;

				const QPointer<AutomatableModel> patch(state.model(it_inst.data(), "patch"));
				state.apply([&] { patch->setValue(0); });
				currentPatch.program = 0;
			}
			else { it_inst = state.instrument(it.data(), "patman"); }
#else
			it_inst = state.instrument(it.data(), "patman");
#endif
			trackName = tn;
			if (trackName != "") { state.apply([&] { it->setName(tn); }); }
			// General MIDI default
			state.apply([&] { it->pitchRangeModel()->setInitValue(2); });
			// Match the native SoundFont controller defaults before a later curve
			// takes over. In particular, expression must not start by muting notes.
			state.apply([&] { it->initializeMidiCC(8, 64); });
			for (const int controller : {11, 43, 98, 99, 100, 101}) { state.apply([&] { it->initializeMidiCC(controller, 127); }); }
			for (int controller = 70; controller <= 79; ++controller) { state.apply([&] { it->initializeMidiCC(controller, 64); }); }
			// Native PatternStore construction already creates every column cell.
			p = dynamic_cast<MidiClip*>(pattern >= 0 ? state.cell(it.data(), pattern) : state.clip(it.data(), 0));
			if (!p) { throw std::runtime_error("MIDI note clip is unavailable"); }
			if (pattern >= 0) { state.apply([&] { p->clearNotes(); }); p->setAutoResize(false); state.apply([&] { p->setType(MidiClip::Type::MelodyClip); }); }
		}
		return this;
	}


	void addNote(Note& n, MidiImportState& state)
	{
		state.context(); if (!p) { p = dynamic_cast<MidiClip*>(state.clip(it.data(), 0)); }
		if (!p) { throw std::runtime_error("MIDI note clip is unavailable"); }
		state.apply([&] { p->addNote(n, false); });
		hasNotes = true;
	}


	void splitMidiClips(MidiImportState& state)
	{
		QPointer<MidiClip> newMidiClip;
		TimePos lastEnd(0);
		state.apply([&] { p->rearrangeAllNotes(); });
		std::vector<Note> notes; std::vector<Note*> identities; std::vector<std::shared_ptr<const void>> lifetimes;
		for (auto note : p->notes()) { notes.emplace_back(*note); identities.push_back(note); lifetimes.push_back(note->selectionIdentity()); }
		const auto validateSource = [&] {
			state.context(); if (p->notes().size() != identities.size()) { throw std::runtime_error("MIDI split source notes changed"); }
			for (std::size_t index = 0; index < identities.size(); ++index)
			{
				const auto current = p->notes()[index]; const auto& expected = notes[index];
				if (current != identities[index] || current->selectionIdentity() != lifetimes[index] || current->pos() != expected.pos() || current->length() != expected.length()
					|| current->key() != expected.key() || current->getVolume() != expected.getVolume() || current->getPanning() != expected.getPanning() || current->type() != expected.type() || current->detuning() != expected.detuning())
				{ throw std::runtime_error("MIDI split source note identity or value changed"); }
			}
		};
		std::size_t count = 0;
		for (const auto& n : notes)
		{
			if ((count++ % 256) == 0) { state.poll(); validateSource(); }
			if (!newMidiClip || n.pos() > lastEnd + DefaultTicksPerBar)
			{
				TimePos pPos = TimePos(n.pos().getBar(), 0);
				newMidiClip = dynamic_cast<MidiClip*>(state.clip(it.data(), pPos)); validateSource();
			}
			if (!newMidiClip) { throw std::runtime_error("MIDI split destination clip is unavailable"); }
			lastEnd = std::max(lastEnd, n.endPos());

			Note newNote(n);
			newNote.setPos(n.pos(newMidiClip->startPosition()));
			state.apply([&] { newMidiClip->addNote(newNote, false); });
		}

		validateSource(); const QPointer<MidiClip> source(p); state.forget(source.data()); p = nullptr;
		delete source.data(); state.context();
	}
};


bool MidiImport::readSMF(TrackContainer* tc, const QByteArray& bytes, int pattern, MidiImportState& state)
{
	const auto tick = [pattern](double beats)
	{ return importTick(beats, pattern >= 0 ? std::int64_t(pattern) * TimePos::ticksPerBar() : 0); };
	constexpr int preTrackSteps = 2;
	QPointer<QProgressDialog> pd;
	const auto closeProgress = qScopeGuard([&] { delete pd.data(); });
	if (gui::getGUI() != nullptr)
	{
		pd = new QProgressDialog(TrackContainer::tr("Importing MIDI-file..."),
			TrackContainer::tr("Cancel"), 0, preTrackSteps, gui::getGUI()->mainWindow());
		state.apply([&] { if (pd) { pd->setWindowTitle(TrackContainer::tr("Please wait...")); } });
		// A modal setValue() pumps events inside partially built native models.
		state.apply([&] { if (pd) { pd->setWindowModality(Qt::NonModal); } });
		state.apply([&] { if (pd) { pd->setMinimumDuration(0); } });
		state.apply([&] { if (pd) { pd->setValue(0); } });
	}

	state.poll();
	std::istringstream stream(bytes.toStdString());
	auto seq = std::make_unique<Alg_seq>(stream, true);
	if (seq->get_read_error()) { throw std::runtime_error("PortSMF rejected the MIDI event stream"); }
	seq->convert_to_beats(); state.poll();
	// PortSMF's sequence duration includes end-of-track trailing silence.
	tick_t patternEnd = pattern >= 0 ? tick(seq->get_dur()) : 0;

	state.apply([&] { if (pd) { pd->setMaximum(seq->tracks() + preTrackSteps); } });
	state.apply([&] { if (pd) { pd->setValue(1); } });

	// Native tracks are split by source track AND logical channel (including
	// MIDI port prefixes). Each target has its own automation curve.
	std::map<std::pair<int, long>, smfMidiChannel> chs;
	std::map<std::tuple<int, long, int>, smfMidiCC> automation;

	MeterModel & timeSigMM = Engine::getSong()->getTimeSigModel();
	auto automationContainer = pattern >= 0 ? tc : static_cast<TrackContainer*>(Engine::getSong());
	const auto automationCell = [pattern, &state](AutomationTrack* track)
	{
		auto clip = dynamic_cast<AutomationClip*>(pattern >= 0 ? state.cell(track, pattern) : state.clip(track, 0));
		if (!clip) { throw std::runtime_error("MIDI metadata automation clip is unavailable"); } return clip;
	};
	const QPointer<AutomationTrack> nt(dynamic_cast<AutomationTrack*>(state.track(Track::Type::Automation, automationContainer)));
	state.apply([&] { nt->setName(tr("MIDI Time Signature Numerator")); });
	const QPointer<AutomationTrack> dt(dynamic_cast<AutomationTrack*>(state.track(Track::Type::Automation, automationContainer)));
	state.apply([&] { dt->setName(tr("MIDI Time Signature Denominator")); });
	const QPointer<AutomationClip> timeSigNumeratorPat(automationCell(nt.data()));
	state.apply([&] { timeSigNumeratorPat->setDisplayName(tr("Numerator")); });
	state.apply([&] { timeSigNumeratorPat->addObject(state.model(&timeSigMM.numeratorModel())); });
	const QPointer<AutomationClip> timeSigDenominatorPat(automationCell(dt.data()));
	state.apply([&] { timeSigDenominatorPat->setDisplayName(tr("Denominator")); });
	state.apply([&] { timeSigDenominatorPat->addObject(state.model(&timeSigMM.denominatorModel())); });


	// Time-sig changes
	Alg_time_sigs* timeSigs = &seq->time_sig;
	for (int s = 0; s < timeSigs->length(); ++s)
	{
		state.poll(); const Alg_time_sig timeSig = (*timeSigs)[s];
		if (pattern >= 0) { patternEnd = std::max(patternEnd, tick(timeSig.beat) + 1); }
		state.apply([&] { timeSigNumeratorPat->putValue(tick(timeSig.beat), importValue(timeSig.num), false); });
		state.apply([&] { timeSigDenominatorPat->putValue(tick(timeSig.beat), importValue(timeSig.den), false); });
	}
	// manually call otherwise the pattern shows being 1 bar
	state.apply([&] { timeSigNumeratorPat->updateLength(); });
	state.apply([&] { timeSigDenominatorPat->updateLength(); });

	state.apply([&] { if (pd) { pd->setValue(2); } });

	// Tempo stuff
	const QPointer<AutomationTrack> tt(dynamic_cast<AutomationTrack*>(state.track(Track::Type::Automation, automationContainer)));
	state.apply([&] { tt->setName(tr("Tempo")); });
	const QPointer<AutomationClip> tap(automationCell(tt.data()));
	state.apply([&] { tap->setDisplayName(tr("Tempo")); });
	state.apply([&] { tap->addObject(state.model(&Engine::getSong()->tempoModel())); });
	if (tap)
	{
		state.apply([&] { tap->clear(); });
		Alg_time_map* timeMap = seq->get_time_map();
		Alg_beats& beats = timeMap->beats;
		for (int i = 0; i < beats.len - 1; ++i)
		{
			Alg_beat* b = &(beats[i]);
			if (pattern >= 0) { patternEnd = std::max(patternEnd, tick(b->beat) + 1); }
			double tempo = (beats[i + 1].beat - b->beat) / (beats[i + 1].time - beats[i].time);
			state.poll(); state.apply([&] { tap->putValue(tick(b->beat), importValue(tempo * 60.0), false); });
		}
		if (timeMap->last_tempo_flag)
		{
			if (beats.len <= 0) { throw std::runtime_error("MIDI tempo map is empty"); }
			Alg_beat* b = &beats[beats.len - 1];
			if (pattern >= 0) { patternEnd = std::max(patternEnd, tick(b->beat) + 1); }
			state.poll(); state.apply([&] { tap->putValue(tick(b->beat), importValue(timeMap->last_tempo * 60.0), false); });
		}
	}

	// Update the tempo to avoid crash when playing a project imported
	// via the command line
	state.apply([&] { Engine::updateFramesPerTick(); });

	std::size_t unhandledEvents = 0;
	// Song events
	for (int e = 0; e < seq->length(); ++e)
	{
		if ((e % 256) == 0) { state.poll(); }
		Alg_event* evt = (*seq)[e];
		if (evt->is_update()) { ++unhandledEvents; }
	}

	// Tracks
	for (int t = 0; t < seq->tracks(); ++t)
	{
		QString trackName = QString(tr("Track") + " %1").arg(t);
		Alg_track* trk = seq->track(t);
		state.apply([&] { if (pd) { pd->setValue(t + preTrackSteps); } });

		state.poll();
		if (pd && pd->wasCanceled()) { throw std::runtime_error("MIDI import cancelled"); }

		// Now look at events
		for (int e = 0; e < trk->length(); ++e)
		{
			if ((e % 256) == 0) { state.poll(); }
			Alg_event* evt = (*trk)[e];
			if (pattern >= 0) { patternEnd = std::max(patternEnd, tick(evt->time) + 1); }

			if (evt->chan == -1)
			{
				bool handled = false;
				if (evt->is_update())
				{
					QString attr = evt->get_attribute();
					// seqnames is a track0 identifier (see allegro code)
					if (attr == (t == 0 ? "seqnames" : "tracknames")
						&& evt->get_update_type() == 's')
					{
						trackName = evt->get_string_value();
						handled = true;
					}
				}
				if (!handled) { ++unhandledEvents; }
			}
			else if (evt->is_note())
			{
				// LMMS does not currently support specifying the channel of a single note
				// To be safe, put the notes from different channels on separate tracks so that no information is lost
				smfMidiChannel* ch = chs[{t, evt->chan}].create(tc, trackName, pattern, state);
				auto noteEvt = static_cast<Alg_note*>(evt);
				const auto start = tick(noteEvt->get_start_time());
				const auto end = tick(noteEvt->get_start_time() + noteEvt->get_duration());
				if (noteEvt->get_duration() < 0 || noteEvt->get_identifier() < 0 || noteEvt->get_identifier() > 127)
				{ throw std::out_of_range("Invalid MIDI note key or duration"); }
				if (!std::isfinite(noteEvt->get_loud()) || noteEvt->get_loud() < 0 || noteEvt->get_loud() > 127)
				{ throw std::out_of_range("Invalid MIDI note velocity"); }
				const auto volume = noteEvt->get_loud() * (200. / 127.);
				Note n(std::max<tick_t>(1, end - start), start, noteEvt->get_identifier(), static_cast<volume_t>(std::lround(volume)));
				ch->addNote(n, state);
				if (pattern >= 0) { patternEnd = std::max(patternEnd, n.endPos().getTicks()); }
			}
			else if (evt->is_update())
			{
				smfMidiChannel* ch = chs[{t, evt->chan}].create(tc, trackName, pattern, state);

				const auto time = tick(evt->time);
				QString update(evt->get_attribute());

				if (update == "programi")
				{
					const auto prog = evt->get_integer_value();
					if (prog < 0 || prog > 127) { throw std::out_of_range("Invalid MIDI program"); }
					if (ch->isSF2)
					{
						auto& pc = automation[{t, evt->chan, 129}];
						const QPointer<AutomatableModel> objModel(state.model(ch->it_inst.data(), "patch"));
						assert(objModel != nullptr);
						if (time == 0)
						{
							state.apply([&] { objModel->setInitValue(prog); });
						}
						if (time != 0 || pattern >= 0)
						{
							if (!pc.at)
							{
								pc.create(tc, trackName + " > " + objModel->displayName(), pattern, state);
							}
							pc.putValue(time, objModel.data(), prog, state);
						}
					}
					else
					{
						const QString num = QString::number(prog);
						const QString filter = QString().fill('0', 3 - num.length()) + num + "*.pat";
						const QString dir = "/usr/share/midi/"
								"freepats/Tone_000/";
						const QStringList files = QDir(dir).
						entryList(QStringList(filter));
						if (ch->it_inst && !files.empty())
						{
							state.apply([&] { ch->it_inst->loadFile(dir + files.front()); });
						}
					}
				}
				else if (update.startsWith("control") || update == "bendr")
				{
					int ccid = update.mid(7, update.length() - 8).toInt();
					if (update == "bendr") { ccid = 128; }
					if (ccid >= 0 && ccid <= 128)
					{
						double cc = importValue(evt->get_real_value());
						QPointer<AutomatableModel> modelObject;
						// Some CC knobs correspond to specific things like pitch and volume, so in that case, set the lmms pitch/volume/etc knobs.
						// Otherwise, set the knob in the instrument track CC rack
						switch (ccid)
						{
							case 0:
								if (ch->isSF2 && ch->it_inst)
								{
									modelObject = state.model(ch->it_inst.data(), "bank");
									assert(modelObject != nullptr);
									ch->currentPatch.setBankMSB(static_cast<std::uint8_t>(std::clamp(std::lround(cc * 127), 0l, 127l)));
									cc = static_cast<double>(ch->currentPatch.bank);
								}
								break;

							case 7:
								modelObject = ch->it->volumeModel();
								cc *= 100.0f;
								break;

							case 10:
								modelObject = ch->it->panningModel();
								cc = cc * 200.f - 100.0f;
								break;

							case 32:
								if (ch->isSF2 && ch->it_inst)
								{
									modelObject = state.model(ch->it_inst.data(), "bank");
									assert(modelObject != nullptr);
									ch->currentPatch.setBankLSB(static_cast<std::uint8_t>(std::clamp(std::lround(cc * 127), 0l, 127l)));
									cc = static_cast<double>(ch->currentPatch.bank);
								}
								break;

							case 128:
								modelObject = ch->it->pitchModel();
								cc *= 100.0 * ch->it->pitchRangeModel()->value();
								break;

							default:
								if (ccid >= 0 && ccid < 128)
								{
									modelObject = ch->it->midiCCModel(ccid);
									state.apply([&] { ch->it->activateMidiCC(ccid); }); ch->hasGenericCC = true;
									cc = cc * 127.0f;
								}
								break;
						}

						if (modelObject)
						{
							state.model(modelObject.data());
							cc = importValue(cc);
							if (time == 0)
							{
								state.apply([&] { modelObject->setInitValue(cc); });
							}
							if (time != 0 || pattern >= 0)
							{
								// Bank MSB and LSB both automate one combined native bank model.
								auto& ccTrack = automation[{t, evt->chan, ccid == 32 ? 0 : ccid}];
								ccTrack.create(tc, trackName + " > " + modelObject->displayName(), pattern, state);
								ccTrack.putValue(time, modelObject.data(), cc, state);
							}
						}
					}
				}
				else { ++unhandledEvents; }
			}
		}
	}

	seq.reset();

	for (auto& c: chs)
	{
		state.poll();
		if (pattern >= 0)
		{
			state.apply([&] { c.second.p->rearrangeAllNotes(); });
			state.apply([&] { c.second.p->changeLength(TimePos(std::max(1, patternEnd)).nextFullBar() * TimePos::ticksPerBar()); });
			state.apply([&] { Engine::patternStore()->updatePatternTrack(c.second.p.data()); });
		}
		else if (c.second.hasNotes)
		{
			c.second.splitMidiClips(state);
		}

		// Set channel 10 to drums as per General MIDI's orders
		if (c.first.second % 16l == 9 /* channel 10 */
			&& c.second.hasNotes && c.second.it_inst && c.second.isSF2)
		{
			const QPointer<AutomatableModel> bank(state.model(c.second.it_inst.data(), "bank"));
			assert(bank != nullptr);
			state.apply([&] { bank->setValue(128); });
			c.second.currentPatch.bank = 128;

			const QPointer<AutomatableModel> patch(state.model(c.second.it_inst.data(), "patch"));
			assert(patch != nullptr);
			state.apply([&] { patch->setValue(0); });
			c.second.currentPatch.program = 0;
		}
		if (c.second.hasGenericCC) { state.apply([&] { c.second.it->midiCCEnabledModel()->setInitValue(true); }); }
	}
	if (pattern >= 0 && chs.empty())
	{
		// PatternStore loop length is carried by instrument cells. A silent,
		// editable row preserves metadata-only imports without changing old rows.
		state.poll(); const QPointer<InstrumentTrack> row(static_cast<InstrumentTrack*>(state.track(Track::Type::Instrument, tc)));
		state.apply([&] { row->setName(tr("MIDI sequence length")); });
		const QPointer<MidiClip> cell(static_cast<MidiClip*>(state.cell(row.data(), pattern))); state.apply([&] { cell->clearNotes(); });
		state.apply([&] { cell->setType(MidiClip::Type::MelodyClip); }); cell->setAutoResize(false);
		state.apply([&] { cell->changeLength(TimePos(std::max(1, patternEnd)).nextFullBar() * TimePos::ticksPerBar()); });
		state.apply([&] { Engine::patternStore()->updatePatternTrack(cell.data()); });
	}
	if (pattern >= 0)
	{
		// Automation geometry follows the final shared period, including any
		// longer existing instrument row. Only new imported cells are resized.
		state.apply([&] { timeSigNumeratorPat->updateLength(); }); state.apply([&] { timeSigDenominatorPat->updateLength(); }); state.apply([&] { tap->updateLength(); });
		for (auto& item : automation) { state.poll(); if (item.second.ap) { state.apply([&] { item.second.ap->updateLength(); }); } }
	}

	if (unhandledEvents) { qWarning() << "MIDI import retained supported content; unmapped events:" << unhandledEvents; }

	state.context(); return true;
}


} // namespace lmms
