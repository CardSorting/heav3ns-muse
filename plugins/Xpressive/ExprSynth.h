/*
 * exprfront.h - header file to a Frontend to ExprTk
 *
 * Copyright (c) 2016-2017 Orr Dvori
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

#ifndef EXPRSYNTH_H
#define EXPRSYNTH_H

#include <cmath>
#include <array>
#include <cstddef>
#include <limits>
#include "GraphModel.h"
#include "Plugin.h"

namespace lmms
{


class ExprFrontData;
class FloatModel;
class NotePlayHandle;
class SampleFrame;


class ExprFront
{
public:
	using ff1data_functor = float (*)(void*, float);
	ExprFront(const char* expr, int last_func_samples);
	~ExprFront();
	bool compile();
	const QString& error() const { return m_error; }
	inline bool isValid() { return m_valid; }
	float evaluate();
	bool add_variable(const char* name, float & ref);
	bool add_constant(const char* name, float  ref);
	bool add_cyclic_vector(const char* name, const float* data, size_t length, bool interp = false);
	void setIntegrate(const unsigned int* frameCounter, unsigned int sample_rate);
	ExprFrontData* getData() { return m_data; }
private:
	ExprFrontData *m_data = nullptr;
	bool m_valid;
	QString m_error;
	
	static const int max_float_integer_mask=(1<<(std::numeric_limits<float>::digits))-1;

};

// The native editor preview context. Output playback still uses each note's
// real time, rate, frequency, release state and live parameters in ExprSynth.
struct XpressiveExpressionContext
{
	bool output = false;
	int length = 4096;
	int sampleRate = 4096;
	float frequency = 10;
	float key = 5;
	float baseNote = 69;
	float volume = .5f;
	float tempo = 120;
	std::array<float, 3> parameters{1, 1, 1};
	std::array<const graphModel*, 3> waves{};
	const graphModel* currentWave = nullptr;
	std::array<bool, 3> interpolate{};
	bool consumeRandom = false;
};

Plugin::ExpressionResult previewXpressiveExpression(const QString& source, const XpressiveExpressionContext& context);

class WaveSample
{
public:
	WaveSample(int length)
	{
		m_length = length;
		m_samples = new float[m_length];
		for(int i = 0 ; i < m_length ; ++i)
			m_samples[i] = 0;
	}
	WaveSample(const graphModel * graph)
	{
		m_length = graph->length();
		m_samples = new float[m_length];
		memcpy(m_samples, graph->samples(), m_length * sizeof(float));
	}
	inline void copyFrom(const graphModel * graph)
	{
		memcpy(m_samples, graph->samples(), m_length * sizeof(float));
	}
	~WaveSample()
	{
		delete [] m_samples;
	}
	inline void setInterpolate(bool _interpolate) { m_interpolate = _interpolate; }
	float *m_samples;
	int m_length;
	bool m_interpolate;
};

class ExprSynth
{
public:
	ExprSynth(const WaveSample* gW1, const WaveSample* gW2, const WaveSample* gW3, ExprFront* exprO1, ExprFront* exprO2, NotePlayHandle* nph,
			const sample_rate_t sample_rate, const FloatModel* pan1, const FloatModel* pan2, float rel_trans);
	virtual ~ExprSynth();

	void renderOutput(f_cnt_t frames, SampleFrame* buf );


private:
	ExprFront *m_exprO1, *m_exprO2;
	const WaveSample *m_W1, *m_W2, *m_W3;
	unsigned int m_note_sample;
	unsigned int m_note_rel_sample;
	float m_note_sample_sec;
	float m_note_rel_sec;
	float m_frequency;
	float m_released;
	NotePlayHandle* m_nph;
	const sample_rate_t m_sample_rate;
	const FloatModel *m_pan1,*m_pan2;
	float m_rel_transition;
	float m_rel_inc;

} ;



inline float positiveFraction(float x)
{
	if (!std::isfinite(x)) { return 0; }
	const float wrapped = x - std::floor(x);
	// Very small negative values can round up to exactly one. Avoid both an
	// out-of-range endpoint and integer conversions for arbitrarily large t*f.
	return wrapped < 1.f ? wrapped : 0.f;
}

template <typename T>
inline void clearArray(T* arr,unsigned int size)
{
	const T* const arr_end = arr + size;
	while(arr < arr_end)
	{
		*arr=0;
		++arr;
	}
}


} // namespace lmms

#endif
