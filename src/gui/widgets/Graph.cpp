/*
 * Graph.cpp - a QT widget for displaying and manipulating waveforms
 *
 * Copyright (c) 2006-2007 Andreas Brandmaier <andy/at/brandmaier/dot/de>
 *               2008 Paul Giblock            <drfaygo/at/gmail/dot/com>
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

#include <QPainter>

#include "Graph.h"
#include "DeprecationHelper.h"
#include "FileDialog.h"
#include "StringPairDrag.h"
#include "GraphModel.h"
#include "Engine.h"
#include "Song.h"
#include <QPointer>
#include <algorithm>
#include <cmath>

namespace lmms
{

namespace gui
{

Graph::Graph( QWidget * _parent, Style _style, int _width,
		int _height ) :
	QWidget( _parent ),
	/* TODO: size, background? */
	ModelView( new graphModel( -1.0, 1.0, 128, nullptr, true ), this ),
	m_graphStyle( _style )
{
	m_mouseDown = false;
	m_graphColor = QColor( 0xFF, 0xAA, 0x00 );

	resize( _width, _height );
	setAcceptDrops( true );
	setCursor( Qt::CrossCursor );

	auto gModel = castModel<graphModel>();

	QObject::connect( gModel, SIGNAL(samplesChanged(int,int)),
			this, SLOT(updateGraph(int,int)));

	QObject::connect( gModel, SIGNAL(lengthChanged()),
			this, SLOT(updateGraph()));
}

QString Graph::loadUserWaveform(graphModel* target)
{
	QPointer<graphModel> model = target;
	const auto path = FileDialog::openWaveformFile();
	if (path.isEmpty() || !model) { return {}; }
	QString error;
	if (!model->loadWaveform(path, error))
	{
		if (Engine::getSong()) { Engine::getSong()->collectError(error); }
		return {};
	}
	return path;
}

void Graph::setForeground( const QPixmap &_pixmap )
{
	m_foreground = _pixmap;
}

void Graph::setGraphColor( QColor _graphcol )
{
	m_graphColor = _graphcol;
}

/*
void graph::loadSampleFromFile( const QString & _filename )
{

	int i;

	// zero sample_shape
	for( i = 0; i < sampleLength; i++ )
	{
		samplePointer[i] = 0;
	}

	// load user shape
	sampleBuffer buffer( _filename );

	// copy buffer data
	int trimSize = fmin( size(), static_cast<int>(buffer.frames()) );


	for( i = 0; i < trimSize; i++ )
	{
		samplePointer[i] = (float)*buffer.data()[i];
	}
}
*/



void Graph::mouseMoveEvent ( QMouseEvent * _me )
{
	const auto pos = position(_me);

	// get position
	int x = pos.x();
	int y = pos.y();

/*	static bool skip = false;

	if( skip )
	{
		skip = false;
		return;
	}
*/
	// avoid mouse leaps
	int diff = x - m_lastCursorX;

/*	if( diff >= 1 )
	{
		x = qMin( width() - 3, m_lastCursorX + 1 );
	}
	else if( diff <= -1 )
	{
		x = qMax( 2, m_lastCursorX - 1 );
	}*/

	x = qMax( 2, qMin( x, width()-3 ) );
	y = qMax( 2, qMin( y, height()-3 ) );


	drawLineAt( x, y, m_lastCursorX );

	// update mouse
	if( diff != 0 )
	{
		m_lastCursorX = x;

		QPoint pt = mapToGlobal( QPoint( x, y ) );

		QCursor::setPos( pt.x(), pt.y() );
	}

//	skip = true;
}



void Graph::mousePressEvent( QMouseEvent * _me )
{
	const auto pos = position(_me);

	if( _me->button() == Qt::LeftButton )
	{
		if ( !( _me->modifiers() & Qt::ShiftModifier ) )
		{
			// get position
			int x = pos.x();
			int y = pos.y();

			changeSampleAt( x, y );

			// toggle mouse state
			m_mouseDown = true;
			setCursor( Qt::BlankCursor );
			m_lastCursorX = x;
		}
		else
		{
			//when shift-clicking, draw a line from last position to current
			//position
			int x = pos.x();
			int y = pos.y();

			drawLineAt( x, y, m_lastCursorX );

			m_mouseDown = true;
			setCursor( Qt::BlankCursor );
			m_lastCursorX = x;
		}

	}
}

void Graph::drawLineAt( int _x, int _y, int _lastx )
{
	float minVal = model()->minValue();
	float maxVal = model()->maxValue();
	if (width() <= 4 || height() <= 5)
	{
		return;
	}

	float xscale = static_cast<float>( model()->length() ) /
					( width()-4 );

	//consider border
	_x -= 2;
	_y -= 2;
	_lastx -= 2;

	_x = std::clamp(_x, 0, width() - 5);
	_lastx = qMax( 0, qMin( _lastx, width()-5 ) );

	float range = minVal - maxVal;
	float val = ( _y*range/( height()-5 ) ) + maxVal;
	
	int sample_begin, sample_end;
	float lastval;
	float val_begin, val_end;
	
	if (_lastx > _x)
	{
		sample_begin = (int)((_x) * xscale);
		sample_end = (int)ceil((_lastx+1) * xscale);
		lastval = model() -> m_samples[ (int)( sample_end - 1 ) ];
		val_begin = val;
		val_end = lastval;

	}
	else
	{
		sample_begin = (int)(_lastx * xscale);
		sample_end =  (int)ceil((_x+1) * xscale);
		lastval = model() -> m_samples[ (int)( sample_begin ) ];
		val_begin = lastval;
		val_end = val;
		
	}
	
	// calculate line drawing variables
	int linelen = sample_end - sample_begin;
	if (linelen <= 0) { return; }
	if (linelen == 1)
	{
		val_begin = val;
	}
	//int xstep = _x > _lastx ? -1 : 1;
	float ystep = ( val_end - val_begin ) / linelen;

	// Publish the entire stroke through the model's checked audio boundary.
	QVector<float> samples(model()->length());
	std::copy_n(model()->samples(), model()->length(), samples.begin());
	for ( int i = 0 ; i < linelen; i++ )
	{
		float value = val_begin + i * ystep;
		if (model()->step() > 0) { value = std::floor(value / model()->step()) * model()->step(); }
		samples[std::clamp(sample_begin + i, 0, model()->length() - 1)] = std::clamp(value, minVal, maxVal);
	}
	QString error;
	model()->setSamplesChecked({samples.constData(), static_cast<std::size_t>(samples.size())}, error);
}

void Graph::changeSampleAt( int _x, int _y )
{
	float minVal = model()->minValue();
	float maxVal = model()->maxValue();

	if ( width() <= 4 )
	{
		return;
	}

	float xscale = static_cast<float>( model()->length() ) /
					( width()-4 );


	// consider border of background image
	_x -= 2;
	_y -= 2;

	// subtract max from min because Qt's Y-axis is backwards
	float range = minVal - maxVal;
	float val = ( _y*range/( height()-5 ) ) + maxVal;

	model()->setSampleAt( (int)( _x*xscale ), val );
}



void Graph::mouseReleaseEvent( QMouseEvent * _me )
{
	if( _me->button() == Qt::LeftButton )
	{
		// toggle mouse state
		m_mouseDown = false;
		setCursor( Qt::CrossCursor );
		update();
		emit drawn();
	}
}



void Graph::paintEvent( QPaintEvent * )
{
	QPainter p( this );

	p.setPen( QPen( m_graphColor, 1 ) );
	QColor gcol = QColor( m_graphColor.red(), m_graphColor.green(), m_graphColor.blue(), 100 );

	QVector<float> * samps = &(model()->m_samples);
	int length = model()->length();
	const float maxVal = model()->maxValue();
	const float minVal = model()->minValue();

	float xscale = (float)( width()-4 ) / length;
	float yscale = (float)( height()-4 ) / ( minVal - maxVal );

	// Max index, more useful below
	length--;


	switch( m_graphStyle )
	{
		case Style::Linear:
			p.setRenderHints( QPainter::Antialiasing, true );

			for( int i=0; i < length; i++ )
			{
				// Needs to be rewritten
				p.drawLine(
					2+static_cast<int>(i*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
					2+static_cast<int>((i+1)*xscale),
					2+static_cast<int>( ( (*samps)[i+1] - maxVal ) * yscale )
					);
			}

			// Draw last segment wrapped around
			p.drawLine(2+static_cast<int>(length*xscale),
				2+static_cast<int>( ( (*samps)[length] - maxVal ) * yscale ),
				width()-3,
				2+static_cast<int>( ( (*samps)[0] - maxVal ) * yscale ) );

			p.setRenderHints( QPainter::Antialiasing, false );
			break;


		case Style::Nearest:
			for( int i=0; i < length; i++ )
			{
				p.drawLine(2+static_cast<int>(i*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
					2+static_cast<int>((i+1)*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale )
					);
				p.drawLine(2+static_cast<int>((i+1)*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
					2+static_cast<int>((i+1)*xscale),
					2+static_cast<int>( ( (*samps)[i+1] - maxVal ) * yscale )
					);
			}

			p.drawLine(2+static_cast<int>(length*xscale),
				2+static_cast<int>( ( (*samps)[length] - maxVal ) * yscale ),
				width()-3,
				2+static_cast<int>( ( (*samps)[length] - maxVal ) * yscale ) );
			break;

		case Style::LinearNonCyclic:
			p.setRenderHints( QPainter::Antialiasing, true );

			for( int i=0; i < length; i++ )
			{
				// Needs to be rewritten
				p.drawLine(
					2+static_cast<int>(i*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
					2+static_cast<int>((i+1)*xscale),
					2+static_cast<int>( ( (*samps)[i+1] - maxVal ) * yscale )
					);
			}

			// Do not draw last segment wrapped around - hence, "non-cyclic"

			p.setRenderHints( QPainter::Antialiasing, false );
			break;

		case Style::Bar:
			for( int i=0; i <= length; i++ )
			{
				p.fillRect( 2+static_cast<int>( i*xscale ),
							2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
							qMax( static_cast<int>(xscale) - 1, 1 ),
							qMax( static_cast<int>( ( minVal - maxVal ) * yscale ) - static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ), 1 ),
							gcol );

				p.setPen( QPen( m_graphColor, 1.0 ) );

				p.drawLine( 2+static_cast<int>(i*xscale),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale ),
					qMax( static_cast<int>(i*xscale) + static_cast<int>(xscale), 2+static_cast<int>(i*xscale) ),
					2+static_cast<int>( ( (*samps)[i] - maxVal ) * yscale )
					);
			}

			break;

		default:
			break;
	}


	// draw Pointer
	if( m_mouseDown )
	{
		QPoint cursor = mapFromGlobal( QCursor::pos() );
		p.setPen( QColor( 0x70, 0x7C, 0x91 ) );
		p.drawLine( 2, cursor.y(), width()-2, cursor.y() );
		p.drawLine( cursor.x(), 2, cursor.x(), height()-2 );
	}
	p.drawPixmap( 0, 0, m_foreground );
}



void Graph::dropEvent( QDropEvent * _de )
{
	QString type = StringPairDrag::decodeKey( _de );
	QString value = StringPairDrag::decodeValue( _de );

	if( type == "samplefile" )
	{
		// TODO: use the native waveform loader for audio-file drops.
		// loadSampleFromFile( value );
		_de->accept();
	}
}

void Graph::dragEnterEvent( QDragEnterEvent * _dee )
{
	if( StringPairDrag::processDragEnterEvent( _dee,
		QString( "samplefile" ) ) == false )
	{
		_dee->ignore();
	}
}



void Graph::modelChanged()
{
	auto gModel = castModel<graphModel>();

	QObject::connect( gModel, SIGNAL(samplesChanged(int,int)),
			this, SLOT(updateGraph(int,int)));

	QObject::connect( gModel, SIGNAL(lengthChanged()),
			this, SLOT(updateGraph()));
}


void Graph::updateGraph( int _startPos, int _endPos )
{
	// Can optimize by only drawing changed position
	update();
}


void Graph::updateGraph()
{
    updateGraph( 0, model()->length() - 1 );
}


} // namespace gui

} // namespace lmms
