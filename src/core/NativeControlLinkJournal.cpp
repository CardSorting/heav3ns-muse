/* Native user-edit history for linked control topology.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeControlLinkJournal.h"
#include <QHash>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <cmath>
#include <limits>
#include <vector>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NativeLinkedControls.h"
#include "ProjectJournal.h"
namespace lmms
{
namespace
{
QString number(float value) { return QString::number(value, 'g', std::numeric_limits<float>::max_digits10); }
}
struct NativeControlLinkJournal::Snapshot
{
	struct Entry
	{
		QPointer<AutomatableModel> model; jo_id_t id; std::uint64_t lifetime; std::size_t next;
		float value; float old; bool changed; float min; float max; float step; AutomatableModel::ScaleType scale; bool strict;
		QPointer<ControllerConnection> connection; QPointer<Controller> controller;
	};
	std::vector<Entry> entries;
};

NativeControlLinkJournal::Snapshot NativeControlLinkJournal::capture(const QList<AutomatableModel*>& models, QString& error)
{
	Snapshot state; QHash<const AutomatableModel*, std::size_t> indices;
	for (auto model : models)
	{
		if (!model || indices.contains(model) || model->m_restoringValueJournal || !std::isfinite(model->m_value)
			|| !std::isfinite(model->m_minValue) || !std::isfinite(model->m_maxValue) || !std::isfinite(model->m_step)
			|| Engine::projectJournal()->journallingObject(model->id()) != model)
		{ error = "Native link journal contains an invalid, duplicated or restoring model"; return {}; }
		indices.insert(model, state.entries.size()); const auto connection = model->controllerConnection();
		state.entries.push_back({model, model->id(), model->m_journalIdentity, 0, model->m_value, model->m_oldValue, model->m_valueChanged,
			model->m_minValue, model->m_maxValue, model->m_step, model->m_scaleType, model->m_hasStrictStepSize, connection, connection ? connection->getController() : nullptr});
	}
	for (auto& entry : state.entries)
	{
		if (!indices.contains(entry.model->m_nextLink)) { error = "Native link journal requires complete affected groups"; return {}; }
		entry.next = indices.value(entry.model->m_nextLink);
	}
	error.clear(); return state;
}

QDomElement NativeControlLinkJournal::write(const Snapshot& state, QDomDocument& document, const QString& tag)
{
	auto root = document.createElement(tag);
	for (const auto& entry : state.entries)
	{
		auto member = document.createElement("member"); member.setAttribute("id", entry.id); member.setAttribute("next", state.entries[entry.next].id);
		member.setAttribute("identity", QString::number(static_cast<qulonglong>(entry.lifetime))); member.setAttribute("value", number(entry.value));
		member.setAttribute("min", number(entry.min)); member.setAttribute("max", number(entry.max)); member.setAttribute("step", number(entry.step));
		member.setAttribute("scale", static_cast<int>(entry.scale)); member.setAttribute("strict", entry.strict ? 1 : 0); root.appendChild(member);
	}
	return root;
}

NativeControlLinkJournal::Snapshot NativeControlLinkJournal::read(const QDomElement& element, QString& error)
{
	Snapshot state; QHash<jo_id_t, std::size_t> indices; std::vector<jo_id_t> next;
	for (auto member = element.firstChildElement(); !member.isNull(); member = member.nextSiblingElement())
	{
		bool idOk = false, nextOk = false, lifeOk = false, valueOk = false;
		const auto id = member.attribute("id").toUInt(&idOk), nextId = member.attribute("next").toUInt(&nextOk);
		const auto life = member.attribute("identity").toULongLong(&lifeOk); const float value = member.attribute("value").toFloat(&valueOk);
		auto model = dynamic_cast<AutomatableModel*>(Engine::projectJournal()->journallingObject(id));
		if (member.tagName() != "member" || !member.firstChildElement().isNull() || !idOk || !id || !nextOk || !lifeOk || !valueOk
			|| !std::isfinite(value) || !model || indices.contains(id) || model->m_journalIdentity != life || model->m_restoringValueJournal
			|| member.attribute("min") != number(model->m_minValue) || member.attribute("max") != number(model->m_maxValue)
			|| member.attribute("step") != number(model->m_step) || member.attribute("scale") != QString::number(static_cast<int>(model->m_scaleType))
			|| member.attribute("strict") != QString::number(model->m_hasStrictStepSize ? 1 : 0) || model->fittedValue(value) != value)
		{ error = "Native link journal member identity, lifetime, value or fitting changed"; return {}; }
		const auto connection = model->controllerConnection(); indices.insert(id, state.entries.size()); next.push_back(nextId);
		state.entries.push_back({model, id, life, 0, value, model->m_oldValue, model->m_valueChanged, model->m_minValue, model->m_maxValue,
			model->m_step, model->m_scaleType, model->m_hasStrictStepSize, connection, connection ? connection->getController() : nullptr});
	}
	QSet<jo_id_t> incoming;
	for (std::size_t index = 0; index < next.size(); ++index)
	{
		if (!indices.contains(next[index]) || incoming.contains(next[index])) { error = "Native link journal edges do not form complete rings"; return {}; }
		incoming.insert(next[index]); state.entries[index].next = indices.value(next[index]);
	}
	if (state.entries.size() < 2) { error = "Native link journal requires at least two affected controls"; return {}; }
	error.clear(); return state;
}

bool NativeControlLinkJournal::matches(const Snapshot& state, bool values)
{
	for (const auto& entry : state.entries)
	{
		const auto model = entry.model.data();
		if (!model || model->id() != entry.id || Engine::projectJournal()->journallingObject(entry.id) != model
			|| model->m_journalIdentity != entry.lifetime || model->m_nextLink != state.entries[entry.next].model.data()
			|| model->m_minValue != entry.min || model->m_maxValue != entry.max || model->m_step != entry.step
			|| model->m_scaleType != entry.scale || model->m_hasStrictStepSize != entry.strict || (values && model->m_value != entry.value)) { return false; }
	}
	return true;
}

bool NativeControlLinkJournal::decode(AutomatableModel* model, const QDomElement& state, Snapshot& desired, Snapshot& expected, QString& error)
{
	bool idOk = false, lifeOk = false; const auto id = state.attribute("receiver").toUInt(&idOk); const auto life = state.attribute("identity").toULongLong(&lifeOk);
	if (!model || state.tagName() != "nativeControlLinkJournal" || state.attribute("version") != "1" || !idOk || id != model->id()
		|| !lifeOk || life != model->m_journalIdentity) { error = "Invalid native link journal receiver or version"; return false; }
	const auto first = state.firstChildElement(), second = first.nextSiblingElement();
	if (first.tagName() != "desired" || second.tagName() != "expected" || !second.nextSiblingElement().isNull()) { error = "Invalid native link journal sections"; return false; }
	desired = read(first, error); if (!error.isEmpty()) { return false; } expected = read(second, error); if (!error.isEmpty()) { return false; }
	if (desired.entries.size() != expected.entries.size() || desired.entries.front().model.data() != model) { error = "Native link journal affected controls changed"; return false; }
	for (std::size_t index = 0; index < desired.entries.size(); ++index)
	{ if (desired.entries[index].model != expected.entries[index].model) { error = "Native link journal affected controls differ"; return false; } }
	if (!matches(expected, false)) { error = "Native link journal expected topology no longer matches"; return false; }
	return true;
}

QDomElement NativeControlLinkJournal::counterpart(AutomatableModel* model, const QDomElement& state, QDomDocument& document, QDomElement parent, QString& error)
{
	const auto ownership = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	Snapshot desired, expected; if (!decode(model, state, desired, expected, error)) { return {}; }
	QList<AutomatableModel*> models; for (const auto& entry : desired.entries) { models.append(entry.model.data()); }
	const auto current = capture(models, error); if (!error.isEmpty()) { return {}; }
	auto result = document.createElement("nativeControlLinkJournal"); result.setAttribute("version", 1); result.setAttribute("receiver", model->id());
	result.setAttribute("identity", QString::number(static_cast<qulonglong>(model->m_journalIdentity)));
	result.appendChild(write(current, document, "desired")); result.appendChild(write(desired, document, "expected")); parent.appendChild(result); return result;
}

void NativeControlLinkJournal::rollback(const Snapshot& state) noexcept
{
	bool bindingsUnchanged = true;
	for (const auto& entry : state.entries)
	{
		if (!entry.model) { continue; } const auto connection = entry.model->controllerConnection();
		bindingsUnchanged = bindingsUnchanged && connection == entry.connection.data() && (!connection || connection->getController() == entry.controller.data());
		entry.model->unlink(); entry.model->m_value = entry.value; entry.model->m_oldValue = entry.old; entry.model->m_valueChanged = entry.changed;
		entry.model->m_lastUpdatedPeriod = -1;
	}
	if (bindingsUnchanged)
	{
		for (std::size_t index = 0; index < state.entries.size(); ++index)
		{
			if (!state.entries[index].model) { continue; } auto next = state.entries[index].next;
			while (!state.entries[next].model) { next = state.entries[next].next; }
			state.entries[index].model->m_nextLink = state.entries[next].model.data();
		}
	}
	AutomatableModel::invalidateLinkedTopology();
}

bool NativeControlLinkJournal::restore(AutomatableModel* model, const QDomElement& state, QString& error)
{
	const auto ownership = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	Snapshot desired, expected; if (!decode(model, state, desired, expected, error)) { return false; }
	QList<AutomatableModel*> models; for (const auto& entry : desired.entries) { models.append(entry.model.data()); }
	const auto previous = capture(models, error); if (!error.isEmpty()) { return false; }
	std::vector<std::vector<QPointer<AutomatableModel>>> groups; QSet<std::size_t> visited;
	for (std::size_t index = 0; index < desired.entries.size(); ++index)
	{
		if (visited.contains(index)) { continue; } std::vector<QPointer<AutomatableModel>> group; auto cursor = index;
		do { visited.insert(cursor); group.push_back(desired.entries[cursor].model); cursor = desired.entries[cursor].next; } while (cursor != index);
		groups.push_back(std::move(group));
	}
	if (!FileResourceIO::check(error)) { return false; }
	if (!matches(previous, true)) { error = "Native link journal controls changed during admission"; return false; }
	const auto journal = Engine::projectJournal(); const bool journalling = journal->isJournalling(); journal->setJournalling(false);
	bool accepted = false; const auto cleanup = qScopeGuard([&] {
		if (!accepted) { rollback(previous); }
		for (const auto& entry : previous.entries) { if (entry.model) { entry.model->m_restoringValueJournal = false; } }
		journal->setJournalling(journalling);
	});
	for (const auto& entry : previous.entries) { entry.model->m_restoringValueJournal = true; }
	if (!NativeLinkedControls::applyGroups(groups, error)) { return false; }
	for (std::size_t index = 0; index < desired.entries.size(); ++index)
	{
		const auto& entry = desired.entries[index]; entry.model->m_oldValue = previous.entries[index].value; entry.model->m_value = entry.value;
		if (entry.value != previous.entries[index].value) { entry.model->m_valueChanged = true; }
		entry.model->m_lastUpdatedPeriod = -1;
	}
	for (std::size_t index = 0; index < desired.entries.size(); ++index)
	{
		const auto& entry = desired.entries[index];
		if (!entry.model) { error = "Native link journal receiver retired during notification"; return false; }
		if (entry.value != previous.entries[index].value)
		{
			entry.model->notifyValueData();
			if (!matches(desired, true)) { error = "Native link journal changed during notification"; return false; }
		}
	}
	if (!matches(desired, true)) { error = "Native link journal changed before final feedback validation"; return false; }
	// Recheck native controller feedback after notifications can rewire bindings.
	if (!NativeLinkedControls::applyGroups(groups, error)) { return false; }
	accepted = true; error.clear(); return true;
}

bool NativeControlLinkJournal::edit(AutomatableModel* model, AutomatableModel* peer, bool joining, QString& error)
{
	if (!model || (joining && !peer)) { error = "Native link edit has no receiver"; return false; }
	const auto ownership = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	const bool changed = joining ? !model->isLinkedToModel(peer) : model->isLinked();
	if (!changed) { error.clear(); return true; }
	const auto journal = Engine::projectJournal();
	if (!journal->isJournalling() || !model->isJournalling())
	{ if (joining) { const bool accepted = model->linkToModel(peer); if (!accepted) { error = "Native linking rejected feedback or receiver lifetime"; } else { error.clear(); } return accepted; } model->unlink(); error.clear(); return true; }
	const QPointer<AutomatableModel> receiver(model); QList<AutomatableModel*> models{model}; QSet<AutomatableModel*> seen{model};
	const auto append = [&](AutomatableModel* root) {
		for (auto member = root; ; member = member->m_nextLink) { if (!seen.contains(member)) { seen.insert(member); models.append(member); } if (member->m_nextLink == root) { break; } }
	};
	append(model); if (joining) { append(peer); }
	const auto before = capture(models, error); if (!error.isEmpty()) { return false; }
	if (!FileResourceIO::check(error)) { return false; }
	if (!matches(before, true)) { error = "Native link edit changed during admission"; return false; }
	DataFile document(DataFile::Type::JournalData); auto state = document.createElement("nativeControlLinkJournal"); state.setAttribute("version", 1);
	state.setAttribute("receiver", model->id()); state.setAttribute("identity", QString::number(static_cast<qulonglong>(model->m_journalIdentity))); state.appendChild(write(before, document, "desired")); document.content().appendChild(state);
	{
		journal->setJournalling(false); const auto restoreJournalling = qScopeGuard([&] { journal->setJournalling(true); });
		if (joining) { if (!model->linkToModel(peer)) { error = "Native linking rejected feedback or receiver lifetime"; return false; } } else { model->unlink(); }
	}
	if (!receiver) { error = "Native link journal receiver retired during edit"; return false; }
	models.clear(); for (const auto& entry : before.entries) { if (!entry.model) { error = "Native link journal member retired during edit"; return false; } models.append(entry.model.data()); }
	const auto after = capture(models, error); if (!error.isEmpty()) { return false; } state.appendChild(write(after, document, "expected"));
	if (!journal->addPreparedJournalCheckPoint(receiver.data(), document)) { error = "Native link edit completed but its journal checkpoint could not be published"; return false; }
	error.clear(); return true;
}
bool NativeControlLinkJournal::link(AutomatableModel* model, AutomatableModel* peer, QString& error) { return edit(model, peer, true, error); }
bool NativeControlLinkJournal::unlink(AutomatableModel* model, QString& error) { return edit(model, nullptr, false, error); }
}
