/*
 * Controller.h - declaration of class controller, which provides a
 *                standard for all controllers and controller plugins
 *
 * Copyright (c) 2008-2009 Paul Giblock <pgllama/at/gmail.com>
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

#ifndef LMMS_CONTROLLER_H
#define LMMS_CONTROLLER_H

#include "lmms_export.h"
#include "Engine.h"
#include "LmmsTypes.h"
#include "Model.h"
#include "JournallingObject.h"
#include "ValueBuffer.h"
#include <cstdint>
#include <QPointer>

namespace lmms
{

class Controller;
class ControllerConnection;

namespace gui
{

class ControllerDialog;

} // namespace gui

using ControllerVector = std::vector<Controller*>;

class LMMS_EXPORT Controller : public Model, public JournallingObject
{
	Q_OBJECT
public:
	enum class ControllerType
	{
		Dummy,
		Lfo,
		Midi,
		Peak,
		/*
		XY,
		Equation
		*/
	} ;

	Controller( ControllerType _type, Model * _parent,
						const QString & _display_name );

	~Controller() override;
	bool isRetiring() const noexcept { return m_retiring; }

	virtual float currentValue( int _offset );
	// The per-controller get-value-in-buffers function
	virtual ValueBuffer * valueBuffer();

	inline bool isSampleExact() const
	{
		return m_sampleExact;
	}

	void setSampleExact( bool _exact )
	{
		m_sampleExact = _exact;
	}

	inline ControllerType type() const
	{
		return( m_type );
	}

	// return whether this controller updates models frequently - used for
	// determining when to update GUI
	inline bool frequentUpdates() const
	{
		switch( m_type )
		{
			case ControllerType::Lfo: return( true );
			case ControllerType::Peak: return( true );
			default:
				break;
		}
		return( false );
	}

	virtual const QString & name() const
	{
		return( m_name );
	}


	void saveSettings( QDomDocument & _doc, QDomElement & _this ) override;
	void loadSettings( const QDomElement & _this ) override;
	QString nodeName() const override;

	static Controller * create( ControllerType _tt, Model * _parent );
	static Controller * create( const QDomElement & _this,
							Model * _parent );

	inline static float fittedValue( float _val )
	{
		return std::clamp(_val, 0.0f, 1.0f);
	}

	static long runningPeriods()
	{
		return s_periods;
	}
	static f_cnt_t runningFrames();
	static float runningTime();

	static void triggerFrameCounter();
	static void resetFrameCounter();
	// Native model ownership required. Preserve musical time while making the
	// next read recompute buffers, including connection-owned MIDI controllers.
	static void invalidateValueBuffers() noexcept { ++s_bufferGeneration; }

	//Accepts a ControllerConnection * as it may be used in the future.
	void addConnection( ControllerConnection * );
	void removeConnection( ControllerConnection * );
	int connectionCount() const;

	bool hasModel( const Model * m ) const;

public slots:
	virtual gui::ControllerDialog * createDialog( QWidget * _parent );

	virtual void setName( const QString & _new_name )
	{
		m_name = _new_name;
	}


protected:
	void notifyValueChange();
	class LMMS_EXPORT SettingsGuard
	{
	public:
		explicit SettingsGuard(Controller* controller);
		~SettingsGuard();
		SettingsGuard(const SettingsGuard&) = delete;
		SettingsGuard& operator=(const SettingsGuard&) = delete;
		void check() const;
	private:
		QPointer<Controller> m_owner;
	};
	// Builtin serializers hold one SettingsGuard across these base fields and their own fields.
	void saveBaseSettings(QDomDocument& document, QDomElement& element);
	void loadBaseSettings(const QDomElement& element);
	// Begin before derived members/resources are retired. Song may initiate
	// this phase after detaching the rack entry and before removal observers.
	// Retirement listeners participate in destruction and must not throw.
	void beginRetirement();
	// The internal per-controller get-value function
	virtual float value( int _offset );

	virtual void updateValueBuffer();

	// buffer for storing sample-exact values in case there
	// are more than one model wanting it, so we don't have to create it
	// again every time
	ValueBuffer m_valueBuffer;
	// when we last updated the valuebuffer - so we know if we have to update it
	long m_bufferLastUpdated;
	std::uint64_t m_bufferGeneration;

	float m_currentValue;
	bool  m_sampleExact;
	int m_connectionCount;

	QString m_name;
	ControllerType m_type;
	bool m_retiring = false;
	bool m_changingSettings = false;

	static ControllerVector s_controllers;

	static long s_periods;
	static std::uint64_t s_bufferGeneration;


signals:
	void retiring();
	// The value changed while the audio engine isn't running (i.e: MIDI CC)
	void valueChanged();

	friend class gui::ControllerDialog;
	friend class Song;
	friend class LfoCatchUpTestAccess;
	friend class ControllerStateTestAccess;

} ;


} // namespace lmms

#endif // LMMS_CONTROLLER_H
