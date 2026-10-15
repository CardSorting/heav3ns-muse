/*
 * GraphModel.cpp - native waveform and transfer-curve model
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

#include "GraphModel.h"

#include <QByteArray>
#include <QDomElement>
#include <QThread>
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Oscillator.h"
#include "SampleBuffer.h"
#include "Song.h"

namespace lmms
{

graphModel::graphModel(float ymin, float ymax, int length, Model* parent, bool defaultConstructed, float step) :
	Model(parent, tr("Graph"), defaultConstructed),
	m_samples(std::max(1, length)),
	m_length(length),
	m_minValue(ymin),
	m_maxValue(ymax),
	m_step(std::clamp(step, 0.f, std::abs(ymax - ymin)))
{
	if (length < 1 || !std::isfinite(ymin) || !std::isfinite(ymax) || ymin > ymax || !std::isfinite(step))
	{
		throw std::invalid_argument("Invalid native graph dimensions or range");
	}
}

bool graphModel::setSamplesChecked(std::span<const float> samples, QString& error, bool constrain)
{
	if (QThread::currentThread() != thread() || m_setting) { error = "Native graph update requires its owner thread outside an active update"; return false; }
	if (samples.size() != static_cast<std::size_t>(length())) { error = "Native graph sample count does not match its length"; return false; }
	auto replacement = m_samples; // Retain any hidden waveform tail.
	for (int index = 0; index < length(); ++index)
	{
		auto value = samples[index];
		if (!std::isfinite(value) || (constrain && (value < minValue() || value > maxValue())))
		{
			error = "Native graph samples must be finite and edited values must stay within its range"; return false;
		}
		if (constrain && m_step > 0) { value = std::clamp(std::floor(value / m_step) * m_step, minValue(), maxValue()); }
		replacement[index] = value;
	}
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	// Keep the existing sample address for native borrowers while publishing
	// the validated prefix under the engine's change guard.
	std::copy_n(replacement.constData(), length(), m_samples.data());
	emit samplesChanged(0, length() - 1);
	error.clear();
	return true;
}

bool graphModel::loadBase64(const QString& encoded, QString& error, bool allowShort)
{
	if (QThread::currentThread() != thread() || m_setting) { error = "Native graph loading requires its owner thread outside an active update"; return false; }
	const qint64 expected = static_cast<qint64>(length()) * sizeof(float);
	if (encoded.size() > ((expected + 2) / 3) * 4 + 4096) { error = "Encoded native graph exceeds its expected size"; return false; }
	QByteArray compact;
	for (const auto character : encoded)
	{
		const auto value = character.unicode();
		if (value == ' ' || (value >= '\t' && value <= '\r')) { continue; }
		if (value > 127) { error = "Invalid native graph Base64 character"; return false; }
		compact.append(static_cast<char>(value));
	}
	const auto decoded = QByteArray::fromBase64Encoding(compact, QByteArray::AbortOnBase64DecodingErrors);
	const auto size = decoded.decoded.size();
	if (!decoded || size < static_cast<int>(sizeof(float)) || size % sizeof(float) != 0 || size > expected || (!allowShort && size != expected))
	{
		error = "Invalid or truncated native graph sample data"; return false;
	}
	QVector<float> samples(length(), 0.f);
	std::memcpy(samples.data(), decoded.decoded.constData(), static_cast<std::size_t>(size));
	return setSamplesChecked({samples.constData(), static_cast<std::size_t>(samples.size())}, error);
}

bool graphModel::loadWaveform(const QString& path, QString& error)
{
	if (QThread::currentThread() != thread() || m_setting) { error = "Native waveform loading requires its owner thread outside an active update"; return false; }
	const FileResourceIO::EventSuspension events;
	const auto source = SampleBuffer::fromFile(path);
	if (!source || source->empty()) { error = "Native waveform source could not be decoded or contains no frames"; return false; }
	QVector<float> values(length());
	for (int index = 0; index < length(); ++index) { values[index] = Oscillator::userWaveSample(source.get(), index / static_cast<float>(length())); }
	return setSamplesChecked({values.constData(), static_cast<std::size_t>(values.size())}, error);
}

bool graphModel::loadSampleAttribute(const QDomElement& element, const QString& attribute, bool allowShort)
{
	// Older presets can omit a curve; keep the native default in that case.
	if (!element.hasAttribute(attribute)) { return true; }
	QString error;
	if (loadBase64(element.attribute(attribute), error, allowShort)) { return true; }
	if (Engine::getSong()) { Engine::getSong()->collectError(element.tagName() + "/" + attribute + ": " + error); }
	return false;
}

void graphModel::setRange(float ymin, float ymax)
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	if (!std::isfinite(ymin) || !std::isfinite(ymax) || ymin > ymax) { throw std::invalid_argument("Invalid native graph range"); }
	if (ymin == m_minValue && ymax == m_maxValue) { return; }
	// Step sizes less than zero or larger than the entire range of
	// values are nonsense, clamp those
	m_step = std::clamp(m_step, 0.f, std::abs(ymax - ymin));
	m_minValue = ymin;
	m_maxValue = ymax;
	// Trim existing values
	for (float& sample : m_samples) {
		sample = std::clamp(sample, ymin, ymax);
	}
	emit rangeChanged();
	emit samplesChanged(0, length() - 1);
}

void graphModel::setLength( int _length )
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	if (_length < 1) { throw std::invalid_argument("Native graphs require at least one sample"); }
	if( _length != m_length )
	{
		if (m_samples.size() < _length) { m_samples.resize(_length); }
		m_length = _length;
		emit lengthChanged();
	}
}



void graphModel::setSampleAt( int x, float val )
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	if (!std::isfinite(val)) { return; }
	x = std::clamp(x, 0, length() - 1);
	drawSampleAt(x, val);
	emit samplesChanged(x, x);
}



void graphModel::setSamples( const float * _samples )
{
	if (!_samples) { return; }
	QString error;
	setSamplesChecked({_samples, static_cast<std::size_t>(length())}, error);
}



void graphModel::setWaveToSine()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	for( int i = 0; i < length(); i++ )
	{
		m_samples[i] = Oscillator::sinSample(
				i / static_cast<float>( length() ) );
	}

	emit samplesChanged( 0, length() - 1 );
};



void graphModel::setWaveToTriangle()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	for( int i = 0; i < length(); i++ )
	{
		m_samples[i] = Oscillator::triangleSample(
				i / static_cast<float>( length() ) );
	}

	emit samplesChanged( 0, length() - 1 );
};



void graphModel::setWaveToSaw()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	for( int i = 0; i < length(); i++ )
	{
		m_samples[i] = Oscillator::sawSample(
				i / static_cast<float>( length() ) );
	}

	emit samplesChanged( 0, length() - 1 );
};



void graphModel::setWaveToSquare()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	for( int i = 0; i < length(); i++ )
	{
		m_samples[i] = Oscillator::squareSample(
				i / static_cast<float>( length() ) );
	}

	emit samplesChanged( 0, length() - 1 );
};



void graphModel::setWaveToNoise()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	for( int i = 0; i < length(); i++ )
	{
		m_samples[i] = Oscillator::noiseSample(
				i / static_cast<float>( length() ) );
	}

	emit samplesChanged( 0, length() - 1 );
};

void graphModel::smooth()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	if (length() == 1) { emit samplesChanged(0, 0); return; }
	// store values in temporary array
	QVector<float> temp = m_samples;

	// Smoothing
	m_samples[0] = ( temp[length()-1] + ( temp[0] * 2 ) + temp[1] ) * 0.25f;
	for ( int i=1; i < ( length()-1 ); i++ )
	{
		m_samples[i] = ( temp[i-1] + ( temp[i] * 2 ) + temp[i+1] ) * 0.25f;
	}
	m_samples[length()-1] = ( temp[length()-2] + ( temp[length()-1] * 2 ) + temp[0] ) * 0.25f;

	emit samplesChanged(0, length()-1);
}

void graphModel::smoothNonCyclic()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	// store values in temporary array
	QVector<float> temp = m_samples;

	// Smoothing
	//m_samples[0] = ( ( temp[0] * 3 ) + temp[1] ) * 0.25f;
	for ( int i=1; i < ( length()-1 ); i++ )
	{
		m_samples[i] = ( temp[i-1] + ( temp[i] * 2 ) + temp[i+1] ) * 0.25f;
	}
	//m_samples[length()-1] = ( temp[length()-2] + ( temp[length()-1] * 3 ) ) * 0.25f;

	emit samplesChanged(0, length()-1);
}

void graphModel::convolve(const float *convolution,
	const int convolutionLength, const int centerOffset)
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	if (!convolution || convolutionLength < 1) { return; }
	// store values in temporary array
	QVector<float> temp = m_samples;
	const int graphLength = length();
	// make a cyclic convolution
	for ( int i = 0; i <  graphLength; i++ )
	{
		float sum = 0.0f;
		for ( int j = 0; j < convolutionLength; j++ )
		{
			sum += convolution[j] * temp[static_cast<int>((static_cast<qint64>(i) + j) % graphLength)];
		}
		const auto offset = (static_cast<qint64>(i) + centerOffset) % graphLength;
		m_samples[static_cast<int>(offset < 0 ? offset + graphLength : offset)] = sum;
	}
	emit samplesChanged(0, graphLength - 1);
}

void graphModel::normalize()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	float max = 0.0001f;
	float avg = 0.0f;

	// first correct dc offset by normalizing to average
	for( int i = 0; i < length(); i++ )
		avg += m_samples[i];
	avg /= length();
	for( int i = 0; i < length(); i++ )
		m_samples[i] -= avg;

	// then maximize
	for( int i = 0; i < length(); i++ )
		max = qMax( max, qAbs( m_samples[i] ) );

	for( int i = 0; i < length(); i++ )
		m_samples[i] = qBound( m_minValue, m_samples[i] / max, m_maxValue );

	// signal changes if any
	if( max != 1.0f || avg != 0.0f )
		emit samplesChanged( 0, length()-1 );
}



void graphModel::invert()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const float range = m_maxValue - m_minValue;

	for( int i = 0; i < length(); i++ )
		m_samples[i] = m_minValue + ( range - ( m_samples[i] - m_minValue ) );

	emit samplesChanged( 0, length()-1 );
}



void graphModel::shiftPhase( int _deg )
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	// calculate offset in samples
	const qint64 offset = (static_cast<qint64>(_deg) * length()) / 360;

	// store values in temporary array
	QVector<float> temp = m_samples;

	// shift phase
	for( int i = 0; i < length(); i++ )
	{
		int o = static_cast<int>((i + offset) % length());
		while( o < 0 ) o += length();
		m_samples[i] = temp[o];
	}

	emit samplesChanged( 0, length()-1 );
}

void graphModel::clear()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const int graph_length = length();
	for( int i = 0; i < graph_length; i++ )
		m_samples[i] = 0;
	emit samplesChanged( 0, graph_length - 1 );
}


// Clear any part of the graph that isn't displayed
void graphModel::clearInvisible()
{
	if (m_setting || QThread::currentThread() != thread()) { return; }
	QScopedValueRollback<bool> setting(m_setting, true);
	const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const int graph_length = length();
	const int full_graph_length = m_samples.size();
	for( int i = graph_length; i < full_graph_length; i++ )
		m_samples[i] = 0;
	emit samplesChanged( graph_length, full_graph_length - 1 );
}

void graphModel::drawSampleAt(int x, float val)
{
	// snap to the grid
	if (m_step > 0) { val = std::floor(val / m_step) * m_step; }
	// boundary crop
	x = std::clamp(x, 0, length() - 1);
	val = std::clamp(val, minValue(), maxValue());
	// change sample shape
	m_samples[x] = val;
}


} // namespace lmms
