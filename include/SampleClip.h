/*
 * SampleClip.h
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

#ifndef LMMS_SAMPLE_CLIP_H
#define LMMS_SAMPLE_CLIP_H

#include <memory>
#include <optional>
#include <utility>
#include <QList>
#include "Clip.h"
#include "Sample.h"

namespace lmms
{

class SampleBuffer;
class SampleRecordingBuffer;

namespace gui
{

class SampleClipView;

} // namespace gui


class LMMS_EXPORT SampleClip : public Clip
{
	Q_OBJECT
	mapPropertyFromModel(bool,isRecord,setRecord,m_recordModel);
public:
	SampleClip(Track* track, Sample sample, bool isPlaying);
	SampleClip(Track* track);
	~SampleClip() override;

	SampleClip& operator=( const SampleClip& that ) = delete;

	void changeLength( const TimePos & _length ) override;
	void updateLength() override;

	const QString& sampleFile() const;
	bool hasSampleFileLoaded(const QString & filename) const;
	bool sampleUnresolved() const { return m_unresolved.has_value(); }
	int sourceSampleRate() const;
	std::optional<std::pair<int, int>> sourceSampleWindow() const;
	// Preserve retained placement/playback settings when repairing a missing source.
	bool loadSampleFile(const QString& path, QString& error, bool preserveSettings = false);

	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;
	inline QString nodeName() const override
	{
		return "sampleclip";
	}

	Sample& sample()
	{
		return m_sample;
	}
	const Sample& sample() const { return m_sample; }

	TimePos sampleLength() const;
	//! Source frame bounds are half-open and measured in playback order.
	bool setSampleWindow(int startFrame, int endFrame);
	//! Return an independent playback snapshot without changing editable clip state.
	std::optional<Sample> playbackSample(const TimePos& relativePosition) const;
	void reverseSample();
	void setStartTimeOffset(const TimePos& startTimeOffset) override;
	gui::ClipView * createView( gui::TrackView * _tv ) override;


	bool isPlaying() const;
	void setIsPlaying(bool isPlaying);
	void setSampleBuffer(std::shared_ptr<const SampleBuffer> sb);
	// Guarded by the native engine model lock; publication runs on the clip thread.
	bool beginRecordingTake(bool reserved = false);
	bool reserveRecordingCompletion(QString& error);
	bool finishReservedRecording(f_cnt_t expectedFrames, sample_rate_t expectedRate, QString& error);
	void discardReservedRecording();
	bool hasReservedRecording() const { return m_recordingReserved; }
	void stageRecordingCompletion(std::shared_ptr<SampleRecordingBuffer> recording, const QString& error = {});
	bool finalizeRecordings(QString& error);
	bool hasActiveRecording() const { return m_activeRecordingTakes != 0; }
	bool hasPendingRecording() const { return !m_completedRecordings.isEmpty() || !m_recordingCompletionError.isEmpty(); }
	bool isFinalizingRecording() const { return m_finalizingRecording; }

	SampleClip* clone() override
	{
		return static_cast<SampleClip*>(cloneNative());
	}

public slots:
	void setSampleFile(const QString& sf);
	void toggleRecord();
	void playbackPositionChanged();
	void updateTrackClips();
	void tempoChanged();

protected:
	SampleClip( const SampleClip& orig );
	SampleClip(const SampleClip& orig, Track* destination);

private:
	class ResourceGuard;
	struct MissingSource
	{
		QString path;
		int rate = 0;
		std::optional<std::pair<int, int>> window;
		bool operator==(const MissingSource&) const = default;
	};
	Sample m_sample;
	std::optional<MissingSource> m_unresolved;
	bool m_changingResource = false;
	BoolModel m_recordModel;
	bool m_isPlaying;
	double m_startFrameOffset;
	std::size_t m_activeRecordingTakes = 0;
	QList<std::shared_ptr<SampleRecordingBuffer>> m_completedRecordings;
	QString m_recordingCompletionError;
	bool m_finalizingRecording = false;
	bool m_recordingReserved = false;

	friend class gui::SampleClipView;
	friend class Clip;


signals:
	void sampleChanged();
	void wasReversed();
} ;


} // namespace lmms

#endif // LMMS_SAMPLE_CLIP_H
