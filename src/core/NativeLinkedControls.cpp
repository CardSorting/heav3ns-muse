/* Native scoped linked-control serialization.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeLinkedControls.h"
#include <QHash>
#include <QMap>
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include <limits>
#include "AutomatableModel.h"
#include "AudioBusHandle.h"
#include "ControllerConnection.h"
#include "Controller.h"
#include "Clip.h"
#include "DetuningHelper.h"
#include "DataFile.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"

namespace lmms
{
namespace
{
thread_local NativeLinkedControls::SaveScope* saving = nullptr;
thread_local NativeLinkedControls::RestoreScope* restoring = nullptr;
const QString referenceKey = "native_link_ref";
const QString graphTag = "nativeLinkedControls";
bool check(QString& error) { return FileResourceIO::check(error); }
bool positive(const QString& text, int& value) { bool valid = false; value = text.toInt(&valid); return valid && value > 0; }
template<class Visitor> bool walk(const QDomDocument& document, Visitor visitor, QString& error)
{
	std::vector<QDomElement> pending{document.documentElement()};
	while (!pending.empty())
	{
		if (!check(error)) { return false; } auto element = pending.back(); pending.pop_back(); if (element.isNull()) { continue; }
		if (!visitor(element)) { return false; }
		for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) { pending.push_back(child); }
	}
	return true;
}
}
struct NativeLinkedControls::SaveScope::State
{
	QDomDocument document;
	std::vector<std::vector<QPointer<AutomatableModel>>> groups;
	QHash<const AutomatableModel*, QPointer<AutomatableModel>> expected;
	QHash<const AutomatableModel*, int> refs;
	QHash<int, std::vector<QDomElement>> elements;
	QHash<const AutomatableModel*, QDomElement> inlineOwners;
	QString error;
	bool portable = false;
};
struct NativeLinkedControls::RestoreScope::State
{
	QDomDocument document;
	QHash<int, QPointer<AutomatableModel>> refs;
	QHash<int, std::vector<QDomElement>> elements;
	QString error;
};

QList<AutomatableModel*> NativeLinkedControls::projectModels()
{
	QList<QObject*> roots{Engine::getSong(), Engine::patternStore(), Engine::mixer()};
	const auto chain = [&](EffectChain* effects) {
		roots.append(effects); roots.append(effects->enabledModel()); for (auto effect : effects->effects()) { roots.append(effect); }
	};
	for (auto owner : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		if (!owner) { continue; } for (auto track : owner->tracks())
		{
			for (auto clip : track->getClips()) { for (auto model : clipModels(clip)) { roots.append(model); } }
			if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { chain(instrument->audioBusHandle()->effects()); roots.append(instrument->instrument()); }
			else if (auto sample = dynamic_cast<SampleTrack*>(track)) { chain(sample->audioBusHandle()->effects()); }
		}
	}
	if (const auto mixer = Engine::mixer())
	{
		for (int index = 0; index < mixer->numChannels(); ++index)
		{ auto channel = mixer->mixerChannel(index); chain(&channel->m_fxChain); roots.append(&channel->m_volumeModel); roots.append(&channel->m_muteModel); roots.append(&channel->m_soloModel); }
		for (auto route : mixer->m_mixerRoutes) { roots.append(route->amount()); }
	}
	if (Engine::getSong()) { for (auto controller : Engine::getSong()->controllers()) { roots.append(controller); } }
	QList<AutomatableModel*> result; QSet<QObject*> visited; QSet<AutomatableModel*> seen;
	for (int index = 0; index < roots.size(); ++index)
	{
		auto root = roots.at(index); if (!root || visited.contains(root)) { continue; } visited.insert(root);
		auto models = root->findChildren<AutomatableModel*>(); if (auto model = dynamic_cast<AutomatableModel*>(root)) { models.prepend(model); }
		for (auto model : models)
		{
			if (seen.contains(model)) { continue; } seen.insert(model); result.append(model);
			if (auto connection = model->controllerConnection()) { roots.append(connection->getController()); }
		}
	}
	return result;
}

void NativeLinkedControls::unlinkProjectModels()
{ for (auto model : projectModels()) { if (model->isLinked()) { model->unlink(); } } }

QList<AutomatableModel*> NativeLinkedControls::trackModels(Track* track, bool includeClips)
{
	QList<QObject*> roots{track};
	if (includeClips) { for (auto clip : track->getClips()) { for (auto model : clipModels(clip)) { roots.append(model); } } }
	if (auto instrument = dynamic_cast<InstrumentTrack*>(track))
	{ roots.append(instrument->instrument()); roots.append(instrument->audioBusHandle()->effects()); roots.append(instrument->audioBusHandle()->effects()->enabledModel()); }
	else if (auto sample = dynamic_cast<SampleTrack*>(track))
	{ roots.append(sample->audioBusHandle()->effects()); roots.append(sample->audioBusHandle()->effects()->enabledModel()); }
	QList<AutomatableModel*> result; QSet<AutomatableModel*> seen; QSet<QObject*> visited;
	for (int index = 0; index < roots.size(); ++index)
	{
		auto root = roots.at(index); if (!root || visited.contains(root)) { continue; } visited.insert(root);
		auto models = root->findChildren<AutomatableModel*>(); if (auto model = dynamic_cast<AutomatableModel*>(root)) { models.prepend(model); }
		if (auto chain = dynamic_cast<EffectChain*>(root)) { for (auto effect : chain->effects()) { roots.append(effect); } }
		for (auto model : models)
		{
			bool clipOwned = false;
			for (auto parent = model->parent(); parent && parent != track; parent = parent->parent()) { if (dynamic_cast<Clip*>(parent)) { clipOwned = true; break; } }
			if (clipOwned && !includeClips) { continue; }
			if (seen.contains(model)) { continue; } seen.insert(model); result.append(model);
			const auto connection = model->controllerConnection(); const auto controller = connection ? connection->getController() : nullptr;
			if (controller && controller->type() == Controller::ControllerType::Midi) { roots.append(controller); }
		}
	}
	return result;
}

QList<AutomatableModel*> NativeLinkedControls::clipModels(Clip* clip)
{
	QList<QObject*> roots{clip}; QList<AutomatableModel*> result; QSet<AutomatableModel*> seen; QSet<QObject*> visited;
	if (const auto midi = dynamic_cast<MidiClip*>(clip))
	{
		for (auto note : midi->notes())
		{
			if (!note || !note->detuning()) { continue; } roots.append(note->detuning().get());
			if (const auto curve = note->detuning()->existingAutomationClip()) { roots.append(const_cast<AutomationClip*>(curve)); }
		}
	}
	for (int index = 0; index < roots.size(); ++index)
	{
		auto root = roots.at(index); if (!root || visited.contains(root)) { continue; } visited.insert(root);
		auto models = root->findChildren<AutomatableModel*>(); if (auto model = dynamic_cast<AutomatableModel*>(root)) { models.prepend(model); }
		for (auto model : models)
		{
			if (seen.contains(model)) { continue; } seen.insert(model); result.append(model);
			const auto connection = model->controllerConnection(); const auto controller = connection ? connection->getController() : nullptr;
			if (controller && controller->type() == Controller::ControllerType::Midi) { roots.append(controller); }
		}
	}
	return result;
}

QList<AutomatableModel*> NativeLinkedControls::noteModels(const Note* note)
{
	QList<AutomatableModel*> result; if (!note || !note->detuning()) { return result; }
	QList<QObject*> roots{note->detuning().get()}; if (const auto curve = note->detuning()->existingAutomationClip()) { roots.append(const_cast<AutomationClip*>(curve)); }
	QSet<QObject*> visited; QSet<AutomatableModel*> seen;
	for (int index = 0; index < roots.size(); ++index)
	{
		auto root = roots.at(index); if (!root || visited.contains(root)) { continue; } visited.insert(root);
		auto models = root->findChildren<AutomatableModel*>(); if (auto model = dynamic_cast<AutomatableModel*>(root)) { models.prepend(model); }
		for (auto model : models)
		{ if (seen.contains(model)) { continue; } seen.insert(model); result.append(model); const auto connection = model->controllerConnection(); const auto controller = connection ? connection->getController() : nullptr; if (controller && controller->type() == Controller::ControllerType::Midi) { roots.append(controller); } }
	}
	return result;
}

void NativeLinkedControls::unlinkInternalGroups(const QList<AutomatableModel*>& models)
{
	QSet<const AutomatableModel*> owned; for (auto model : models) { owned.insert(model); }
	QSet<AutomatableModel*> visited; std::vector<std::vector<AutomatableModel*>> groups;
	for (auto model : models)
	{
		if (!model->isLinked() || visited.contains(model)) { continue; } std::vector<AutomatableModel*> group; bool internal = true;
		for (auto peer = model; ; peer = const_cast<AutomatableModel*>(peer->nextLinkedModel()))
		{ group.push_back(peer); visited.insert(peer); internal = internal && owned.contains(peer); if (peer->nextLinkedModel() == model) { break; } }
		if (internal) { groups.push_back(std::move(group)); }
	}
	for (const auto& group : groups) { for (auto model : group) { model->unlink(); } }
}

bool NativeLinkedControls::isRestoring(const QDomDocument& document)
{ return restoring && restoring->m_state->document == document; }
bool NativeLinkedControls::isRestoring() { return restoring != nullptr; }

NativeLinkedControls::SaveScope::SaveScope(QDomDocument& document) : SaveScope(document, NativeLinkedControls::projectModels())
{ m_state->portable = false; }

NativeLinkedControls::SaveScope::SaveScope(QDomDocument& document, const QList<AutomatableModel*>& models) : m_state(std::make_unique<State>()), m_previous(saving)
{
	m_state->document = document; m_state->portable = true;
	for (auto model : models)
	{
		if (!model->isLinked() || m_state->expected.contains(model)) { continue; }
		std::vector<QPointer<AutomatableModel>> group;
		for (auto member = model; ; member = const_cast<AutomatableModel*>(member->nextLinkedModel()))
		{ group.emplace_back(member); m_state->expected.insert(member, member); if (member->nextLinkedModel() == model) { break; } }
		m_state->groups.push_back(std::move(group));
	}
	saving = this;
}
NativeLinkedControls::SaveScope::~SaveScope() { saving = m_previous; }
bool NativeLinkedControls::needsReference(const AutomatableModel* model, const QDomDocument& document)
{ return saving && saving->m_state->document == document && (saving->m_state->expected.contains(model) || model->isLinked()); }
void NativeLinkedControls::recordSaved(AutomatableModel* model, QDomDocument& document, QDomElement element)
{
	if (!needsReference(model, document)) { return; } auto& state = *saving->m_state;
	if (!state.expected.contains(model)) { state.error = "Native linked controls changed during serialization"; return; }
	int ref = state.refs.value(model);
	if (!ref)
	{
		if (state.refs.size() == std::numeric_limits<int>::max()) { state.error = "Too many native linked model references"; return; }
		ref = static_cast<int>(state.refs.size()) + 1; state.refs.insert(model, ref);
	}
	element.setAttribute(referenceKey, ref); state.elements[ref].push_back(element);
}

void NativeLinkedControls::recordInlineOwner(AutomatableModel* model, const QDomDocument& document, const QDomElement& owner)
{
	if (!saving || saving->m_state->document != document) { return; } auto& state = *saving->m_state; const auto previous = state.inlineOwners.value(model);
	if (previous.isNull()) { state.inlineOwners.insert(model, owner); return; }
	if (previous == owner) { return; }
	// A graph marker anywhere under either owner's native inline state also
	// covers linked owned-MIDI controls, not just the helper's scalar value.
	std::vector<QDomElement> pending{previous, owner};
	while (!pending.empty())
	{
		const auto element = pending.back(); pending.pop_back();
		if (element.hasAttribute(referenceKey)) { state.error = "A shared native inline helper cannot restore into multiple note receivers"; return; }
		for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) { pending.push_back(child); }
	}
}

bool NativeLinkedControls::SaveScope::finish(QDomElement parent, QString& error)
{
	FileResourceIO::EventSuspension events; error = m_state->error; if (!error.isEmpty()) { return false; }
	QMap<int, std::vector<int>> ordered;
	for (const auto& group : m_state->groups)
	{
		std::vector<int> refs;
		for (std::size_t index = 0; index < group.size(); ++index)
		{
			if (!check(error)) { return false; } const auto& model = group[index];
			if (!model || model->nextLinkedModel() != group[(index + 1) % group.size()].data()) { error = "Native linked controls changed during serialization"; return false; }
			const int ref = m_state->refs.value(model.data());
			if (!ref) { error = "A native linked control was not serialized; complete group membership is required"; return false; } refs.push_back(ref);
		}
		const auto first = std::min_element(refs.begin(), refs.end()); std::rotate(refs.begin(), first, refs.end()); ordered.insert(refs.front(), refs);
	}
	if (!walk(m_state->document, [&](const QDomElement& element) {
		if (!element.hasAttribute(referenceKey)) { return true; } int ref = 0;
		if (!positive(element.attribute(referenceKey), ref)) { error = "Invalid native linked control reference during serialization"; return false; }
		const auto nodes = m_state->elements.value(ref);
		if (std::find(nodes.begin(), nodes.end(), element) == nodes.end()) { error = "Unrecognized native linked control reference during serialization"; return false; } return true;
	}, error)) { return false; }
	if (ordered.isEmpty() && !m_state->portable) { error.clear(); return true; }
	auto graph = m_state->document.createElement(graphTag); graph.setAttribute("version", 1);
	for (const auto& refs : ordered)
	{
		auto group = m_state->document.createElement("group");
		for (int ref : refs) { auto member = m_state->document.createElement("member"); member.setAttribute("ref", ref); group.appendChild(member); } graph.appendChild(group);
	}
	parent.appendChild(graph); error.clear(); return true;
}

NativeLinkedControls::RestoreScope::RestoreScope(const QDomDocument& document) : m_state(std::make_unique<State>()), m_previous(restoring)
{ m_state->document = document; restoring = this; }
NativeLinkedControls::RestoreScope::~RestoreScope() { restoring = m_previous; }
void NativeLinkedControls::recordLoaded(AutomatableModel* model, const QDomElement& element)
{
	if (!restoring || restoring->m_state->document != element.ownerDocument() || !element.hasAttribute(referenceKey)) { return; }
	auto& state = *restoring->m_state; int ref = 0;
	if (!positive(element.attribute(referenceKey), ref)) { state.error = "Invalid native linked control reference"; return; }
	if (state.refs.contains(ref) && state.refs.value(ref).data() != model) { state.error = "Duplicate native linked control reference"; return; }
	model->unlink(); state.refs.insert(ref, model); state.elements[ref].push_back(element);
}

bool NativeLinkedControls::RestoreScope::finish(const QDomElement& parent, QString& error)
{
	FileResourceIO::EventSuspension events; error = m_state->error; if (!error.isEmpty()) { return false; }
	QDomElement graph;
	for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
	{
		if (child.tagName() != graphTag) { continue; }
		if (!graph.isNull()) { error = "Duplicate native linked control graph"; return false; } graph = child;
	}
	if (graph.isNull())
	{
		if (!m_state->refs.isEmpty()) { error = "Native linked control references have no graph"; return false; }
		return walk(m_state->document, [&](const QDomElement& element) {
			if (element.hasAttribute(referenceKey)) { error = "Native linked control references have no graph"; return false; } return true;
		}, error);
	}
	if (graph.attribute("version") != "1") { error = "Unsupported native linked control graph version"; return false; }
	QSet<int> used; std::vector<std::vector<QPointer<AutomatableModel>>> groups;
	for (auto node = graph.firstChildElement(); !node.isNull(); node = node.nextSiblingElement())
	{
		if (node.tagName() != "group") { error = "Invalid native linked control group"; return false; }
		std::vector<QPointer<AutomatableModel>> group;
		for (auto member = node.firstChildElement(); !member.isNull(); member = member.nextSiblingElement())
		{
			if (!check(error)) { return false; } int ref = 0;
			if (member.tagName() != "member" || !positive(member.attribute("ref"), ref) || used.contains(ref) || !m_state->refs.value(ref))
			{ error = "Missing, duplicate or invalid native linked control member"; return false; }
			used.insert(ref); group.push_back(m_state->refs.value(ref));
		}
		if (group.size() < 2) { error = "Native linked control groups require at least two members"; return false; } groups.push_back(std::move(group));
	}
	if (used.size() != m_state->refs.size()) { error = "Unclaimed native linked control references"; return false; }
	if (!walk(m_state->document, [&](const QDomElement& element) {
		if (!element.hasAttribute(referenceKey)) { return true; } int ref = 0;
		if (!positive(element.attribute(referenceKey), ref)) { error = "Invalid native linked control reference"; return false; }
		const auto nodes = m_state->elements.value(ref);
		if (std::find(nodes.begin(), nodes.end(), element) == nodes.end()) { error = "A native linked model reference was not restored by its serializer"; return false; } return true;
	}, error)) { return false; }
	return NativeLinkedControls::applyGroups(groups, error);
}

bool NativeLinkedControls::applyGroups(const std::vector<std::vector<QPointer<AutomatableModel>>>& groups, QString& error)
{
	struct Edge { QPointer<AutomatableModel> model; QPointer<AutomatableModel> next; };
	QHash<AutomatableModel*, Edge> previous;
	QSet<AutomatableModel*> selected;
	std::vector<std::vector<QPointer<Controller>>> controllers;
	for (const auto& group : groups)
	{
		if (group.empty()) { error = "Native linked control transition has an empty group"; return false; }
		for (const auto& model : group)
		{
			if (!model) { error = "Native linked control retired during restoration"; return false; }
			if (selected.contains(model.data())) { error = "Native linked control occurs in multiple transition positions"; return false; }
			selected.insert(model.data());
			if (previous.contains(model.data())) { continue; }
			for (auto peer = model.data(); ; peer = peer->m_nextLink)
			{
				if (previous.contains(peer)) { error = "Native linked control ring does not close at its receiver"; return false; }
				previous.insert(peer, {peer, peer->m_nextLink}); if (peer->m_nextLink == model.data()) { break; }
			}
		}
	}
	// Plan from immutable old edges. Each removed run is skipped only by its
	// remaining predecessor, so peers outside the proposed groups keep order.
	QHash<AutomatableModel*, AutomatableModel*> remaining;
	for (auto it = previous.cbegin(); it != previous.cend(); ++it)
	{
		auto next = it.value().next.data();
		if (selected.contains(it.key())) { remaining.insert(it.key(), it.key()); continue; }
		while (selected.contains(next)) { next = previous.value(next).next.data(); }
		remaining.insert(it.key(), next);
	}
	if (!check(error)) { return false; }
	for (auto it = previous.cbegin(); it != previous.cend(); ++it)
	{
		const auto& edge = it.value();
		if (!edge.model || !edge.next || edge.model->m_nextLink != edge.next.data())
		{ error = "Native linked control edges changed during transition admission"; return false; }
	}
	// Cancellation callbacks can rewire a binding without changing ring edges.
	// Gather the current controllers only after the final cooperative check.
	for (const auto& group : groups)
	{
		QSet<Controller*> seen; std::vector<QPointer<Controller>> bindings;
		for (const auto& model : group)
		{
			const auto connection = model->controllerConnection(); const auto controller = connection ? connection->getController() : nullptr;
			if (controller && !seen.contains(controller)) { seen.insert(controller); bindings.emplace_back(controller); }
		}
		controllers.push_back(std::move(bindings));
	}
	bool accepted = false; const auto rollback = qScopeGuard([&] {
		if (!accepted)
		{
			for (const auto& edge : previous) { if (edge.model) { edge.model->m_nextLink = edge.next ? edge.next.data() : edge.model.data(); } }
			AutomatableModel::invalidateLinkedTopology();
		}
	});
	if (!groups.empty()) { AutomatableModel::invalidateLinkedTopology(); }
	for (auto it = remaining.cbegin(); it != remaining.cend(); ++it) { it.key()->m_nextLink = it.value(); }
	for (const auto& group : groups) { for (std::size_t index = 0; index < group.size(); ++index) { group[index]->m_nextLink = group[(index + 1) % group.size()].data(); } }
	for (std::size_t index = 0; index < groups.size(); ++index)
	{
		for (const auto& controller : controllers[index])
		{
			if (!controller) { error = "Native linked controller retired during transition"; return false; }
			for (const auto& member : groups[index]) { if (controller->hasModel(member)) { error = "Native linked control graph creates controller feedback"; return false; } }
		}
	}
	for (auto it = previous.cbegin(); it != previous.cend(); ++it) { it.key()->m_lastUpdatedPeriod = -1; }
	accepted = true; error.clear(); return true;
}
}
