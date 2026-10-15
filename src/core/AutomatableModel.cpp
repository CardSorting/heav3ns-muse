/*
 * AutomatableModel.cpp - some implementations of AutomatableModel-class
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

#include "AutomatableModel.h"
#include "NativeValueNotification.h"

#include <QRegularExpression>
#include <QCoreApplication>
#include <QThread>
#include <QScopeGuard>
#include <memory>
#include <atomic>
#include <limits>
#include <stdexcept>

#include "lmms_math.h"

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ControllerConnection.h"
#include "Controller.h"
#include "MidiController.h"
#include <QSet>
#include <QPointer>
#include <QHash>
#include "LocaleHelper.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeLinkedControls.h"
#include "FileResourceIO.h"

namespace lmms
{

namespace
{
// Test-peer installation is thread-local and normally null. Model instances
// carry no callback storage and the normal path copies no callable.
thread_local const std::function<void(AutomatableModel*, bool)>* valueNotificationProbe = nullptr;
std::uint64_t nextModelJournalIdentity()
{
	static std::atomic<std::uint64_t> next{0};
	auto value = next.load(std::memory_order_relaxed);
	for (;;)
	{
		if (value == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("Native model journal identity exhausted"); }
		if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) { return value + 1; }
	}
}
QString journalFloat(float value) { return QString::number(value, 'g', std::numeric_limits<float>::max_digits10); }
}

long AutomatableModel::s_periodCounter = 0;
std::uint64_t AutomatableModel::s_bufferGeneration = 0;
std::uint64_t AutomatableModel::s_linkGeneration = 0;



AutomatableModel::AutomatableModel(
						const float val, const float min, const float max, const float step,
						Model* parent, const QString & displayName, bool defaultConstructed ) :
	Model( parent, displayName, defaultConstructed ),
	m_scaleType( ScaleType::Linear ),
	m_minValue( min ),
	m_maxValue( max ),
	m_step( step ),
	m_range( max - min ),
	m_centerValue( m_minValue ),
	m_valueChanged( false ),
	m_oldValue(val),
	m_hasStrictStepSize( false ),
	m_nextLink(this),
	m_controllerConnection( nullptr ),
	m_valueBuffer( static_cast<int>( Engine::audioEngine()->framesPerPeriod() ) ),
	m_lastUpdatedPeriod( -1 ),
	m_bufferGeneration( s_bufferGeneration ),
	m_hasSampleExactData(false),
	m_useControllerValue(true),
	m_journalIdentity(nextModelJournalIdentity())

{
	m_value = fittedValue( val );
	setInitValue( val );
}




AutomatableModel::~AutomatableModel()
{
	unlink();

	if (const auto connection = controllerConnection())
	{
		delete connection;
	}

	m_valueBuffer.clear();

	emit destroyed( id() );
}




bool AutomatableModel::isAutomated() const
{
	return AutomationClip::isAutomated( this );
}

QDomElement AutomatableModel::saveJournalState(QDomDocument& document, QDomElement& parent)
{
	if (!isLinked()) { return JournallingObject::saveJournalState(document, parent); }
	if (!isJournalling()) { return {}; }
	const auto ownership = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	auto state = document.createElement("nativeLinkedValueState"); state.setAttribute("version", 1);
	state.setAttribute("receiver", id()); state.setAttribute("identity", QString::number(static_cast<qulonglong>(m_journalIdentity)));
	QSet<AutomatableModel*> seen;
	for (auto model = this; ; model = model->m_nextLink)
	{
		if (seen.contains(model) || !std::isfinite(model->m_value) || !std::isfinite(model->m_minValue)
			|| !std::isfinite(model->m_maxValue) || !std::isfinite(model->m_step) || model->m_restoringValueJournal
			|| Engine::projectJournal()->journallingObject(model->id()) != model) { return {}; }
		seen.insert(model);
		auto member = document.createElement("member"); member.setAttribute("id", model->id());
		member.setAttribute("identity", QString::number(static_cast<qulonglong>(model->m_journalIdentity))); member.setAttribute("value", journalFloat(model->m_value));
		member.setAttribute("min", journalFloat(model->m_minValue)); member.setAttribute("max", journalFloat(model->m_maxValue));
		member.setAttribute("step", journalFloat(model->m_step)); member.setAttribute("scale", static_cast<int>(model->m_scaleType));
		member.setAttribute("strict", model->m_hasStrictStepSize ? 1 : 0); state.appendChild(member);
		if (model->m_nextLink == this) { break; }
	}
	parent.appendChild(state); return state;
}

bool AutomatableModel::restoreJournalState(const QDomElement& state, QString& error)
{
	const auto reject = [&](const QString& detail) { error = "Native linked value journal: " + detail; return false; };
	if (state.tagName() != "nativeLinkedValueState")
	{
		if (isLinked()) { return reject("a formerly unlinked value now belongs to a linked group"); }
		return JournallingObject::restoreJournalState(state, error);
	}
	const auto ownership = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<AutomatableModel> receiver(this);
	bool receiverOk = false, identityOk = false;
	const auto receiverId = state.attribute("receiver").toUInt(&receiverOk);
	const auto receiverIdentity = state.attribute("identity").toULongLong(&identityOk);
	if (state.attribute("version") != "1" || !receiverOk || receiverId != id() || !identityOk || receiverIdentity != m_journalIdentity)
	{ return reject("invalid version or receiver lifetime"); }
	struct Entry
	{
		QPointer<AutomatableModel> model; jo_id_t id; float value; float previous; float previousOld; bool previousChanged;
		float min; float max; float step; ScaleType scale; bool strict;
	};
	std::vector<Entry> entries; QSet<AutomatableModel*> seen;
	auto model = this;
	for (auto member = state.firstChildElement(); !member.isNull(); member = member.nextSiblingElement())
	{
		bool idOk = false, lifeOk = false, valueOk = false;
		const auto memberId = member.attribute("id").toUInt(&idOk); const auto life = member.attribute("identity").toULongLong(&lifeOk);
		const float value = member.attribute("value").toFloat(&valueOk);
		if (member.tagName() != "member" || !member.firstChildElement().isNull() || !idOk || !lifeOk || !valueOk || !std::isfinite(value)
			|| seen.contains(model) || memberId != model->id() || life != model->m_journalIdentity
			|| Engine::projectJournal()->journallingObject(memberId) != model || model->m_restoringValueJournal)
		{ return reject("invalid member, changed group order or retired model lifetime"); }
		if (member.attribute("min") != journalFloat(model->m_minValue) || member.attribute("max") != journalFloat(model->m_maxValue)
			|| member.attribute("step") != journalFloat(model->m_step) || member.attribute("scale") != QString::number(static_cast<int>(model->m_scaleType))
			|| member.attribute("strict") != QString::number(model->m_hasStrictStepSize ? 1 : 0) || model->fittedValue(value) != value)
		{ return reject("native range, step, scale or value fitting changed"); }
		seen.insert(model); entries.push_back({model, model->id(), value, model->m_value, model->m_oldValue, model->m_valueChanged,
			model->m_minValue, model->m_maxValue, model->m_step, model->m_scaleType, model->m_hasStrictStepSize}); model = model->m_nextLink;
	}
	if (entries.size() < 2 || model != this) { return reject("incomplete or changed native linked group"); }
	const auto sameGroup = [&] {
		if (!receiver) { return false; }
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			const auto& entry = entries[index]; const auto member = entry.model.data();
			if (!member || member->id() != entry.id || Engine::projectJournal()->journallingObject(entry.id) != member
				|| member->m_nextLink != entries[(index + 1) % entries.size()].model.data() || member->m_minValue != entry.min
				|| member->m_maxValue != entry.max || member->m_step != entry.step || member->m_scaleType != entry.scale
				|| member->m_hasStrictStepSize != entry.strict) { return false; }
		}
		return true;
	};
	// Complete validation/allocation precedes the first native value change.
	if (!FileResourceIO::check(error)) { return false; }
	if (!sameGroup()) { return reject("linked membership changed during admission"); }
	for (const auto& entry : entries)
	{ if (entry.model->m_value != entry.previous || entry.model->m_oldValue != entry.previousOld || entry.model->m_valueChanged != entry.previousChanged) { return reject("values changed during admission"); } }
	bool accepted = false;
	const auto restore = qScopeGuard([&] {
		for (const auto& entry : entries)
		{
			if (!entry.model) { continue; } entry.model->m_restoringValueJournal = false;
			if (!accepted)
			{ entry.model->m_value = entry.previous; entry.model->m_oldValue = entry.previousOld; entry.model->m_valueChanged = entry.previousChanged; entry.model->m_lastUpdatedPeriod = -1; }
		}
	});
	for (const auto& entry : entries)
	{
		entry.model->m_restoringValueJournal = true;
		entry.model->m_oldValue = entry.previous; entry.model->m_value = entry.value;
		if (entry.previous != entry.value) { entry.model->m_valueChanged = true; }
		entry.model->m_lastUpdatedPeriod = -1;
	}
	// Publish the complete raw-value vector before notifying any peer. Calling
	// setValue here would propagate each peer's fitted value over the others.
	auto generation = s_linkGeneration;
	for (const auto& entry : entries)
	{
		if (!entry.model) { return reject("a receiver retired during notification"); }
		if (entry.previous != entry.value) { entry.model->notifyValueData(); }
		if (!receiver || (generation != s_linkGeneration && !sameGroup())) { return reject("linked membership changed during notification"); }
		generation = s_linkGeneration;
	}
	if (!sameGroup()) { return reject("native group or fitting changed during notification"); }
	for (const auto& entry : entries)
	{ if (!entry.model || entry.model->m_value != entry.value) { return reject("a callback changed the restored values"); } }
	accepted = true; error.clear(); return true;
}



bool AutomatableModel::mustQuoteName(const QString& name)
{
	QRegularExpression reg("^[A-Za-z0-9._-]+$");
	return !reg.match(name).hasMatch();
}

void AutomatableModel::saveSettings( QDomDocument& doc, QDomElement& element, const QString& name )
{
	bool mustQuote = mustQuoteName(name);

	if (AutomationClip::isConnected(this) || m_scaleType != ScaleType::Linear || NativeLinkedControls::needsReference(this, doc))
	{
		// automation needs tuple of data (name, id, value)
		// scale type also needs an extra value
		// => it must be appended as a node

		QDomElement me = doc.createElement( mustQuote ? QString("automatablemodel") : name );
		me.setAttribute( "id", ProjectJournal::idToSave( id() ) );
		me.setAttribute( "value", m_value );
		me.setAttribute( "scale_type", m_scaleType == ScaleType::Logarithmic ? "log" : "linear" );
		NativeLinkedControls::recordSaved(this, doc, me);
		if(mustQuote) {
			me.setAttribute( "nodename", name );
		}
		element.appendChild( me );
	}
	else
	{
		if(mustQuote)
		{
			QDomElement me = doc.createElement( "automatablemodel" );
			me.setAttribute( "nodename", name );
			me.setAttribute( "value", m_value );
			element.appendChild( me );
		}
		else
		{
			// non automation, linear scale (default), can be saved as attribute
			element.setAttribute( name, m_value );
		}
	}

	// Skip saving MIDI connections if we're saving project and
	// the discardMIDIConnections option is true.
	const QPointer<ControllerConnection> binding(controllerConnection());
	auto controllerType = binding && binding->getController()
			? binding->getController()->type()
			: Controller::ControllerType::Dummy;
	const bool skipMidiController = Engine::getSong()->shouldDiscardMidiConnections();
	if (binding && controllerType != Controller::ControllerType::Dummy
		&& !(skipMidiController && controllerType == Controller::ControllerType::Midi))
	{
		QDomElement controllerElement;

		// get "connection" element (and create it if needed)
		QDomNode node = element.namedItem( "connection" );
		if( node.isElement() )
		{
			controllerElement = node.toElement();
		}
		else
		{
			controllerElement = doc.createElement( "connection" );
			element.appendChild( controllerElement );
		}

		bool mustQuote = mustQuoteName(name);
		QString elementName = mustQuote ? "controllerconnection"
						: name;

		QDomElement element = doc.createElement( elementName );
		if(mustQuote)
			element.setAttribute( "nodename", name );
		binding->saveSettings( doc, element );

		controllerElement.appendChild( element );
	}
}




void AutomatableModel::loadSettings( const QDomElement& element, const QString& name )
{
	const QPointer<AutomatableModel> receiver(this);
	// compat code
	QDomNode node = element.namedItem( AutomationClip::classNodeName() );
	if( node.isElement() )
	{
		node = node.namedItem( name );
		if( node.isElement() )
		{
			QPointer<AutomationClip> p = AutomationClip::globalAutomationClip(this);
			p->loadSettings( node.toElement() );
			if (!receiver || !p) { return; }
			setValue( p->valueAt( 0 ) );
			if (!receiver || !p) { return; }
			// in older projects we sometimes have odd automations
			// with just one value in - eliminate if necessary
			if( !p->hasAutomation() )
			{
				delete p.data();
			}
			return;
		}
		// logscales were not existing at this point of time
		// so they can be ignored
	}

	QDomNode connectionNode = element.namedItem( "connection" );
	// reads controller connection
	if( connectionNode.isElement() )
	{
		QDomNode thisConnection = connectionNode.toElement().namedItem( name );
		if( !thisConnection.isElement() )
		{
			thisConnection = connectionNode.toElement().namedItem( "controllerconnection" );
			QDomElement tcElement = thisConnection.toElement();
			// sanity check
			if( tcElement.isNull() || tcElement.attribute( "nodename" ) != name )
			{
				// no, that wasn't it, act as if we never found one
				thisConnection.clear();
			}
		}
		if( thisConnection.isElement() )
		{
			auto replacement = std::make_unique<ControllerConnection>(nullptr);
			replacement->loadSettings(thisConnection.toElement());
			if (!receiver) { return; }
			{
				const QPointer<ControllerConnection> previous(m_controllerConnection);
				const auto releasePrevious = qScopeGuard([previous] { if (previous && !previous->isRetiring()) { delete previous.data(); } });
				setControllerConnection(replacement.release());
			}
			if (!receiver) { return; }
			//m_controllerConnection->setTargetName( displayName() );
		}
	}

	// models can be stored as elements (port00) or attributes (port10):
	// <ladspacontrols port10="4.41">
	//   <port00 value="4.41" id="4249278"/>
	// </ladspacontrols>
	// element => there is automation data, or scaletype information

	node = element.namedItem( name ); // maybe we have luck?

	// either: no node with name "name" found
	//  => look for nodes with attribute name="nodename"
	// or: element with namedItem() "name" was found, but it's real nodename
	// is given as attribute and does not match
	//  => look for the right node
	if(node.isNull() ||
		( node.isElement() &&
		node.toElement().hasAttribute("nodename") &&
		node.toElement().attribute("nodename") != name))
	{
		for(QDomElement othernode = element.firstChildElement();
			!othernode.isNull();
			othernode = othernode.nextSiblingElement())
		{
			if((!othernode.hasAttribute("nodename") &&
				othernode.nodeName() == name) ||
				othernode.attribute("nodename") == name)
			{
				node = othernode;
				break;
			}
		}
	}
	if( node.isElement() )
	{
		QDomElement nodeElement = node.toElement();
		NativeLinkedControls::recordLoaded(this, nodeElement);
		changeID( nodeElement.attribute( "id" ).toInt() );
		setValue( LocaleHelper::toFloat( nodeElement.attribute( "value" ) ) );
		if (!receiver) { return; }
		if( nodeElement.hasAttribute( "scale_type" ) )
		{
			if( nodeElement.attribute( "scale_type" ) == "linear" )
			{
				setScaleType( ScaleType::Linear );
			}
			else if( nodeElement.attribute( "scale_type" ) == "log" )
			{
				setScaleType( ScaleType::Logarithmic );
			}
		}
	}
	else
	{

		setScaleType( ScaleType::Linear );

		if( element.hasAttribute( name ) )
			// attribute => read the element's value from the attribute list
		{
			setInitValue( LocaleHelper::toFloat( element.attribute( name ) ) );
		}
		else
		{
			reset();
		}
	}
}




void AutomatableModel::setValue(const float value, const bool isAutomated)
{
	if (fittedValue(value) == m_value)
	{
		// TODO check why we need this signal, is there a better solution?
		if (!isAutomated) { notifyValueData(false); }
		return;
	}

	const QPointer<AutomatableModel> receiver(this);
	if (!isAutomated) { addJournalCheckPoint(); }
	if (!receiver) { return; }
	setValueInternal(value);
	if (!receiver || m_nextLink == this) { return; }

	// Callbacks can detach, retire or join controls. Preserve native traversal
	// of the current ring, but never dereference the previous sender afterward.
	// Weak visited identities also distinguish another object at a reused address.
	QHash<AutomatableModel*, QPointer<AutomatableModel>> visited; visited.insert(this, receiver);
	const auto unvisited = [&]() -> QPointer<AutomatableModel> {
		for (auto peer = m_nextLink; peer != this; peer = peer->m_nextLink)
		{ if (visited.value(peer).data() != peer) { return peer; } }
		return nullptr;
	};
	QPointer<AutomatableModel> model(m_nextLink);
	while (model && model != receiver)
	{
		if (visited.value(model.data()).data() == model.data()) { model = unvisited(); continue; }
		visited.insert(model.data(), model); const auto generation = s_linkGeneration;
		const QPointer<AutomatableModel> next(model->m_nextLink);
		model->setValueInternal(value);
		if (!receiver) { return; }
		model = generation == s_linkGeneration ? next : unvisited();
	}
}




const AutomatableModel::ValueNotificationProbe* AutomatableModel::exchangeValueNotificationProbe(const ValueNotificationProbe* probe)
{
	const auto previous = valueNotificationProbe; valueNotificationProbe = probe; return previous;
}

void AutomatableModel::notifyValueData(bool changed)
{
	NativeValueNotification notification;
	if (!valueNotificationProbe)
	{
		if (changed) { emit dataChanged(); } else { emit dataUnchanged(); }
		notification.rethrow();
		return;
	}
	const auto probe = *valueNotificationProbe;
	const QPointer<AutomatableModel> receiver(this);
	const bool blocked = signalsBlocked();
	if (changed) { emit dataChanged(); } else { emit dataUnchanged(); }
	notification.rethrow();
	// The test fault is an ordinary call after Qt delivery. A real Qt observer
	// may retire this model; no checkpoint runs on that retired identity.
	if (receiver && !blocked) { probe(receiver.data(), changed); }
}

void AutomatableModel::setValueInternal(const float value)
{
	m_oldValue = m_value;
	m_value = fittedValue(value);

	if (m_oldValue != m_value)
	{
		m_valueChanged = true;
		notifyValueData();
	}
}



template<class T> T AutomatableModel::logToLinearScale( T value ) const
{
	return castValue<T>( lmms::logToLinearScale( minValue<float>(), maxValue<float>(), static_cast<float>( value ) ) );
}


float AutomatableModel::scaledValue( float value ) const
{
	return m_scaleType == ScaleType::Linear
		? value
		: logToLinearScale<float>( ( value - minValue<float>() ) / m_range );
}


float AutomatableModel::inverseScaledValue( float value ) const
{
	return m_scaleType == ScaleType::Linear
		? value
		: lmms::linearToLogScale( minValue<float>(), maxValue<float>(), value );
}




template<class T>
void AutomatableModel::roundAt( T& value, const T& where ) const
{
	lmms::roundAt(value, where, m_step);
}








void AutomatableModel::setRange( const float min, const float max,
							const float step )
{
	if( ( m_maxValue != max ) || ( m_minValue != min ) )
	{
		const QPointer<AutomatableModel> receiver(this);
		m_minValue = min;
		m_maxValue = max;
		if( m_minValue > m_maxValue )
		{
			qSwap<float>( m_minValue, m_maxValue );
		}
		m_range = m_maxValue - m_minValue;

		setStep( step );
		if (!receiver) { return; }

		// re-adjust value
		setValue( value<float>() );
		if (!receiver) { return; }

		emit propertiesChanged();
	}
}




void AutomatableModel::setStep( const float step )
{
	if( m_step != step )
	{
		m_step = step;
		emit propertiesChanged();
	}
}




float AutomatableModel::fittedValue(float value) const
{
	value = std::clamp(value, m_minValue, m_maxValue);

	if (m_step != 0 && m_hasStrictStepSize)
	{
		value = std::round(value / m_step) * m_step;
	}

	roundAt(value, m_maxValue);
	roundAt(value, m_minValue);
	roundAt(value, 0.0f);
	return std::clamp(value, m_minValue, m_maxValue);
}




AutomatableModel* AutomatableModel::getLastLinkedModel() const
{
	for (auto model = m_nextLink; ; model = model->m_nextLink)
	{
		// The last model in the circular reference links back to this
		if (model->m_nextLink == this) { return model; }
	}
}




bool AutomatableModel::isLinkedToModel(AutomatableModel* model) const
{
	if (model == this) { return true; }
	for (auto next = m_nextLink; next != this; next = next->m_nextLink)
	{
		if (next == model) { return true; }
	}
	return false;
}

size_t AutomatableModel::countLinks() const
{
	size_t output = 0;
	for (auto model = m_nextLink; model != this; model = model->m_nextLink)
	{
		output++;
	}
	return output;
}




bool AutomatableModel::linkToModel(AutomatableModel* other)
{
	if (!other) { return false; }
	if (isLinkedToModel(other)) { return true; }
	// A newly merged group can make a controller depend on itself, even if
	// every pre-existing binding was valid. Check before changing either ring.
	const auto safe = [this, other] {
		auto members = linkedModels();
		const auto others = other->linkedModels(); members.insert(members.end(), others.begin(), others.end());
		QSet<const Controller*> checked;
		for (const auto member : members)
		{
			const auto connection = member->controllerConnection();
			const auto controller = connection ? connection->getController() : nullptr;
			if (!controller || checked.contains(controller)) { continue; }
			checked.insert(controller);
			for (const auto candidate : members) { if (controller->hasModel(candidate)) { return false; } }
		}
		return true;
	};
	if (!safe()) { return false; }
	// copy data from other to this
	const QPointer<AutomatableModel> target(this), source(other);
	setValue(other->m_value);
	if (!target || !source) { return false; }
	if (isLinkedToModel(other)) { return true; }
	if (!safe()) { return false; }
	// link the models
	AutomatableModel* thisEnd = getLastLinkedModel();
	AutomatableModel* otherEnd = other->getLastLinkedModel();
	thisEnd->m_nextLink = other;
	otherEnd->m_nextLink = this;
	invalidateLinkedTopology();
	invalidateLinkedValueBuffers();
	return true;
}




void AutomatableModel::unlink()
{
	auto remaining = m_nextLink;
	getLastLinkedModel()->m_nextLink = m_nextLink;
	m_nextLink = this;
	invalidateLinkedTopology();
	invalidateLinkedValueBuffers();
	if (remaining != this) { remaining->invalidateLinkedValueBuffers(); }
}

void AutomatableModel::invalidateLinkedValueBuffers() noexcept
{
	auto model = this;
	do { model->m_lastUpdatedPeriod = -1; model = model->m_nextLink; } while (model != this);
}




ControllerConnection* AutomatableModel::controllerConnection() const
{
	const auto connection = m_controllerConnection.data();
	return connection && !connection->isRetiring() ? connection : nullptr;
}

void AutomatableModel::setControllerConnection( ControllerConnection* c )
{
	if (c && c->isRetiring()) { return; }
	if (m_controllerConnection == c) { return; }
	if (m_controllerConnectionGeneration == std::numeric_limits<std::uint64_t>::max())
	{ throw std::overflow_error("Native controller binding generation exhausted"); }
	const QPointer<AutomatableModel> receiver(this);
	const QPointer<ControllerConnection> replacement(c);
	const auto previous = m_controllerConnection;
	const auto previousGeneration = m_controllerConnectionGeneration;
	if (previous && !previous->isRetiring()) { previous->disconnect(this); }
	if (!receiver || receiver->m_controllerConnection != previous || receiver->m_controllerConnectionGeneration != previousGeneration) { return; }
	const auto generation = ++receiver->m_controllerConnectionGeneration;
	receiver->m_controllerConnection = replacement.data();
	if (replacement)
	{
		QObject::connect(replacement.data(), &ControllerConnection::retiring, receiver.data(), [this, c, generation] {
			if (m_controllerConnectionGeneration == generation && m_controllerConnection == c) { unlinkControllerConnection(); }
		}, Qt::AutoConnection);
		if (!receiver) { return; }
		if (!replacement || replacement->isRetiring() || receiver->m_controllerConnection != c || receiver->m_controllerConnectionGeneration != generation) { return; }
		// Install retirement handling first. The generation prevents a deferred
		// destruction callback from clearing a later binding at a reused address.
		QObject::connect(replacement.data(), &QObject::destroyed, receiver.data(), [this, c, generation] {
			if (m_controllerConnectionGeneration == generation && (!m_controllerConnection || m_controllerConnection == c)) { unlinkControllerConnection(); }
		});
		if (!receiver) { return; }
		if (!replacement || receiver->m_controllerConnection != c || receiver->m_controllerConnectionGeneration != generation)
		{
			if (!replacement && receiver->m_controllerConnectionGeneration == generation) { receiver->unlinkControllerConnection(); }
			return;
		}
		QObject::connect(replacement.data(), SIGNAL(valueChanged()), receiver.data(), SIGNAL(dataChanged()), Qt::DirectConnection);
		if (!receiver) { return; }
		if (!replacement || receiver->m_controllerConnection != c || receiver->m_controllerConnectionGeneration != generation)
		{
			if (!replacement && receiver->m_controllerConnectionGeneration == generation) { receiver->unlinkControllerConnection(); }
			return;
		}
	}
	receiver->m_valueChanged = true;
	receiver->invalidateLinkedValueBuffers();
	receiver->notifyValueData();
}

struct AutomatableModel::ControllerState
{
	struct Entry
	{
		QPointer<AutomatableModel> model;
		QPointer<ControllerConnection> connection; ControllerConnection* identity;
		QPointer<Controller> controller; Controller* controllerIdentity;
		float value, initial, oldValue, minimum, maximum, step;
		ScaleType scale; bool strict, useController;
	};
	std::vector<Entry> entries;
};

std::shared_ptr<AutomatableModel::ControllerState> AutomatableModel::captureControllerState() const
{
	auto source = std::make_shared<ControllerState>();
	for (const auto member : linkedModels())
	{
		const auto binding = member->controllerConnection(); const auto controller = binding ? binding->getController() : nullptr;
		if (member->thread() != QThread::currentThread() || (binding && binding->thread() != QThread::currentThread())
			|| (controller && controller->thread() != QThread::currentThread()))
		{ throw std::invalid_argument("Native linked controller state must belong to the model owner thread"); }
		if (!std::isfinite(member->m_value) || !std::isfinite(member->m_initValue) || !std::isfinite(member->m_oldValue)
			|| !std::isfinite(member->m_minValue) || !std::isfinite(member->m_maxValue) || !std::isfinite(member->m_step))
		{ throw std::invalid_argument("Native controller editing requires finite linked parameter state"); }
		source->entries.push_back({const_cast<AutomatableModel*>(member), binding, binding, controller, controller,
			member->m_value, member->m_initValue, member->m_oldValue, member->m_minValue, member->m_maxValue, member->m_step,
			member->m_scaleType, member->m_hasStrictStepSize, member->m_useControllerValue});
	}
	return source;
}

bool AutomatableModel::controllerStateMatches(const ControllerState& source, bool bindingChanged,
	ControllerConnection* replacement, bool activate, bool compareInterpolation) const
{
	const auto& entries = source.entries;
	if (entries.empty() || entries.front().model.data() != this) { return false; }
	for (std::size_t index = 0; index < entries.size(); ++index)
	{
		const auto& entry = entries[index]; const auto model = entry.model.data(); const bool edited = bindingChanged && index == 0;
		if (!model || model->thread() != QThread::currentThread() || model->m_nextLink != entries[(index + 1) % entries.size()].model.data()
			|| model->m_value != entry.value || model->m_initValue != entry.initial || (compareInterpolation && model->m_oldValue != entry.oldValue)
			|| model->m_minValue != entry.minimum || model->m_maxValue != entry.maximum || model->m_step != entry.step
			|| model->m_scaleType != entry.scale || model->m_hasStrictStepSize != entry.strict
			|| model->m_useControllerValue != (edited && activate ? true : entry.useController)
			|| model->controllerConnection() != (edited ? replacement : entry.identity)
			|| (!edited && (entry.connection.data() != entry.identity || entry.controller.data() != entry.controllerIdentity
				|| (entry.connection && (entry.connection->thread() != QThread::currentThread() || entry.connection->getController() != entry.controllerIdentity))
				|| (entry.controller && entry.controller->thread() != QThread::currentThread()))))
		{ return false; }
	}
	return true;
}

bool AutomatableModel::disconnectController(bool& changed, QString& error)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject())
		{ throw std::invalid_argument("Native controller disconnection requires a ready project on the model owner thread"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const QPointer<AutomatableModel> receiver(this); const QPointer<ControllerConnection> previous(m_controllerConnection);
		const auto source = captureControllerState();
		const auto valid = [&](bool detached) {
			if (!receiver) { throw std::runtime_error("Native parameter retired during controller disconnection"); }
			if (!receiver->controllerStateMatches(*source, detached))
			{ throw std::runtime_error("Native linked parameter or binding changed during controller disconnection"); }
		};
		QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } valid(false);
		if (!previous) { error.clear(); return true; }
		const auto retire = [&] { if (previous && !previous->isRetiring() && (!receiver || receiver->m_controllerConnection != previous.data())) { delete previous.data(); } };
		// Publish the final notification only after connection-owned resources
		// have retired. A weak guard restores an existing signal block safely.
		const bool blocked = receiver->blockSignals(true);
		const auto restoreSignals = qScopeGuard([&] { if (receiver) { receiver->blockSignals(blocked); } });
		const auto cleanup = qScopeGuard(retire);
		receiver->setControllerConnection(nullptr); valid(true);
		retire(); valid(true);
		receiver->blockSignals(blocked); receiver->notifyValueData(); valid(true);
		changed = true; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native controller disconnection failed with a nonstandard exception"; }
	return false;
}




bool AutomatableModel::connectMidiController(int channel, int control, bool readable, bool& changed, QString& error)
{
	return assignMidiController(channel, control, readable, changed, error, nullptr, MidiController::NONE, {}, {});
}

bool AutomatableModel::assignMidiController(int channel, int control, bool readable, bool& changed, QString& error,
	const QStringList* learnedInputs, int learnedValue, const std::function<void(bool)>& validate,
	const std::function<void()>& beforePublish)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject())
		{ throw std::invalid_argument("Native MIDI assignment requires a ready project on the model owner thread"); }
		if (channel < 0 || channel > MidiChannelCount || control < 0 || control >= MidiControllerCount)
		{ throw std::invalid_argument("Invalid native MIDI assignment channel or controller"); }
		if (learnedValue < MidiController::NONE || learnedValue > MidiMaxControllerValue)
		{ throw std::invalid_argument("Invalid native learned MIDI controller value"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const auto engine = Engine::audioEngine(); const auto client = engine->midiClient(); const QPointer<Song> song(Engine::getSong());
		if (!client || engine->isChangingMidiBackend()) { throw std::invalid_argument("Native MIDI backend is unavailable or changing"); }
		const QPointer<AutomatableModel> receiver(this); const auto source = captureControllerState();
		const QPointer<ControllerConnection> previous(controllerConnection());
		bool published = false;
		const auto context = [&] {
			if (validate) { validate(published); }
			if (!receiver || receiver->thread() != QThread::currentThread() || !song || Engine::getSong() != song.data() || Engine::audioEngine() != engine
				|| engine->midiClient() != client || engine->isChangingMidiBackend() || song->isLoadingProject())
			{ throw std::runtime_error("Native MIDI assignment owner or backend changed"); }
			if (!receiver->supportsAutomation()) { throw std::invalid_argument("Native parameter does not support controller input"); }
			if (!receiver || receiver->thread() != QThread::currentThread() || !song || Engine::getSong() != song.data() || Engine::audioEngine() != engine
				|| engine->midiClient() != client || engine->isChangingMidiBackend() || song->isLoadingProject())
			{ throw std::runtime_error("Native MIDI parameter or backend retired during admission"); }
			if (receiver->scaleType() != ScaleType::Linear && receiver->scaleType() != ScaleType::Logarithmic)
			{ throw std::invalid_argument("Native controller DSP does not support this parameter scale"); }
		};
		const auto baseline = [&] { context(); if (!receiver->controllerStateMatches(*source, false)) { throw std::runtime_error("Native linked parameter changed during MIDI preparation"); } };
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } baseline(); };
		check();
		// Preserve native Song ancestry for parameter discovery. Weak cleanup
		// owns preparation, then the connection owns controller retirement;
		// this MIDI controller never enters the shared rack.
		const QPointer<MidiController> native(new MidiController(song.data())); bool connectionOwnsNative = false;
		const auto cleanupNative = qScopeGuard([&] { if (!connectionOwnsNative) { delete native.data(); } });
		baseline();
		if (!native) { throw std::runtime_error("Native MIDI controller retired during preparation"); }
		const QPointer<MidiPort> port(native->midiPort());
		const auto alive = [&] {
			if (!native || native->isRetiring() || !port || native->thread() != QThread::currentThread() || port->thread() != QThread::currentThread()
				|| native->midiPort() != port.data() || port->parent() != native.data() || port->client() != client)
			{ throw std::runtime_error("Native prepared MIDI controller or port changed"); }
		};
		port->setInputChannel(channel); baseline(); alive();
		port->setInputController(control); baseline(); alive();
		port->setReadable(readable); baseline(); alive();
		if (learnedInputs)
		{
			const auto available = port->readablePorts();
			for (const auto& input : *learnedInputs)
			{ if (!available.contains(input)) { throw std::runtime_error("Learned native MIDI input disappeared during preparation"); } }
			for (auto it = available.cbegin(); it != available.cend(); ++it)
			{ port->subscribeReadablePort(it.key(), learnedInputs->contains(it.key())); baseline(); alive(); }
		}
		native->updateName(); baseline(); alive();
		if (learnedValue != MidiController::NONE)
		{
			native->m_previousValue = native->m_lastValue = static_cast<float>(learnedValue) / MidiMaxControllerValue;
			native->updateValueBuffer(); baseline(); alive();
		}
		const auto delivery = client->inputDeliveryStatus(port.data());
		const QPointer<ControllerConnection> replacement(new ControllerConnection(native.data())); connectionOwnsNative = true;
		// No unique_ptr overlaps with model ownership during publication or
		// notifications. Retire an abandoned candidate only if it is still live.
		const auto cleanupReplacement = qScopeGuard([&] {
			if (replacement && !replacement->isRetiring() && (!receiver || receiver->controllerConnection() != replacement.data())) { delete replacement.data(); }
		});
		const auto prepared = [&] {
			alive();
			if (!replacement || replacement->thread() != QThread::currentThread() || replacement->getController() != native.data()
				|| replacement->parent() || native->parent() != song.data() || native->name() != QString("MIDI ch%1 ctrl%2").arg(channel).arg(control)
				|| port->inputChannel() != channel || port->inputController() != control || port->isReadable() != readable
				|| port->isWritable() || port->mode() != (readable ? MidiPort::Mode::Input : MidiPort::Mode::Disabled))
			{ throw std::runtime_error("Native prepared MIDI assignment settings changed"); }
			if (learnedInputs && port->requestedReadablePorts() != *learnedInputs)
			{ throw std::runtime_error("Native learned MIDI input subscriptions changed"); }
			if (learnedValue != MidiController::NONE)
			{
				const auto current = client->inputDeliveryStatus(port.data());
				if (!delivery.registered || delivery.failed || !port->isInputEnabled() || !current.registered || current.failed
					|| current.failureVersion != delivery.failureVersion || current.droppedEvents != delivery.droppedEvents
					|| native->m_previousValue != static_cast<float>(learnedValue) / MidiMaxControllerValue
					|| native->m_lastValue != native->m_previousValue)
				{ throw std::runtime_error("Native learned MIDI input or retained value changed"); }
			}
			if (native->hasModel(receiver.data())) { throw std::invalid_argument("Native MIDI assignment would create controller feedback"); }
		};
		check(); prepared();
		if (beforePublish) { beforePublish(); check(); prepared(); }
		const auto retirePrevious = [&] {
			if (previous && !previous->isRetiring() && (!receiver || receiver->controllerConnection() != previous.data())) { delete previous.data(); }
		};
		const bool blocked = receiver->blockSignals(true);
		const auto restoreSignals = qScopeGuard([&] { if (receiver) { receiver->blockSignals(blocked); } });
		const auto cleanupPrevious = qScopeGuard(retirePrevious);
		published = true; receiver->setControllerConnection(replacement.data()); context(); prepared();
		if (!receiver->controllerStateMatches(*source, true, replacement.data()))
		{ throw std::runtime_error("Native MIDI binding changed during publication"); }
		receiver->setUseControllerValue(true); context(); prepared();
		const auto result = [&] {
			context(); prepared();
			if (!receiver->controllerStateMatches(*source, true, replacement.data(), true))
			{ throw std::runtime_error("Native MIDI assignment result changed during notification"); }
		};
		result(); retirePrevious(); result();
		receiver->blockSignals(blocked); receiver->notifyValueData(); result();
		changed = true; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native MIDI assignment failed with a nonstandard exception"; }
	return false;
}

bool AutomatableModel::connectController(Controller* controller, bool& changed, QString& error)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject())
		{ throw std::invalid_argument("Native controller assignment requires a ready project on the model owner thread"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const QPointer<AutomatableModel> receiver(this); const QPointer<Controller> target(controller); const QPointer<Song> song(Engine::getSong());
		const auto source = captureControllerState(); const QPointer<ControllerConnection> previous(controllerConnection());
		const auto owners = [&] {
			if (!receiver || receiver->thread() != QThread::currentThread() || !target || target->isRetiring() || target->thread() != QThread::currentThread()
				|| !song || Engine::getSong() != song.data() || song->isLoadingProject()
				|| std::find(song->controllers().begin(), song->controllers().end(), target.data()) == song->controllers().end())
			{ throw std::runtime_error("Native controller assignment owner or rack membership changed"); }
			if (target->type() == Controller::ControllerType::Dummy || target->type() == Controller::ControllerType::Midi)
			{ throw std::invalid_argument("Controller is not a shared native rack controller"); }
		};
		const auto context = [&] {
			owners();
			if (!receiver->supportsAutomation()) { throw std::invalid_argument("Native parameter does not support controller input"); }
			owners();
			if (receiver->scaleType() != ScaleType::Linear && receiver->scaleType() != ScaleType::Logarithmic)
			{ throw std::invalid_argument("Native controller DSP does not support this parameter scale"); }
			if (target->hasModel(receiver.data())) { throw std::invalid_argument("Connection would create native controller feedback"); }
		};
		const auto baseline = [&] { context(); if (!receiver->controllerStateMatches(*source, false)) { throw std::runtime_error("Native linked parameter changed during controller assignment"); } };
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } baseline(); };
		check();
		if (previous && previous->getController() == target.data() && previous->isFinalized()) { error.clear(); return true; }
		QPointer<ControllerConnection> binding(previous);
		if (!binding) { binding = new ControllerConnection(nullptr); }
		const auto cleanup = qScopeGuard([&] {
			if (binding && !binding->isRetiring() && (!receiver || receiver->controllerConnection() != binding.data())) { delete binding.data(); }
		});
		const auto assigned = [&] {
			context();
			if (!binding || binding->thread() != QThread::currentThread() || binding->getController() != target.data() || !binding->isFinalized())
			{ throw std::runtime_error("Native controller connection changed during assignment"); }
		};
		if (!previous) { binding->setController(target.data()); check(); assigned(); }
		const bool blocked = receiver->blockSignals(true);
		const auto restoreSignals = qScopeGuard([&] { if (receiver) { receiver->blockSignals(blocked); } });
		if (previous) { binding->setController(target.data()); }
		else { receiver->setControllerConnection(binding.data()); }
		const auto result = [&] {
			assigned();
			if (!receiver->controllerStateMatches(*source, true, binding.data()))
			{ throw std::runtime_error("Native controller assignment result changed during notification"); }
		};
		result(); receiver->blockSignals(blocked); receiver->notifyValueData(); result();
		changed = true; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native controller assignment failed with a nonstandard exception"; }
	return false;
}

float AutomatableModel::controllerValue( int frameOffset ) const
{
	assert(controllerConnection() != nullptr);

	float v = 0;
	switch (m_scaleType)
	{
		case ScaleType::Linear:
			v = minValue<float>() + ( range() * controllerConnection()->currentValue( frameOffset ) );
			break;
		case ScaleType::Logarithmic:
			v = logToLinearScale(
				controllerConnection()->currentValue( frameOffset ));
			break;
		default:
			qFatal("AutomatableModel::controllerValue(int)"
				"lacks implementation for a scale type");
			break;
	}
	if (approximatelyEqual(m_step, 1) && m_hasStrictStepSize)
	{
		return std::round(v);
	}
	return v;
}

ValueBuffer * AutomatableModel::valueBuffer()
{
	QMutexLocker m( &m_valueBufferMutex );
	const auto generation = s_bufferGeneration;
	// if we've already calculated the valuebuffer this period, return the cached buffer
	if (m_lastUpdatedPeriod == s_periodCounter && m_bufferGeneration == generation)
	{
		return m_hasSampleExactData
			? &m_valueBuffer
			: nullptr;
	}

	float val = m_value; // make sure our m_value doesn't change midway

	// TODO
	// Let the Controller set the value of connected Models,
	// instead of each Model checking the Controller value every time.

	// Get value buffer from our controller
	const QPointer<ControllerConnection> binding(controllerConnection());
	if (binding && m_useControllerValue && binding->getController()->isSampleExact())
	{
		auto vb = binding->valueBuffer();
		if( vb )
		{
			float * values = vb->values();
			float * nvalues = m_valueBuffer.values();
			switch( m_scaleType )
			{
			case ScaleType::Linear:
				for( int i = 0; i < m_valueBuffer.length(); i++ )
				{
					nvalues[i] = minValue<float>() + ( range() * values[i] );
				}
				break;
			case ScaleType::Logarithmic:
				for( int i = 0; i < m_valueBuffer.length(); i++ )
				{
					nvalues[i] = logToLinearScale( values[i] );
				}
				break;
			default:
				qFatal("AutomatableModel::valueBuffer() "
					"lacks implementation for a scale type");
				break;
			}
			m_lastUpdatedPeriod = s_periodCounter;
			m_bufferGeneration = generation;
			m_hasSampleExactData = true;
			return &m_valueBuffer;
		}
	}

	// Get value buffer from one of the linked models' controller
	if (m_useControllerValue)
	{
		for (auto next = m_nextLink; next != this; next = next->m_nextLink)
		{
				if (next->controllerConnection() && next->useControllerValue() &&
						next->controllerConnection()->getController()->isSampleExact())
				{
					auto vb = next->valueBuffer();
					float* values = vb->values();
					float* nvalues = m_valueBuffer.values();
					for (int i = 0; i < vb->length(); i++)
					{
						nvalues[i] = fittedValue(values[i]);
					}
					m_lastUpdatedPeriod = s_periodCounter;
					m_bufferGeneration = generation;
					m_hasSampleExactData = true;
					return &m_valueBuffer;
				}
		}
	}

	// Note: if there are linked models but no controller, `setValue()` will have
	// updated `m_value` and `m_oldValue` across all linked models, so the `valueBuffer()`
	// will be the same (even if it is calculated separatly for each model).

	// Populate value buffer by interpolatating between the old and new value
	if (m_oldValue != val)
	{
		m_valueBuffer.interpolate(m_oldValue, val);
		m_oldValue = val;
		m_lastUpdatedPeriod = s_periodCounter;
		m_bufferGeneration = generation;
		m_hasSampleExactData = true;
		return &m_valueBuffer;
	}

	// if we have no sample-exact source for a ValueBuffer, return NULL to signify that no data is available at the moment
	// in which case the recipient knows to use the static value() instead
	m_lastUpdatedPeriod = s_periodCounter;
	m_bufferGeneration = generation;
	m_hasSampleExactData = false;
	return nullptr;
}


void AutomatableModel::unlinkControllerConnection()
{
	const QPointer<ControllerConnection> connection(m_controllerConnection);
	m_controllerConnection = nullptr;
	m_valueChanged = true;
	invalidateLinkedValueBuffers();
	// Qt disconnection hooks may call out. No model access follows this call.
	if (connection && !connection->isRetiring()) { connection->disconnect(this); }
}




void AutomatableModel::setInitValue( const float value )
{
	const QPointer<AutomatableModel> receiver(this);
	m_initValue = fittedValue( value );
	{
		const auto journalRestore = qScopeGuard([receiver, journalling = testAndSetJournalling(false)] { if (receiver) { receiver->setJournalling(journalling); } });
		setValue( value );
		if (!receiver) { return; }
		m_oldValue = m_value;
	}
	emit initValueChanged( value );
}




void AutomatableModel::reset()
{
	setValue( initValue<float>() );
}




float AutomatableModel::globalAutomationValueAt( const TimePos& time )
{
	// get clips that connect to this model
	auto clips = AutomationClip::clipsForModel(this);
	if (clips.empty())
	{
		// if no such clips exist, return current value
		return m_value;
	}
	else
	{
		// of those clips:
		// find the clips which overlap with the time position
		std::vector<AutomationClip*> clipsInRange;
		for (const auto& clip : clips)
		{
			int s = clip->startPosition();
			int e = clip->endPosition();
			if (s <= time && e >= time) { clipsInRange.push_back(clip); }
		}

		AutomationClip * latestClip = nullptr;

		if (!clipsInRange.empty())
		{
			// if there are more than one overlapping clips, just use the first one because
			// multiple clip behaviour is undefined anyway
			latestClip = clipsInRange[0];
		}
		else
		// if we find no clips at the exact time, we need to search for the last clip before time and use that
		{
			int latestPosition = 0;

			for (const auto& clip : clips)
			{
				int e = clip->endPosition();
				if (e <= time && e > latestPosition)
				{
					latestPosition = e;
					latestClip = clip;
				}
			}
		}

		if( latestClip )
		{
			// scale/fit the value appropriately and return it
			const float value = latestClip->valueAt(time - latestClip->startPosition() + latestClip->startTimeOffset());
			const float scaled_value = scaledValue( value );
			return fittedValue( scaled_value );
		}
		// if we still find no clip, the value at that time is undefined so
		// just return current value as the best we can do
		else return m_value;
	}
}

void AutomatableModel::setUseControllerValue(bool b)
{
	if (b)
	{
		m_useControllerValue = true;
		invalidateLinkedValueBuffers();
		notifyValueData();
	}
	else if (controllerConnection() && m_useControllerValue)
	{
		m_useControllerValue = false;
		invalidateLinkedValueBuffers();
		notifyValueData();
	}
}

float FloatModel::getRoundedValue() const
{
	return std::round(value() / step<float>()) * step<float>();
}




int FloatModel::getDigitCount() const
{
	return static_cast<int>(std::ceil(-std::log10(step<float>())));
}



QString FloatModel::displayValue( const float val ) const
{
	return QString::number( castValue<float>( scaledValue( val ) ) );
}

QString IntModel::displayValue( const float val ) const
{
	return QString::number( castValue<int>( scaledValue( val ) ) );
}

QString BoolModel::displayValue( const float val ) const
{
	return QString::number( castValue<bool>( scaledValue( val ) ) );
}


} // namespace lmms
