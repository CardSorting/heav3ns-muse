/*
 * Graph.h - a QT widget for displaying and manipulating waveforms
 *
 * Copyright (c) 2006-2007 Andreas Brandmaier <andy/at/brandmaier/dot/de>
 *               2008 Paul Giblock <drfaygo/at/gmail/dot/com>
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

#ifndef LMMS_GUI_GRAPH_H
#define LMMS_GUI_GRAPH_H

#include <QWidget>
#include <QPixmap>
#include <QCursor>

#include "GraphModel.h"
#include "ModelView.h"
#include "LmmsTypes.h"

namespace lmms
{


class graphModel;

namespace gui
{


class LMMS_EXPORT Graph : public QWidget, public ModelView
{
	Q_OBJECT
public:
	enum class Style
	{
		Nearest, //!< draw as stairs
		Linear, //!< connect each 2 samples with a line, with wrapping
		LinearNonCyclic, //!< Linear without wrapping
		Bar, //!< draw thick bars
	};

	//! @param parent The parent QWidget
	//! @param style The @ref Style to display this Graph with
	//! @param width Pixel width of widget
	//! @param height Pixel height of widget
	Graph(
		QWidget* parent,
		Style style = Style::Linear,
		int width = 132,
		int height = 104
	);
	~Graph() override = default;
	static QString loadUserWaveform(graphModel* target);

	void setForeground( const QPixmap & _pixmap );


	void setGraphColor( const QColor );

	inline graphModel * model()
	{
		return castModel<graphModel>();
	}

	inline Style getGraphStyle()
	{
		return m_graphStyle;
	}


	inline void setGraphStyle( Style _s )
	{
		m_graphStyle = _s;
		update();
	}

signals:
	void drawn();
protected:
	void paintEvent( QPaintEvent * _pe ) override;
	void dropEvent( QDropEvent * _de ) override;
	void dragEnterEvent( QDragEnterEvent * _dee ) override;
	void mousePressEvent( QMouseEvent * _me ) override;
	void mouseMoveEvent( QMouseEvent * _me ) override;
	void mouseReleaseEvent( QMouseEvent * _me ) override;

protected slots:
	void updateGraph( int _startPos, int _endPos );
	void updateGraph();

private:
	void modelChanged() override;

	void changeSampleAt( int _x, int _y );
	void drawLineAt( int _x, int _y, int _lastx );


	QPixmap m_foreground;
	QColor m_graphColor;

	Style m_graphStyle;

	bool m_mouseDown;
	int m_lastCursorX;

} ;


} // namespace gui





} // namespace lmms

#endif // LMMS_GUI_GRAPH_H
