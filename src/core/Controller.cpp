/*
 * Controller.cpp - implementation of class controller which handles
 *                  remote-control of AutomatableModels
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail.com>
 * Copyright (c) 2014 Lukas W <lukaswhl/at/gmail.com>
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
#include <QSet>
#include <QPointer>
#include <QThread>
#include <QScopeGuard>
#include <stdexcept>

#include <vector>

#include "AudioEngine.h"
#include "ControllerConnection.h"
#include "NativeValueNotification.h"
#include "ControllerDialog.h"
#include "LfoController.h"
#include "MidiController.h"
#include "PeakController.h"
#include "Song.h"
#include "FileResourceIO.h"

namespace lmms
{


long Controller::s_periods = 0;
std::uint64_t Controller::s_bufferGeneration = 0;
std::vector<Controller*> Controller::s_controllers;



Controller::Controller( ControllerType _type, Model * _parent,
					const QString & _display_name ) :
	Model( _parent, _display_name ),
	JournallingObject(),
	m_valueBuffer( Engine::audioEngine()->framesPerPeriod() ),
	m_bufferLastUpdated( -1 ),
	m_bufferGeneration( s_bufferGeneration ),
	m_currentValue( 0.5f ),
	m_sampleExact( false ),
	m_connectionCount( 0 ),
	m_type( _type )
{
	if( _type != ControllerType::Dummy && _type != ControllerType::Midi )
	{
		s_controllers.push_back(this);
		// Determine which name to use
		for ( uint i=s_controllers.size(); ; i++ )
		{
			QString new_name = QString( tr( "Controller %1" ) )
					.arg( i );

			// Check if name is already in use
			bool name_used = false;
			for (Controller * controller : s_controllers)
			{
				if ( controller->name() == new_name )
				{
					name_used = true;
					break;
				}
			}
			if ( ! name_used )
			{
				m_name = new_name;
				break;
			}
		}
	}
	updateValueBuffer();
}



Controller::~Controller()
{
	beginRetirement();
	m_valueBuffer.clear();
}

void Controller::beginRetirement()
{
	if (m_retiring) { return; }
	m_retiring = true;
	auto it = std::find(s_controllers.begin(), s_controllers.end(), this);
	if (it != s_controllers.end())
	{
		s_controllers.erase(it);
	}

	const QPointer<Controller> receiver(this); const bool blocked = blockSignals(false);
	emit retiring();
	if (receiver) { receiver->blockSignals(blocked); receiver->m_connectionCount = 0; }
}



// Get current value, with an offset into the current buffer for sample exactness
float Controller::currentValue( int _offset )
{
	if (m_retiring) { return m_currentValue; }
	if( _offset == 0 || isSampleExact() )
	{
		m_currentValue = fittedValue( value( _offset ) );
	}

	return m_currentValue;
}



float Controller::value( int offset )
{
	const auto generation = s_bufferGeneration;
	if (m_bufferLastUpdated != s_periods || m_bufferGeneration != generation)
	{
		updateValueBuffer();
		m_bufferGeneration = generation;
	}
	return m_valueBuffer.values()[ offset ];
}


ValueBuffer * Controller::valueBuffer()
{
	if (m_retiring) { return &m_valueBuffer; }
	const auto generation = s_bufferGeneration;
	if (m_bufferLastUpdated != s_periods || m_bufferGeneration != generation)
	{
		updateValueBuffer();
		m_bufferGeneration = generation;
	}
	return &m_valueBuffer;
}


void Controller::updateValueBuffer()
{
	m_valueBuffer.fill(0.5f);
	m_bufferLastUpdated = s_periods;
}


// Get position in frames
f_cnt_t Controller::runningFrames()
{
	return s_periods * Engine::audioEngine()->framesPerPeriod();
}



// Get position in seconds
float Controller::runningTime()
{
	return static_cast<float>(static_cast<double>(runningFrames()) / Engine::audioEngine()->outputSampleRate());
}



void Controller::notifyValueChange()
{
	NativeValueNotification notification;
	emit valueChanged();
	notification.rethrow();
}

void Controller::triggerFrameCounter()
{
	// A direct model notification can retire another rack controller. Snapshot
	// weak references so native registry erasure cannot invalidate this walk.
	std::vector<QPointer<Controller>> controllers;
	controllers.reserve(s_controllers.size());
	for (auto controller : s_controllers) { controllers.emplace_back(controller); }
	for (const auto& controller : controllers)
	{
		if (!controller || controller->isRetiring()) { continue; }
		// This signal is for updating values for both stubborn knobs and for
		// painting.  If we ever get all the widgets to use or at least check
		// currentValue() then we can throttle the signal and only use it for
		// GUI.
		controller->notifyValueChange();
	}

	s_periods ++;
	//emit s_signaler.triggerValueChanged();
}



void Controller::resetFrameCounter()
{
	for (Controller * controller : s_controllers)
	{
		controller->m_bufferLastUpdated = 0;
	}
	s_periods = 0;
}



Controller * Controller::create( ControllerType _ct, Model * _parent )
{
	static Controller * dummy = nullptr;
	Controller * c = nullptr;

	switch( _ct )
	{
		case ControllerType::Dummy:
			if (!dummy)
				dummy = new Controller( ControllerType::Dummy, nullptr,
								QString() );
			c = dummy;
			break;

		case ControllerType::Lfo:
			c = new class LfoController( _parent );
			break;

		case ControllerType::Peak:
			//Already instantiated in EffectChain::loadSettings()
			Q_ASSERT( false );
			break;

		case ControllerType::Midi:
			c = new class MidiController( _parent );
			break;

		default:
			break;
	}

	return( c );
}



Controller * Controller::create( const QDomElement & _this, Model * _parent )
{
	if (_parent && _parent->thread() != QThread::currentThread()) { throw std::logic_error("Native controllers require their parent's owner thread"); }
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	const QPointer<Model> parent(_parent); const bool hadParent = _parent != nullptr;
	Model* const parentIdentity = _parent;
	const QPointer<Song> song(Engine::getSong());
	bool validType = false;
	const int rawType = _this.attribute("type").toInt(&validType);
	if (!validType) { return nullptr; }
	const auto controllerType = static_cast<ControllerType>(rawType);
	const QPointer<Controller> controller(controllerType == ControllerType::Peak
		? PeakController::getControllerBySetting(_this)
		: create(controllerType, _parent));
	bool adopted = false;
	const bool owned = controllerType == ControllerType::Lfo || controllerType == ControllerType::Midi;
	const auto cleanup = qScopeGuard([&] {
		if (!adopted && owned && controller && controller->parent() == parentIdentity && controller->connectionCount() == 0
			&& (!song || !song->containsController(controller.data()))) { delete controller.data(); }
	});
	const auto ready = [&] {
		if ((hadParent && !parent) || !song || Engine::getSong() != song.data()) { throw std::runtime_error("Native controller restoration owner retired"); }
	};
	ready();
	if (controller)
	{
		controller->restoreState(_this); ready();
		if (!controller || controller->isRetiring() || (owned && controller->parent() != parent.data())) { throw std::runtime_error("Native restored controller retired or changed owner"); }
	}
	adopted = true; return controller.data();
}



bool Controller::hasModel( const Model * m ) const
{
	QSet<const Controller*> visited;
	std::vector<const Controller*> pending{this};
	while (!pending.empty())
	{
		const auto controller = pending.back(); pending.pop_back();
		if (!controller || visited.contains(controller)) { continue; }
		visited.insert(controller);
		for (auto model : controller->findChildren<AutomatableModel*>())
		{
			for (const auto linked : model->linkedModels())
			{
				if (linked == m) { return true; }
				if (const auto connection = linked->controllerConnection()) { pending.push_back(connection->getController()); }
			}
		}
	}
	return false;
}



void Controller::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	SettingsGuard changing(this); saveBaseSettings(_doc, _this); changing.check();
}

Controller::SettingsGuard::SettingsGuard(Controller* controller) : m_owner(controller)
{
	if (!controller || controller->thread() != QThread::currentThread() || controller->m_retiring || controller->m_changingSettings)
	{ throw std::logic_error("Native controller settings cannot reenter or run during retirement or on another thread"); }
	controller->m_changingSettings = true;
}
Controller::SettingsGuard::~SettingsGuard() { if (m_owner) { m_owner->m_changingSettings = false; } }
void Controller::SettingsGuard::check() const
{
	if (!m_owner || m_owner->m_retiring) { throw std::runtime_error("Native controller retired during settings transition"); }
	QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
	if (!m_owner || m_owner->m_retiring) { throw std::runtime_error("Native controller retired during cooperative settings work"); }
}

void Controller::saveBaseSettings(QDomDocument&, QDomElement& _this)
{
	_this.setAttribute( "type", static_cast<int>(type()) );
	_this.setAttribute( "name", name() );
}



void Controller::loadSettings( const QDomElement & _this )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	SettingsGuard changing(this); loadBaseSettings(_this); changing.check();
}

void Controller::loadBaseSettings(const QDomElement& _this)
{
	if( static_cast<ControllerType>(_this.attribute( "type" ).toInt()) != type() )
	{
		qWarning( "controller-type does not match controller-type of "
							"settings-node!\n" );
	}

	setName( _this.attribute( "name" ) );
}


QString Controller::nodeName() const
{
	return( "Controller" );
}



gui::ControllerDialog * Controller::createDialog( QWidget * _parent )
{
	auto d = new gui::ControllerDialog(this, _parent);

	return d;
}




void Controller::addConnection( ControllerConnection * )
{
	m_connectionCount++;
}




void Controller::removeConnection( ControllerConnection * )
{
	m_connectionCount--;
	Q_ASSERT( m_connectionCount >= 0 );
}




int Controller::connectionCount() const{
	return m_connectionCount;
}


} // namespace lmms




