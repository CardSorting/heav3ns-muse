/*
 * Sf2Player.cpp - a soundfont2 player using fluidSynth
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail/dot/com>
 * Copyright (c) 2009-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Sf2Player.h"
#include "Sf2BankSnapshot.h"
#include "Sf2SynthState.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <fluidsynth.h>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QThread>
#include <optional>
#include <stdexcept>
#include "FileResourceIO.h"
#include "AudioFileDevice.h"
#include "RemotePlugin.h"
#include "TrackRenderScope.h"
#include "Mixer.h"
#include "TrackContainer.h"
#include <QScopeGuard>
#include <QDebug>
#include <QDomElement>
#include <QLabel>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "FileDialog.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "InstrumentPlayHandle.h"
#include "Knob.h"
#include "NotePlayHandle.h"
#include "PathUtil.h"
#include "PixmapButton.h"
#include "Song.h"

#include "PatchesDialog.h"
#include "LcdSpinBox.h"

#include "embed.h"
#include "plugin_export.h"

#define FLUID_REVERB_DEFAULT_ROOMSIZE 0.2f
#define FLUID_REVERB_DEFAULT_DAMP 0.0f
#define FLUID_REVERB_DEFAULT_WIDTH 0.5f
#define FLUID_REVERB_DEFAULT_LEVEL 0.9f

#define FLUID_CHORUS_DEFAULT_N 3
#define FLUID_CHORUS_DEFAULT_LEVEL 2.0f
#define FLUID_CHORUS_DEFAULT_SPEED 0.3f
#define FLUID_CHORUS_DEFAULT_DEPTH 8.0f

namespace lmms
{


namespace
{
// Called only after native application-thread resource admission.
bool soundFontRenderConsuming()
{
	const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
	return RemotePlugin::hasActiveRenderContext() || (engine && dynamic_cast<AudioFileDevice*>(engine->audioDev()))
		|| (song && (song->isExporting() || song->hasPendingExportCleanup()));
}

bool soundFontRenderBusy()
{
	const auto engine = Engine::audioEngine();
	return soundFontRenderConsuming() || TrackRenderScope::hasActiveSelection() || (engine && engine->hasStoredAudioDevice());
}
}

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT sf2player_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"Sf2 Player",
	QT_TRANSLATE_NOOP( "PluginBrowser", "Player for SoundFont files" ),
	"Paul Giblock <drfaygo/at/gmail/dot/com>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	"sf2,sf3",
	nullptr,
} ;

}

/**
 * A non-owning reference to a single FluidSynth voice. Captures some initial
 * properties of the referenced voice to help manage changes to it over time.
 */
class FluidVoice
{
public:
	//! Create a reference to the voice currently pointed at by `voice`.
	explicit FluidVoice(fluid_voice_t* voice) :
		m_voice{voice},
		m_id{fluid_voice_get_id(voice)},
		m_coarseTune{fluid_voice_gen_get(voice, GEN_COARSETUNE)}
	{ }

	//! Get a pointer to the referenced voice.
	fluid_voice_t* get() const noexcept { return m_voice; }

	//! Get the original coarse tuning of the referenced voice.
	float coarseTune() const noexcept { return m_coarseTune; }

	//! Test whether this object still refers to the original voice.
	bool isValid() const
	{
		return fluid_voice_get_id(m_voice) == m_id && fluid_voice_is_playing(m_voice);
	}

private:
	fluid_voice_t* m_voice;
	unsigned int m_id;
	float m_coarseTune;
};

struct Sf2PluginData
{
	int midiNote = 0;
	int velocity = 0;
	std::vector<FluidVoice> fluidVoices;
	std::uint64_t generation = 0;
	bool isNew = true;
	bool started = false;
	f_cnt_t offset = 0;
	bool noteOffSent = false;
	panning_t panning = PanningCenter;
	float detuning = 0;
};



// Model signal suppression must survive a callback retiring the native owner.
class Sf2ModelSignals
{
public:
	Sf2ModelSignals(QObject* bank, QObject* patch) : m_bank(bank), m_patch(patch), m_bankBlocked(bank->blockSignals(true)), m_patchBlocked(patch->blockSignals(true)) { }
	~Sf2ModelSignals() { if (m_bank) { m_bank->blockSignals(m_bankBlocked); } if (m_patch) { m_patch->blockSignals(m_patchBlocked); } }
private:
	QPointer<QObject> m_bank, m_patch; bool m_bankBlocked, m_patchBlocked;
};

class Sf2Instrument::ResourceGuard
{
public:
	explicit ResourceGuard(Sf2Instrument* owner, bool freeze = true, bool admit = true) : m_owner(owner), m_freeze(freeze), m_admit(admit)
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || !owner || owner->thread() != QThread::currentThread()
			|| !Engine::audioEngine() || !Engine::getSong() || !Engine::mixer()) { throw std::logic_error("SoundFont resource work requires the native application thread"); }
		if (admit && owner->m_changingResources) { throw std::logic_error("SoundFont settings/resources cannot reenter"); }
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
		{ throw std::runtime_error("Native SoundFont resource context changed"); }
	}
	void check() const
	{ actors(); if (m_owner->m_synth != m_synth || m_owner->m_settings != m_settings || m_owner->m_synthGeneration != m_generation || m_owner->m_font != m_font || m_owner->m_fontId != m_fontId
		|| m_owner->m_bankSnapshot != m_snapshot || m_owner->m_filename != m_file || m_owner->m_unresolvedBank != m_unresolved || (m_freeze && values(m_owner.data()) != m_values)) { throw std::runtime_error("Native SoundFont synth/resource/controls changed"); } }
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
	void adopted()
	{
		actors(); const auto snapshot = m_owner->m_bankSnapshot; const QString file = m_owner->m_filename; const bool unresolved = m_owner->m_unresolvedBank; auto synth = m_owner->m_synth; auto settings = m_owner->m_settings; auto font = m_owner->m_font; const auto fontId = m_owner->m_fontId; const auto generation = m_owner->m_synthGeneration;
		m_snapshot = snapshot; m_file = file; m_unresolved = unresolved; m_synth = synth; m_settings = settings; m_font = font; m_fontId = fontId; m_generation = generation; check();
	}
	void freezeControls() { check(); m_values = values(m_owner.data()); m_freeze = true; }
	void thawControls() { check(); m_freeze = false; }
	void selectModels(int bank, int patch)
	{ check(); m_values[0] = bank; m_owner->m_bankNum.setValue(bank); check(); m_values[1] = patch; m_owner->m_patchNum.setValue(patch); check(); }
private:
	static std::array<float, 13> values(const Sf2Instrument* p)
	{ return {p->m_bankNum.storedValue(), p->m_patchNum.storedValue(), p->m_gain.storedValue(), p->m_reverbOn.storedValue(), p->m_reverbRoomSize.storedValue(), p->m_reverbDamping.storedValue(), p->m_reverbWidth.storedValue(), p->m_reverbLevel.storedValue(), p->m_chorusOn.storedValue(), p->m_chorusNum.storedValue(), p->m_chorusLevel.storedValue(), p->m_chorusSpeed.storedValue(), p->m_chorusDepth.storedValue()}; }
	QPointer<Sf2Instrument> m_owner; QPointer<InstrumentTrack> m_track; QPointer<Instrument> m_selected; Instrument* m_selection = nullptr; QPointer<TrackContainer> m_container;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<Mixer> m_mixer; std::uint64_t m_project = 0, m_routing = 0, m_generation = 0; sample_rate_t m_rate = 0;
	fluid_synth_t* m_synth = nullptr; fluid_settings_t* m_settings = nullptr; fluid_sfont_t* m_font = nullptr; int m_fontId = -1; QString m_filename, m_file; std::shared_ptr<Sf2BankSnapshot> m_snapshot;
	std::array<float, 13> m_values{}; bool m_unresolved = false; bool m_freeze, m_admit; std::optional<AudioEngine::RequestChangesGuard> m_changes; FileResourceIO::EventSuspension m_events;
};

Sf2Instrument::Sf2Instrument( InstrumentTrack * _instrument_track ) :
	Instrument(_instrument_track, &sf2player_plugin_descriptor, nullptr, Flag::IsSingleStreamed),
	m_resampler(AudioResampler::Mode::Linear),
	m_synth(nullptr),
	m_font( nullptr ),
	m_fontId( -1 ),
	m_filename( "" ),
	m_lastMidiPitch( -1 ),
	m_lastMidiPitchRange( -1 ),
	m_channel( 1 ),
	m_bankNum( 0, 0, 16383, this, tr("Bank") ),
	m_patchNum( 0, 0, 127, this, tr("Patch") ),
	m_gain( 1.0f, 0.0f, 5.0f, 0.01f, this, tr( "Gain" ) ),
	m_reverbOn( false, this, tr( "Reverb" ) ),
	m_reverbRoomSize( FLUID_REVERB_DEFAULT_ROOMSIZE, 0, 1.0, 0.01f, this, tr( "Reverb room size" ) ),
	m_reverbDamping(FLUID_REVERB_DEFAULT_DAMP, 0, 1.f, 0.01f, this, tr("Reverb damping")),
	m_reverbWidth( FLUID_REVERB_DEFAULT_WIDTH, 0, 1.0, 0.01f, this, tr( "Reverb width" ) ),
	m_reverbLevel( FLUID_REVERB_DEFAULT_LEVEL, 0, 1.0, 0.01f, this, tr( "Reverb level" ) ),
	m_chorusOn( false, this, tr( "Chorus" ) ),
	m_chorusNum( FLUID_CHORUS_DEFAULT_N, 0, 10.0, 1.0, this, tr( "Chorus voices" ) ),
	m_chorusLevel(FLUID_CHORUS_DEFAULT_LEVEL, 0, 10.f, 0.01f, this, tr("Chorus level")),
	m_chorusSpeed(FLUID_CHORUS_DEFAULT_SPEED, 0.29f, 5.f, 0.01f, this, tr("Chorus speed")),
	m_chorusDepth(FLUID_CHORUS_DEFAULT_DEPTH, 0, 46.f, 0.05f, this, tr("Chorus depth"))
{
	// Deactivate all audio drivers in fluidsynth
	const char* none[] = { nullptr };
	fluid_audio_driver_register(none);

	m_settings = new_fluid_settings();

	//fluid_settings_setint( m_settings, (char *) "audio.period-size", engine::audioEngine()->framesPerPeriod() );

	// This sets up m_synth and updates reverb/chorus/gain
	reloadSynth();

	// Get the default values from the setting
	double settingVal = 0;

	fluid_settings_getnum_default(m_settings, "synth.reverb.room-size", &settingVal);
	m_reverbRoomSize.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.reverb.damping", &settingVal);
	m_reverbDamping.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.reverb.width", &settingVal);
	m_reverbWidth.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.reverb.level", &settingVal);
	m_reverbLevel.setInitValue(settingVal);

	fluid_settings_getnum_default(m_settings, "synth.chorus.nr", &settingVal);
	m_chorusNum.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.chorus.level", &settingVal);
	m_chorusLevel.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.chorus.speed", &settingVal);
	m_chorusSpeed.setInitValue(settingVal);
	fluid_settings_getnum_default(m_settings, "synth.chorus.depth", &settingVal);
	m_chorusDepth.setInitValue(settingVal);

	// FIXME: there's no good way to tell if we're loading a preset or an empty instrument
	// We rely on instantiate() to load the default soundfont for new instruments,
	// but we don't need that when loading a project/preset/preview
	if (!Engine::getSong()->isLoadingProject() && !instrumentTrack()->isPreviewMode())
	{
		loadFile(ConfigManager::inst()->sf2File());
	}

	connect( &m_bankNum, SIGNAL( dataChanged() ), this, SLOT( updatePatch() ) );
	connect( &m_patchNum, SIGNAL( dataChanged() ), this, SLOT( updatePatch() ) );

	const QPointer<AudioEngine> engine(Engine::audioEngine());
	connect(engine.data(), &AudioEngine::sampleRateChanged, this, [this, engine] {
		try { reloadSynth(); }
		catch (...)
		{
			// Do not unwind Qt dispatch. Native engine transitions propagate the
			// retained failure after all connected notification slots return.
			if (engine && !engine->reportDeviceNotificationFailure(std::current_exception()))
			{ qWarning("SoundFont reload failed outside a native device notification frame"); }
		}
	});

	// Gain
	connect( &m_gain, SIGNAL( dataChanged() ), this, SLOT( updateGain() ) );

	// Reverb
	connect( &m_reverbOn, SIGNAL( dataChanged() ), this, SLOT( updateReverbOn() ) );
	connect( &m_reverbRoomSize, SIGNAL( dataChanged() ), this, SLOT( updateReverb() ) );
	connect( &m_reverbDamping, SIGNAL( dataChanged() ), this, SLOT( updateReverb() ) );
	connect( &m_reverbWidth, SIGNAL( dataChanged() ), this, SLOT( updateReverb() ) );
	connect( &m_reverbLevel, SIGNAL( dataChanged() ), this, SLOT( updateReverb() ) );

	// Chorus
	connect( &m_chorusOn, SIGNAL( dataChanged() ), this, SLOT( updateChorusOn() ) );
	connect( &m_chorusNum, SIGNAL( dataChanged() ), this, SLOT( updateChorus() ) );
	connect( &m_chorusLevel, SIGNAL( dataChanged() ), this, SLOT( updateChorus() ) );
	connect( &m_chorusSpeed, SIGNAL( dataChanged() ), this, SLOT( updateChorus() ) );
	connect( &m_chorusDepth, SIGNAL( dataChanged() ), this, SLOT( updateChorus() ) );

	// Microtuning
	connect(Engine::getSong(), &Song::scaleListChanged, this, &Sf2Instrument::updateTuning);
	connect(Engine::getSong(), &Song::keymapListChanged, this, &Sf2Instrument::updateTuning);
	connect(instrumentTrack()->microtuner()->enabledModel(), &Model::dataChanged, this, &Sf2Instrument::updateTuning, Qt::DirectConnection);
	connect(instrumentTrack()->microtuner()->scaleModel(), &Model::dataChanged, this, &Sf2Instrument::updateTuning, Qt::DirectConnection);
	connect(instrumentTrack()->microtuner()->keymapModel(), &Model::dataChanged, this, &Sf2Instrument::updateTuning, Qt::DirectConnection);
	connect(instrumentTrack()->microtuner()->keyRangeImportModel(), &Model::dataChanged, this, &Sf2Instrument::updateTuning, Qt::DirectConnection);
	connect(instrumentTrack()->baseNoteModel(), &Model::dataChanged, this, &Sf2Instrument::updateTuning, Qt::DirectConnection);

	auto iph = new InstrumentPlayHandle(this, _instrument_track);
	Engine::audioEngine()->addPlayHandle( iph );
}



Sf2Instrument::~Sf2Instrument()
{
	Engine::audioEngine()->removePlayHandlesOfTypes( instrumentTrack(),
				PlayHandle::Type::NotePlayHandle
				| PlayHandle::Type::InstrumentPlayHandle );
	freeFont();
	delete_fluid_synth( m_synth );
	delete_fluid_settings( m_settings );
}



void Sf2Instrument::saveSettings(QDomDocument& document, QDomElement& element)
{
	ResourceGuard work(this); const QString filename = m_filename; const auto snapshot = m_bankSnapshot;
	element.setAttribute("src", filename);
	if (m_unresolvedBank) { element.setAttribute("unresolved", 1); } else { element.removeAttribute("unresolved"); }
	if (snapshot) { DataFile::registerResourceSnapshot(document, element, "src", snapshot); work.check(); }
	m_patchNum.saveSettings(document, element, "patch"); work.check();
	m_bankNum.saveSettings(document, element, "bank"); work.check();
	m_gain.saveSettings(document, element, "gain"); work.check();
	m_reverbOn.saveSettings(document, element, "reverbOn"); work.check();
	m_reverbRoomSize.saveSettings(document, element, "reverbRoomSize"); work.check();
	m_reverbDamping.saveSettings(document, element, "reverbDamping"); work.check();
	m_reverbWidth.saveSettings(document, element, "reverbWidth"); work.check();
	m_reverbLevel.saveSettings(document, element, "reverbLevel"); work.check();
	m_chorusOn.saveSettings(document, element, "chorusOn"); work.check();
	m_chorusNum.saveSettings(document, element, "chorusNum"); work.check();
	m_chorusLevel.saveSettings(document, element, "chorusLevel"); work.check();
	m_chorusSpeed.saveSettings(document, element, "chorusSpeed"); work.check();
	m_chorusDepth.saveSettings(document, element, "chorusDepth"); work.check();
}

void Sf2Instrument::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); const QString path = settings.attribute("src"); const bool pending = settings.attribute("unresolved") == "1"; ResourceGuard work(this, false);
	work.poll(); DataFile::ResourceReferenceScope resource(settings, "src"); work.check();
	if (path.contains(QChar::Null) || (pending && path.isEmpty())) { throw std::invalid_argument("An unresolved SoundFont requires one valid filename"); }
	QString absolute; bool absent = false;
	if (!path.isEmpty())
	{
		bool invalid = false; absolute = PathUtil::toAbsolute(path, &invalid);
		if (invalid || absolute.isEmpty()) { throw std::invalid_argument("Invalid native SoundFont source path"); }
		if (pending && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved SoundFont cannot discard an accepted resource"); }
		absent = !pending && !QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists(); work.check();
	}
	work.freezeControls(); emit fileLoading(); work.check(); QString error;
	const bool loaded = loadSoundFont(pending || absent ? absolute : path, false, error, work, pending || absent ? BankLoad::Unresolved : BankLoad::Settings); work.check();
	if (!loaded) { collectErrorForUI(error); work.check(); }
	else if (pending || absent) { Engine::getSong()->collectUnresolvedResource(absolute); work.check(); }
	emit fileChanged(); work.check(); work.thawControls();
	{
		Sf2ModelSignals blocked(&m_bankNum, &m_patchNum);
		m_bankNum.loadSettings(settings, "bank"); work.check(); m_patchNum.loadSettings(settings, "patch"); work.check();
	}
	emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check();
	m_gain.loadSettings(settings, "gain"); work.check();
	m_reverbOn.loadSettings(settings, "reverbOn"); work.check();
	m_reverbRoomSize.loadSettings(settings, "reverbRoomSize"); work.check();
	m_reverbDamping.loadSettings(settings, "reverbDamping"); work.check();
	m_reverbWidth.loadSettings(settings, "reverbWidth"); work.check();
	m_reverbLevel.loadSettings(settings, "reverbLevel"); work.check();
	m_chorusOn.loadSettings(settings, "chorusOn"); work.check();
	m_chorusNum.loadSettings(settings, "chorusNum"); work.check();
	m_chorusLevel.loadSettings(settings, "chorusLevel"); work.check();
	m_chorusSpeed.loadSettings(settings, "chorusSpeed"); work.check();
	m_chorusDepth.loadSettings(settings, "chorusDepth"); work.check();
}


void Sf2Instrument::loadFile(const QString& path)
{
	QString error;
	if (!setFileResource("soundfont", path, error)) { collectErrorForUI(error); }
}

std::vector<Plugin::FileResource> Sf2Instrument::fileResources() const
{
	return {{"soundfont", tr("SoundFont bank"), m_filename, {"sf2", "sf3"}, nullptr, 0, true, m_bankSnapshot, m_unresolvedBank}};
}

bool Sf2Instrument::setFileResource(const QString& key, const QString& path, QString& error)
{
	const QString requestedKey(key), requested(path); ResourceGuard work(this);
	if (requestedKey != "soundfont") { error = tr("Unknown SoundFont resource"); return false; }
	emit fileLoading(); work.check(); const bool loaded = loadSoundFont(requested, true, error, work); work.check(); emit fileChanged(); work.check(); return loaded;
}

bool Sf2Instrument::selectProgram(int bank, int program, QString& error)
{
	ResourceGuard work(this); if (!previewProgramImpl(bank, program, error, work)) { return false; }
	{ Sf2ModelSignals blocked(&m_bankNum, &m_patchNum); work.selectModels(bank, program); }
	updateProgramLabels(work); work.check(); emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check(); emit patchChanged(); work.check(); error.clear(); return true;
}

std::vector<Instrument::Program> Sf2Instrument::programSnapshot()
{
	QMutexLocker lock(&m_synthMutex);
	return m_programs;
}

MidiPatch Sf2Instrument::currentProgramModels()
{
	ResourceGuard work(this, true, false); const auto bank = m_bankNum.value(); work.check(); const auto program = m_patchNum.value(); work.check();
	return {static_cast<std::uint16_t>(bank), static_cast<std::uint8_t>(program)};
}

struct Sf2Instrument::ProgramPreview { };

bool Sf2Instrument::previewProgram(int bank, int program, QString& error)
{ ProgramPreviewToken token; return previewProgram(bank, program, error, token); }

bool Sf2Instrument::previewProgram(int bank, int program, QString& error, ProgramPreviewToken& token)
{
	// Refuse an export before waiting on a DSP period's model-change mutex.
	if (RemotePlugin::hasActiveRenderContext()) { error = tr("SoundFont audition is unavailable during native export"); return false; }
	ResourceGuard work(this);
	if (soundFontRenderBusy()) { error = tr("SoundFont audition is unavailable during native export or device reservation"); return false; }
	const auto candidate = std::make_shared<const ProgramPreview>();
	if (!previewProgramImpl(bank, program, error, work)) { return false; }
	work.check(); m_programPreview = candidate; token = candidate; return true;
}

bool Sf2Instrument::restoreProgramPreview(const ProgramPreviewToken& token, QString& error)
{
	if (RemotePlugin::hasActiveRenderContext()) { error = tr("SoundFont audition cleanup is unavailable during native export"); return false; }
	const auto requested = token; ResourceGuard work(this, true, false);
	if (soundFontRenderConsuming()) { error = tr("SoundFont audition cleanup is unavailable during native export"); return false; }
	return restoreProgramPreviewImpl(requested, error, work);
}

bool Sf2Instrument::restoreProgramPreviewImpl(const ProgramPreviewToken& token, QString& error, ResourceGuard& work)
{
	const auto requested = token; work.check();
	if (!requested || requested != m_programPreview) { error = tr("The SoundFont preview was superseded"); return false; }
	const int bank = m_bankNum.value(); work.check(); const int program = m_patchNum.value(); work.check();
	QMutexLocker lock(&m_synthMutex);
	if (requested != m_programPreview) { error = tr("The SoundFont preview was superseded"); return false; }
	const bool available = std::any_of(m_programs.begin(), m_programs.end(), [=](const Program& item) { return item.bank == bank && item.program == program; });
	if (m_synth && m_font && available)
	{
		if (fluid_synth_program_select(m_synth, m_channel, m_fontId, bank, program) != FLUID_OK) { error = tr("FluidSynth rejected the current native program"); return false; }
	}
	else if (m_synth && fluid_synth_unset_program(m_synth, m_channel) != FLUID_OK) { error = tr("FluidSynth could not clear the current native program"); return false; }
	m_programPreview.reset(); error.clear(); return true;
}

bool Sf2Instrument::previewProgramImpl(int bank, int program, QString& error, ResourceGuard& work)
{
	work.check();
	QMutexLocker lock(&m_synthMutex);
	const auto found = std::find_if(m_programs.begin(), m_programs.end(), [=](const Program& item) {
		return item.bank == bank && item.program == program;
	});
	if (found == m_programs.end()) { error = tr("The loaded SoundFont has no bank %1 program %2").arg(bank).arg(program); return false; }
	if (!m_synth || fluid_synth_program_select(m_synth, m_channel, m_fontId, bank, program) != FLUID_OK)
	{
		error = tr("FluidSynth rejected the requested native program"); return false;
	}
	m_programPreview.reset(); error.clear(); return true;
}

void Sf2Instrument::updateProgramLabels(ResourceGuard& work)
{
	work.check(); const auto available = m_programs; const auto bank = m_bankNum.value(); work.check();
	QMap<int, QString> banks, patches;
	for (const auto& item : available)
	{
		banks.insert(item.bank, tr("Bank %1").arg(item.bank));
		if (item.bank == bank) { patches.insert(item.program, item.name); }
	}
	m_bankNum.setValueLabels(std::move(banks)); work.check(); m_patchNum.setValueLabels(std::move(patches)); work.check();
}


auto Sf2Instrument::midiPatch() const -> std::optional<MidiPatch>
{
	return MidiPatch {
		.bank = static_cast<std::uint16_t>(m_bankNum.value()),
		.program = static_cast<std::uint8_t>(m_patchNum.value())
	};
}




AutomatableModel* Sf2Instrument::childModel(std::string_view modelName)
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



QString Sf2Instrument::nodeName() const
{
	return sf2player_plugin_descriptor.name;
}




void Sf2Instrument::freeFont()
{
	m_synthMutex.lock();

	if (m_font != nullptr)
	{
		fluid_synth_sfunload(m_synth, m_fontId, true);
		m_font = nullptr;
	}

	m_synthMutex.unlock();
}



bool Sf2Instrument::loadSoundFont(const QString& path, bool selectFirst, QString& error, ResourceGuard& work, BankLoad mode)
{
	const bool unresolved = mode == BankLoad::Unresolved;
	const QString requested(path); if (requested.contains(QChar::Null)) { error = tr("SoundFont filename contains a null character"); return false; }
	work.poll(); QString relativePath; std::shared_ptr<Sf2BankSnapshot> snapshot;
	if (unresolved && (requested.isEmpty() || !QDir::isAbsolutePath(requested))) { error = tr("An unresolved SoundFont requires an absolute filename"); return false; }
	if (!requested.isEmpty() && !unresolved)
	{
		bool invalid = false; const auto absolute = PathUtil::toAbsolute(requested, &invalid); const QFileInfo file(FileResourceSnapshot::readableSourcePath(absolute)); work.check();
		if (invalid || !file.isFile() || !file.isReadable()) { error = tr("SoundFont path is not a readable file: %1").arg(requested); return false; }
		relativePath = PathUtil::toShortestRelative(absolute); snapshot = Sf2BankSnapshot::capture(absolute, error); work.check(); if (!snapshot) { return false; }
	}
	// FileResourceIO callbacks run before either instance mutex is acquired.
	const bool repairing = mode == BankLoad::File && m_unresolvedBank && !requested.isEmpty() && !unresolved;
	const int currentBank = repairing ? m_bankNum.storedValue() : m_bankNum.value(); work.check();
	const int currentPatch = repairing ? m_patchNum.storedValue() : m_patchNum.value(); work.check(); std::optional<Program> selectedProgram;

	{
		QMutexLocker loading(&m_loadMutex); QMutexLocker lock(&m_synthMutex);
		if (!m_synth) { error = tr("The native FluidSynth synthesizer is unavailable"); return false; }
		pruneRetiredBankSnapshots();
		// Reserve before native state changes, including a rejected loaded candidate.
		m_retiredBankSnapshots.reserve(m_retiredBankSnapshots.size() + 2);
		if (requested.isEmpty() || unresolved)
		{
			if (m_synthGeneration == std::numeric_limits<std::uint64_t>::max())
			{
				error = tr("FluidSynth voice generation is exhausted"); return false;
			}
			if (m_font && fluid_synth_sfunload(m_synth, m_fontId, false) != FLUID_OK)
			{
				error = tr("FluidSynth could not unload the current SoundFont"); return false;
			}
			fluid_synth_unset_program(m_synth, m_channel);
			fluid_synth_all_sounds_off(m_synth, m_channel);
			++m_synthGeneration;
			m_notesRunning.fill(0);
			m_playingNotes.clear();
			if (m_bankSnapshot) { m_retiredBankSnapshots.push_back({m_fontId, std::move(m_bankSnapshot)}); }
			m_font = nullptr; m_fontId = -1; m_filename = unresolved ? requested : QString{}; m_unresolvedBank = unresolved; m_programs.clear();
		}
		else
		{
			int previousId = -1, previousBank = 0, previousProgram = 0;
			fluid_synth_get_program(m_synth, m_channel, &previousId, &previousBank, &previousProgram);
			const auto encoded = snapshot->path().toUtf8();
			const int candidateId = fluid_synth_sfload(m_synth, encoded.constData(), false);
			if (candidateId < 0) { error = tr("FluidSynth rejected SoundFont: %1").arg(path); return false; }
			// Also retire a candidate on an exception while collecting its native programs.
			auto discardCandidate = qScopeGuard([this, candidateId, previousId, previousBank, previousProgram, &snapshot] {
				if (previousId >= 0) { fluid_synth_program_select(m_synth, m_channel, previousId, previousBank, previousProgram); }
				else { fluid_synth_unset_program(m_synth, m_channel); }
				fluid_synth_sfunload(m_synth, candidateId, false);
				m_retiredBankSnapshots.push_back({candidateId, std::move(snapshot)});
			});
			auto candidate = fluid_synth_get_sfont_by_id(m_synth, candidateId);
			std::vector<Program> available;
			if (candidate)
			{
				fluid_sfont_iteration_start(candidate);
				while (const auto preset = fluid_sfont_iteration_next(candidate))
				{
					const int bank = fluid_preset_get_banknum(preset), program = fluid_preset_get_num(preset);
					if (bank < 0 || bank > m_bankNum.maxValue() || program < 0 || program > m_patchNum.maxValue()) { continue; }
					available.push_back({bank, program, QString::fromUtf8(fluid_preset_get_name(preset))});
				}
			}
			if (available.empty())
			{
				error = tr("SoundFont contains no native MIDI programs"); return false;
			}
			std::sort(available.begin(), available.end(), [](const Program& a, const Program& b) {
				return a.bank < b.bank || (a.bank == b.bank && a.program < b.program);
			});
			const auto existing = std::find_if(available.begin(), available.end(), [currentBank, currentPatch](const Program& item) {
				return item.bank == currentBank && item.program == currentPatch;
			});
			if (repairing && existing == available.end())
			{
				error = tr("The replacement SoundFont has no saved bank %1 program %2").arg(currentBank).arg(currentPatch); return false;
			}
			const auto selected = (repairing || !selectFirst) && existing != available.end() ? *existing : available.front();
			if (fluid_synth_program_select(m_synth, m_channel, candidateId, selected.bank, selected.program) != FLUID_OK)
			{
				error = tr("FluidSynth could not select a program from the new SoundFont"); return false;
			}
			if (m_font && fluid_synth_sfunload(m_synth, m_fontId, false) != FLUID_OK)
			{
				error = tr("FluidSynth could not replace the previous SoundFont"); return false;
			}
			if (m_bankSnapshot) { m_retiredBankSnapshots.push_back({m_fontId, std::move(m_bankSnapshot)}); }
			m_font = candidate; m_fontId = candidateId;
			m_bankSnapshot = std::move(snapshot);
			discardCandidate.dismiss();
			m_filename = relativePath; m_unresolvedBank = false; m_programs = std::move(available);
			selectedProgram = selected;
		}
		m_programPreview.reset(); m_bufferView = {}; m_resampler.reset();
		pruneRetiredBankSnapshots();
	}
	work.adopted();
	if (selectedProgram) { Sf2ModelSignals blocked(&m_bankNum, &m_patchNum); work.selectModels(selectedProgram->bank, selectedProgram->program); }
	updateProgramLabels(work); work.check();
	emit m_bankNum.dataChanged(); work.check(); emit m_patchNum.dataChanged(); work.check(); emit patchChanged(); work.check();
	error.clear(); return true;
}

void Sf2Instrument::openFile(const QString& path, bool updateTrackName)
{
	const QString requested(path); ResourceGuard work(this); const QPointer<InstrumentTrack> track(instrumentTrack());
	emit fileLoading(); work.check(); QString error;
	if (loadSoundFont(requested, false, error, work))
	{
		if (!requested.isEmpty())
		{
			const auto trackName = track->displayName(); work.check(); const auto pluginName = displayName(); work.check();
			if (updateTrackName || trackName == pluginName) { track->setName(PathUtil::cleanName(requested)); work.check(); }
		}
	}
	else { collectErrorForUI(error); work.check(); }
	emit fileChanged(); work.check();
}


void Sf2Instrument::updatePatch()
{
	ResourceGuard work(this, true, false); updateProgramLabels(work); work.check(); const auto bank = m_bankNum.value(); work.check(); const auto patch = m_patchNum.value(); work.check();
	QMutexLocker lock(&m_synthMutex); m_programPreview.reset();
	const bool available = std::any_of(m_programs.begin(), m_programs.end(), [=](const Program& item) { return item.bank == bank && item.program == patch; });
	if (m_synth && m_font && available) { fluid_synth_program_select(m_synth, m_channel, m_fontId, bank, patch); }
	else if (m_synth) { fluid_synth_unset_program(m_synth, m_channel); }
}

QString Sf2Instrument::getCurrentPatchName()
{
	ResourceGuard work(this, true, false); const auto available = m_programs; const auto bank = m_bankNum.value(); work.check(); const auto patch = m_patchNum.value(); work.check();
	for (const auto& item : available) { if (item.bank == bank && item.program == patch) { return item.name; } } return {};
}


void Sf2Instrument::updateGain()
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	fluid_synth_set_gain( m_synth, m_gain.value() );
}

void Sf2Instrument::updateReverbOn()
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	fluid_synth_reverb_on(m_synth, -1, m_reverbOn.value() ? 1 : 0);
}

void Sf2Instrument::updateReverb()
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	fluid_synth_set_reverb_group_roomsize(m_synth, -1, m_reverbRoomSize.value());
	fluid_synth_set_reverb_group_damp(m_synth, -1, m_reverbDamping.value());
	fluid_synth_set_reverb_group_width(m_synth, -1, m_reverbWidth.value());
	fluid_synth_set_reverb_group_level(m_synth, -1, m_reverbLevel.value());
}

void Sf2Instrument::updateChorusOn()
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	fluid_synth_chorus_on(m_synth, -1, m_chorusOn.value() ? 1 : 0);
}

void Sf2Instrument::updateChorus()
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	fluid_synth_set_chorus_group_nr(m_synth, -1, static_cast<int>(m_chorusNum.value()));
	fluid_synth_set_chorus_group_level(m_synth, -1, m_chorusLevel.value());
	fluid_synth_set_chorus_group_speed(m_synth, -1, m_chorusSpeed.value());
	fluid_synth_set_chorus_group_depth(m_synth, -1, m_chorusDepth.value());
	fluid_synth_set_chorus_group_type(m_synth, -1, FLUID_CHORUS_MOD_SINE);
}

void Sf2Instrument::updateTuning()
{
	auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth) { return; }
	if (instrumentTrack()->microtuner()->enabledModel()->value())
	{
		auto centArray = std::array<double, 128>{};
		double lowestHz = std::exp2(-69. / 12.) * 440.; // Frequency of MIDI note 0, which is approximately 8.175798916 Hz
		for (int i = 0; i < 128; ++i)
		{
			// Get desired Hz of note
			double noteHz = instrumentTrack()->nominalFrequency(i + instrumentTrack()->baseNoteModel()->value() - DefaultBaseKey);
			// Convert Hz to cents
			centArray[i] = noteHz == 0. ? 0. : 1200. * log2(noteHz / lowestHz);
		}

		fluid_synth_activate_key_tuning(m_synth, 0, 0, "", centArray.data(), true);
		for (int chan = 0; chan < 16; chan++)
		{
		    fluid_synth_activate_tuning(m_synth, chan, 0, 0, true);
		}
	}
	else
	{
		fluid_synth_activate_key_tuning(m_synth, 0, 0, "", nullptr, true);
		for (int chan = 0; chan < 16; chan++)
		{
		    fluid_synth_activate_tuning(m_synth, chan, 0, 0, true);
		}
	}
}



void Sf2Instrument::reloadSynth()
{
	ResourceGuard work(this);
	// The engine has already published its output clock. Even an early resource
	// cancellation, failed program getter or rejected bank verification can leave
	// this synth in service, so prepare its conversion before fallible callbacks.
	{ QMutexLocker synthLock(&m_synthMutex); updateResampling(); }
	if (m_programPreview)
	{
		const auto token = m_programPreview; QString error;
		if (!restoreProgramPreviewImpl(token, error, work)) { throw std::runtime_error(error.toStdString()); }
		work.check();
	}
	const bool rebuilt = reloadSynthImpl(work); work.check();
	if (!rebuilt && soundFontRenderBusy()) { throw std::runtime_error("The native SoundFont synth could not prepare committed state for export"); }
}

bool Sf2Instrument::reloadSynthImpl(ResourceGuard& work)
{
	work.poll(); const auto snapshot = m_bankSnapshot;
	if (m_font)
	{
		if (!snapshot) { collectErrorForUI(tr("The loaded SoundFont has no source snapshot for rebuilding")); work.check(); return false; }
		QString error; const bool verified = snapshot->verify(error); work.check(); if (!verified) { collectErrorForUI(error); work.check(); return false; }
	}
	work.check(); const int bank = m_bankNum.value(); work.check(); const int patch = m_patchNum.value(); work.check();
	QMutexLocker synthLock(&m_synthMutex);
	const auto report = [&](const QString& error) { synthLock.unlock(); collectErrorForUI(error); work.check(); };
	if (m_synthGeneration == std::numeric_limits<std::uint64_t>::max())
	{
		report(tr("FluidSynth voice generation is exhausted"));
		return false;
	}
	// Keep the working synth, its settings, bank and voice pool intact until
	// all replacement allocations have succeeded. Never share native settings
	// between two synths: deleting one would also unregister the other's callbacks.
	Sf2SynthState::Error stageError;
	auto candidate = Sf2SynthState::create(m_settings, Engine::audioEngine()->outputSampleRate(), stageError);
	if (!candidate)
	{
		switch (stageError)
		{
		case Sf2SynthState::Error::SettingsAllocation:
			report(tr("FluidSynth could not allocate replacement settings")); break;
		case Sf2SynthState::Error::SettingsCopy:
			report(tr("FluidSynth could not preserve the synthesizer settings")); break;
		case Sf2SynthState::Error::Polyphony:
			report(tr("FluidSynth reported invalid native polyphony")); break;
		case Sf2SynthState::Error::InvalidSampleRate:
			report(tr("FluidSynth reported an invalid native sample rate")); break;
		default: report(tr("FluidSynth could not allocate the replacement synthesizer"));
		}
		return false;
	}
	std::vector<fluid_voice_t*> scratch;
	try { scratch.resize(static_cast<std::size_t>(candidate->polyphony)); }
	catch (const std::bad_alloc&)
	{
		report(tr("Could not allocate replacement SoundFont voice storage"));
		return false;
	}
	if (m_font)
	{
		const auto encoded = m_bankSnapshot->path().toUtf8();
		if (!candidate->loadBank(encoded.constData(), m_channel, bank, patch, stageError))
		{
			report(stageError == Sf2SynthState::Error::BankSelection
				? tr("FluidSynth could not select the current SoundFont program in the replacement synthesizer")
				: tr("FluidSynth could not load the SoundFont snapshot into the replacement synthesizer"));
			return false;
		}
	}
	// No old note may dereference a voice after its owning synth is deleted.
	m_programPreview.reset(); ++m_synthGeneration;
	m_notesRunning.fill(0);
	m_playingNotes.clear();

	if (m_synth) { delete_fluid_synth(m_synth); }
	if (m_settings) { delete_fluid_settings(m_settings); }
	m_synth = candidate->synth.release();
	m_settings = candidate->settings.release();
	m_fontId = candidate->fontId;
	m_font = m_fontId >= 0 ? fluid_synth_get_sfont_by_id(m_synth, m_fontId) : nullptr;
	// Every older bank was destroyed with its owning synth before cache cleanup.
	m_retiredBankSnapshots.clear();
	m_voiceScratch.swap(scratch);
	m_internalSampleRate = candidate->sampleRate;
	updateResampling();

	work.adopted(); synthLock.unlock();
	updateReverb(); work.check();
	updateChorus(); work.check();
	updateReverbOn(); work.check();
	updateChorusOn(); work.check();
	updateGain(); work.check();
	updateTuning(); work.check();

	// Reset last MIDI pitch properties, which will be set to the correct values
	// upon playing the next note
	m_lastMidiPitch = -1;
	m_lastMidiPitchRange = -1;
	return true;
}


void Sf2Instrument::updateResampling()
{
	// The caller holds the synth mutex and the engine's model-change guard.
	m_resampler.setRatio(m_internalSampleRate, Engine::audioEngine()->outputSampleRate());
	m_bufferView = {}; m_resampler.reset();
	if (m_synth && m_internalSampleRate != Engine::audioEngine()->outputSampleRate())
	{
		// Retain the native linear conversion pipeline for engine rates that this
		// FluidSynth version cannot synthesize directly.
		fluid_synth_set_interp_method(m_synth, -1, FLUID_INTERP_LINEAR);
	}
}


void Sf2Instrument::pruneRetiredBankSnapshots()
{
	// Control operations hold both guards; filesystem cleanup stays out of play().
	// An unloaded bank cannot receive new notes, and idle synthesis has no sample
	// readers. Keep failed removals for a later retry or native-synth destruction.
	if (!m_synth || fluid_synth_get_active_voice_count(m_synth) != 0) { return; }
	m_retiredBankSnapshots.erase(std::remove_if(m_retiredBankSnapshots.begin(), m_retiredBankSnapshots.end(),
		[this](const auto& retired) {
			return retired.snapshot.use_count() == 1 && !fluid_synth_get_sfont_by_id(m_synth, retired.fontId)
				&& retired.snapshot->remove();
		}),
		m_retiredBankSnapshots.end());
}




void Sf2Instrument::playNote( NotePlayHandle * _n, SampleFrame* )
{
	QMutexLocker lock(&m_synthMutex);
	if( _n->isMasterNote() || ( _n->hasParent() && _n->isReleased() ) )
	{
		return;
	}

	int masterPitch = instrumentTrack()->useMasterPitchModel()->value() ? Engine::getSong()->masterPitch() : 0;
	int baseNote = instrumentTrack()->baseNoteModel()->value();
	int midiNote = _n->midiKey() - baseNote + DefaultBaseKey + masterPitch;

	// out of range?
	if (midiNote < 0 || midiNote >= 128)
	{
		if (const auto data = static_cast<Sf2PluginData*>(_n->m_pluginData)) { noteOff(data); }
		m_playingNotes.removeAll(_n);
		return;
	}

	if (!_n->m_pluginData)
	{
		auto pluginData = std::make_unique<Sf2PluginData>();
		pluginData->fluidVoices.reserve(m_voiceScratch.size());
		pluginData->midiNote = midiNote;
		pluginData->offset = _n->offset();
		pluginData->generation = m_synthGeneration;
		_n->m_pluginData = pluginData.release();
	}
	auto data = static_cast<Sf2PluginData*>(_n->m_pluginData);
	if (data->generation != m_synthGeneration)
	{
		// The pointers belong to a retired synth. Clear them without asking
		// FluidSynth about their IDs or state, even if an allocator reused them.
		data->fluidVoices.clear();
		data->generation = m_synthGeneration;
		data->started = false;
		data->noteOffSent = _n->isReleased() && !instrumentTrack()->isSustainPedalPressed();
		data->offset = 0;
	}
	data->velocity = std::clamp(_n->midiVelocity(instrumentTrack()->midiPort()->baseVelocity()), 0, 127);
	data->panning = std::clamp(_n->getPanning(), PanningLeft, PanningRight);
	data->detuning = _n->currentDetuning();
	if (!data->started && !data->noteOffSent && m_synth && m_font && data->velocity > 0)
	{
		data->midiNote = midiNote;
		data->isNew = true;
		if (!m_playingNotes.contains(_n)) { m_playingNotes.append(_n); }
	}
	else if (data->started && !data->noteOffSent && _n->isReleased() && !instrumentTrack()->isSustainPedalPressed())
	{
		data->offset = _n->framesBeforeRelease();
		data->isNew = false;
		if (!m_playingNotes.contains(_n)) { m_playingNotes.append(_n); }
	}
	updateVoices(data);
}


void Sf2Instrument::noteOn( Sf2PluginData * n )
{
	QMutexLocker lock(&m_synthMutex);
	if (!m_synth || !m_font || n->generation != m_synthGeneration || n->started
		|| n->noteOffSent || n->midiNote < 0 || n->midiNote >= 128 || n->velocity <= 0
		|| m_notesRunning[n->midiNote] == std::numeric_limits<int>::max()) { return; }
	if (fluid_synth_noteon(m_synth, m_channel, n->midiNote, n->velocity) != FLUID_OK) { return; }
	n->started = true;
	++m_notesRunning[n->midiNote];
	n->fluidVoices.clear();

	// Get any new voices and store them in the plugin data
	fluid_synth_get_voicelist(m_synth, m_voiceScratch.data(), static_cast<int>(m_voiceScratch.size()), -1);
	for (const auto voice : m_voiceScratch)
	{
		if (!voice) { break; }
		// FluidSynth stops voices with the same channel and pitch upon note-on,
		// so voices with the current channel and pitch are playing this note.
		if (fluid_voice_get_channel(voice) == m_channel
			&& fluid_voice_get_key(voice) == n->midiNote
			&& fluid_voice_is_on(voice)
		) {
			n->fluidVoices.emplace_back(voice);
		}
	}
	updateVoices(n);
}

void Sf2Instrument::updateVoices(Sf2PluginData* n)
{
	// The caller holds the synth mutex across both validity checks and updates.
	if (!m_synth || n->generation != m_synthGeneration) { return; }

	// Smallest balance value that results in full attenuation of one channel.
	// Corresponds to internal FluidSynth macro `FLUID_CB_AMP_SIZE`.
	constexpr static auto maxBalance = 1441.f;
	// Convert panning from linear to exponential for FluidSynth
	const auto panning = n->panning;
	const auto factor = 1.f - std::abs(panning) / static_cast<float>(PanningRight);
	const auto balance = std::copysign(
		factor <= 0 ? maxBalance : std::min(-200.f * std::log10(factor), maxBalance),
		panning
	);
	// Set note panning on all the voices
	for (const auto& voice : n->fluidVoices) {
		if (voice.isValid()) {
			if (std::isfinite(n->detuning))
			{
				fluid_voice_gen_set(voice.get(), GEN_COARSETUNE, voice.coarseTune() + n->detuning);
				fluid_voice_update_param(voice.get(), GEN_COARSETUNE);
			}
			fluid_voice_gen_set(voice.get(), GEN_CUSTOM_BALANCE, balance);
			fluid_voice_update_param(voice.get(), GEN_CUSTOM_BALANCE);
		}
	}
}


void Sf2Instrument::noteOff( Sf2PluginData * n )
{
	QMutexLocker lock(&m_synthMutex);
	if (n->noteOffSent) { return; }
	n->noteOffSent = true;
	if (!n->started || n->generation != m_synthGeneration || n->midiNote < 0 || n->midiNote >= 128) { return; }
	n->started = false;
	auto& notes = m_notesRunning[n->midiNote];
	if (notes > 0) { --notes; }
	if (notes == 0 && m_synth)
	{
		fluid_synth_noteoff( m_synth, m_channel, n->midiNote );
	}
}


void Sf2Instrument::play( SampleFrame* _working_buffer )
{
	QMutexLocker lock(&m_synthMutex);
	const f_cnt_t frames = Engine::audioEngine()->framesPerPeriod();
	if (!m_synth || m_unresolvedBank) { zeroSampleFrames(_working_buffer, frames); m_playingNotes.clear(); return; }

	// set midi pitch for this period
	const int currentMidiPitch = instrumentTrack()->midiPitch();
	if( m_lastMidiPitch != currentMidiPitch )
	{
		m_lastMidiPitch = currentMidiPitch;
		fluid_synth_pitch_bend( m_synth, m_channel, m_lastMidiPitch );
	}

	const int currentMidiPitchRange = instrumentTrack()->midiPitchRange();
	if( m_lastMidiPitchRange != currentMidiPitchRange )
	{
		m_lastMidiPitchRange = currentMidiPitchRange;
		fluid_synth_pitch_wheel_sens( m_synth, m_channel, m_lastMidiPitchRange );
	}
	// if we have no new noteons/noteoffs, just render a period and call it a day
	if( m_playingNotes.isEmpty() )
	{
		renderFrames( frames, _working_buffer );
		return;
	}

	// processing loop
	// go through noteplayhandles in processing order
	f_cnt_t currentFrame = 0;

	while( ! m_playingNotes.isEmpty() )
	{
		// find the note with lowest offset
		NotePlayHandle * currentNote = m_playingNotes[0];
		for( int i = 1; i < m_playingNotes.size(); ++i )
		{
			auto currentData = static_cast<Sf2PluginData*>(currentNote->m_pluginData);
			auto iData = static_cast<Sf2PluginData*>(m_playingNotes[i]->m_pluginData);
			if( currentData->offset > iData->offset )
			{
				currentNote = m_playingNotes[i];
			}
		}

		// process the current note:
		// first see if we're synced in frame count
		auto currentData = static_cast<Sf2PluginData*>(currentNote->m_pluginData);
		if (currentData->generation != m_synthGeneration)
		{
			m_playingNotes.removeOne(currentNote);
			continue;
		}
		currentData->offset = std::min(currentData->offset, frames);
		if( currentData->offset > currentFrame )
		{
			renderFrames( currentData->offset - currentFrame, _working_buffer + currentFrame );
			currentFrame = currentData->offset;
		}
		if( currentData->isNew )
		{
			noteOn( currentData );
			if( currentNote->isReleased() ) // if the note is released during the same period, we have to process it again for noteoff
			{
				currentData->isNew = false;
				currentData->offset = currentNote->framesBeforeRelease();
			}
			else // otherwise remove the handle
			{
				m_playingNotes.removeOne(currentNote);
			}
		}
		else
		{
			noteOff( currentData );
			m_playingNotes.removeOne(currentNote);
		}
	}

	if( currentFrame < frames )
	{
		renderFrames( frames - currentFrame, _working_buffer + currentFrame );
	}
}


void Sf2Instrument::renderFrames( f_cnt_t frames, SampleFrame* buf )
{
	const auto guard = std::lock_guard{m_synthMutex};
	if (!m_synth) { zeroSampleFrames(buf, frames); return; }

	fluid_synth_get_gain(m_synth); // This flushes voice updates as a side effect

	if (m_internalSampleRate == Engine::audioEngine()->outputSampleRate()) {
		fluid_synth_write_float(m_synth, frames, buf, 0, 2, buf, 1, 2);
		return;
	}

	// TODO: These kind of playback pipelines/graphs are repeated within other parts of the codebase that work with
	// audio samples. We should find a way to unify this but the right abstraction is not so clear yet.
	while (frames > 0)
	{
		if (m_bufferView.empty())
		{
			fluid_synth_write_float(m_synth, m_buffer.size(), m_buffer.data(), 0, 2, m_buffer.data(), 1, 2);
			m_bufferView = m_buffer;
		}

		const auto [inputFramesUsed, outputFramesGenerated]
			= m_resampler.process({&m_bufferView.data()[0][0], 2, m_bufferView.size()}, {&buf[0][0], 2, frames});

		if (inputFramesUsed == 0 && outputFramesGenerated == 0)
		{
			std::fill_n(buf, frames, SampleFrame{});
			break;
		}

		m_bufferView = m_bufferView.subspan(inputFramesUsed);
		buf += outputFramesGenerated;
		frames -= outputFramesGenerated;
	}
}




void Sf2Instrument::deleteNotePluginData( NotePlayHandle * _n )
{
	QMutexLocker lock(&m_synthMutex);
	auto pluginData = static_cast<Sf2PluginData*>(_n->m_pluginData);
	if (!pluginData) { return; }
	noteOff(pluginData);
	m_playingNotes.removeAll(_n);
	delete pluginData;
	_n->m_pluginData = nullptr;
}




gui::PluginView * Sf2Instrument::instantiateView( QWidget * _parent )
{
	return new gui::Sf2InstrumentView( this, _parent );
}




namespace gui
{


class Sf2Knob : public Knob
{
public:
	Sf2Knob( QWidget * _parent ) :
			Knob( KnobType::Styled, _parent )
	{
		setFixedSize( 31, 38 );
	}
};



Sf2InstrumentView::Sf2InstrumentView( Instrument * _instrument, QWidget * _parent ) :
	InstrumentViewFixedSize( _instrument, _parent )
{
//	QVBoxLayout * vl = new QVBoxLayout( this );
//	QHBoxLayout * hl = new QHBoxLayout();

	auto k = castModel<Sf2Instrument>();

	connect(&k->m_bankNum, SIGNAL(dataChanged()), this, SLOT(updatePatchName()));
	connect(&k->m_patchNum, SIGNAL(dataChanged()), this, SLOT(updatePatchName()));

	// File Button
	m_fileDialogButton = new PixmapButton(this);
	m_fileDialogButton->setCursor(Qt::PointingHandCursor);
	m_fileDialogButton->setActiveGraphic(PLUGIN_NAME::getIconPixmap("fileselect_on"));
	m_fileDialogButton->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("fileselect_off"));
	m_fileDialogButton->move(217, 107);

	connect(m_fileDialogButton, SIGNAL(clicked()), this, SLOT(showFileDialog()));

	m_fileDialogButton->setToolTip(tr("Open SoundFont file"));

	// Patch Button
	m_patchDialogButton = new PixmapButton(this);
	m_patchDialogButton->setCursor(Qt::PointingHandCursor);
	m_patchDialogButton->setActiveGraphic(PLUGIN_NAME::getIconPixmap("patches_on"));
	m_patchDialogButton->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("patches_off"));
	m_patchDialogButton->setEnabled(false);
	m_patchDialogButton->move(217, 125);

	connect(m_patchDialogButton, SIGNAL(clicked()), this, SLOT(showPatchDialog()));

	m_patchDialogButton->setToolTip(tr("Choose patch"));

	// LCDs
	m_bankNumLcd = new LcdSpinBox(5, "11green", this);
	m_bankNumLcd->move(131, 67);
//	m_bankNumLcd->addTextForValue( -1, "---" );
//	m_bankNumLcd->setEnabled( false );

	m_patchNumLcd = new LcdSpinBox( 3, "21pink", this );
	m_patchNumLcd->move(190, 62);
//	m_patchNumLcd->addTextForValue( -1, "---" );
//	m_patchNumLcd->setEnabled( false );

	/*hl->addWidget( m_fileDialogButton );
	hl->addWidget( m_bankNumLcd );
	hl->addWidget( m_patchNumLcd );
	hl->addWidget( m_patchDialogButton );

	vl->addLayout( hl );*/

	// Next row

	//hl = new QHBoxLayout();

	m_filenameLabel = new QLabel( this );
	m_filenameLabel->setGeometry( 58, 109, 156, 11 );
	m_patchLabel = new QLabel( this );
	m_patchLabel->setGeometry( 58, 127, 156, 11 );

	//hl->addWidget( m_filenameLabel );
//	vl->addLayout( hl );

	// Gain
	m_gainKnob = new Sf2Knob( this );
	m_gainKnob->setHintText( tr("Gain:"), "" );
	m_gainKnob->move( 86, 55 );
//	vl->addWidget( m_gainKnob );

	// Reverb
//	hl = new QHBoxLayout();


	m_reverbButton = new PixmapButton( this );
	m_reverbButton->setCheckable( true );
	m_reverbButton->move( 14, 180 );
	m_reverbButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap( "reverb_on" ) );
	m_reverbButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap( "reverb_off" ) );
	m_reverbButton->setToolTip(tr("Apply reverb (if supported)"));


	m_reverbRoomSizeKnob = new Sf2Knob( this );
	m_reverbRoomSizeKnob->setHintText( tr("Room size:"), "" );
	m_reverbRoomSizeKnob->move( 93, 160 );

	m_reverbDampingKnob = new Sf2Knob( this );
	m_reverbDampingKnob->setHintText( tr("Damping:"), "" );
	m_reverbDampingKnob->move( 130, 160 );

	m_reverbWidthKnob = new Sf2Knob( this );
	m_reverbWidthKnob->setHintText( tr("Width:"), "" );
	m_reverbWidthKnob->move( 167, 160 );

	m_reverbLevelKnob = new Sf2Knob( this );
	m_reverbLevelKnob->setHintText( tr("Level:"), "" );
	m_reverbLevelKnob->move( 204, 160 );

/*	hl->addWidget( m_reverbOnLed );
	hl->addWidget( m_reverbRoomSizeKnob );
	hl->addWidget( m_reverbDampingKnob );
	hl->addWidget( m_reverbWidthKnob );
	hl->addWidget( m_reverbLevelKnob );

	vl->addLayout( hl );
*/

	// Chorus
//	hl = new QHBoxLayout();

	m_chorusButton = new PixmapButton( this );
	m_chorusButton->setCheckable( true );
	m_chorusButton->move( 14, 226 );
	m_chorusButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap( "chorus_on" ) );
	m_chorusButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap( "chorus_off" ) );
	m_chorusButton->setToolTip(tr("Apply chorus (if supported)"));

	m_chorusNumKnob = new Sf2Knob( this );
	m_chorusNumKnob->setHintText( tr("Voices:"), "" );
	m_chorusNumKnob->move( 93, 206 );

	m_chorusLevelKnob = new Sf2Knob( this );
	m_chorusLevelKnob->setHintText( tr("Level:"), "" );
	m_chorusLevelKnob->move( 130 , 206 );

	m_chorusSpeedKnob = new Sf2Knob( this );
	m_chorusSpeedKnob->setHintText( tr("Speed:"), "" );
	m_chorusSpeedKnob->move( 167 , 206 );

	m_chorusDepthKnob = new Sf2Knob( this );
	m_chorusDepthKnob->setHintText( tr("Depth:"), "" );
	m_chorusDepthKnob->move( 204 , 206 );
/*
	hl->addWidget( m_chorusOnLed );
	hl->addWidget( m_chorusNumKnob);
	hl->addWidget( m_chorusLevelKnob);
	hl->addWidget( m_chorusSpeedKnob);
	hl->addWidget( m_chorusDepthKnob);

	vl->addLayout( hl );
*/
	setAutoFillBackground( true );
	QPalette pal;
	pal.setBrush( backgroundRole(), PLUGIN_NAME::getIconPixmap( "artwork" ) );
	setPalette( pal );

	updateFilename();
}




void Sf2InstrumentView::modelChanged()
{
	auto k = castModel<Sf2Instrument>();
	m_bankNumLcd->setModel( &k->m_bankNum );
	m_patchNumLcd->setModel( &k->m_patchNum );

	m_gainKnob->setModel( &k->m_gain );

	m_reverbButton->setModel( &k->m_reverbOn );
	m_reverbRoomSizeKnob->setModel( &k->m_reverbRoomSize );
	m_reverbDampingKnob->setModel( &k->m_reverbDamping );
	m_reverbWidthKnob->setModel( &k->m_reverbWidth );
	m_reverbLevelKnob->setModel( &k->m_reverbLevel );

	m_chorusButton->setModel( &k->m_chorusOn );
	m_chorusNumKnob->setModel( &k->m_chorusNum );
	m_chorusLevelKnob->setModel( &k->m_chorusLevel );
	m_chorusSpeedKnob->setModel( &k->m_chorusSpeed );
	m_chorusDepthKnob->setModel( &k->m_chorusDepth );


	connect( k, SIGNAL( fileChanged() ), this, SLOT( updateFilename() ) );

	connect( k, SIGNAL( fileLoading() ), this, SLOT( invalidateFile() ) );

	updateFilename();
}




void Sf2InstrumentView::updateFilename()
{
	auto i = castModel<Sf2Instrument>();
	QFontMetrics fm( m_filenameLabel->font() );
	QString file = i->m_filename.endsWith( ".sf2", Qt::CaseInsensitive ) ?
			i->m_filename.left( i->m_filename.length() - 4 ) :
			i->m_filename;
	m_filenameLabel->setText( fm.elidedText( file, Qt::ElideLeft, m_filenameLabel->width() ) );
			//		i->m_filename + "\nPatch: TODO" );

	m_patchDialogButton->setEnabled(!i->m_unresolvedBank && !i->m_programs.empty());

	updatePatchName();

	update();
}




void Sf2InstrumentView::updatePatchName()
{
	auto i = castModel<Sf2Instrument>();
	QFontMetrics fm( font() );
	QString patch = i->getCurrentPatchName();
	m_patchLabel->setText( fm.elidedText( patch, Qt::ElideLeft, m_patchLabel->width() ) );


	update();
}




void Sf2InstrumentView::invalidateFile()
{
	m_patchDialogButton->setEnabled( false );
}




void Sf2InstrumentView::showFileDialog()
{
	const QPointer<Sf2InstrumentView> owner(this); const QPointer<Sf2Instrument> native(castModel<Sf2Instrument>()); const QPointer<PixmapButton> button(m_fileDialogButton);
	const QPointer<InstrumentTrack> track(native ? native->instrumentTrack() : nullptr); const QPointer<Song> song(Engine::getSong()); const auto generation = song ? song->replacementGeneration() : 0;
	if (!native || !button) { return; } const QString filename = native->m_filename;
	const auto current = [&] { return owner && native && button && owner->model() == native.data() && owner->m_fileDialogButton == button.data() && track && !track->isRetiring()
		&& track->instrument() == native.data() && song && Engine::getSong() == song.data() && song->replacementGeneration() == generation; };
	FileDialog dialog(nullptr, tr("Open SoundFont file")); dialog.setFileMode(FileDialog::ExistingFiles); dialog.setNameFilters({tr("SoundFont Files (*.sf2 *.sf3)")});
	if (!filename.isEmpty()) { const auto absolute = PathUtil::toAbsolute(filename); dialog.setDirectory(QFileInfo(absolute).absolutePath()); dialog.selectFile(QFileInfo(absolute).fileName()); }
	else { dialog.setDirectory(ConfigManager::inst()->sf2Dir()); }
	if (!current()) { return; } button->setEnabled(false); const auto restore = qScopeGuard([&] { if (button) { button->setEnabled(true); } });
	if (!current()) { return; }
	const auto accepted = dialog.exec() == QDialog::Accepted; if (!current() || !accepted || dialog.selectedFiles().isEmpty()) { return; }
	const QString path = dialog.selectedFiles()[0]; if (path.isEmpty()) { return; }
	try { native->openFile(path); if (current()) { song->setModified(); } }
	catch (const std::exception& error) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError(QString::fromUtf8(error.what())); } }
	catch (...) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError("Native SoundFont loading failed"); } }
}


void Sf2InstrumentView::showPatchDialog()
{
	const QPointer<Sf2InstrumentView> owner(this); const QPointer<Sf2Instrument> native(castModel<Sf2Instrument>());
	const QPointer<InstrumentTrack> track(native ? native->instrumentTrack() : nullptr); const QPointer<Song> song(Engine::getSong()); const auto generation = song ? song->replacementGeneration() : 0;
	const auto current = [&] { return owner && native && owner->model() == native.data() && track && !track->isRetiring() && track->instrument() == native.data()
		&& song && Engine::getSong() == song.data() && song->replacementGeneration() == generation; };
	if (!current()) { return; } const QString& reported = track->name(); if (!current()) { return; } const QString caption(reported);
	const QPointer<PatchesDialog> dialog(new PatchesDialog(this)); const auto cleanup = qScopeGuard([dialog] { if (dialog) { delete dialog.data(); } });
	if (!current() || !dialog || !dialog->setup(native.data(), caption)) { return; }
	if (current() && dialog) { dialog->exec(); }
}


} // namespace gui

extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main( Model *m, void * )
{
	return new Sf2Instrument( static_cast<InstrumentTrack *>( m ) );
}
}


} // namespace lmms
