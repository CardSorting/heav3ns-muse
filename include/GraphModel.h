/*
 * GraphModel.h - native waveform and transfer-curve model
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

#ifndef LMMS_GRAPH_MODEL_H
#define LMMS_GRAPH_MODEL_H

#include <QVector>
#include <span>
#include "Model.h"

class QDomElement;

namespace lmms
{
namespace gui { class Graph; }

/**
	@brief 2 dimensional function plot

	Function plot graph with discrete x scale and continous y scale
	This makes it possible to display "#x" samples
*/
class LMMS_EXPORT graphModel : public Model
{
	Q_OBJECT
public:
	//! @param _min Minimum y value to display
	//! @param _max Maximum y value to display
	//! @param _size Number of samples (e.g. x value)
	//! @param _step Step size on y axis where values snap to, or 0.0f
	//! for "no snapping"
	// TODO: @param _parent
	// TODO: @param _default_constructed
	graphModel( float _min,
			float _max,
			int _size,
			Model * _parent,
			bool _default_constructed = false,
			float _step = 0.0 );

	~graphModel() override = default;

	// Checked native bulk/file entry points; no editor or file dialog is needed.
	bool setSamplesChecked(std::span<const float> samples, QString& error, bool constrain = false);
	bool loadBase64(const QString& encoded, QString& error, bool allowShort = false);
	bool loadSampleAttribute(const QDomElement& element, const QString& attribute, bool allowShort = false);
	bool loadWaveform(const QString& path, QString& error);
	float step() const { return m_step; }

	inline float minValue() const
	{
		return( m_minValue );
	}

	inline float maxValue() const
	{
		return( m_maxValue );
	}

	inline int length() const
	{
		return m_length;
	}

	inline const float * samples() const
	{
		return( m_samples.data() );
	}

	//! Make cyclic convolution
	//! @param convolution Samples to convolve with
	//! @param convolutionLength Number of samples to take for each sum
	//! @param centerOffset Offset for resulting values
	void convolve(const float *convolution,
		const int convolutionLength, const int centerOffset);

public slots:
	//! Set range of y values
	void setRange(float ymin, float ymax);

	void setLength( int _size );
	//! Update one sample
	void setSampleAt( int x, float val );
	//! Update samples array
	void setSamples( const float * _value );

	void setWaveToSine();
	void setWaveToTriangle();
	void setWaveToSaw();
	void setWaveToSquare();
	void setWaveToNoise();

	void smooth();
	void smoothNonCyclic();
	void normalize();
	void invert();
	void shiftPhase( int _deg );
	void clear();
	void clearInvisible();

signals:
	void lengthChanged();
	void samplesChanged( int startPos, int endPos );
	void rangeChanged();

private:
	void drawSampleAt( int x, float val );

	QVector<float> m_samples;
	int m_length;
	float m_minValue;
	float m_maxValue;
	float m_step;
	bool m_setting = false;

	friend class gui::Graph;

};

} // namespace lmms
#endif
