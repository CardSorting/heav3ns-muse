/*
 * EffectChain.cpp - class for processing and effects chain
 *
 * Copyright (c) 2006-2008 Danny McRae <khjklujn/at/users.sourceforge.net>
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "EffectChain.h"

#include <QDomElement>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <stdexcept>

#include "AudioBuffer.h"
#include "Effect.h"
#include "DummyEffect.h"

namespace lmms
{

class EffectChain::ChangeGuard
{
	QPointer<EffectChain> m_owner;
	bool m_previous;
public:
	explicit ChangeGuard(EffectChain* owner) : m_owner(owner), m_previous(owner->m_changing) { owner->m_changing = true; }
	~ChangeGuard() { if (m_owner && !m_owner->m_retiring) { m_owner->m_changing = m_previous; } }
};


EffectChain::EffectChain( Model * _parent ) :
	Model( _parent ),
	SerializingObject(),
	m_enabledModel( false, nullptr, tr( "Effects enabled" ) )
{
}




EffectChain::~EffectChain()
{
	m_retiring = true;
	m_lifecycleContext.reset(); m_lifecycleHooks.clear();
	m_changing = true;
	clearImpl();
}




void EffectChain::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if (QThread::currentThread() != thread() || m_retiring) { throw std::logic_error("Effect settings require a live chain on its owner thread"); }
	// Plugin serializers can call back into the model. Keep the ordered set
	// stable for the entire traversal, including saves from change observers.
	ChangeGuard changing(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<EffectChain> owner(this); std::vector<QPointer<Effect>> effects;
	for (auto effect : m_effects) { effects.emplace_back(effect); }
	const auto current = [&](std::size_t index) {
		if (!owner || owner->m_effects.size() != effects.size() || !effects[index] || owner->m_effects[index] != effects[index].data())
		{ throw std::runtime_error("Native effect chain changed during serialization"); }
	};
	m_enabledModel.saveSettings( _doc, _this, "enabled" );
	if (!owner) { throw std::runtime_error("Native effect chain retired during serialization"); }
	_this.setAttribute("numofeffects", static_cast<int>(effects.size()));

	for (std::size_t index = 0; index < effects.size(); ++index)
	{
		current(index); const auto effect = effects[index].data();
		if (auto dummy = dynamic_cast<DummyEffect*>(effect)) { _this.appendChild(_doc.importNode(dummy->originalPluginData(), true)); }
		else
		{
			QDomElement ef = effect->saveState( _doc, _this );
			current(index);
			ef.setAttribute( "name", QString::fromUtf8( effect->descriptor()->name ) );
			ef.appendChild( effect->key().saveXML( _doc ) );
		}
	}
	for (std::size_t index = 0; index < effects.size(); ++index) { current(index); }
}




void EffectChain::loadSettings( const QDomElement & _this )
{
	if (!canChange()) { throw std::logic_error("Effect settings cannot be replaced during an effect-chain transition or from another thread"); }
	ChangeGuard changing(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<EffectChain> owner(this);
	std::vector<QPointer<Effect>> restored;
	const auto ready = [&] {
		if (!owner || owner->m_retiring || owner->m_effects.size() != restored.size())
		{ throw std::runtime_error("Native effect chain retired or changed during settings replacement"); }
	};
	// Preserve native plugin/controller teardown-before-load ordering. An
	// unpublished candidate has local ownership if instantiation/restore fails.
	if (!clearImpl()) { throw std::runtime_error("Native effect chain could not complete settings teardown"); } ready();
	owner->m_enabledModel.loadSettings(_this, "enabled"); ready();

	const int plugin_cnt = _this.attribute( "numofeffects" ).toInt();

	QDomNode node = _this.firstChild();
	int fx_loaded = 0;
	while( !node.isNull() && fx_loaded < plugin_cnt )
	{
		if( node.isElement() && node.nodeName() == "effect" )
		{
			QDomElement effectData = node.toElement();

			const QString name = effectData.attribute( "name" );
			EffectKey key( effectData.elementsByTagName( "key" ).item( 0 ).toElement() );

			QPointer<Effect> e(Effect::instantiate(name.toUtf8(), owner.data(), &key)); bool published = false;
			const auto unattached = [&] {
				if (!e || (e->parent() && e->parent() != owner.data())
					|| (e->effectChain() && e->effectChain() != owner.data())) { return false; }
				return !owner || std::find(owner->m_effects.begin(), owner->m_effects.end(), e.data()) == owner->m_effects.end();
			};
			const auto cleanup = qScopeGuard([&] { if (!published && unattached()) { delete e.data(); } });
			ready();
			if (e && !unattached()) { throw std::runtime_error("Native restored effect ownership changed before restoration"); }

			if( e != nullptr && e->isOkay() && e->nodeName() == node.nodeName() )
			{
				ready(); if (!unattached()) { throw std::runtime_error("Native restored effect ownership changed before restoration"); }
				e->restoreState( effectData );
				ready(); if (!e) { throw std::runtime_error("Native effect retired during settings restoration"); }
			}
			else
			{
				ready(); if (e && !unattached()) { throw std::runtime_error("Native restored effect ownership changed before fallback"); }
				delete e.data(); ready(); e = new DummyEffect(owner.data(), effectData); ready();
			}

			if (!unattached()) { throw std::runtime_error("Native restored effect ownership changed before publication"); }
			owner->m_effects.push_back(e.data()); e->m_parent = owner.data(); published = true; restored.push_back(e);
			owner->watchEffect(e.data()); ready();
			++fx_loaded;
		}
		node = node.nextSibling();
	}

	emit owner->dataChanged(); ready();
	for (std::size_t index = 0; index < restored.size(); ++index)
	{ if (!restored[index] || owner->m_effects[index] != restored[index].data()) { throw std::runtime_error("Native restored effect identity changed during notification"); } }
}




bool EffectChain::canChange() const
{
	return QThread::currentThread() == thread() && !m_changing && !m_retiring;
}

void EffectChain::watchEffect(Effect* effect)
{
	const QPointer<EffectChain> owner(this);
	m_lifecycleHooks.insert(effect, QObject::connect(effect, &QObject::destroyed, m_lifecycleContext.get(), [owner, effect] {
		if (!owner || owner->m_retiring) { return; }
		owner->m_lifecycleHooks.remove(effect);
		auto& effects = owner->m_effects; const auto found = std::find(effects.begin(), effects.end(), effect);
		if (found == effects.end()) { return; }
		effects.erase(found);
		if (effects.empty()) { owner->m_enabledModel.setValue(false); }
		if (owner && !owner->m_changing) { emit owner->dataChanged(); }
	}, Qt::DirectConnection));
}

void EffectChain::unwatchEffect(Effect* effect)
{
	const auto found = m_lifecycleHooks.find(effect);
	if (found != m_lifecycleHooks.end()) { QObject::disconnect(found.value()); m_lifecycleHooks.erase(found); }
}

bool EffectChain::appendEffect(Effect* effect)
{
	std::unique_ptr<Effect> candidate(effect);
	try
	{
		const bool accepted = appendEffect(candidate);
		if (!accepted) { candidate.release(); }
		return accepted;
	}
	catch (...) { candidate.release(); throw; }
}

bool EffectChain::appendEffect(std::unique_ptr<Effect>& candidate)
{
	if (!canChange() || !candidate || candidate->thread() != thread()
		|| (candidate->effectChain() && candidate->effectChain() != this)
		|| (candidate->parent() && candidate->parent() != this)
		|| std::find(m_effects.begin(), m_effects.end(), candidate.get()) != m_effects.end()) { return false; }
	ChangeGuard changing(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<EffectChain> owner(this); const QPointer<Effect> target(candidate.get());
	m_effects.push_back(candidate.get());
	auto effect = candidate.release();
	effect->m_parent = this;
	watchEffect(effect);
	if (effect->parent() != this) { effect->setParent(this); }
	if (!owner || !target) { return false; }
	owner->m_enabledModel.setValue(true);
	if (!owner || !target) { return false; }
	emit owner->dataChanged();
	return owner && target && target->effectChain() == owner.data()
		&& std::find(owner->m_effects.begin(), owner->m_effects.end(), target.data()) != owner->m_effects.end();
}




std::unique_ptr<Effect> EffectChain::takeEffect(Effect* effect)
{
	return detachEffect(effect, true);
}

std::unique_ptr<Effect> EffectChain::detachEffect(Effect* effect, bool releaseParent)
{
	if (!canChange()) { return {}; }
	ChangeGuard changing(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto found = std::find(m_effects.begin(), m_effects.end(), effect);
	if (found == m_effects.end()) { return {}; }
	const QPointer<EffectChain> owner(this); const QPointer<Effect> target(effect);
	m_effects.erase(found);
	unwatchEffect(effect);
	effect->m_parent = nullptr;
	bool transferred = false;
	const auto cleanup = qScopeGuard([&] { if (!transferred && target && !target->effectChain()) { delete target.data(); } });
	if (releaseParent) { effect->setParent(nullptr); }
	if (!owner || !target || target->effectChain()) { return {}; }
	if (owner->m_effects.empty()) { owner->m_enabledModel.setValue(false); }
	if (!owner || !target || target->effectChain()) { return {}; }
	emit owner->dataChanged();
	if (!owner || !target || target->effectChain()) { return {}; }
	transferred = true; return std::unique_ptr<Effect>(target.data());
}

bool EffectChain::removeEffect(Effect* effect)
{
	// Legacy callers delete the detached effect themselves. Retain its QObject
	// parent until then, as before; the owned API explicitly releases that parent.
	auto detached = detachEffect(effect, false);
	return detached.release() != nullptr;
}




bool EffectChain::moveEffect(int from, int to)
{
	if (!canChange()) { return false; }
	ChangeGuard changing(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto count = static_cast<int>(m_effects.size());
	if (from < 0 || to < 0 || from >= count || to >= count) { return false; }
	if (from == to) { return true; }
	const QPointer<EffectChain> owner(this); const QPointer<Effect> target(m_effects[from]);
	if (from < to) { std::rotate(m_effects.begin() + from, m_effects.begin() + from + 1, m_effects.begin() + to + 1); }
	else { std::rotate(m_effects.begin() + to, m_effects.begin() + from, m_effects.begin() + from + 1); }
	emit dataChanged();
	return owner && target && to < static_cast<int>(owner->m_effects.size()) && owner->m_effects[to] == target.data();
}




bool EffectChain::moveDown(Effect* effect)
{
	if (!canChange()) { return false; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto found = std::find(m_effects.begin(), m_effects.end(), effect);
	if (found == m_effects.end()) { return false; }
	const int index = static_cast<int>(std::distance(m_effects.begin(), found));
	return moveEffect(index, std::min(index + 1, static_cast<int>(m_effects.size()) - 1));
}

bool EffectChain::moveUp(Effect* effect)
{
	if (!canChange()) { return false; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto found = std::find(m_effects.begin(), m_effects.end(), effect);
	if (found == m_effects.end()) { return false; }
	const int index = static_cast<int>(std::distance(m_effects.begin(), found));
	return moveEffect(index, std::max(index - 1, 0));
}




bool EffectChain::processAudioBuffer(AudioBuffer& buffer)
{
	if( m_enabledModel.value() == false )
	{
		return false;
	}

	bool moreEffects = false;
	for (Effect* effect : m_effects)
	{
		moreEffects |= effect->processAudioBuffer(buffer);
	}

	return moreEffects;
}




bool EffectChain::clear()
{
	if (!canChange()) { return false; }
	ChangeGuard changing(this); const QPointer<EffectChain> owner(this);
	if (!clearImpl() || !owner) { return false; }
	emit owner->dataChanged();
	return owner && owner->m_effects.empty();
}

bool EffectChain::clearImpl()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<EffectChain> owner(this); const bool previous = m_clearing; m_clearing = true;
	const auto restore = qScopeGuard([&] { if (owner) { owner->m_clearing = previous; } });
	if (!previous) { emit aboutToClear(); }
	if (!owner) { return false; }

	while (!owner->m_effects.empty())
	{
		auto e = owner->m_effects.back();
		owner->m_effects.pop_back(); owner->unwatchEffect(e);
		e->m_parent = nullptr;
		delete e;
		if (!owner) { return false; }
	}

	owner->m_enabledModel.setValue(false);
	return owner && owner->m_effects.empty();
}

} // namespace lmms
