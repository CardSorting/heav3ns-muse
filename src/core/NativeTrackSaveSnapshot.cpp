/* Native track serialization source identity and parameter state. SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeTrackSaveSnapshot.h"
#include <QPointer>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <stdexcept>
#include <optional>
#include <vector>
#include "AutomatableModel.h"
#include "AutomationTrack.h"
#include "Clip.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "Controller.h"
#include "Mixer.h"
#include "PatternStore.h"
#include "Timeline.h"
#include "Scale.h"
#include "Keymap.h"
#include "NativeLinkedControls.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"
namespace lmms
{
struct NativeTrackSaveSnapshot::State
{
	struct Group { QPointer<AutomatableModel> root; std::shared_ptr<AutomatableModel::ControllerState> state; };
	struct Model { QPointer<AutomatableModel> model; jo_id_t id; };
	QPointer<Track> track;
	QPointer<TrackContainer> owner;
	QPointer<Song> song;
	QPointer<Instrument> instrument; Instrument* instrumentIdentity = nullptr;
	QPointer<EffectChain> chain; EffectChain* chainIdentity = nullptr;
	std::vector<QPointer<Effect>> effects;
	std::vector<QPointer<Clip>> clips;
	std::vector<Model> models;
	std::vector<Group> groups;
	QString name;
	std::optional<QColor> color;
	int height;
	bool beforeSolo, includeClips;
};
NativeTrackSaveSnapshot::NativeTrackSaveSnapshot(Track* track, bool includeClips) : m_state(std::make_unique<State>())
{
	auto& state = *m_state; state.track = track; state.owner = track ? track->trackContainer() : nullptr; state.song = Engine::getSong(); state.includeClips = includeClips;
	if (!track || !state.owner || !state.song || track->thread() != QThread::currentThread()) { throw std::invalid_argument("Native serialization requires a live track on its owner thread"); }
	state.name = track->name(); if (!state.track) { throw std::runtime_error("Native track retired during save preparation"); }
	state.color = track->color(); state.height = track->getHeight(); state.beforeSolo = track->isMutedBeforeSolo();
	if (auto native = dynamic_cast<InstrumentTrack*>(track)) { if (native->isRetiring()) { throw std::runtime_error("Native instrument track is retiring during save preparation"); } state.instrument = native->instrument(); state.chain = native->audioBusHandle()->effects(); }
	else if (auto sample = dynamic_cast<SampleTrack*>(track)) { state.chain = sample->audioBusHandle()->effects(); }
	state.instrumentIdentity = state.instrument.data(); state.chainIdentity = state.chain.data();
	if (state.chain) { for (auto effect : state.chain->effects()) { state.effects.emplace_back(effect); } }
	if (includeClips) { for (auto clip : track->getClips()) { state.clips.emplace_back(clip); } }
	QSet<const AutomatableModel*> captured;
	for (auto model : NativeLinkedControls::trackModels(track, includeClips))
	{
		state.models.push_back({model, model->id()});
		if (captured.contains(model)) { continue; }
		state.groups.push_back({model, model->captureControllerState()});
		for (const auto peer : model->linkedModels()) { captured.insert(peer); }
	}
	validate();
}
NativeTrackSaveSnapshot::~NativeTrackSaveSnapshot() = default;
struct NativeProjectSaveSnapshot::State
{
	struct Group { QPointer<AutomatableModel> root; std::shared_ptr<AutomatableModel::ControllerState> state; };
	struct Model { QPointer<AutomatableModel> model; jo_id_t id; };
	struct Container { QPointer<TrackContainer> owner; std::vector<QPointer<Track>> tracks; };
	struct Channel
	{
		MixerChannel* identity;
		QPointer<EffectChain> anchor;
		QString name;
		std::optional<QColor> color;
		bool beforeSolo;
		std::vector<QPointer<Effect>> effects;
		std::vector<QPointer<MixerRoute>> sends;
	};
	struct Route { QPointer<MixerRoute> route; MixerChannel* from; MixerChannel* to; };
	struct ControllerEntry { QPointer<Controller> controller; QString name; };
	QPointer<Song> song;
	QPointer<PatternStore> patterns;
	QPointer<Mixer> mixer;
	QPointer<Track> hidden;
	std::vector<Container> containers;
	std::vector<std::unique_ptr<NativeTrackSaveSnapshot>> tracks;
	std::vector<Model> models;
	std::vector<Group> groups;
	std::vector<ControllerEntry> controllers;
	std::vector<Channel> channels;
	std::vector<Route> routes;
	std::vector<std::shared_ptr<const Scale>> scales;
	std::vector<std::shared_ptr<const Keymap>> keymaps;
	TimePos loopBegin, loopEnd;
	Timeline::StopBehaviour stop;
	bool loop;
	int pattern, solo;
};
NativeProjectSaveSnapshot::NativeProjectSaveSnapshot(Song* song) : m_state(std::make_unique<State>())
{
	auto& s = *m_state; s.song = song; s.patterns = Engine::patternStore(); s.mixer = Engine::mixer();
	if (!song || song != Engine::getSong() || !s.patterns || !s.mixer || song->thread() != QThread::currentThread()) { throw std::logic_error("Native project save requires live owner-thread models"); }
	s.hidden = song->globalAutomationTrack();
	for (auto container : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(s.patterns.data())})
	{
		State::Container entry; entry.owner = container;
		for (auto track : container->tracks()) { entry.tracks.emplace_back(track); }
		s.containers.push_back(std::move(entry));
	}
	for (const auto& container : s.containers)
	{ for (const auto& track : container.tracks) { s.tracks.push_back(std::make_unique<NativeTrackSaveSnapshot>(track.data(), true)); } }
	s.tracks.push_back(std::make_unique<NativeTrackSaveSnapshot>(s.hidden.data(), true));
	QSet<const AutomatableModel*> captured;
	for (auto model : NativeLinkedControls::projectModels())
	{
		s.models.push_back({model, model->id()});
		if (captured.contains(model)) { continue; }
		s.groups.push_back({model, model->captureControllerState()});
		for (auto peer : model->linkedModels()) { captured.insert(peer); }
	}
	for (auto controller : song->controllers()) { s.controllers.push_back({controller, {}}); }
	for (auto& entry : s.controllers)
	{
		if (!entry.controller) { throw std::runtime_error("Native controller retired during project save capture"); }
		entry.name = entry.controller->Controller::name();
		if (!s.song || !entry.controller) { throw std::runtime_error("Native project or controller retired during save capture"); }
	}
	for (unsigned int index = 0; index < MaxScaleCount; ++index) { s.scales.push_back(song->getScale(index)); }
	for (unsigned int index = 0; index < MaxKeymapCount; ++index) { s.keymaps.push_back(song->getKeymap(index)); }
	for (int index = 0; index < s.mixer->numChannels(); ++index)
	{
		auto channel = s.mixer->mixerChannel(index);
		State::Channel entry{channel, &channel->m_fxChain, channel->m_name, channel->color(), channel->m_muteBeforeSolo, {}, {}};
		for (auto effect : channel->m_fxChain.effects()) { entry.effects.emplace_back(effect); }
		for (auto route : channel->m_sends) { entry.sends.emplace_back(route); }
		s.channels.push_back(std::move(entry));
	}
	for (auto route : s.mixer->m_mixerRoutes) { s.routes.push_back({route, route->sender(), route->receiver()}); }
	const auto& timeline = song->getTimeline(Song::PlayMode::Song);
	s.loopBegin = timeline.loopBegin(); s.loopEnd = timeline.loopEnd(); s.loop = timeline.loopEnabled(); s.stop = timeline.stopBehaviour();
	s.pattern = s.patterns->currentPattern(); s.solo = s.mixer->soloChannel();
	validate();
}
NativeProjectSaveSnapshot::~NativeProjectSaveSnapshot() = default;
void NativeProjectSaveSnapshot::validate() const
{
	const auto& s = *m_state;
	const auto refuse = [] { throw std::runtime_error("Native project source changed during serialization"); };
	if (!s.song || Engine::getSong() != s.song.data() || !s.patterns || Engine::patternStore() != s.patterns.data()
		|| !s.mixer || Engine::mixer() != s.mixer.data() || s.song->thread() != QThread::currentThread()
		|| !s.hidden || s.song->globalAutomationTrack() != s.hidden.data()) { refuse(); }
	for (const auto& container : s.containers)
	{
		if (!container.owner || container.owner->tracks().size() != container.tracks.size()) { refuse(); }
		for (std::size_t index = 0; index < container.tracks.size(); ++index)
		{ if (!container.tracks[index] || container.owner->tracks()[index] != container.tracks[index].data()) { refuse(); } }
	}
	for (const auto& track : s.tracks) { track->validate(); }
	const auto models = NativeLinkedControls::projectModels();
	if (static_cast<std::size_t>(models.size()) != s.models.size()) { refuse(); }
	for (std::size_t index = 0; index < s.models.size(); ++index)
	{ if (!s.models[index].model || models[index] != s.models[index].model.data() || models[index]->id() != s.models[index].id) { refuse(); } }
	for (const auto& group : s.groups)
	{ if (!group.root || !group.root->controllerStateMatches(*group.state, false, nullptr, false, false)) { refuse(); } }
	if (s.song->controllers().size() != s.controllers.size() || s.mixer->numChannels() != s.channels.size()
		|| s.mixer->m_mixerRoutes.size() != s.routes.size() || s.mixer->soloChannel() != s.solo) { refuse(); }
	for (std::size_t index = 0; index < s.controllers.size(); ++index)
	{
		const auto& entry = s.controllers[index];
		if (!entry.controller || s.song->controllers()[index] != entry.controller.data()) { refuse(); }
		const QString name = entry.controller->Controller::name();
		if (!s.song || !entry.controller || name != entry.name) { refuse(); }
	}
	for (unsigned int index = 0; index < s.scales.size(); ++index) { if (s.song->getScale(index) != s.scales[index]) { refuse(); } }
	for (unsigned int index = 0; index < s.keymaps.size(); ++index) { if (s.song->getKeymap(index) != s.keymaps[index]) { refuse(); } }
	for (std::size_t index = 0; index < s.channels.size(); ++index)
	{
		const auto& entry = s.channels[index];
		if (!entry.anchor || s.mixer->mixerChannel(index) != entry.identity) { refuse(); }
		const auto channel = entry.identity;
		if (channel->m_name != entry.name || channel->color() != entry.color || channel->m_muteBeforeSolo != entry.beforeSolo
			|| channel->m_fxChain.effects().size() != entry.effects.size() || channel->m_sends.size() != entry.sends.size()) { refuse(); }
		for (std::size_t row = 0; row < entry.effects.size(); ++row)
		{ if (!entry.effects[row] || channel->m_fxChain.effects()[row] != entry.effects[row].data()) { refuse(); } }
		for (std::size_t row = 0; row < entry.sends.size(); ++row)
		{ if (!entry.sends[row] || channel->m_sends[row] != entry.sends[row].data()) { refuse(); } }
	}
	for (std::size_t index = 0; index < s.routes.size(); ++index)
	{ const auto& route = s.routes[index]; if (!route.route || s.mixer->m_mixerRoutes[index] != route.route.data() || route.route->sender() != route.from || route.route->receiver() != route.to) { refuse(); } }
	const auto& timeline = s.song->getTimeline(Song::PlayMode::Song);
	if (timeline.loopBegin() != s.loopBegin || timeline.loopEnd() != s.loopEnd || timeline.loopEnabled() != s.loop
		|| timeline.stopBehaviour() != s.stop || s.patterns->currentPattern() != s.pattern) { refuse(); }
}
void NativeTrackSaveSnapshot::validate(bool complete) const
{
	const auto& state = *m_state; const auto track = state.track.data();
	if (!track || !state.owner || !state.song || Engine::getSong() != state.song.data() || track->thread() != QThread::currentThread()
		|| track->trackContainer() != state.owner.data())
	{ throw std::runtime_error("Native track serialization owner retired or detached"); }
	const bool hidden = track->type() == Track::Type::HiddenAutomation && state.owner.data() == state.song.data() && state.song->globalAutomationTrack() == track;
	if (!hidden && std::find(state.owner->tracks().begin(), state.owner->tracks().end(), track) == state.owner->tracks().end())
	{ throw std::runtime_error("Native track serialization receiver is no longer registered"); }
	const QString name = track->name();
	if (!state.track) { throw std::runtime_error("Native track retired during save inspection"); }
	if (const auto native = dynamic_cast<InstrumentTrack*>(track); native && native->isRetiring()) { throw std::runtime_error("Native instrument track is retiring during serialization"); }
	if (name != state.name || track->color() != state.color || track->getHeight() != state.height || track->isMutedBeforeSolo() != state.beforeSolo)
	{ throw std::runtime_error("Native track metadata changed during serialization"); }
	if (state.instrument.data() != state.instrumentIdentity || (dynamic_cast<InstrumentTrack*>(track) && static_cast<InstrumentTrack*>(track)->instrument() != state.instrumentIdentity)
		|| state.chain.data() != state.chainIdentity)
	{ throw std::runtime_error("Native instrument or effect chain changed during serialization"); }
	if (state.chain)
	{
		if (state.chain->isRetiring() || state.chain->effects().size() != state.effects.size()) { throw std::runtime_error("Native effect roster changed during track serialization"); }
		if (complete)
		{
			for (std::size_t index = 0; index < state.effects.size(); ++index)
			{ if (!state.effects[index] || state.chain->effects()[index] != state.effects[index].data()) { throw std::runtime_error("Native effect identity changed during track serialization"); } }
		}
	}
	if (state.includeClips)
	{
		if (track->getClips().size() != state.clips.size()) { throw std::runtime_error("Native clip roster changed during track serialization"); }
		if (complete)
		{
			for (std::size_t index = 0; index < state.clips.size(); ++index)
			{ if (!state.clips[index] || track->getClips()[index] != state.clips[index].data()) { throw std::runtime_error("Native clip identity changed during track serialization"); } }
		}
	}
	if (!complete) { return; }
	const auto models = NativeLinkedControls::trackModels(track, state.includeClips);
	if (static_cast<std::size_t>(models.size()) != state.models.size()) { throw std::runtime_error("Native parameter inventory changed during track serialization"); }
	for (std::size_t index = 0; index < state.models.size(); ++index)
	{ if (!state.models[index].model || models[index] != state.models[index].model.data() || models[index]->id() != state.models[index].id) { throw std::runtime_error("Native parameter identity changed during track serialization"); } }
	for (const auto& group : state.groups)
	{ if (!group.root || !group.root->controllerStateMatches(*group.state, false, nullptr, false, false)) { throw std::runtime_error("Native linked parameter state changed during serialization"); } }
}
}
