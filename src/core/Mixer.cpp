/*
 * Mixer.cpp - effect mixer for LMMS
 *
 * Copyright (c) 2008-2011 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Mixer.h"

#include <QDomElement>
#include <QHash>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QVector>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

#include "AudioEngine.h"
#include "AudioEngineWorkerThread.h"
#include "Mixer.h"
#include "Song.h"
#include "FileResourceIO.h"

#include "InstrumentTrack.h"
#include "MixHelpers.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "TrackContainer.h" // For TrackContainer::TrackList typedef

namespace lmms
{

namespace
{
std::vector<QPointer<IntModel>> channelSelectors()
{
	std::vector<QPointer<IntModel>> result;
	for (auto container : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		if (!container) { continue; }
		for (auto track : container->tracks())
		{
			if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { result.emplace_back(instrument->mixerChannelModel()); }
			else if (auto sample = dynamic_cast<SampleTrack*>(track)) { result.emplace_back(sample->mixerChannelModel()); }
		}
	}
	return result;
}
}

MixerControlModel::MixerControlModel(bool value, Model* parent)
	: BoolModel(value, parent), m_mixer(dynamic_cast<Mixer*>(parent)) { }

JournallingObject* MixerControlModel::journalTarget()
{
	return m_mixer && m_mixer->m_updatingSolo ? nullptr : this;
}

QDomElement MixerControlModel::saveJournalState(QDomDocument& document, QDomElement& parent)
{
	return m_mixer ? m_mixer->saveSoloJournal(document, parent) : BoolModel::saveJournalState(document, parent);
}

bool MixerControlModel::restoreJournalState(const QDomElement& state, QString& error)
{
	return m_mixer ? m_mixer->restoreSoloJournal(state, error) : BoolModel::restoreJournalState(state, error);
}


MixerRoute::MixerRoute( MixerChannel * from, MixerChannel * to, float amount ) :
	m_from( from ),
	m_to( to ),
	m_fromAnchor(&from->m_fxChain), m_toAnchor(&to->m_fxChain),
	m_amount(amount, 0, 1, 0.001f, nullptr,
			tr("Amount to send from channel %1 to channel %2").arg(m_from->index()).arg(m_to->index()))
{
	//qDebug( "created: %d to %d", m_from->m_channelIndex, m_to->m_channelIndex );
	// create send amount model
}

MixerRoute::~MixerRoute()
{
	// Detach before the amount model emits destruction notifications. A listener
	// can retire the mixer while this route's members are still unwinding.
	if (!m_owner || m_owner->m_retiring) { return; }
	const auto erase = [this](MixerRouteVector& routes) { routes.erase(std::remove(routes.begin(), routes.end(), this), routes.end()); };
	erase(m_owner->m_mixerRoutes);
	if (m_fromAnchor) { erase(m_from->m_sends); }
	if (m_toAnchor) { erase(m_to->m_receives); }
}


void MixerRoute::updateName()
{
	m_amount.setDisplayName(
			tr("Amount to send from channel %1 to channel %2").arg(m_from->index()).arg(m_to->index()));
}


MixerChannel::MixerChannel( int idx, Model * _parent ) :
	m_fxChain( nullptr ),
	m_stillRunning( false ),
	m_peakLeft( 0.0f ),
	m_peakRight( 0.0f ),
	m_buffer(Engine::audioEngine()->framesPerPeriod()),
	m_muteModel( false, _parent ),
	m_soloModel( false, _parent ),
	m_volumeModel(1.f, 0.f, 2.f, 0.001f, _parent),
	m_name(),
	m_lock(),
	m_queued( false ),
	m_dependenciesMet(0),
	m_channelIndex(idx)
{
	m_buffer.allocateInterleavedBuffer();
}




MixerChannel::~MixerChannel()
{
	// These are members, not separately allocated children. A destruction
	// observer may retire the mixer before the remaining members unwind.
	m_volumeModel.setParent(nullptr);
	m_muteModel.setParent(nullptr);
	m_soloModel.setParent(nullptr);
}


inline void MixerChannel::processed()
{
	for( const MixerRoute * receiverRoute : m_sends )
	{
		if( receiverRoute->receiver()->m_muted == false )
		{
			receiverRoute->receiver()->incrementDeps();
		}
	}
}

void MixerChannel::incrementDeps()
{
	const auto i = m_dependenciesMet++ + 1;
	if (i == m_receives.size() && !m_queued.exchange(true))
	{
		AudioEngineWorkerThread::addJob( this );
	}
}




void MixerChannel::doProcessing()
{
	const f_cnt_t fpp = Engine::audioEngine()->framesPerPeriod();

	if( m_muted == false )
	{
		for( MixerRoute * senderRoute : m_receives )
		{
			MixerChannel * sender = senderRoute->sender();
			FloatModel * sendModel = senderRoute->amount();
			if( ! sendModel ) qFatal( "Error: no send model found from %d to %d", senderRoute->senderIndex(), m_channelIndex );

			if (sender->m_buffer.hasAnySignal() || sender->m_stillRunning)
			{
				auto buffer = m_buffer.interleavedBuffer().asSampleFrames();

				// figure out if we're getting sample-exact input
				ValueBuffer * sendBuf = sendModel->valueBuffer();
				ValueBuffer * volBuf = sender->m_volumeModel.valueBuffer();

				// mix it's output with this one's output
				auto ch_buf = sender->m_buffer.interleavedBuffer().asSampleFrames();

				// use sample-exact mixing if sample-exact values are available
				if( ! volBuf && ! sendBuf ) // neither volume nor send has sample-exact data...
				{
					const float v = sender->m_volumeModel.value() * sendModel->value();
					MixHelpers::addMultiplied(buffer.data(), ch_buf.data(), v, fpp);
				}
				else if( volBuf && sendBuf ) // both volume and send have sample-exact data
				{
					MixHelpers::addMultipliedByBuffers(buffer.data(), ch_buf.data(), volBuf, sendBuf, fpp);
				}
				else if( volBuf ) // volume has sample-exact data but send does not
				{
					const float v = sendModel->value();
					MixHelpers::addMultipliedByBuffer(buffer.data(), ch_buf.data(), v, volBuf, fpp);
				}
				else // vice versa
				{
					const float v = sender->m_volumeModel.value();
					MixHelpers::addMultipliedByBuffer(buffer.data(), ch_buf.data(), v, sendBuf, fpp);
				}
				toPlanar(m_buffer.interleavedBuffer(), m_buffer.groupBuffers(0));
				m_buffer.mixSilenceFlags(sender->m_buffer);
			}
		}


		const float v = m_volumeModel.value();

		m_stillRunning = m_fxChain.processAudioBuffer(m_buffer);

		const auto peakSamples = SampleFrame{m_buffer.absPeakValue(0), m_buffer.absPeakValue(1)};
		m_peakLeft = std::max(m_peakLeft, peakSamples[0] * v);
		m_peakRight = std::max(m_peakRight, peakSamples[1] * v);
	}
	else
	{
		m_peakLeft = m_peakRight = 0.0f;
	}

	// increment dependency counter of all receivers
	processed();
}



Mixer::Mixer() :
	Model( nullptr ),
	JournallingObject(),
	m_mixerChannels(),
	m_lastSoloed(-1)
{
	// create master channel
	createChannel();
}



Mixer::~Mixer()
{
	m_retiring = true;
	m_changingTopology = true;
	m_updatingSolo = true;
	m_lastSoloed = -1;
	while (!m_mixerRoutes.empty())
	{
		deleteRouteImpl(m_mixerRoutes.front());
	}
	while( m_mixerChannels.size() )
	{
		MixerChannel * f = m_mixerChannels[m_mixerChannels.size() - 1];
		m_mixerChannels.pop_back();
		delete f;
	}
}



bool Mixer::canChangeTopology() const
{
	return !m_retiring && QThread::currentThread() == thread() && !m_changingTopology && !m_updatingSolo
		&& std::all_of(m_mixerChannels.begin(), m_mixerChannels.end(), [](const auto channel) { return !channel->m_fxChain.m_changing; });
}

Mixer::FlagGuard::FlagGuard(Mixer* owner, bool Mixer::* flag)
	: m_owner(owner), m_flag(flag), m_previous(owner->*flag)
{
	if (owner->m_retiring) { throw std::logic_error("Native mixer is retiring"); }
	owner->*flag = true;
}
Mixer::FlagGuard::~FlagGuard() { restore(); }
void Mixer::FlagGuard::restore() const { if (m_owner && !m_owner->m_retiring) { m_owner.data()->*m_flag = m_previous; } }
void Mixer::FlagGuard::check() const
{ if (!m_owner || m_owner->m_retiring) { throw std::runtime_error("Native mixer retired during transition"); } }
MixerChannel* Mixer::ChannelRef::get() const
{
	if (!owner || owner->m_retiring || !anchor || row >= owner->m_mixerChannels.size() || owner->m_mixerChannels[row] != identity)
	{ throw std::runtime_error("Native mixer channel retired or changed during transition"); }
	return identity;
}
Mixer::ChannelRef Mixer::watchChannel(std::size_t row)
{
	if (row >= m_mixerChannels.size()) { throw std::out_of_range("Native mixer channel is unavailable"); }
	return {this, m_mixerChannels[row], &m_mixerChannels[row]->m_fxChain, row};
}
std::vector<Mixer::ChannelRef> Mixer::watchChannels()
{
	std::vector<ChannelRef> channels; channels.reserve(m_mixerChannels.size());
	for (std::size_t row = 0; row < m_mixerChannels.size(); ++row) { channels.push_back(watchChannel(row)); }
	return channels;
}

int Mixer::createChannel()
{
	if (!canChangeTopology()) { throw std::logic_error("Mixer channels can only be added on their owner thread outside a mixer transition"); }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	return createChannelImpl();
}

int Mixer::createChannelImpl()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_mixerChannels.size() >= std::numeric_limits<mix_ch_t>::max())
	{
		throw std::length_error("Native mixer channel count exceeds its index representation");
	}
	const int index = m_mixerChannels.size();
	// create new channel
	auto channel = std::make_unique<MixerChannel>(index, this);
	m_mixerChannels.push_back(channel.get());
	channel.release();
	const auto watched = watchChannel(index);

	// reset channel state
	if (!clearChannelImpl(index)) { throw std::runtime_error("Native new mixer channel could not be reset"); }
	watched.check();

	const auto added = m_mixerChannels[index];
	connect(&added->m_soloModel, &BoolModel::dataChanged, this, [this, added] { soloChanged(added); }, Qt::DirectConnection);
	const QPointer<Mixer> owner(this); const QPointer<BoolModel> mute(&added->m_muteModel);
	const auto rememberMute = [owner, mute, added] {
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		// Explicit mute changes during solo also survive its eventual release.
		if (owner && mute && !owner->m_updatingSolo && owner->m_lastSoloed >= 0) { added->m_muteBeforeSolo = mute->value(); }
	};
	connect(&added->m_muteModel, &BoolModel::dataChanged, this, rememberMute, Qt::DirectConnection);
	connect(&added->m_muteModel, &BoolModel::dataUnchanged, this, [owner, mute, added, rememberMute] {
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		if (owner && mute && !owner->m_updatingSolo && owner->m_lastSoloed >= 0 && added->m_muteBeforeSolo != mute->value())
		{
			// The effective value did not change, but its solo-release meaning
			// did. Capture that change before the dataUnchanged handler applies it.
			added->m_muteModel.addJournalCheckPoint();
		}
		rememberMute();
	}, Qt::DirectConnection);
	refreshSolo();
	watched.check();
	updateChannelSelectorRanges();
	watched.check();

	return index;
}

void Mixer::updateChannelSelectorRanges()
{
	const QPointer<Mixer> owner(this);
	// This belongs to the native mixer, including headless creation/deletion.
	// Track constructors/loaders also set their range when they first appear.
	const auto maximum = static_cast<int>(m_mixerChannels.size()) - 1;
	for (const auto& selector : channelSelectors())
	{
		if (selector) { selector->setRange(0, maximum, 1); }
		if (!owner || owner->m_retiring) { throw std::runtime_error("Native mixer retired during selector range update"); }
	}
}

QDomElement Mixer::saveSoloJournal(QDomDocument& document, QDomElement& parent)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	auto state = document.createElement("mixersolostate"); state.setAttribute("version", 1);
	state.setAttribute("selected", m_lastSoloed < 0 ? 0u : m_mixerChannels[m_lastSoloed]->m_soloModel.id());
	for (const auto channel : m_mixerChannels)
	{
		auto entry = document.createElement("channel");
		entry.setAttribute("muteId", channel->m_muteModel.id()); entry.setAttribute("soloId", channel->m_soloModel.id());
		entry.setAttribute("muted", channel->m_muteModel.value() ? 1 : 0);
		entry.setAttribute("beforeSolo", channel->m_muteBeforeSolo ? 1 : 0);
		state.appendChild(entry);
	}
	parent.appendChild(state); return state;
}

bool Mixer::restoreSoloJournal(const QDomElement& state, QString& error)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto reject = [&](const QString& detail) { error = "Native mixer solo journal: " + detail; return false; };
	if (m_updatingSolo || m_changingTopology) { return reject("a mixer transition is already active"); }
	if (state.tagName() != "mixersolostate" || state.attribute("version") != "1") { return reject("invalid state format"); }
	bool selectedOk = false; const auto selectedId = state.attribute("selected").toUInt(&selectedOk);
	if (!selectedOk) { return reject("invalid selected model identity"); }
	struct Entry { MixerChannel* channel; bool muted; bool beforeSolo; };
	std::vector<Entry> entries; entries.reserve(m_mixerChannels.size());
	QHash<jo_id_t, MixerChannel*> current;
	for (const auto channel : m_mixerChannels) { current.insert(channel->m_muteModel.id(), channel); }
	QSet<jo_id_t> seen; int selected = -1;
	for (auto item = state.firstChildElement("channel"); !item.isNull(); item = item.nextSiblingElement("channel"))
	{
		bool muteOk = false, soloOk = false;
		const auto muteId = item.attribute("muteId").toUInt(&muteOk), soloId = item.attribute("soloId").toUInt(&soloOk);
		const auto channel = current.value(muteId, nullptr);
		if (!muteOk || !soloOk || !channel || channel->m_soloModel.id() != soloId || seen.contains(muteId))
		{
			return reject("referenced channels no longer match the current mixer");
		}
		const auto muted = item.attribute("muted"), before = item.attribute("beforeSolo");
		if ((muted != "0" && muted != "1") || (before != "0" && before != "1")) { return reject("invalid mute state"); }
		seen.insert(muteId); entries.push_back({channel, muted == "1", before == "1"});
		if (soloId == selectedId) { selected = channel->index(); }
	}
	if (entries.size() != m_mixerChannels.size() || (selectedId != 0 && selected < 0))
	{
		return reject("the channel set or selected channel has changed");
	}
	// Replay only controls on the same native objects. Effects, automation
	// ownership, volumes, routes and GUI pointers must not be recreated by solo undo.
	FlagGuard updating(this, &Mixer::m_updatingSolo);
	const auto channels = watchChannels();
	m_lastSoloed = selected;
	for (std::size_t row = 0; row < entries.size(); ++row)
	{
		const auto& entry = entries[row]; channels[row].check();
		entry.channel->m_muteBeforeSolo = entry.beforeSolo;
		entry.channel->m_soloModel.setValue(entry.channel->index() == selected);
		channels[row].check();
		entry.channel->m_muteModel.setValue(entry.muted);
		channels[row].check();
	}
	return true;
}

void Mixer::soloChanged(MixerChannel* channel)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_updatingSolo) { return; }
	const auto found = std::find(m_mixerChannels.begin(), m_mixerChannels.end(), channel);
	if (found == m_mixerChannels.end()) { return; }
	const int index = static_cast<int>(std::distance(m_mixerChannels.begin(), found));
	if (channel->m_soloModel.value()) { selectSolo(index); }
	else if (m_lastSoloed == index) { selectSolo(-1); }
}

void Mixer::selectSolo(int index, bool preserveSnapshot)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_updatingSolo || index == m_lastSoloed || index < -1
		|| index >= static_cast<int>(m_mixerChannels.size())) { return; }
	FlagGuard updating(this, &Mixer::m_updatingSolo);
	const auto channels = watchChannels();
	if (m_lastSoloed == -1 && index >= 0 && !preserveSnapshot)
	{
		for (const auto channel : m_mixerChannels) { channel->m_muteBeforeSolo = channel->m_muteModel.value(); }
	}
	m_lastSoloed = index;
	for (const auto& channel : channels) { auto native = channel.get(); native->m_soloModel.setValue(native->index() == index); channel.check(); }
	if (index < 0)
	{
		for (const auto& channel : channels) { auto native = channel.get(); native->m_muteModel.setValue(native->m_muteBeforeSolo); channel.check(); }
	}
	else { applySoloMask(); }
}

void Mixer::refreshSolo()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_updatingSolo || m_lastSoloed < 0) { return; }
	FlagGuard updating(this, &Mixer::m_updatingSolo);
	applySoloMask();
}

void Mixer::applySoloMask()
{
	const auto channels = watchChannels();
	// Preserve native solo routing: the selected channel, its ancestors, and
	// every output of those ancestors are audible. Reaching master does not
	// make master's other inputs audible. Each traversal visits a channel once.
	const auto selected = m_mixerChannels[m_lastSoloed];
	QSet<MixerChannel*> ancestors{selected};
	std::vector<MixerChannel*> pending{selected};
	while (!pending.empty())
	{
		const auto channel = pending.back(); pending.pop_back();
		if (channel->isMaster()) { continue; }
		for (const auto route : channel->m_receives)
		{
			const auto sender = route->sender();
			if (!ancestors.contains(sender)) { ancestors.insert(sender); pending.push_back(sender); }
		}
	}
	auto audible = ancestors;
	for (const auto channel : ancestors) { pending.push_back(channel); }
	while (!pending.empty())
	{
		const auto channel = pending.back(); pending.pop_back();
		if (channel->isMaster()) { continue; }
		for (const auto route : channel->m_sends)
		{
			const auto receiver = route->receiver();
			if (!audible.contains(receiver)) { audible.insert(receiver); pending.push_back(receiver); }
		}
	}
	for (const auto& watched : channels)
	{
		const auto channel = watched.get();
		const bool muted = !audible.contains(channel) && (!channel->isMaster() || channel->m_muteBeforeSolo);
		channel->m_muteModel.setValue(muted);
		watched.check();
	}
}



void MixerChannel::setName(const QString& name)
{
	const QString requested(name); const QPointer<EffectChain> owner(&m_fxChain);
	const auto ready = [&] { if (!owner) { throw std::runtime_error("Native mixer channel retired during rename"); } };
	m_name = requested;
	m_volumeModel.setDisplayName(requested + ">" + Mixer::tr("Volume")); ready();
	m_muteModel.setDisplayName(requested + ">" + Mixer::tr("Mute")); ready();
	m_soloModel.setDisplayName(requested + ">" + Mixer::tr("Solo")); ready();
}

bool Mixer::deleteChannel( int index )
{
	if (!canChangeTopology()) { return false; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	return deleteChannelImpl(index);
}

bool Mixer::deleteChannelImpl(int index)
{
	// channel deletion is performed between mixer rounds
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (index <= 0 || index >= static_cast<int>(m_mixerChannels.size())) { return false; }
	const auto watched = watchChannel(index);
	if (m_lastSoloed == index) { selectSolo(-1); }
	watched.check();
	FlagGuard updating(this, &Mixer::m_updatingSolo);

	const auto remap = [index](int value) { return value == index ? 0 : value > index ? value - 1 : value; };
	struct Assignment { QPointer<IntModel> model; int initial; int value; };
	std::vector<Assignment> assignments;
	for (const auto& selector : channelSelectors())
	{
		if (selector) { assignments.push_back({selector, remap(selector->initValue()), remap(static_cast<int>(selector->storedValue()))}); }
	}

	MixerChannel * ch = m_mixerChannels[index];

	// delete all of this channel's sends and receives
	while (!ch->m_sends.empty())
	{
		deleteRouteImpl(ch->m_sends.front());
		watched.check();
	}
	while (!ch->m_receives.empty())
	{
		deleteRouteImpl(ch->m_receives.front());
		watched.check();
	}

	// if m_lastSoloed was our index, reset it
	if (m_lastSoloed == index) { m_lastSoloed = -1; }
	// if m_lastSoloed is > delete index, it will move left
	else if (m_lastSoloed > index) { --m_lastSoloed; }

	// actually delete the channel
	m_mixerChannels.erase(m_mixerChannels.begin() + index);
	delete ch;
	updating.check();

	for (auto i = static_cast<std::size_t>(index); i < m_mixerChannels.size(); ++i)
	{
		const auto current = watchChannel(i);
		validateChannelName( i, i + 1 );
		current.check();

		// set correct channel index
		m_mixerChannels[i]->setIndex(i);

		// now check all routes and update names of the send models
		std::vector<QPointer<MixerRoute>> routes;
		for (auto route : m_mixerChannels[i]->m_sends) { routes.emplace_back(route); }
		for (auto route : m_mixerChannels[i]->m_receives) { routes.emplace_back(route); }
		for (const auto& route : routes)
		{
			current.check(); if (!route) { throw std::runtime_error("Native mixer route retired during channel deletion"); }
			route->updateName(); current.check();
		}
	}

	for (const auto& assignment : assignments)
	{
		if (!assignment.model) { continue; }
		if (assignment.model->initValue() != assignment.initial) { assignment.model->setInitValue(assignment.initial); }
		updating.check();
		if (assignment.model) { assignment.model->setValue(assignment.value); }
		updating.check();
	}
	updating.restore();
	updateChannelSelectorRanges();
	updating.check();
	refreshSolo();
	updating.check();
	return true;
}



bool Mixer::moveChannel(int from, int to)
{
	if (!canChangeTopology()) { return false; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto count = static_cast<int>(m_mixerChannels.size());
	if (from <= 0 || to <= 0 || from >= count || to >= count) { return false; }
	if (from == to) { return true; }
	for (int index = 0; index < count; ++index)
	{
		if (!m_mixerChannels[index] || m_mixerChannels[index]->index() != index) { return false; }
	}
	const auto remap = [from, to](int index) {
		if (index == from) { return to; }
		if (from < to && index > from && index <= to) { return index - 1; }
		if (from > to && index >= to && index < from) { return index + 1; }
		return index;
	};
	struct Assignment { QPointer<IntModel> model; int initial; int value; };
	std::vector<Assignment> assignments;
	for (const auto& selector : channelSelectors())
	{
		// Snapshot all assignments before linked controls can update each other.
		if (selector) { assignments.push_back({selector, remap(selector->initValue()), remap(static_cast<int>(selector->storedValue()))}); }
	}

	if (from < to) { std::rotate(m_mixerChannels.begin() + from, m_mixerChannels.begin() + from + 1, m_mixerChannels.begin() + to + 1); }
	else { std::rotate(m_mixerChannels.begin() + to, m_mixerChannels.begin() + from, m_mixerChannels.begin() + from + 1); }
	m_lastSoloed = remap(m_lastSoloed);
	for (int index = std::min(from, to); index <= std::max(from, to); ++index)
	{
		const auto watched = watchChannel(index);
		validateChannelName(index, m_mixerChannels[index]->index());
		watched.check();
		m_mixerChannels[index]->setIndex(index);
	}
	std::vector<QPointer<MixerRoute>> routes;
	for (auto route : m_mixerRoutes) { routes.emplace_back(route); }
	for (const auto& route : routes) { changing.check(); if (!route) { throw std::runtime_error("Native mixer route retired during channel move"); } route->updateName(); changing.check(); }
	for (const auto& assignment : assignments)
	{
		if (!assignment.model) { continue; }
		if (assignment.model->initValue() != assignment.initial) { assignment.model->setInitValue(assignment.initial); }
		changing.check();
		if (assignment.model) { assignment.model->setValue(assignment.value); }
		changing.check();
	}
	if (auto song = Engine::getSong()) { song->setModified(); }
	changing.check();
	emit channelMoved(from, to);
	changing.check();
	return true;
}

void Mixer::moveChannelLeft(int index)
{
	if (index > 1) { moveChannel(index, index - 1); }
}


void Mixer::moveChannelRight( int index )
{
	if (index > 0 && index < static_cast<int>(m_mixerChannels.size()) - 1) { moveChannel(index, index + 1); }
}



MixerRoute * Mixer::createChannelSend( mix_ch_t fromChannel, mix_ch_t toChannel,
								float amount )
{
	if (!canChangeTopology()) { return nullptr; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (fromChannel >= m_mixerChannels.size() || toChannel >= m_mixerChannels.size()) { return nullptr; }
	return createRouteImpl(m_mixerChannels[fromChannel], m_mixerChannels[toChannel], amount);
}


MixerRoute * Mixer::createRoute( MixerChannel * from, MixerChannel * to, float amount )
{
	if (!canChangeTopology()) { return nullptr; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	return createRouteImpl(from, to, amount);
}

MixerRoute* Mixer::createRouteImpl(MixerChannel* from, MixerChannel* to, float amount)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (!std::isfinite(amount) || amount < 0.f || amount > 1.f || checkInfiniteLoop(from, to)) { return nullptr; }
	const auto source = watchChannel(from->index()), receiver = watchChannel(to->index());
	for (const auto send : from->m_sends)
	{
		if (send->receiver() == to)
		{
			const QPointer<MixerRoute> live(send); send->amount()->setValue(amount); source.check(); receiver.check();
			if (!live) { throw std::runtime_error("Native mixer route retired during amount update"); } return live.data();
		}
	}
	// Reserve every ownership view before publication; allocation failure must
	// not leave a route in only one endpoint's dependency list.
	auto route = std::make_unique<MixerRoute>(from, to, amount);
	from->m_sends.reserve(from->m_sends.size() + 1);
	to->m_receives.reserve(to->m_receives.size() + 1);
	m_mixerRoutes.reserve(m_mixerRoutes.size() + 1);
	from->m_sends.push_back(route.get());
	to->m_receives.push_back(route.get());
	m_mixerRoutes.push_back(route.get());
	const QPointer<MixerRoute> published(route.release());
	published->m_owner = this;
	refreshSolo();
	source.check(); receiver.check();
	if (!published) { throw std::runtime_error("Native mixer route retired during creation"); }
	return published.data();
}


// delete the connection made by createChannelSend
void Mixer::deleteChannelSend( mix_ch_t fromChannel, mix_ch_t toChannel )
{
	if (!canChangeTopology()) { return; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (fromChannel >= m_mixerChannels.size() || toChannel >= m_mixerChannels.size()) { return; }
	// delete the send
	MixerChannel * from = m_mixerChannels[fromChannel];
	MixerChannel * to	 = m_mixerChannels[toChannel];

	// find and delete the send entry
	for (const auto& send : from->m_sends)
	{
		if (send->receiver() == to)
		{
			deleteRouteImpl(send);
			break;
		}
	}
}


void Mixer::deleteChannelSend( MixerRoute * route )
{
	if (!canChangeTopology()) { return; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	deleteRouteImpl(route);
}

void Mixer::deleteRouteImpl(MixerRoute* route)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<Mixer> owner(this); const bool retiring = m_retiring;
	if (!route || std::find(m_mixerRoutes.begin(), m_mixerRoutes.end(), route) == m_mixerRoutes.end()) { return; }

	auto removeFromMixerRoute = [route](MixerRouteVector& routeVec)
	{
		auto it = std::find(routeVec.begin(), routeVec.end(), route);
		if (it != routeVec.end()) { routeVec.erase(it); }
	};

	// remove us from from's sends
	removeFromMixerRoute(route->sender()->m_sends);

	// remove us from to's receives
	removeFromMixerRoute(route->receiver()->m_receives);

	// remove us from mixer's list
	removeFromMixerRoute(m_mixerRoutes);

	delete route;
	if (retiring) { return; }
	if (!owner || owner->m_retiring) { throw std::runtime_error("Native mixer retired during route removal"); }
	refreshSolo();
}


bool Mixer::isInfiniteLoop( mix_ch_t sendFrom, mix_ch_t sendTo )
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (sendFrom >= m_mixerChannels.size() || sendTo >= m_mixerChannels.size()) { return true; }
	return checkInfiniteLoop(m_mixerChannels[sendFrom], m_mixerChannels[sendTo]);
}


bool Mixer::checkInfiniteLoop( MixerChannel * from, MixerChannel * to )
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QSet<const MixerChannel*> members;
	for (const auto channel : m_mixerChannels) { members.insert(channel); }
	const auto owns = [&](const MixerChannel* channel) { return channel && members.contains(channel); };
	if (!owns(from) || !owns(to) || from == m_mixerChannels[0] || from == to) { return true; }
	// Visit each downstream channel once. Recursive path enumeration can repeat
	// exponentially in a converging DAG or never return on a malformed cycle.
	QSet<MixerChannel*> visited; std::vector<MixerChannel*> pending{to};
	visited.insert(to);
	while (!pending.empty())
	{
		const auto channel = pending.back(); pending.pop_back();
		if (channel == from) { return true; }
		for (const auto send : channel->m_sends)
		{
			if (!send || !owns(send->receiver())) { return true; }
			const auto receiver = send->receiver();
			if (!visited.contains(receiver)) { visited.insert(receiver); pending.push_back(receiver); }
		}
	}
	return false;
}


// how much does fromChannel send its output to the input of toChannel?
FloatModel * Mixer::channelSendModel( mix_ch_t fromChannel, mix_ch_t toChannel )
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (fromChannel == toChannel || fromChannel >= m_mixerChannels.size() || toChannel >= m_mixerChannels.size())
	{
		return nullptr;
	}
	const MixerChannel * from = m_mixerChannels[fromChannel];
	const MixerChannel * to = m_mixerChannels[toChannel];

	for( MixerRoute * route : from->m_sends )
	{
		if( route->receiver() == to )
		{
			return route->amount();
		}
	}

	return nullptr;
}



void Mixer::mixToChannel(const AudioBuffer& buffer, mix_ch_t dest)
{
	const auto channel = m_mixerChannels[dest];
	if (!channel->m_muteModel.value())
	{
		channel->m_lock.lock();
		MixHelpers::add(channel->m_buffer.groupBuffers(0), buffer.groupBuffers(0));

		// Copy the planar buffer to the temporary interleaved buffer so they stay in sync
		toInterleaved(channel->m_buffer.groupBuffers(0), channel->m_buffer.interleavedBuffer());

		channel->m_buffer.mixSilenceFlags(buffer);
		channel->m_lock.unlock();
	}
}




void Mixer::prepareMasterMix()
{
	m_mixerChannels[0]->m_buffer.silenceAllChannels();
}



void Mixer::masterMix( SampleFrame* _buf )
{
	const int fpp = Engine::audioEngine()->framesPerPeriod();

	// add the channels that have no dependencies (no incoming senders, ie.
	// no receives) to the jobqueue. The channels that have receives get
	// added when their senders get processed, which is detected by
	// dependency counting.
	// also instantly add all muted channels as they don't need to care
	// about their senders, and can just increment the deps of their
	// recipients right away.
	AudioEngineWorkerThread::resetJobQueue( AudioEngineWorkerThread::JobQueue::OperationMode::Dynamic );
	// Every receiver must have this period's mute state before the first
	// sender can publish completion, including senders earlier in channel order.
	for (const auto channel : m_mixerChannels) { channel->m_muted = channel->m_muteModel.value(); }
	for( MixerChannel * ch : m_mixerChannels )
	{
		if( ch->m_muted ) // instantly "process" muted channels
		{
			ch->m_buffer.silenceAllChannels();
			ch->m_peakLeft = ch->m_peakRight = 0.f;
			ch->done();
			ch->processed();
		}
		else if( ch->m_receives.size() == 0 )
		{
			ch->m_queued = true;
			AudioEngineWorkerThread::addJob( ch );
		}
	}
	// Drain the whole graph even when master is muted or finishes early.
	// Unrouted buses still own effect state that must advance once per period.
	AudioEngineWorkerThread::startAndWaitForJobs();
	const auto queueFailed = AudioEngineWorkerThread::jobQueueFailure() != AudioEngineWorkerThread::JobQueue::Failure::None;
	const auto incomplete = std::any_of(m_mixerChannels.begin(), m_mixerChannels.end(), [](const auto channel) {
		return channel->state() != ThreadableJob::ProcessingState::Done;
	});
	if (queueFailed || incomplete)
	{
		if (!queueFailed)
		{
			Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::MixerDependency);
		}
		discardProcessingPeriod(); return;
	}

	auto buffer = m_mixerChannels[0]->m_buffer.interleavedBuffer().asSampleFrames();

	// handle sample-exact data in master volume fader
	ValueBuffer * volBuf = m_mixerChannels[0]->m_volumeModel.valueBuffer();

	if( volBuf )
	{
		for( int f = 0; f < fpp; f++ )
		{
			buffer[f][0] *= volBuf->values()[f];
			buffer[f][1] *= volBuf->values()[f];
		}
	}

	const float v = volBuf
		? 1.0f
		: m_mixerChannels[0]->m_volumeModel.value();
	MixHelpers::addMultiplied(_buf, buffer.data(), v, fpp);
	discardProcessingPeriod();
}

void Mixer::discardProcessingPeriod()
{
	// clear all channel buffers and
	// reset channel process state
	for( int i = 0; i < numChannels(); ++i)
	{
		m_mixerChannels[i]->m_buffer.silenceAllChannels();
		m_mixerChannels[i]->reset();
		m_mixerChannels[i]->m_queued = false;
		m_mixerChannels[i]->m_dependenciesMet = 0;
	}
}




void Mixer::clear()
{
	if (!canChangeTopology()) { return; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	clearImpl();
}

void Mixer::clearImpl()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FlagGuard updating(this, &Mixer::m_updatingSolo);
	if (m_replacementGeneration == std::numeric_limits<std::uint64_t>::max()) { throw std::length_error("Native mixer replacement generation exhausted"); }
	++m_replacementGeneration;
	m_lastSoloed = -1;
	while( m_mixerChannels.size() > 1 )
	{
		deleteChannelImpl(1);
		updating.check();
	}

	clearChannelImpl(0);
	updating.check();
}



bool Mixer::clearChannel(mix_ch_t index)
{
	if (!canChangeTopology()) { return false; }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	return clearChannelImpl(index);
}

bool Mixer::clearChannelImpl(mix_ch_t index)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (index >= m_mixerChannels.size()) { return false; }
	const auto watched = watchChannel(index);
	MixerChannel * ch = m_mixerChannels[index];
	if (!ch->m_fxChain.clear()) { return false; }
	watched.check();
	if (m_lastSoloed == index) { selectSolo(-1); }
	watched.check();
	FlagGuard updating(this, &Mixer::m_updatingSolo);
	ch->m_volumeModel.setValue( 1.0f );
	watched.check();
	ch->m_muteModel.setValue( false );
	watched.check();
	ch->m_soloModel.setValue( false );
	watched.check();
	ch->m_muteBeforeSolo = false;
	ch->setName(index == 0 ? tr("Master") : tr("Channel %1").arg(index));
	watched.check();
	ch->setColor(std::nullopt);

	// send only to master
	if( index > 0)
	{
		// delete existing sends
		while (!ch->m_sends.empty())
		{
			deleteRouteImpl(ch->m_sends.front());
			watched.check();
		}

		// add send to master
		createRouteImpl(ch, m_mixerChannels[0], 1.f);
		watched.check();
	}

	// delete receives
	while (!ch->m_receives.empty())
	{
		deleteRouteImpl(ch->m_receives.front());
		watched.check();
	}
	updating.restore();
	refreshSolo();
	watched.check();
	return true;
}

void Mixer::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if (!canChangeTopology()) { throw std::logic_error("Native mixer cannot serialize during a topology transition or from another thread"); }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<Mixer> owner(this);
	m_changingTopology = true;
	const auto changing = qScopeGuard([owner] { if (owner) { owner->m_changingTopology = false; } });
	struct Channel { MixerChannel* identity; QPointer<EffectChain> anchor; std::vector<QPointer<MixerRoute>> sends; };
	std::vector<Channel> channels;
	for (auto channel : m_mixerChannels)
	{
		Channel entry{channel, &channel->m_fxChain, {}};
		for (auto send : channel->m_sends) { entry.sends.emplace_back(send); }
		channels.push_back(std::move(entry));
	}
	const auto ready = [&] {
		if (!owner || owner->m_mixerChannels.size() != channels.size()) { throw std::runtime_error("Native mixer retired or changed during serialization"); }
		for (std::size_t index = 0; index < channels.size(); ++index)
		{
			const auto& entry = channels[index];
			if (!entry.anchor || owner->m_mixerChannels[index] != entry.identity || entry.identity->m_sends.size() != entry.sends.size()) { throw std::runtime_error("Native mixer channel changed during serialization"); }
			for (std::size_t row = 0; row < entry.sends.size(); ++row)
			{ if (!entry.sends[row] || entry.identity->m_sends[row] != entry.sends[row].data()) { throw std::runtime_error("Native mixer send changed during serialization"); } }
		}
	};
	// save channels
	for (auto i = std::size_t{0}; i < channels.size(); ++i)
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready();
		MixerChannel * ch = channels[i].identity;

		QDomElement mixch = _doc.createElement( QString( "mixerchannel" ) );
		_this.appendChild( mixch );

		ch->m_fxChain.saveState( _doc, mixch );
		ready();
		ch->m_volumeModel.saveSettings( _doc, mixch, "volume" );
		ready();
		ch->m_muteModel.saveSettings( _doc, mixch, "muted" );
		ready();
		ch->m_soloModel.saveSettings( _doc, mixch, "soloed" );
		ready();
		if (m_lastSoloed >= 0) { mixch.setAttribute("mutedBeforeSolo", ch->m_muteBeforeSolo ? 1 : 0); }
		mixch.setAttribute("num", static_cast<qulonglong>(i));
		mixch.setAttribute( "name", ch->m_name );
		if (const auto& color = ch->color()) { mixch.setAttribute("color", color->name()); }

		// add the channel sends
		for (const auto& send : channels[i].sends)
		{
			QDomElement sendsDom = _doc.createElement( QString( "send" ) );
			mixch.appendChild( sendsDom );

			sendsDom.setAttribute("channel", send->receiverIndex());
			send->amount()->saveSettings(_doc, sendsDom, "amount");
			ready();
		}
	}
}

// make sure we have at least num channels
void Mixer::allocateChannelsTo(int num)
{
	if (num <= 0) { return; }
	const QPointer<Mixer> owner(this);
	while (static_cast<std::size_t>(num) > m_mixerChannels.size() - 1)
	{
		createChannelImpl();
		if (!owner || owner->m_retiring) { throw std::runtime_error("Native mixer retired during allocation"); }

		// delete the default send to master
		if (!m_mixerChannels.back()->m_sends.empty()) { deleteRouteImpl(m_mixerChannels.back()->m_sends.front()); }
		if (!owner || owner->m_retiring) { throw std::runtime_error("Native mixer retired during default route removal"); }
	}
}


void Mixer::loadSettings( const QDomElement & _this )
{
	if (!canChangeTopology()) { throw std::logic_error("Mixer settings cannot be replaced during a mixer transition or from another thread"); }
	FlagGuard changing(this, &Mixer::m_changingTopology);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const auto check = [&] { changing.check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } changing.check(); };
	check();
	// Validate the complete XML routing graph before clearing an existing
	// mixer or indexing/allocating channels. Forward references remain valid.
	QHash<int, QVector<int>> sends;
	QHash<int, int> incoming;
	const auto readIndex = [&](const QDomElement& element, const QString& attribute, int& result) {
		bool ok = false; result = element.attribute(attribute).toInt(&ok);
		if (!ok || result < 0 || result >= std::numeric_limits<mix_ch_t>::max())
		{
			Engine::getSong()->collectError(tr("Invalid native mixer %1 index: %2")
				.arg(attribute, element.attribute(attribute)));
			return false;
		}
		return true;
	};
	for (auto channel = _this.firstChildElement("mixerchannel"); !channel.isNull(); channel = channel.nextSiblingElement("mixerchannel"))
	{
		check();
		int from; if (!readIndex(channel, "num", from)) { return; }
		if (channel.hasAttribute("mutedBeforeSolo") && channel.attribute("mutedBeforeSolo") != "0"
			&& channel.attribute("mutedBeforeSolo") != "1")
		{
			Engine::getSong()->collectError(tr("Invalid native mixer solo mute snapshot")); return;
		}
		if (!incoming.contains(from)) { incoming.insert(from, 0); }
		for (auto send = channel.firstChildElement("send"); !send.isNull(); send = send.nextSiblingElement("send"))
		{
			check();
			int to; if (!readIndex(send, "channel", to)) { return; }
			if (from == 0 || from == to)
			{
				Engine::getSong()->collectError(tr("Invalid native mixer route %1 to %2").arg(from).arg(to)); return;
			}
			sends[from].push_back(to); ++incoming[to];
		}
	}
	QVector<int> ready;
	for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) { if (it.value() == 0) { ready.push_back(it.key()); } }
	for (int next = 0; next < ready.size(); ++next)
	{
		check();
		for (const auto receiver : sends.value(ready[next])) { if (--incoming[receiver] == 0) { ready.push_back(receiver); } }
	}
	if (ready.size() != incoming.size())
	{
		Engine::getSong()->collectError(tr("Native mixer routing contains a feedback cycle")); return;
	}
	FlagGuard loading(this, &Mixer::m_updatingSolo);
	bool hasSoloSnapshot = false;
	clearImpl();
	check();
	for (auto mixch = _this.firstChildElement("mixerchannel"); !mixch.isNull(); mixch = mixch.nextSiblingElement("mixerchannel"))
	{
		check();
		// index of the channel we are about to load
		int num = mixch.attribute( "num" ).toInt();

		// allocate enough channels
		allocateChannelsTo( num );
		check(); const auto watched = watchChannel(num);

		m_mixerChannels[num]->m_volumeModel.loadSettings( mixch, "volume" );
		watched.check(); check();
		m_mixerChannels[num]->m_muteModel.loadSettings( mixch, "muted" );
		watched.check(); check();
		m_mixerChannels[num]->m_soloModel.loadSettings( mixch, "soloed" );
		watched.check(); check();
		hasSoloSnapshot = hasSoloSnapshot || mixch.hasAttribute("mutedBeforeSolo");
		m_mixerChannels[num]->m_muteBeforeSolo = mixch.hasAttribute("mutedBeforeSolo")
			? mixch.attribute("mutedBeforeSolo") == "1" : m_mixerChannels[num]->m_muteModel.value();
		m_mixerChannels[num]->setName(mixch.attribute("name"));
		watched.check();
		if (mixch.hasAttribute("color"))
		{
			m_mixerChannels[num]->setColor(QColor{mixch.attribute("color")});
		}

		m_mixerChannels[num]->m_fxChain.restoreState( mixch.firstChildElement(
			m_mixerChannels[num]->m_fxChain.nodeName() ) );
		watched.check(); check();

		// mixer sends
		QDomNodeList chData = mixch.childNodes();
		for (auto i = 0; i < chData.length(); ++i)
		{
			check(); watched.check();
			QDomElement chDataItem = chData.at(i).toElement();
			if( chDataItem.nodeName() == QString( "send" ) )
			{
				int sendTo = chDataItem.attribute( "channel" ).toInt();
				allocateChannelsTo( sendTo ) ;
				watched.check(); const auto receiver = watchChannel(sendTo);
				const QPointer<MixerRoute> mxr(createRouteImpl(m_mixerChannels[num], m_mixerChannels[sendTo], 1.f));
				watched.check(); receiver.check();
				if (mxr) { mxr->amount()->loadSettings(chDataItem, "amount"); }
				else { Engine::getSong()->collectError(tr("Native mixer route %1 to %2 was rejected").arg(num).arg(sendTo)); }
				watched.check(); receiver.check();
				if (!mxr) { throw std::runtime_error("Native restored mixer route is unavailable"); }
			}
		}
	}

	int selected = -1;
	for (const auto channel : m_mixerChannels) { if (channel->m_soloModel.value()) { selected = channel->index(); } }
	loading.restore();
	if (selected >= 0)
	{
		if (hasSoloSnapshot)
		{
			// A new-format snapshot stores both effective mute overrides and the
			// state to restore on release. Do not reapply the mask over overrides.
			FlagGuard updating(this, &Mixer::m_updatingSolo);
			m_lastSoloed = selected;
			for (const auto& channel : watchChannels()) { auto native = channel.get(); native->m_soloModel.setValue(native->index() == selected); channel.check(); }
		}
		else { selectSolo(selected, true); }
	}
	check();
	emit dataChanged();
	check();
}


void Mixer::validateChannelName( int index, int oldIndex )
{
	const auto channel = m_mixerChannels[index];
	channel->setName(channel->m_name == tr("Channel %1").arg(oldIndex) ? tr("Channel %1").arg(index) : channel->m_name);
}

bool Mixer::isChannelInUse(int index)
{
	// check if the index mixer channel receives audio from any other channel
	if (!m_mixerChannels[index]->m_receives.empty())
	{
		return true;
	}

	// check if the destination mixer channel on any instrument or sample track is the index mixer channel
	TrackContainer::TrackList tracks;

	auto& songTracks = Engine::getSong()->tracks();
	auto& patternStoreTracks = Engine::patternStore()->tracks();
	tracks.insert(tracks.end(), songTracks.begin(), songTracks.end());
	tracks.insert(tracks.end(), patternStoreTracks.begin(), patternStoreTracks.end());

	for (const auto t : tracks)
	{
		if (t->type() == Track::Type::Instrument)
		{
			auto inst = dynamic_cast<InstrumentTrack*>(t);
			if (inst->mixerChannelModel()->value() == index)
			{
				return true;
			}
		}
		else if (t->type() == Track::Type::Sample)
		{
			auto strack = dynamic_cast<SampleTrack*>(t);
			if (strack->mixerChannelModel()->value() == index)
			{
				return true;
			}
		}
	}

	return false;
}


} // namespace lmms
