/*
 * Mixer.h - effect-mixer for LMMS
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_MIXER_H
#define LMMS_MIXER_H

#include "AudioBuffer.h"
#include "EffectChain.h"
#include "JournallingObject.h"
#include "Model.h"
#include "ThreadableJob.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <QColor>
#include <QPointer>

namespace lmms
{


class MixerRoute;
class Mixer;
using MixerRouteVector = std::vector<MixerRoute*>;

class LMMS_EXPORT MixerControlModel : public BoolModel
{
public:
	MixerControlModel(bool value, Model* parent);
	JournallingObject* journalTarget() override;
	QDomElement saveJournalState(QDomDocument& document, QDomElement& parent) override;
	bool restoreJournalState(const QDomElement& state, QString& error) override;
private:
	QPointer<Mixer> m_mixer;
};

class MixerChannel : public ThreadableJob
{
public:
	MixerChannel(int idx, Model* _parent);
	virtual ~MixerChannel();

	EffectChain m_fxChain;

	// set to true if any effect in the channel is enabled and running
	bool m_stillRunning;

	float m_peakLeft;
	float m_peakRight;
	AudioBuffer m_buffer;
	bool m_muteBeforeSolo = false;
	MixerControlModel m_muteModel;
	MixerControlModel m_soloModel;
	FloatModel m_volumeModel;
	QString m_name;
	QMutex m_lock;
	std::atomic_bool m_queued; // are we queued up for rendering yet?
	bool m_muted = false; // snapshot initialized for every channel before publishing dependencies

	// pointers to other channels that this one sends to
	MixerRouteVector m_sends;

	// pointers to other channels that send to this one
	MixerRouteVector m_receives;

	int index() const { return m_channelIndex; }
	void setIndex(int index) { m_channelIndex = index; }
	void setName(const QString& name);

	bool isMaster() { return m_channelIndex == 0; }

	bool requiresProcessing() const override { return true; }

	auto color() const -> const std::optional<QColor>& { return m_color; }
	void setColor(const std::optional<QColor>& color) { m_color = color; }

	std::atomic_size_t m_dependenciesMet;
	void incrementDeps();
	void processed();

private:
	void doProcessing() override;
	int m_channelIndex;
	std::optional<QColor> m_color;
};

class MixerRoute : public QObject
{
	Q_OBJECT
public:
	MixerRoute( MixerChannel * from, MixerChannel * to, float amount );
	~MixerRoute() override;

	mix_ch_t senderIndex() const
	{
		return m_from->index();
	}

	mix_ch_t receiverIndex() const
	{
		return m_to->index();
	}

	FloatModel * amount()
	{
		return &m_amount;
	}

	MixerChannel * sender() const
	{
		return m_from;
	}

	MixerChannel * receiver() const
	{
		return m_to;
	}

	void updateName();

	private:
		MixerChannel * m_from;
		MixerChannel * m_to;
		QPointer<EffectChain> m_fromAnchor, m_toAnchor;
		QPointer<Mixer> m_owner;
		FloatModel m_amount;
		friend class Mixer;
};


class LMMS_EXPORT Mixer : public Model, public JournallingObject
{
	Q_OBJECT
public:
	Mixer();
	~Mixer() override;

	void mixToChannel(const AudioBuffer& buffer, mix_ch_t dest);

	void prepareMasterMix();
	void discardProcessingPeriod();
	void masterMix( SampleFrame* _buf );

	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;

	QString nodeName() const override
	{
		return "mixer";
	}

	MixerChannel * mixerChannel( int _ch )
	{
		return m_mixerChannels[_ch];
	}

	// make the output of channel fromChannel go to the input of channel toChannel
	// it is safe to call even if the send already exists
	MixerRoute * createChannelSend(mix_ch_t fromChannel, mix_ch_t toChannel,
						   float amount = 1.0f);
	MixerRoute * createRoute( MixerChannel * from, MixerChannel * to, float amount );

	// delete the connection made by createChannelSend
	void deleteChannelSend(mix_ch_t fromChannel, mix_ch_t toChannel);
	void deleteChannelSend( MixerRoute * route );

	// determine if adding a send from sendFrom to
	// sendTo would result in an infinite mixer loop.
	bool isInfiniteLoop(mix_ch_t fromChannel, mix_ch_t toChannel);
	bool checkInfiniteLoop( MixerChannel * from, MixerChannel * to );

	// return the FloatModel of fromChannel sending its output to the input of
	// toChannel. NULL if there is no send.
	FloatModel * channelSendModel(mix_ch_t fromChannel, mix_ch_t toChannel);

	// add a new channel to the mixer.
	// returns the index of the channel that was just added
	int createChannel();

	// Delete a non-master channel. False means no admitted change.
	bool deleteChannel(int index);

	// delete all the mixer channels except master and remove all effects
	void clear();

	// re-arrange channels
	// Move to a final index; master stays at zero. Existing channels, effects,
	// route models and their automation bindings keep their native identities.
	bool moveChannel(int from, int to);
	void moveChannelLeft(int index);
	void moveChannelRight(int index);

	// Reset a channel's name, fx, sends, etc. False means no admitted change.
	bool clearChannel(mix_ch_t channelIndex);

	// rename channels when moving etc. if they still have their original name
	void validateChannelName( int index, int oldIndex );

	// check if the index channel receives audio from any other channel
	// or from any instrument or sample track
	bool isChannelInUse(int index);

	int soloChannel() const { return m_lastSoloed; }
	std::uint64_t replacementGeneration() const noexcept { return m_replacementGeneration; }

	inline mix_ch_t numChannels() const
	{
		return m_mixerChannels.size();
	}

	MixerRouteVector m_mixerRoutes;

signals:
	void channelMoved(int from, int to);

private:
	class FlagGuard
	{
	public:
		FlagGuard(Mixer* owner, bool Mixer::* flag);
		~FlagGuard();
		void check() const;
		void restore() const;
		FlagGuard(const FlagGuard&) = delete;
		FlagGuard& operator=(const FlagGuard&) = delete;
	private:
		QPointer<Mixer> m_owner;
		bool Mixer::* m_flag;
		bool m_previous;
	};
	struct ChannelRef
	{
		QPointer<Mixer> owner;
		MixerChannel* identity;
		QPointer<EffectChain> anchor;
		std::size_t row;
		MixerChannel* get() const;
		void check() const { get(); }
	};
	ChannelRef watchChannel(std::size_t row);
	std::vector<ChannelRef> watchChannels();
	// the mixer channels in the mixer. index 0 is always master.
	std::vector<MixerChannel*> m_mixerChannels;

	// Public topology mutations reject callbacks into an active transition.
	// Internal composition (load/clear/create) stays inside the admitted change.
	bool canChangeTopology() const;
	int createChannelImpl();
	bool deleteChannelImpl(int index);
	bool clearChannelImpl(mix_ch_t index);
	void clearImpl();
	MixerRoute* createRouteImpl(MixerChannel* from, MixerChannel* to, float amount);
	void deleteRouteImpl(MixerRoute* route);

	// make sure we have at least num channels
	void allocateChannelsTo(int num);
	void updateChannelSelectorRanges();
	void soloChanged(MixerChannel* channel);
	void selectSolo(int index, bool preserveSnapshot = false);
	void refreshSolo();
	void applySoloMask();
	QDomElement saveSoloJournal(QDomDocument& document, QDomElement& parent);
	bool restoreSoloJournal(const QDomElement& state, QString& error);
	friend class MixerControlModel;
	friend class MixerRoute;

	int m_lastSoloed;
	bool m_updatingSolo = false;
	bool m_changingTopology = false;
	bool m_retiring = false;
	std::uint64_t m_replacementGeneration = 0;
} ;


} // namespace lmms

#endif // LMMS_MIXER_H
