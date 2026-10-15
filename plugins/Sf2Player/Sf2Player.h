/*
 * Sf2Player.h - a soundfont2 player using fluidSynth
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


#ifndef SF2_PLAYER_H
#define SF2_PLAYER_H

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <fluidsynth/types.h>
#include <QMutex>
#include <QRecursiveMutex>
#include <samplerate.h>

#include "AudioEngine.h"
#include "AudioResampler.h"
#include "Instrument.h"
#include "InstrumentView.h"
#include "LcdSpinBox.h"
#include "SampleFrame.h"
#include "Sf2BankSnapshot.h"

class QLabel;

namespace lmms
{


struct Sf2PluginData;
class NotePlayHandle;

namespace gui
{
class Knob;
class PixmapButton;
class Sf2InstrumentView;
class PatchesDialog;
} // namespace gui


class Sf2Instrument : public Instrument
{
	Q_OBJECT
	mapPropertyFromModel(int,getBank,setBank,m_bankNum);
	mapPropertyFromModel(int,getPatch,setPatch,m_patchNum);

public:
	Sf2Instrument( InstrumentTrack * _instrument_track );
	~Sf2Instrument() override;

	void play( SampleFrame* _working_buffer ) override;

	void playNote( NotePlayHandle * _n,
						SampleFrame* _working_buffer ) override;
	void deleteNotePluginData( NotePlayHandle * _n ) override;


	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;

	void loadFile( const QString & _file ) override;
	std::vector<FileResource> fileResources() const override;
	bool setFileResource(const QString& key, const QString& path, QString& error) override;
	const std::vector<Program>& programs() const override { return m_programs; }
	bool selectProgram(int bank, int program, QString& error) override;
	bool previewProgram(int bank, int program, QString& error);
	struct ProgramPreview;
	using ProgramPreviewToken = std::shared_ptr<const ProgramPreview>;
	bool previewProgram(int bank, int program, QString& error, ProgramPreviewToken& token);
	bool restoreProgramPreview(const ProgramPreviewToken& token, QString& error);
	std::vector<Program> programSnapshot();
	MidiPatch currentProgramModels();

	auto midiPatch() const -> std::optional<MidiPatch> override;
	AutomatableModel* childModel(std::string_view modelName) override;

	QString nodeName() const override;

	gui::PluginView* instantiateView( QWidget * _parent ) override;

	QString getCurrentPatchName();


	void setParameter( const QString & _param, const QString & _value );


public slots:
	void openFile( const QString & _sf2File, bool updateTrackName = true );
	void updatePatch();
	void reloadSynth();

	// We can't really support sample-exact with the way IPH and FS work.
	// So, sig/slots work just fine for the synth settings right now.
	void updateReverbOn();
	void updateReverb();
	void updateChorusOn();
	void updateChorus();
	void updateGain();
	void updateTuning();

private:
	AudioResampler m_resampler;
	std::array<SampleFrame, DEFAULT_BUFFER_SIZE> m_buffer;
	std::span<SampleFrame> m_bufferView;

	fluid_settings_t* m_settings;
	fluid_synth_t* m_synth;

	fluid_sfont_t* m_font;

	int m_fontId;
	QString m_filename;
	bool m_unresolvedBank = false;
	std::vector<Program> m_programs;
	ProgramPreviewToken m_programPreview;
	std::shared_ptr<Sf2BankSnapshot> m_bankSnapshot;
	struct RetiredBankSnapshot
	{
		int fontId;
		std::shared_ptr<Sf2BankSnapshot> snapshot;
	};
	std::vector<RetiredBankSnapshot> m_retiredBankSnapshots;

	// Serialize synth lifetime, low-level voices, counters and queued note events.
	QRecursiveMutex m_synthMutex;
	QMutex m_loadMutex;
	std::uint64_t m_synthGeneration = 1;
	std::vector<fluid_voice_t*> m_voiceScratch;

	std::array<int, 128> m_notesRunning = {};
	sample_rate_t m_internalSampleRate = 44100;
	int m_lastMidiPitch;
	int m_lastMidiPitchRange;
	int m_channel;

	gui::LcdSpinBoxModel m_bankNum;
	gui::LcdSpinBoxModel m_patchNum;

	FloatModel m_gain;

	BoolModel m_reverbOn;
	FloatModel m_reverbRoomSize;
	FloatModel m_reverbDamping;
	FloatModel m_reverbWidth;
	FloatModel m_reverbLevel;

	BoolModel m_chorusOn;
	FloatModel m_chorusNum;
	FloatModel m_chorusLevel;
	FloatModel m_chorusSpeed;
	FloatModel m_chorusDepth;

	QVector<NotePlayHandle *> m_playingNotes;

private:
	class ResourceGuard;
	enum class BankLoad { File, Settings, Unresolved };
	bool m_changingResources = false;
	void freeFont();
	bool loadSoundFont(const QString& path, bool selectFirst, QString& error, ResourceGuard& work, BankLoad mode = BankLoad::File);
	bool previewProgramImpl(int bank, int program, QString& error, ResourceGuard& work);
	void updateProgramLabels(ResourceGuard& work);
	bool reloadSynthImpl(ResourceGuard& work);
	bool restoreProgramPreviewImpl(const ProgramPreviewToken& token, QString& error, ResourceGuard& work);
	void noteOn( Sf2PluginData * n );
	void noteOff( Sf2PluginData * n );
	void updateVoices(Sf2PluginData* n);
	void updateResampling();
	void pruneRetiredBankSnapshots();
	void renderFrames( f_cnt_t frames, SampleFrame* buf );

	friend class gui::Sf2InstrumentView;
	friend class Sf2ResourceTestAccess;
	friend class Sf2PatchDialogTestAccess;
	friend class Sf2RenderAuditionTestAccess;
	friend class Sf2RateFallbackTestAccess;

signals:
	void fileLoading();
	void fileChanged();
	void patchChanged();

} ;


namespace gui
{


class Sf2InstrumentView : public InstrumentViewFixedSize
{
	Q_OBJECT
public:
	Sf2InstrumentView( Instrument * _instrument,
					QWidget * _parent );
	~Sf2InstrumentView() override = default;

private:
	void modelChanged() override;

	PixmapButton * m_fileDialogButton;
	PixmapButton * m_patchDialogButton;

	LcdSpinBox * m_bankNumLcd;
	LcdSpinBox * m_patchNumLcd;

	QLabel * m_filenameLabel;
	QLabel * m_patchLabel;

	Knob	* m_gainKnob;

	PixmapButton * m_reverbButton;
	Knob	* m_reverbRoomSizeKnob;
	Knob	* m_reverbDampingKnob;
	Knob	* m_reverbWidthKnob;
	Knob	* m_reverbLevelKnob;

	PixmapButton * m_chorusButton;
	Knob * m_chorusNumKnob;
	Knob * m_chorusLevelKnob;
	Knob * m_chorusSpeedKnob;
	Knob * m_chorusDepthKnob;

	static PatchesDialog * s_patchDialog;

protected slots:
	void invalidateFile();
	void showFileDialog();
	void showPatchDialog();
	void updateFilename();
	void updatePatchName();
} ;


} // namespace gui

} // namespace lmms

#endif
