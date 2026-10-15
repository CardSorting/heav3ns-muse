/*
 * ControllerConnection.cpp - implementation of class controller connection
 *            which handles the link between AutomatableModels and controllers
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail.com>
 * Copyright (c) 2010 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <QDomElement>
#include <QObject>
#include <QPointer>
#include <QScopeGuard>
#include <limits>
#include <stdexcept>


#include "Song.h"
#include "AutomatableModel.h"
#include "ControllerConnection.h"
#include "NativeValueNotification.h"

namespace lmms
{


ControllerConnectionVector ControllerConnection::s_connections;



ControllerConnection::ControllerConnection(Controller * _controller) :
	m_controller( nullptr ),
	m_controllerId( -1 ),
	m_ownsController(false)
{
	if( _controller != nullptr )
	{
		if (_controller->isRetiring()) { throw std::invalid_argument("Cannot construct a binding to a retiring controller"); }
		setController( _controller );
	}
	else
	{
		m_controller = Controller::create( Controller::ControllerType::Dummy,
									nullptr );
	}
	s_connections.push_back(this);
}




ControllerConnection::ControllerConnection( int _controllerId ) :
	m_controller( Controller::create( Controller::ControllerType::Dummy, nullptr ) ),
	m_controllerId( _controllerId ),
	m_ownsController( false )
{
	s_connections.push_back(this);
}




ControllerConnection::~ControllerConnection()
{
	m_retiring = true;
	const auto controller = m_controller; const bool owned = m_ownsController, counted = m_controllerCounted;
	m_controller = nullptr; m_ownsController = false; m_controllerCounted = false;
	auto it = std::find(s_connections.begin(), s_connections.end(), this);
	if (it != s_connections.end()) { s_connections.erase(it); };
	if (controller && counted) { controller->removeConnection(this); }
	const bool blocked = blockSignals(false); emit retiring(); blockSignals(blocked);
	if (controller) { controller->disconnect(this); }
	if (owned && controller && !controller->isRetiring()) { delete controller.data(); }
}




void ControllerConnection::setController( int /*_controllerId*/ )
{
}




void ControllerConnection::setController( Controller * _controller )
{
	if (m_retiring) { return; }
	if (_controller && _controller->isRetiring()) { return; }
	if (_controller == m_controller) { m_controllerId = -1; return; }
	if (m_controllerGeneration == std::numeric_limits<std::uint64_t>::max())
	{ throw std::overflow_error("Native connection controller generation exhausted"); }
	const QPointer<ControllerConnection> receiver(this);
	const QPointer<Controller> target(_controller ? _controller : Controller::create(Controller::ControllerType::Dummy, nullptr));
	if (!target) { return; }
	const bool counted = target->type() != Controller::ControllerType::Dummy;
	if (counted && target->connectionCount() == std::numeric_limits<int>::max())
	{ throw std::overflow_error("Native controller connection count exhausted"); }
	const auto previous = m_controller; const bool ownedPrevious = m_ownsController;
	const auto previousGeneration = m_controllerGeneration;
	const auto retire = [&] { if (ownedPrevious && previous && !previous->isRetiring() && (!receiver || receiver->m_controller != previous)) { delete previous.data(); } };
	const auto cleanup = qScopeGuard(retire);
	if (previous)
	{
		previous->disconnect(this);
	}
	if (!receiver || receiver->m_controllerGeneration != previousGeneration || receiver->m_controller != previous || !target || target->isRetiring()) { return; }
	if (counted && target->connectionCount() == std::numeric_limits<int>::max())
	{ throw std::overflow_error("Native controller connection count exhausted during rewire"); }
	if (previous && receiver->m_controllerCounted) { previous->removeConnection(receiver.data()); }
	const auto generation = ++receiver->m_controllerGeneration;
	receiver->m_controller = target; receiver->m_controllerId = -1;
	receiver->m_ownsController = target->type() == Controller::ControllerType::Midi; receiver->m_controllerCounted = counted;
	// Complete the replacement count before any Qt hook or owned-controller
	// destructor can retire this connection and decrement that count.
	if (counted) { target->addConnection(receiver.data()); }
	const auto valid = [&] { return receiver && target && !target->isRetiring() && receiver->m_controllerGeneration == generation && receiver->m_controller == target; };
	// Any independent controller retirement also retires its connection. Our
	// destructor/rewire disconnects this hook before deleting an owned MIDI
	// controller, so ordinary ownership cleanup cannot recurse through it.
	const auto retireTarget = [this, generation] {
		if (m_controllerGeneration != generation) { return; }
		// QObject emits destroyed after Controller's members have been destroyed.
		// The connection destructor must not dereference that controller.
		m_controller = nullptr;
		m_ownsController = false; m_controllerCounted = false;
		deleteConnection();
	};
	// Dummy connections are uncounted, but a custom dummy can still die.
	QObject::connect(target.data(), &Controller::retiring, receiver.data(), retireTarget, Qt::DirectConnection);
	if (!valid()) { return; }
	QObject::connect(target.data(), &QObject::destroyed, receiver.data(), retireTarget, Qt::DirectConnection);
	if (!valid()) { return; }
	if (counted)
	{
		QObject::connect(target.data(), &Controller::valueChanged, receiver.data(), &ControllerConnection::valueChanged, Qt::DirectConnection);
		if (!valid()) { return; }
	}
	retire(); if (!valid()) { return; }
	NativeValueNotification notification;
	emit receiver->valueChanged();
	notification.rethrow();
}



inline void ControllerConnection::setTargetName( const QString & _name )
{
	m_targetName = _name;
	if( m_controller )
	{
	//	m_controller->getMidiPort()->setName( _name );
	}
}



/*
 * A connection may not be finalized.  This means, the connection should exist,
 * but the controller does not yet exist.  This happens when loading.  Even
 * loading connections last won't help, since there can be connections BETWEEN
 * controllers. So, we remember the controller-ID and use a dummyController
 * instead.  Once the song is loaded, finalizeConnections() connects to the proper controllers
 */
void ControllerConnection::finalizeConnections()
{
	// Resolving one connection can notify models and retire another connection.
	// Snapshot weak references rather than indices into the mutable registry.
	std::vector<QPointer<ControllerConnection>> connections;
	connections.reserve(s_connections.size());
	for (auto connection : s_connections) { connections.emplace_back(connection); }
	for (const auto& c : connections)
	{
		if (!c) { continue; }
		if (!c->isFinalized() && static_cast<std::size_t>(c->m_controllerId) < Engine::getSong()->controllers().size())
		{
			c->setController( Engine::getSong()->
					controllers().at( c->m_controllerId ) );
		}
		// An unavailable controller occupies a dummy slot to preserve later IDs.
		// Discard its connection in this same pass, after attempting resolution.
		if (c && c->getController()->type() == Controller::ControllerType::Dummy)
		{
			delete c.data();
			Engine::getSong()->collectError(QObject::tr("Removed unresolved controller connection"));
		}
	}
	// A project can contain cyclic controller IDs even when interactive editing
	// would have rejected them. Remove one dependent edge before DSP evaluates it.
	for (const auto controller : Engine::getSong()->controllers())
	{
		for (const auto model : controller->findChildren<AutomatableModel*>())
		{
			const auto connection = model->controllerConnection();
			if (connection && connection->getController()->hasModel(model))
			{
				delete connection;
				Engine::getSong()->collectError(QObject::tr("Removed cyclic controller connection"));
			}
		}
	}
}




void ControllerConnection::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if( Engine::getSong() )
	{
		if( m_ownsController )
		{
			m_controller->saveState( _doc, _this );
		}
		else
		{
			const auto& controllers = Engine::getSong()->controllers();
			const auto it = std::find(controllers.begin(), controllers.end(), m_controller.data());
			if (it != controllers.end())
			{
				const int id = std::distance(controllers.begin(), it);
				_this.setAttribute("id", id);
			}
		}
	}
}




void ControllerConnection::loadSettings( const QDomElement & _this )
{
	const auto node = _this.firstChildElement();
	setController(static_cast<Controller*>(nullptr));
	if( !node.isNull() )
	{
		setController( Controller::create( node.toElement(), Engine::getSong() ) );
	}
	else
	{
		bool validId = false;
		m_controllerId = _this.attribute( "id", "-1" ).toInt(&validId);
		if (!validId || m_controllerId < 0)
		{
			qWarning( "controller index invalid\n" );
			m_controllerId = -1;
		}

		// Keep the dummy during project loading until all controllers exist.
		if (!Engine::getSong()->isLoadingProject()
			&& m_controllerId != -1
			&& static_cast<std::size_t>(m_controllerId) < Engine::getSong()->controllers().size())
		{
			setController( Engine::getSong()->
					controllers().at( m_controllerId ) );
		}
	}
}


void ControllerConnection::deleteConnection()
{
	if (!m_retiring) { delete this; }
}



} // namespace lmms
