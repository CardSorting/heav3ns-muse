/*
 * GigPlayer.cpp - a GIG player using libgig (based on Sf2 player plugin)
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail/dot/com>
 * Copyright (c) 2009-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * A few lines of code taken from LinuxSampler (also GPLv2) where noted:
 * Copyright (C) 2003,2004 by Benno Senoner and Christian Schoenebeck
 * Copyright (C) 2005-2008 Christian Schoenebeck
 * Copyright (C) 2009-2010 Christian Schoenebeck and Grigor Iliev
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

#include "GigPlayer.h"
#include "GigPcm.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <iterator>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QThread>
#include <QScopeGuard>
#include <array>
#include <optional>
#include <stdexcept>
#include <QDebug>
#include <QLayout>
#include <QLabel>
#include <QDomDocument>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Mixer.h"
#include "TrackContainer.h"
#include "FileDialog.h"
#include "InstrumentTrack.h"
#include "InstrumentPlayHandle.h"
#include "Knob.h"
#include "MixHelpers.h"
#include "NotePlayHandle.h"
#include "PathUtil.h"
#include "Sample.h"
#include "Song.h"

#include "PatchesDialog.h"
#include "LcdSpinBox.h"

#include "embed.h"
#include "plugin_export.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT gigplayer_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"GIG Player",
	QT_TRANSLATE_NOOP( "PluginBrowser", "Player for GIG files" ),
	"Garrett Wilson <g/at/floft/dot/net>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	"gig",
	nullptr,
} ;

}

// QObject members may be retired by returned native model work.
class GigModelSignals
{
public:
	GigModelSignals(QObject* bank, QObject* patch) : m_bank(bank), m_patch(patch), m_bankBlocked(bank->blockSignals(true)), m_patchBlocked(patch->blockSignals(true)) { }
	~GigModelSignals() { if (m_bank) { m_bank->blockSignals(m_bankBlocked); } if (m_patch) { m_patch->blockSignals(m_patchBlocked); } }
private:
	QPointer<QObject> m_bank, m_patch; bool m_bankBlocked, m_patchBlocked;
};

class GigInstrument::ResourceGuard
{
public:
	explicit ResourceGuard(GigInstrument* owner, bool freeze = true, bool admit = true) : m_owner(owner), m_freeze(freeze), m_admit(admit)
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || !owner || owner->thread() != QThread::currentThread()
			|| !Engine::audioEngine() || !Engine::getSong() || !Engine::mixer()) { throw std::logic_error("GIG resource work requires the native application thread"); }
		if (admit && owner->m_changingResources) { throw std::logic_error("GIG settings/resources cannot reenter"); }
		m_track = owner->instrumentTrack(); m_selected = m_track ? m_track->instrument() : nullptr; m_selection = m_selected.data(); m_container = m_track ? m_track->trackContainer() : nullptr;
		m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_mixer = Engine::mixer(); m_project = m_song->replacementGeneration(); m_routing = m_mixer->replacementGeneration(); m_filename = m_song->projectFileName(); m_rate = m_engine->outputSampleRate();
		m_values = values(owner); adopted(); m_changes.emplace(m_engine->requestChangesGuard()); if (admit) { owner->m_changingResources = true; }
	}
	~ResourceGuard() { if (m_admit && m_owner) { m_owner->m_changingResources = false; } }
	void actors() const
	{
		if (!m_owner || m_owner->thread() != QThread::currentThread() || !m_track || m_track->isRetiring() || !m_owner->isFromTrack(m_track.data()) || m_track->thread() != QThread::currentThread()
			|| m_selected.data() != m_selection || m_track->instrument() != m_selection || !m_container || m_track->trackContainer() != m_container.data()
			|| std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) == m_container->tracks().end()
			|| !m_engine || Engine::audioEngine() != m_engine.data() || m_engine->outputSampleRate() != m_rate || !m_song || Engine::getSong() != m_song.data()
			|| !m_mixer || Engine::mixer() != m_mixer.data() || m_song->replacementGeneration() != m_project || m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename)
		{ throw std::runtime_error("Native GIG resource context changed"); }
	}
	void check() const
	{ actors(); if (m_owner->m_instance != m_instance || m_owner->m_filename != m_file || m_owner->m_unresolvedBank != m_unresolved || (m_owner->m_instance ? m_owner->m_instance->snapshot() : nullptr) != m_snapshot
		|| (m_freeze && values(m_owner.data()) != m_values)) { throw std::runtime_error("Native GIG instance/resource/controls changed"); } }
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
	void adopted()
	{
		actors(); const auto instance = m_owner->m_instance; const auto snapshot = instance ? instance->snapshot() : nullptr; const QString file = m_owner->m_filename; const bool unresolved = m_owner->m_unresolvedBank;
		m_instance = instance; m_file = file; m_unresolved = unresolved; m_snapshot = snapshot; check();
	}
	void freezeControls() { check(); m_values = values(m_owner.data()); m_freeze = true; }
	void thawControls() { check(); m_freeze = false; }
	void selectModels(int bank, int patch)
	{ check(); m_values[0] = bank; m_owner->m_bankNum.setValue(bank); check(); m_values[1] = patch; m_owner->m_patchNum.setValue(patch); check(); }
private:
	static std::array<float, 3> values(const GigInstrument* p) { return {p->m_bankNum.storedValue(), p->m_patchNum.storedValue(), p->m_gain.storedValue()}; }
	QPointer<GigInstrument> m_owner; QPointer<InstrumentTrack> m_track; QPointer<Instrument> m_selected; Instrument* m_selection = nullptr; QPointer<TrackContainer> m_container;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<Mixer> m_mixer; std::uint64_t m_project = 0, m_routing = 0; sample_rate_t m_rate = 0;
	GigInstance* m_instance = nullptr; QString m_filename, m_file; std::shared_ptr<FileResourceSnapshot> m_snapshot; std::array<float, 3> m_values{};
	bool m_unresolved = false; bool m_freeze, m_admit; std::optional<AudioEngine::RequestChangesGuard> m_changes; FileResourceIO::EventSuspension m_events;
};

GigInstrument::GigInstrument(InstrumentTrack* _instrument_track)
	: Instrument(_instrument_track, &gigplayer_plugin_descriptor, nullptr, Flag::IsSingleStreamed | Flag::IsNotBendable)
	, m_instance(nullptr)
	, m_instrument(nullptr)
	, m_filename("")
	, m_bankNum(0, 0, 16383, this, tr("Bank"))
	, m_patchNum(0, 0, 127, this, tr("Patch"))
	, m_gain(1.0f, 0.0f, 5.0f, 0.01f, this, tr("Gain"))
	, m_RandomSeed(0)
	, m_currentKeyDimension(0)
{
	auto iph = new InstrumentPlayHandle(this, _instrument_track);
	Engine::audioEngine()->addPlayHandle( iph );

	updateSampleRate();

	connect( &m_bankNum, SIGNAL( dataChanged() ), this, SLOT( updatePatch() ) );
	connect( &m_patchNum, SIGNAL( dataChanged() ), this, SLOT( updatePatch() ) );
	connect( Engine::audioEngine(), SIGNAL( sampleRateChanged() ), this, SLOT( updateSampleRate() ) );
}




GigInstrument::~GigInstrument()
{
	Engine::audioEngine()->removePlayHandlesOfTypes( instrumentTrack(),
				PlayHandle::Type::NotePlayHandle
				| PlayHandle::Type::InstrumentPlayHandle );
	freeInstance();
}




void GigInstrument::saveSettings(QDomDocument& document, QDomElement& element)
{
	ResourceGuard work(this); const QString filename = m_filename; const auto snapshot = m_instance ? m_instance->snapshot() : nullptr;
	element.setAttribute("src", filename);
	if (m_unresolvedBank) { element.setAttribute("unresolved", 1); } else { element.removeAttribute("unresolved"); }
	if (snapshot) { DataFile::registerResourceSnapshot(document, element, "src", snapshot); work.poll(); }
	m_patchNum.saveSettings(document, element, "patch"); work.poll();
	m_bankNum.saveSettings(document, element, "bank"); work.poll();
	m_gain.saveSettings(document, element, "gain"); work.poll();
}

void GigInstrument::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); const QString path = settings.attribute("src"); const bool pending = settings.attribute("unresolved") == "1"; ResourceGuard work(this, false);
	work.poll(); DataFile::ResourceReferenceScope resource(settings, "src"); work.check();
	if (path.contains(QChar::Null) || (pending && path.isEmpty())) { throw std::invalid_argument("An unresolved GIG bank requires one valid filename"); }
	QString absolute; bool absent = false;
	if (!path.isEmpty())
	{
		bool invalid = false; absolute = PathUtil::toAbsolute(path, &invalid);
		if (invalid || absolute.isEmpty()) { throw std::invalid_argument("Invalid native GIG source path"); }
		if (pending && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved GIG bank cannot discard an accepted resource"); }
		if (!pending) { const QFileInfo sourceFile(FileResourceSnapshot::readableSourcePath(absolute)); absent = !sourceFile.exists() && !sourceFile.isSymLink(); } work.check();
	}
	work.freezeControls(); emit fileLoading(); work.check(); QString error;
	const bool loaded = loadBank(pending || absent ? absolute : path, false, error, work, pending || absent ? BankLoad::Unresolved : BankLoad::Settings); work.check();
	if (!loaded) { collectErrorForUI(error); work.check(); }
	else if (m_unresolvedBank) { const QString missing = m_filename; Engine::getSong()->collectUnresolvedResource(missing); work.check(); }
	emit fileChanged(); work.poll(); work.thawControls();
	{
		GigModelSignals blocked(&m_bankNum, &m_patchNum);
		m_bankNum.loadSettings(settings, "bank"); work.check(); m_patchNum.loadSettings(settings, "patch"); work.check();
	}
	emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check();
	m_gain.loadSettings(settings, "gain"); work.check(); updatePatch(); work.check();
}

void GigInstrument::loadFile(const QString& path)
{
	QString error;
	if (!setFileResource("bank", path, error)) { collectErrorForUI(error); }
}

std::vector<Plugin::FileResource> GigInstrument::fileResources() const
{
	return {{"bank", tr("GIG bank"), m_filename, {"gig"}, nullptr, 0, true, m_instance ? m_instance->snapshot() : nullptr, m_unresolvedBank}};
}

bool GigInstrument::setFileResource(const QString& key, const QString& path, QString& error)
{
	const QString requestedKey(key), requested(path); ResourceGuard work(this);
	if (requestedKey != "bank") { error = tr("Unknown GIG resource"); return false; }
	emit fileLoading(); work.check(); const bool loaded = loadBank(requested, true, error, work); work.check(); emit fileChanged(); work.check(); return loaded;
}

bool GigInstrument::selectProgram(int bank, int program, QString& error)
{
	ResourceGuard work(this); const auto available = m_programs;
	const auto found = std::find_if(available.begin(), available.end(), [=](const Program& item) { return item.bank == bank && item.program == program; });
	if (found == available.end()) { error = tr("The loaded GIG bank has no bank %1 program %2").arg(bank).arg(program); return false; }
	{ GigModelSignals blocked(&m_bankNum, &m_patchNum); work.selectModels(bank, program); }
	updatePatch(); work.check(); emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check(); emit patchChanged(); work.check(); error.clear(); return true;
}

std::vector<Instrument::Program> GigInstrument::programSnapshot()
{ QMutexLocker lock(&m_synthMutex); return m_programs; }

void GigInstrument::updateProgramLabels(ResourceGuard& work)
{
	work.check(); const auto available = m_programs; const auto bank = m_bankNum.value(); work.check(); QMap<int, QString> banks, patches;
	for (const auto& item : available) { banks.insert(item.bank, tr("Bank %1").arg(item.bank)); if (item.bank == bank) { patches.insert(item.program, item.name); } }
	m_bankNum.setValueLabels(std::move(banks)); work.check(); m_patchNum.setValueLabels(std::move(patches)); work.check();
}

auto GigInstrument::midiPatch() const -> std::optional<MidiPatch>
{
	return MidiPatch {
		.bank = static_cast<std::uint16_t>(m_bankNum.value()),
		.program = static_cast<std::uint8_t>(m_patchNum.value())
	};
}




AutomatableModel* GigInstrument::childModel(std::string_view modelName)
{
	if (modelName == "bank")
	{
		return &m_bankNum;
	}
	else if (modelName == "patch")
	{
		return &m_patchNum;
	}

	qCritical() << "requested unknown model " << QString::fromUtf8(modelName.data(), modelName.size());

	return nullptr;
}




QString GigInstrument::nodeName() const
{
	return gigplayer_plugin_descriptor.name;
}




void GigInstrument::freeInstance()
{
	QMutexLocker synthLock( &m_synthMutex );
	QMutexLocker notesLock( &m_notesMutex );

	if( m_instance != nullptr )
	{
		delete m_instance;
		m_instance = nullptr;

		// If we're changing instruments, we got to make sure that we
		// remove all pointers to the old samples and don't try accessing
		// that instrument again
		m_instrument = nullptr;
		m_notes.clear();
	}
}




bool GigInstrument::loadBank(const QString& path, bool selectFirst, QString& error, ResourceGuard& work, BankLoad mode)
{
	bool unresolved = mode == BankLoad::Unresolved;
	const QString requested(path); if (requested.contains(QChar::Null)) { error = tr("GIG filename contains a null character"); return false; }
	work.poll(); QString relativePath; QString retainedPath(requested);
	if (unresolved && (requested.isEmpty() || !QDir::isAbsolutePath(requested))) { error = tr("An unresolved GIG bank requires an absolute filename"); return false; }
	std::unique_ptr<GigInstance> candidate;
	std::vector<Program> available;
	gig::Instrument* selectedInstrument = nullptr;
	const bool repairing = mode == BankLoad::File && m_unresolvedBank && !requested.isEmpty();
	const int bank = repairing ? m_bankNum.storedValue() : m_bankNum.value(); work.check(); const int patch = repairing ? m_patchNum.storedValue() : m_patchNum.value(); work.check(); Program selected{bank, patch, {}};
	if (!requested.isEmpty() && !unresolved)
	{
		bool invalid = false;
		const auto absolute = PathUtil::toAbsolute(requested, &invalid);
		const QFileInfo file(FileResourceSnapshot::readableSourcePath(absolute)); work.check();
		if (invalid || !file.isFile() || !file.isReadable()) { error = tr("GIG path is not a readable file: %1").arg(path); return false; }
		relativePath = PathUtil::toShortestRelative(absolute);
		// Cooperative file work must return outside instance mutexes and the libgig catch boundary.
		FileResourceSnapshot::GigCaptureFailure failure;
		auto accepted = FileResourceSnapshot::captureGig(absolute, error, failure); work.check();
		if (!accepted)
		{
			if (mode != BankLoad::Settings || failure != FileResourceSnapshot::GigCaptureFailure::MissingCompanion) { return false; }
			unresolved = true; retainedPath = absolute;
		}
		if (accepted)
		{
			try
			{
				candidate = std::make_unique<GigInstance>(std::move(accepted));
				// Resolve extension IDs and sample associations before publication. The
				// native reader can still stream sample PCM later from its owned group.
				(void)candidate->gig.GetFirstSample();
				for (auto instrument = candidate->gig.GetFirstInstrument(); instrument; instrument = candidate->gig.GetNextInstrument())
				{
					const int bank = instrument->MIDIBank, program = instrument->MIDIProgram;
					if (bank < 0 || bank > m_bankNum.maxValue() || program < 0 || program > m_patchNum.maxValue()) { continue; }
					const auto name = instrument->pInfo ? QString::fromStdString(instrument->pInfo->Name) : tr("Unnamed program");
					for (auto region = instrument->GetFirstRegion(); region; region = instrument->GetNextRegion())
					{
						if (region->DimensionRegions > std::size(region->pDimensionRegions)) { error = tr("GIG dimension-region count is invalid"); return false; }
						for (unsigned index = 0; index < region->DimensionRegions; ++index)
						{
							const auto dimension = region->pDimensionRegions[index];
							if (!dimension || !dimension->pSample) { continue; }
							const auto sample = dimension->pSample;
							if (sample->SamplesTotal && ((sample->Channels != 1 && sample->Channels != 2)
								|| (sample->BitDepth != 16 && sample->BitDepth != 24)
								|| sample->FrameSize != sample->Channels * (sample->BitDepth / 8) || !sample->SamplesPerSecond))
							{
								error = tr("GIG sample metadata is unsupported or invalid; native playback requires mono/stereo 16/24-bit PCM and a nonzero sample rate"); return false;
							}
							if (!dimension->SampleLoops) { continue; }
							if (!dimension->pSampleLoops) { error = tr("GIG sample loop declaration is missing"); return false; }
							const auto& loop = dimension->pSampleLoops[0];
							const auto length = dimension->pSample->SamplesTotal;
							if (!loop.LoopLength || loop.LoopStart >= length || loop.LoopLength > length - loop.LoopStart)
							{
								error = tr("GIG sample loop is empty or exceeds its sample"); return false;
							}
						}
					}
					available.push_back({bank, program, name});
					if (!selectedInstrument || ((repairing || !selectFirst) && bank == selected.bank && program == selected.program)) { selectedInstrument = instrument; }
				}
				if (available.empty()) { error = tr("GIG file contains no native MIDI programs"); return false; }
				std::stable_sort(available.begin(), available.end(), [](const Program& a, const Program& b) {
					return a.bank < b.bank || (a.bank == b.bank && a.program < b.program);
				});
				if (repairing && std::none_of(available.begin(), available.end(), [bank, patch](const Program& item) { return item.bank == bank && item.program == patch; }))
				{
					error = tr("The replacement GIG bank has no saved bank %1 program %2").arg(bank).arg(patch); return false;
				}
				selected = {static_cast<int>(selectedInstrument->MIDIBank), static_cast<int>(selectedInstrument->MIDIProgram), {}};
			}
			catch (...) { work.check(); error = tr("libgig rejected GIG bank: %1").arg(requested); return false; }
		}
	}
	work.poll();
	{
		QMutexLocker synth(&m_synthMutex), notes(&m_notesMutex);
		// Sample pointers belong to the old instance. Retire its note state before deleting it.
		m_notes.clear();
		std::unique_ptr<GigInstance> previous(m_instance);
		m_instance = candidate.release(); m_instrument = selectedInstrument;
		m_filename = unresolved ? retainedPath : relativePath; m_unresolvedBank = unresolved; m_programs = std::move(available);
	}
	work.adopted();
	if (!requested.isEmpty() && !unresolved) { GigModelSignals blocked(&m_bankNum, &m_patchNum); work.selectModels(selected.bank, selected.program); }
	updateProgramLabels(work); work.check();
	emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check(); emit patchChanged(); work.check();
	error.clear(); return true;
}

void GigInstrument::openFile(const QString& path, bool updateTrackName)
{
	const QString requested(path); ResourceGuard work(this); const QPointer<InstrumentTrack> track(instrumentTrack());
	emit fileLoading(); work.check(); QString error;
	if (loadBank(requested, false, error, work))
	{ if (updateTrackName && !requested.isEmpty()) { track->setName(PathUtil::cleanName(requested)); work.check(); } }
	else { collectErrorForUI(error); work.check(); }
	emit fileChanged(); work.check();
}

void GigInstrument::updatePatch()
{
	ResourceGuard work(this, true, false); updateProgramLabels(work); work.check(); const int bank = m_bankNum.value(); work.check(); const int patch = m_patchNum.value(); work.check();
	if (bank >= 0 && patch >= 0) { getInstrument(bank, patch); work.check(); }
}

QString GigInstrument::getCurrentPatchName()
{
	ResourceGuard work(this, true, false); const auto available = m_programs; const int bank = m_bankNum.value(); work.check(); const int patch = m_patchNum.value(); work.check();
	for (const auto& item : available) { if (item.bank == bank && item.program == patch) { return item.name; } } return {};
}

// A key has been pressed
void GigInstrument::playNote( NotePlayHandle * _n, SampleFrame* )
{
	const float LOG440 = 2.643452676f;

	int midiNote = (int) floor( 12.0 * ( log2( _n->unpitchedFrequency() ) - LOG440 ) - 4.0 );

	// out of range?
	if( midiNote <= 0 || midiNote >= 128 )
	{
		return;
	}

	if (!_n->m_pluginData)
	{
		auto pluginData = new GIGPluginData;
		pluginData->midiNote = midiNote;
		_n->m_pluginData = pluginData;

		const int baseVelocity = instrumentTrack()->midiPort()->baseVelocity();
		const uint velocity = _n->midiVelocity( baseVelocity );

		QMutexLocker locker( &m_notesMutex );
		m_notes.push_back( GigNote( midiNote, velocity, _n->unpitchedFrequency(), pluginData ) );
	}
}




// Process the notes and output a certain number of frames (e.g. 256, set in
// the preferences)
void GigInstrument::play( SampleFrame* _working_buffer )
{
	const f_cnt_t frames = Engine::audioEngine()->framesPerPeriod();
	const auto rate = Engine::audioEngine()->outputSampleRate();

	// Initialize to zeros
	std::memset( &_working_buffer[0][0], 0, DEFAULT_CHANNELS * frames * sizeof( float ) );

	QMutexLocker synth(&m_synthMutex), notes(&m_notesMutex);

	if (m_unresolvedBank || m_instance == nullptr || m_instrument == nullptr)
	{
		if (m_unresolvedBank) { m_notes.clear(); }
		return;
	}

	for( QList<GigNote>::iterator it = m_notes.begin(); it != m_notes.end(); ++it )
	{
		// Process notes in the KeyUp state, adding release samples if desired
		if( it->state == GigState::KeyUp )
		{
			// If there are no samples, we're done
			if( it->samples.empty() )
			{
				it->state = GigState::Completed;
			}
			else
			{
				it->state = GigState::PlayingKeyUp;

				// Notify each sample that the key has been released
				for (auto& sample : it->samples)
				{
					sample.adsr.keyup();
				}

				// Add release samples if available
				if( it->release == true )
				{
					addSamples( *it, true );
				}
			}
		}
		// Process notes in the KeyDown state, adding samples for the notes
		else if( it->state == GigState::KeyDown )
		{
			it->state = GigState::PlayingKeyDown;
			addSamples( *it, false );
		}

		// Delete ended samples
		for (auto sample = it->samples.begin(); sample != it->samples.end(); ++sample)
		{
			// Delete if the ADSR for a sample is complete for normal
			// notes, or if a release sample, then if we've reached
			// the end of the sample
			if( sample->sample == nullptr || sample->adsr.done() ||
				( it->isRelease == true &&
				  sample->pos >= sample->sample->SamplesTotal - 1 ) )
			{
				sample = it->samples.erase( sample );

				if( sample == it->samples.end() )
				{
					break;
				}
			}
		}

		// Delete ended notes (either in the completed state or all the samples ended)
		if( it->state == GigState::Completed || it->samples.empty() )
		{
			it = m_notes.erase( it );

			if( it == m_notes.end() )
			{
				break;
			}
		}
	}

	// Fill buffer with portions of the note samples
	for (auto& note : m_notes)
	{
		// Only process the notes if we're in a playing state
		if (!(note.state == GigState::PlayingKeyDown || note.state == GigState::PlayingKeyUp ))
		{
			continue;
		}

		for (auto& sample : note.samples)
		{
			if (sample.sample == nullptr || sample.region == nullptr) { continue; }

			float freq_factor = 1.0; // How much to resample

			// Resample to be the correct pitch when the sample provided isn't
			// solely for this one note (e.g. one or two samples per octave) or
			// we are processing at a different sample rate
			if (sample.region->PitchTrack == true || rate != sample.sample->SamplesPerSecond)
			{
				// Factor just for resampling
				freq_factor = 1.0 * rate / sample.sample->SamplesPerSecond;

				// Factor for pitch shifting as well as resampling
				if (sample.region->PitchTrack == true) { freq_factor *= sample.freqFactor; }
			}

			// Apply ADSR using a copy so if we don't use these samples when
			// resampling, the ADSR doesn't get messed up
			ADSR copy = sample.adsr;

			sample.m_resampler.setRatio(freq_factor);

			// TODO: These kind of playback pipelines/graphs are repeated within other parts of the codebase that work
			// with audio samples. We should find a way to unify this but the right abstraction is not so clear yet.
			auto framesMixed = f_cnt_t{0};
			while (framesMixed < frames)
			{
				if (sample.m_sourceBufferView.empty())
				{
					loadSample(sample, sample.m_sourceBuffer.data(), sample.m_sourceBuffer.size());

					for (auto& frame : sample.m_sourceBuffer)
					{
						frame *= copy.value();
					}

					sample.pos += sample.m_sourceBuffer.size();
					sample.adsr.inc(sample.m_sourceBuffer.size());
					sample.m_sourceBufferView = sample.m_sourceBuffer;
				}

				if (sample.m_mixBufferView.empty()) { sample.m_mixBufferView = sample.m_mixBuffer; }

				const auto [inputFramesUsed, outputFramesGenerated] = sample.m_resampler.process(
					{&sample.m_sourceBufferView.data()[0][0], 2, sample.m_sourceBufferView.size()},
					{&sample.m_mixBufferView.data()[0][0], 2, sample.m_mixBufferView.size()});

				if (inputFramesUsed == 0 && outputFramesGenerated == 0)
				{
					// A stalled voice contributes nothing further. Earlier voices
					// already mixed into this period retain their contribution.
					break;
				}

				const auto framesToMix = std::min(outputFramesGenerated, frames - framesMixed);
				for (auto i = f_cnt_t{0}; i < framesToMix; ++i)
				{
					_working_buffer[framesMixed + i] += sample.m_mixBufferView[i];
				}

				sample.m_sourceBufferView = sample.m_sourceBufferView.subspan(inputFramesUsed);
				sample.m_mixBufferView = sample.m_mixBufferView.subspan(framesToMix);
				framesMixed += framesToMix;
			}
		}
	}

	notes.unlock(); synth.unlock();

	// Set gain properly based on volume control
	for( f_cnt_t i = 0; i < frames; ++i )
	{
		_working_buffer[i][0] *= m_gain.value();
		_working_buffer[i][1] *= m_gain.value();
	}
}




void GigInstrument::loadSample(GigSample& sample, SampleFrame* sampleData, f_cnt_t samples)
{
	if (!sampleData || !samples || samples > DEFAULT_BUFFER_SIZE) { return; }
	const auto output = std::span<SampleFrame>(sampleData, static_cast<std::size_t>(samples));
	std::fill(output.begin(), output.end(), SampleFrame{});
	const auto native = sample.sample;
	if (!native || !sample.region || (native->Channels != 1 && native->Channels != 2)
		|| (native->BitDepth != 16 && native->BitDepth != 24)
		|| native->FrameSize != native->Channels * (native->BitDepth / 8)) { return; }

	bool loop = false;
	gig::loop_type_t loopType = gig::loop_type_normal;
	f_cnt_t loopStart = 0, loopLength = 0;
	if (sample.region->SampleLoops && sample.region->pSampleLoops)
	{
		const auto& definition = sample.region->pSampleLoops[0];
		loopStart = definition.LoopStart; loopLength = definition.LoopLength;
		loopType = static_cast<gig::loop_type_t>(definition.LoopType);
		loop = loopLength && loopStart < native->SamplesTotal && loopLength <= native->SamplesTotal - loopStart;
	}

	// A fixed, initialized block covers the native mono/stereo 16/24-bit shapes.
	// EOF and short loop reads leave deterministic silence, never stack bytes.
	alignas(std::int16_t) std::array<std::uint8_t, DEFAULT_BUFFER_SIZE * 6> buffer{};
	if (loop && (sample.pos >= loopStart || samples > loopStart - sample.pos))
	{
		const auto end = loopStart + loopLength;
		sample.pos = loopType == gig::loop_type_bidirectional
			? getPingPongIndex(sample.pos, loopStart, end)
			: getLoopedIndex(sample.pos, loopStart, end);
		native->SetPos(sample.pos);
		f_cnt_t total = 0;
		while (total < samples)
		{
			auto position = native->GetPos();
			if (position >= end) { native->SetPos(loopStart); position = loopStart; }
			const auto requested = std::min<f_cnt_t>(samples - total, end - position);
			if (!requested) { break; }
			const auto read = native->Read(buffer.data() + total * native->FrameSize, requested, m_instance->decompressionBuffer());
			if (!read || read > requested) { break; }
			total += read;
		}
	}
	else
	{
		native->SetPos(sample.pos);
		(void)native->Read(buffer.data(), samples, m_instance->decompressionBuffer());
	}
	const auto bytes = std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(samples * native->FrameSize));
	(void)gigPcm::decode(bytes, native->BitDepth, native->Channels, sample.attenuation, output);
}




// These two loop index functions taken from SampleBuffer.cpp
f_cnt_t GigInstrument::getLoopedIndex( f_cnt_t index, f_cnt_t startf, f_cnt_t endf ) const
{
	if( index < endf )
	{
		return index;
	}

	return startf + ( index - startf )
				% ( endf - startf );
}




f_cnt_t GigInstrument::getPingPongIndex( f_cnt_t index, f_cnt_t startf, f_cnt_t endf ) const
{
	if( index < endf )
	{
		return index;
	}

	const f_cnt_t looplen = endf - startf;
	const f_cnt_t looppos = ( index - endf ) % ( looplen * 2 );

	return ( looppos < looplen )
		? endf - looppos
		: startf + ( looppos - looplen );
}




// A key has been released
void GigInstrument::deleteNotePluginData( NotePlayHandle * _n )
{
	auto pluginData = static_cast<GIGPluginData*>(_n->m_pluginData);
	QMutexLocker locker( &m_notesMutex );

	// Mark the note as being released, but only if it was playing or was just
	// pressed (i.e., not if the key was already released)
	for (auto& note : m_notes)
	{
		// Find the note by matching pointers to the plugin data
		if (note.handle == pluginData && (note.state == GigState::KeyDown || note.state == GigState::PlayingKeyDown))
		{
			note.state = GigState::KeyUp;
		}
	}

	// TODO: not sample exact? What about in the middle of us writing out the sample?

	delete pluginData;
}




gui::PluginView* GigInstrument::instantiateView( QWidget * _parent )
{
	return new gui::GigInstrumentView( this, _parent );
}




// Add the desired samples (either the normal samples or the release samples)
// to the GigNote
//
// Note: not thread safe since libgig stores current region position data in
// the instrument object
void GigInstrument::addSamples( GigNote & gignote, bool wantReleaseSample )
{
	// Change key dimension, e.g. change samples based on what key is pressed
	// in a certain range. From LinuxSampler
	if( wantReleaseSample == true &&
			gignote.midiNote >= m_instrument->DimensionKeyRange.low &&
			gignote.midiNote <= m_instrument->DimensionKeyRange.high )
	{
		m_currentKeyDimension = float( gignote.midiNote -
				m_instrument->DimensionKeyRange.low ) / (
					m_instrument->DimensionKeyRange.high -
					m_instrument->DimensionKeyRange.low + 1 );
	}

	gig::Region* pRegion = m_instrument->GetFirstRegion();

	while( pRegion != nullptr )
	{
		Dimension dim = getDimensions( pRegion, gignote.velocity, wantReleaseSample );
		gig::DimensionRegion * pDimRegion = pRegion->GetDimensionRegionByValue( dim.DimValues );
		gig::Sample * pSample = pDimRegion ? pDimRegion->pSample : nullptr;

		// If this is a release sample, the note won't ever be
		// released, so we handle it differently
		gignote.isRelease = wantReleaseSample;

		// Does this note have release samples? Set this only on the original
		// notes and not when we get the release samples.
		if( wantReleaseSample != true )
		{
			gignote.release = dim.release;
		}

		if( pSample != nullptr && pSample->SamplesTotal != 0 )
		{
			int keyLow = pRegion->KeyRange.low;
			int keyHigh = pRegion->KeyRange.high;

			if( gignote.midiNote >= keyLow && gignote.midiNote <= keyHigh )
			{
				float attenuation = pDimRegion->GetVelocityAttenuation( gignote.velocity );
				float length = (float) pSample->SamplesTotal / Engine::audioEngine()->outputSampleRate();

				// TODO: sample panning? crossfade different layers?

				if( wantReleaseSample == true )
				{
					// From LinuxSampler, not sure how it was created
					attenuation *= 1 - 0.01053 * ( 256 >> pDimRegion->ReleaseTriggerDecay ) * length;
				}
				else
				{
					attenuation *= pDimRegion->SampleAttenuation;
				}

				gignote.samples.emplace_back(pSample, pDimRegion, attenuation, AudioResampler::Mode::Linear, gignote.frequency);
			}
		}

		pRegion = m_instrument->GetNextRegion();
	}
}




// Based on our input parameters, generate a "dimension" that specifies which
// note we wish to select from the GIG file with libgig. libgig will use this
// information to select the sample.
Dimension GigInstrument::getDimensions( gig::Region * pRegion, int velocity, bool release )
{
	Dimension dim;

	if( pRegion == nullptr )
	{
		return dim;
	}

	for( int i = pRegion->Dimensions - 1; i >= 0; --i )
	{
		switch( pRegion->pDimensionDefinitions[i].dimension )
		{
			case gig::dimension_layer:
				// TODO: implement this
				dim.DimValues[i] = 0;
				break;
			case gig::dimension_velocity:
				dim.DimValues[i] = velocity;
				break;
			case gig::dimension_releasetrigger:
				dim.release = true;
				dim.DimValues[i] = (uint) release;
				break;
			case gig::dimension_keyboard:
				dim.DimValues[i] = (uint) ( m_currentKeyDimension * pRegion->pDimensionDefinitions[i].zones );
				break;
			case gig::dimension_roundrobin:
			case gig::dimension_roundrobinkeyboard:
				// TODO: implement this
				dim.DimValues[i] = 0;
				break;
			case gig::dimension_random:
				// From LinuxSampler, untested
				m_RandomSeed = m_RandomSeed * 1103515245 + 12345;
				dim.DimValues[i] = uint(
					m_RandomSeed / 4294967296.0f * pRegion->pDimensionDefinitions[i].bits );
				break;
			case gig::dimension_samplechannel:
			case gig::dimension_channelaftertouch:
			case gig::dimension_modwheel:
			case gig::dimension_breath:
			case gig::dimension_foot:
			case gig::dimension_portamentotime:
			case gig::dimension_effect1:
			case gig::dimension_effect2:
			case gig::dimension_genpurpose1:
			case gig::dimension_genpurpose2:
			case gig::dimension_genpurpose3:
			case gig::dimension_genpurpose4:
			case gig::dimension_sustainpedal:
			case gig::dimension_portamento:
			case gig::dimension_sostenutopedal:
			case gig::dimension_softpedal:
			case gig::dimension_genpurpose5:
			case gig::dimension_genpurpose6:
			case gig::dimension_genpurpose7:
			case gig::dimension_genpurpose8:
			case gig::dimension_effect1depth:
			case gig::dimension_effect2depth:
			case gig::dimension_effect3depth:
			case gig::dimension_effect4depth:
			case gig::dimension_effect5depth:
			case gig::dimension_none:
			default:
				dim.DimValues[i] = 0;
				break;
		}
	}

	return dim;
}




// Get the selected instrument from the GIG file we opened if we haven't gotten
// it already. This is based on the bank and patch numbers.
void GigInstrument::getInstrument(int iBankSelected, int iProgSelected)
{
	// The guarded caller owns the effective model values before locking.

	QMutexLocker locker( &m_synthMutex );

	if( m_instance != nullptr )
	{
		gig::Instrument * pInstrument = m_instance->gig.GetFirstInstrument();

		while( pInstrument != nullptr )
		{
			int iBank = pInstrument->MIDIBank;
			int iProg = pInstrument->MIDIProgram;

			if( iBank == iBankSelected && iProg == iProgSelected )
			{
				break;
			}

			pInstrument = m_instance->gig.GetNextInstrument();
		}

		m_instrument = pInstrument;
	}
}




// Since the sample rate changes when we start an export, clear all the
// currently-playing notes when we get this signal. Then, the export won't
// include leftover notes that were playing in the program.
void GigInstrument::updateSampleRate()
{
	QMutexLocker locker( &m_notesMutex );
	m_notes.clear();
}



namespace gui
{


class gigKnob : public Knob
{
public:
	gigKnob( QWidget * _parent ) :
			Knob( KnobType::Bright26, _parent )
	{
		setFixedSize( 31, 38 );
	}
} ;




GigInstrumentView::GigInstrumentView( Instrument * _instrument, QWidget * _parent ) :
        InstrumentViewFixedSize( _instrument, _parent )
{
	auto k = castModel<GigInstrument>();

	connect( &k->m_bankNum, SIGNAL( dataChanged() ), this, SLOT( updatePatchName() ) );
	connect( &k->m_patchNum, SIGNAL( dataChanged() ), this, SLOT( updatePatchName() ) );

	// File Button
	m_fileDialogButton = new PixmapButton( this );
	m_fileDialogButton->setCursor(Qt::PointingHandCursor);
	m_fileDialogButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap( "fileselect_on" ) );
	m_fileDialogButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap( "fileselect_off" ) );
	m_fileDialogButton->move( 223, 68 );

	connect( m_fileDialogButton, SIGNAL( clicked() ), this, SLOT( showFileDialog() ) );

	m_fileDialogButton->setToolTip(tr("Open GIG file"));

	// Patch Button
	m_patchDialogButton = new PixmapButton( this );
	m_patchDialogButton->setCursor(Qt::PointingHandCursor);
	m_patchDialogButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap( "patches_on" ) );
	m_patchDialogButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap( "patches_off" ) );
	m_patchDialogButton->setEnabled( false );
	m_patchDialogButton->move( 223, 94 );

	connect( m_patchDialogButton, SIGNAL( clicked() ), this, SLOT( showPatchDialog() ) );

	m_patchDialogButton->setToolTip(tr("Choose patch"));

	// LCDs
	m_bankNumLcd = new LcdSpinBox(5, "11green", this);
	m_bankNumLcd->move(111, 155);

	m_patchNumLcd = new LcdSpinBox( 3, "21pink", this );
	m_patchNumLcd->move( 161, 150 );

	// Next row
	m_filenameLabel = new QLabel( this );
	m_filenameLabel->setGeometry( 61, 70, 156, 14 );
	m_patchLabel = new QLabel( this );
	m_patchLabel->setGeometry( 61, 94, 156, 14 );

	// Gain
	m_gainKnob = new gigKnob( this );
	m_gainKnob->setHintText( tr( "Gain:" ) + " ", "" );
	m_gainKnob->move( 32, 140 );

	setAutoFillBackground( true );
	QPalette pal;
	pal.setBrush( backgroundRole(), PLUGIN_NAME::getIconPixmap( "artwork" ) );
	setPalette( pal );

	updateFilename();
}




void GigInstrumentView::modelChanged()
{
	auto k = castModel<GigInstrument>();
	m_bankNumLcd->setModel( &k->m_bankNum );
	m_patchNumLcd->setModel( &k->m_patchNum );

	m_gainKnob->setModel( &k->m_gain );

	connect( k, SIGNAL( fileChanged() ), this, SLOT( updateFilename() ) );
	connect( k, SIGNAL( fileLoading() ), this, SLOT( invalidateFile() ) );

	updateFilename();
}




void GigInstrumentView::updateFilename()
{
	auto i = castModel<GigInstrument>();
	QFontMetrics fm( m_filenameLabel->font() );
	QString file = i->m_filename.endsWith( ".gig", Qt::CaseInsensitive ) ?
			i->m_filename.left( i->m_filename.length() - 4 ) :
			i->m_filename;
	m_filenameLabel->setText( fm.elidedText( file, Qt::ElideLeft, m_filenameLabel->width() ) );

	m_patchDialogButton->setEnabled(!i->m_unresolvedBank && !i->m_programs.empty());

	updatePatchName();
	update();
}




void GigInstrumentView::updatePatchName()
{
	auto i = castModel<GigInstrument>();
	QFontMetrics fm( font() );
	QString patch = i->getCurrentPatchName();
	m_patchLabel->setText( fm.elidedText( patch, Qt::ElideLeft, m_patchLabel->width() ) );

	update();
}




void GigInstrumentView::invalidateFile()
{
	m_patchDialogButton->setEnabled( false );
}




void GigInstrumentView::showFileDialog()
{
	const QPointer<GigInstrumentView> owner(this); const QPointer<GigInstrument> native(castModel<GigInstrument>()); const QPointer<PixmapButton> button(m_fileDialogButton);
	const QPointer<InstrumentTrack> track(native ? native->instrumentTrack() : nullptr); const QPointer<Song> song(Engine::getSong()); const auto generation = song ? song->replacementGeneration() : 0;
	if (!native || !button) { return; } const QString filename = native->m_filename;
	const auto current = [&] { return owner && native && button && owner->model() == native.data() && owner->m_fileDialogButton == button.data() && track && !track->isRetiring()
		&& track->instrument() == native.data() && song && Engine::getSong() == song.data() && song->replacementGeneration() == generation; };
	FileDialog dialog(nullptr, tr("Open GIG file")); dialog.setFileMode(FileDialog::ExistingFiles); dialog.setNameFilters({tr("GIG Files (*.gig)")});
	if (!filename.isEmpty()) { const auto absolute = PathUtil::toAbsolute(filename); dialog.setDirectory(QFileInfo(absolute).absolutePath()); dialog.selectFile(QFileInfo(absolute).fileName()); }
	else { dialog.setDirectory(ConfigManager::inst()->gigDir()); }
	if (!current()) { return; } button->setEnabled(false); const auto restore = qScopeGuard([&] { if (button) { button->setEnabled(true); } });
	if (!current()) { return; }
	const auto accepted = dialog.exec() == QDialog::Accepted; if (!current() || !accepted || dialog.selectedFiles().isEmpty()) { return; }
	const QString path = dialog.selectedFiles()[0]; if (path.isEmpty()) { return; }
	try { native->openFile(path); if (current()) { song->setModified(); } }
	catch (const std::exception& error) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError(QString::fromUtf8(error.what())); } }
	catch (...) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError("Native GIG loading failed"); } }
}


void GigInstrumentView::showPatchDialog()
{
	const QPointer<GigInstrumentView> owner(this); const QPointer<GigInstrument> native(castModel<GigInstrument>());
	const QPointer<InstrumentTrack> track(native ? native->instrumentTrack() : nullptr); const QPointer<Song> song(Engine::getSong()); const auto generation = song ? song->replacementGeneration() : 0;
	const auto current = [&] { return owner && native && owner->model() == native.data() && track && !track->isRetiring() && track->instrument() == native.data()
		&& song && Engine::getSong() == song.data() && song->replacementGeneration() == generation; };
	if (!current()) { return; } const QString& reported = track->name(); if (!current()) { return; } const QString caption(reported);
	const QPointer<PatchesDialog> dialog(new PatchesDialog(this)); const auto cleanup = qScopeGuard([dialog] { if (dialog) { delete dialog.data(); } });
	if (!current() || !dialog || !dialog->setup(native.data(), caption)) { return; }
	if (current() && dialog) { dialog->exec(); }
}


} // namespace gui


// Store information related to playing a sample from the GIG file
GigSample::GigSample(gig::Sample* pSample, gig::DimensionRegion* pDimRegion, float attenuation,
	AudioResampler::Mode interpolation, float desiredFreq)
	: sample(pSample)
	, region(pDimRegion)
	, attenuation(attenuation)
	, pos(0)
	, m_resampler(interpolation)
	, sampleFreq(0)
	, freqFactor(1)
{
	if( sample != nullptr && region != nullptr )
	{
		// Calculate note pitch and frequency factor only if we're actually
		// going to be changing the pitch of the notes
		if( region->PitchTrack == true )
		{
			// Calculate what frequency the provided sample is
			sampleFreq = 440.0f * std::exp2((region->UnityNote - 69 - region->FineTune * 0.01) / 12.0f);
			freqFactor = sampleFreq / desiredFreq;
		}

		// The sample rate we pass in is affected by how we are going to be
		// resampling the note so that a 1.5 second release ends up being 1.5
		// seconds after resampling
		adsr = ADSR( region, sample->SamplesPerSecond / freqFactor );
	}
}

GigSample::GigSample(const GigSample& g)
	: sample(g.sample)
	, region(g.region)
	, attenuation(g.attenuation)
	, adsr(g.adsr)
	, pos(g.pos)
	, m_resampler(AudioResampler::Mode::Linear, DEFAULT_CHANNELS)
	, sampleFreq(g.sampleFreq)
	, freqFactor(g.freqFactor)
{
}




GigSample& GigSample::operator=( const GigSample& g )
{
	sample = g.sample;
	region= g.region;
	attenuation = g.attenuation;
	adsr = g.adsr;
	pos = g.pos;
	sampleFreq = g.sampleFreq;
	freqFactor = g.freqFactor;
	return *this;
}

ADSR::ADSR()
	: preattack( 0 ), attack( 0 ), decay1( 0 ), decay2( 0 ), infiniteSustain( false ),
	  sustain( 0 ), release( 0 ),
	  amplitude( 0 ), isAttack( true ), isRelease( false ), isDone( false ),
	  attackPosition( 0 ), attackLength( 0 ), decayLength( 0 ),
	  releasePosition( 0 ), releaseLength( 0 )
{
}




// Create the ADSR envelope from the settings in the GIG file
ADSR::ADSR( gig::DimensionRegion * region, int sampleRate )
	: preattack( 0 ), attack( 0 ), decay1( 0 ), decay2( 0 ), infiniteSustain( false ),
	  sustain( 0 ), release( 0 ),
	  amplitude( 0 ), isAttack( true ), isRelease( false ), isDone( false ),
	  attackPosition( 0 ), attackLength( 0 ), decayLength( 0 ),
	  releasePosition( 0 ), releaseLength( 0 )
{
	if( region != nullptr )
	{
		// Parameters from GIG file
		preattack = 1.0 * region->EG1PreAttack / 1000; // EG1PreAttack is 0-1000 permille
		attack = region->EG1Attack;
		decay1 = region->EG1Decay1;
		decay2 = region->EG1Decay2;
		infiniteSustain = region->EG1InfiniteSustain;
		sustain = 1.0 * region->EG1Sustain / 1000; // EG1Sustain is 0-1000 permille
		release = region->EG1Release;

		// Simple ADSR using positions in sample
		amplitude = preattack;
		attackLength = attack * sampleRate;
		decayLength = decay1 * sampleRate; // TODO: ignoring decay2 for now
		releaseLength = release * sampleRate;

		// If there is no attack or decay, start at the sustain amplitude
		if( attackLength == 0 && decayLength == 0 )
		{
			amplitude = sustain;
		}
		// If there is no attack, start at the full amplitude
		else if( attackLength == 0 )
		{
			amplitude = 1.0;
		}
	}
}




// Next time we get the amplitude, we'll be releasing the note
void ADSR::keyup()
{
	isRelease = true;
}




// Can we delete the sample now?
bool ADSR::done()
{
	return isDone;
}




// Return the current amplitude and increment internal positions
float ADSR::value()
{
	float currentAmplitude = amplitude;

	// If we're done, don't output any signal
	if( isDone == true )
	{
		return 0;
	}
	// If we're still in the attack phase, release from the current volume
	// instead of jumping to the sustain volume and fading out
	else if( isAttack == true && isRelease == true )
	{
		sustain = amplitude;
		isAttack = false;
	}

	// If we're in the attack phase, start at the preattack amplitude and
	// increase to the full before decreasing to sustain
	if( isAttack == true )
	{
		if( attackPosition < attackLength )
		{
			amplitude = preattack + ( 1.0 - preattack ) / attackLength * attackPosition;
		}
		else if( attackPosition < attackLength + decayLength )
		{
			amplitude = 1.0 - ( 1.0 - sustain ) / decayLength * ( attackPosition - attackLength );
		}
		else
		{
			isAttack = false;
		}

		++attackPosition;
	}
	// If we're in the sustain phase, decrease from sustain to zero
	else if( isRelease == true )
	{
		// Maybe not the best way of doing this, but it appears to be about right
		// Satisfies f(0) = sustain and f(releaseLength) = very small
		amplitude = (sustain + 1e-3) * std::exp(-5.0f / releaseLength * releasePosition) - 1e-3;

		// Don't have an infinite exponential decay
		if( amplitude <= 0 || releasePosition >= releaseLength )
		{
			amplitude = 0;
			isDone = true;
		}

		++releasePosition;
	}

	return currentAmplitude;
}




// Increment internal positions a certain number of times
void ADSR::inc( f_cnt_t num )
{
	for( f_cnt_t i = 0; i < num; ++i )
	{
		value();
	}
}




extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main( Model *m, void * )
{
	return new GigInstrument( static_cast<InstrumentTrack *>( m ) );
}

}


} // namespace lmms
