/*
 * Xpressive.cpp - Instrument which uses a mathematical formula parser
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

#include "Xpressive.h"

#include <QDomElement>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QThread>

#include "AudioEngine.h"
#include "Engine.h"
#include "Graph.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "Knob.h"
#include "LedCheckBox.h"
#include "MainWindow.h"
#include "NotePlayHandle.h"
#include "PixmapButton.h"
#include "Song.h"

#include "base64.h"
#include "embed.h"

#include "ExprSynth.h"

#include "plugin_export.h"

namespace lmms
{


extern "C" {

Plugin::Descriptor PLUGIN_EXPORT xpressive_plugin_descriptor = { LMMS_STRINGIFY(
	PLUGIN_NAME), "Xpressive", QT_TRANSLATE_NOOP("PluginBrowser",
	"Mathematical expression parser"), "Orr Dvori", 0x0100,
	Plugin::Type::Instrument, new PluginPixmapLoader("logo"), nullptr, nullptr };

}



/*
 * nice test:
O1 -> trianglew(2t*f)*(0.5+0.5sinew(12*A1*t+0.5))+sinew(t*f)*(0.5+0.5sinew(12*A1*t))
O2 -> trianglew(2t*f)*(0.5+0.5sinew(12*A1*t))+sinew(t*f)*(0.5+0.5sinew(12*A1*t+0.5))
*/


/***********************************************************************
 *
 *	class Xpressive
 *
 *	lmms - plugin
 *
 ***********************************************************************/
#define GRAPH_LENGTH 4096

namespace
{
const QStringList expressionKeys{"w1", "w2", "w3", "o1", "o2"};
}

Xpressive::Xpressive(InstrumentTrack* instrument_track) :
	Instrument(instrument_track, &xpressive_plugin_descriptor),
	m_graphO1(-1.0f, 1.0f, 360, this),
	m_graphO2(-1.0f, 1.0f, 360, this),
	m_graphW1(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_graphW2(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_graphW3(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_rawgraphW1(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_rawgraphW2(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_rawgraphW3(-1.0f, 1.0f, GRAPH_LENGTH, this),
	m_selectedGraph(0, 0, 6, this, tr("Selected graph")),
	m_parameterA1(1, -1.0f, 1.0f, 0.01f, this, tr("A1")),
	m_parameterA2(1, -1.0f, 1.0f, 0.01f, this, tr("A2")),
	m_parameterA3(1, -1.0f, 1.0f, 0.01f, this, tr("A3")),
	m_smoothW1(0, 0.0f, 70.0f, 1.0f, this, tr("W1 smoothing")),
	m_smoothW2(0, 0.0f, 70.0f, 1.0f, this, tr("W2 smoothing")),
	m_smoothW3(0, 0.0f, 70.0f, 1.0f, this, tr("W3 smoothing")),
	m_interpolateW1(false, this),
	m_interpolateW2(false, this),
	m_interpolateW3(false, this),
	m_panning1( 1, -1.0f, 1.0f, 0.01f, this, tr("Panning 1")),
	m_panning2(-1, -1.0f, 1.0f, 0.01f, this, tr("Panning 2")),
	m_relTransition(50.0f, 0.0f, 500.0f, 1.0f, this, tr("Rel trans")),
	m_W1(GRAPH_LENGTH),
	m_W2(GRAPH_LENGTH),
	m_W3(GRAPH_LENGTH),
	m_exprValid(false, this)
{
	m_outputExpression[0]="sinew(integrate(f*(1+0.05sinew(12t))))*(2^(-(1.1+A2)*t)*(0.4+0.1(1+A3)+0.4sinew((2.5+2A1)t))^2)";
	m_outputExpression[1]="expw(integrate(f*atan(500t)*2/pi))*0.5+0.12";
	graphModel* raw[] = {&m_rawgraphW1, &m_rawgraphW2, &m_rawgraphW3};
	FloatModel* smoothing[] = {&m_smoothW1, &m_smoothW2, &m_smoothW3};
	for (int index = 0; index < 3; ++index)
	{
		connect(raw[index], &graphModel::samplesChanged, this, [this, index] { refreshWave(index); });
		connect(smoothing[index], &FloatModel::dataChanged, this, [this, index] { refreshWave(index); });
	}
}

void Xpressive::refreshWave(int index)
{
	if (index < 0 || index >= 3) { return; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	graphModel* raw[] = {&m_rawgraphW1, &m_rawgraphW2, &m_rawgraphW3};
	graphModel* processed[] = {&m_graphW1, &m_graphW2, &m_graphW3};
	FloatModel* smoothing[] = {&m_smoothW1, &m_smoothW2, &m_smoothW3};
	WaveSample* waves[] = {&m_W1, &m_W2, &m_W3};
	smooth(smoothing[index]->value(), raw[index], processed[index]);
	waves[index]->copyFrom(processed[index]);
}

void Xpressive::graphEdited(const QString& key)
{
	QScopedValueRollback<bool> changing(m_expressionChanging, true);
	for (int index = 0; index < 3; ++index)
	{
		if (key == expressionKeys[index]) { m_wavesExpression[index].clear(); emit expressionSourceChanged(key); }
	}
}

std::vector<Plugin::ExpressionResource> Xpressive::expressionResources() const
{
	const QStringList common{"t", "srate", "pi", "e", "seed", "sinew", "squarew", "trianglew", "saww", "moogsaww", "moogw",
		"expw", "expnw", "cent", "semitone", "rand", "randv", "randsv", "last", "integrate"};
	auto outputSymbols = common; outputSymbols << "f" << "key" << "bnote" << "v" << "tempo" << "rel" << "trel" << "A1" << "A2" << "A3" << "W1" << "W2" << "W3";
	std::vector<ExpressionResource> result;
	for (int index = 0; index < 5; ++index)
	{
		const bool output = index >= 3;
		result.push_back({expressionKeys[index], output ? tr("Output %1 expression").arg(index - 2) : tr("Wave %1 expression").arg(index + 1),
			QString::fromLatin1(output ? m_outputExpression[index - 3] : m_wavesExpression[index]), output ? "output" : "waveform",
			output ? outputSymbols : common,
			output ? "ExprTk scalar formula, e.g. W1(integrate(f)). Preview: 360 frames at 360 Hz, f=10, key=5, v=0.5, rel=trel=0; current tempo/base note/A1-A3 and W1-W3. Playback uses real note values. Empty text silences this output for new notes; edits affect new notes."
				: "ExprTk scalar formula, e.g. sinew(t). Generates 4096 native samples over t=[0,1), srate=4096, followed by native smoothing. Nonfinite generated samples become zero. Empty text retains the existing waveform."
		});
		result.back().semantics += " Standard ExprTk math functions are available; logic, assignment and control structures are disabled. Source is ASCII without NUL, at most 65536 characters.";
	}
	return result;
}

XpressiveExpressionContext Xpressive::expressionContext(int index, bool consumeRandom) const
{
	XpressiveExpressionContext context; context.output = index >= 3;
	context.length = context.sampleRate = context.output ? 360 : GRAPH_LENGTH;
	context.baseNote = instrumentTrack()->baseNote(); context.tempo = Engine::getSong()->getTempo();
	context.parameters = {m_parameterA1.value(), m_parameterA2.value(), m_parameterA3.value()};
	context.waves = {&m_graphW1, &m_graphW2, &m_graphW3};
	const graphModel* raw[]{&m_rawgraphW1, &m_rawgraphW2, &m_rawgraphW3};
	if (!context.output) { context.currentWave = raw[index]; }
	context.interpolate = {m_interpolateW1.value(), m_interpolateW2.value(), m_interpolateW3.value()};
	context.consumeRandom = consumeRandom; return context;
}

Plugin::ExpressionResult Xpressive::validateExpression(const QString& key, const QString& source)
{
	ExpressionResult result;
	if (QThread::currentThread() != thread() || m_expressionChanging) { result.error = "Native expression validation requires its owner thread outside an active edit"; return result; }
	const int index = expressionKeys.indexOf(key);
	if (index < 0) { result.error = "Unknown native Xpressive expression key"; return result; }
	QScopedValueRollback<bool> changing(m_expressionChanging, true);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	return previewXpressiveExpression(source, expressionContext(index));
}

bool Xpressive::setExpression(const QString& key, const QString& source, ExpressionResult& result)
{
	result = {};
	if (QThread::currentThread() != thread() || m_expressionChanging) { result.error = "Native expression editing requires its owner thread outside an active edit"; return false; }
	const int index = expressionKeys.indexOf(key);
	if (index < 0) { result.error = "Unknown native Xpressive expression key"; return false; }
	QScopedValueRollback<bool> changing(m_expressionChanging, true);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	result = previewXpressiveExpression(source, expressionContext(index, true));
	if (!result.valid) { return false; }
	const bool output = index >= 3;
	auto& stored = output ? m_outputExpression[index - 3] : m_wavesExpression[index];
	const auto previous = stored; stored = source.trimmed().isEmpty() ? QByteArray{} : source.toLatin1();
	graphModel* graphs[]{&m_rawgraphW1, &m_rawgraphW2, &m_rawgraphW3, &m_graphO1, &m_graphO2};
	// Empty waveform text releases formula ownership while preserving its
	// accepted samples. Empty output text represents a disabled oscillator.
	if ((output || !stored.isEmpty()) && !graphs[index]->setSamplesChecked(result.preview, result.error))
	{
		stored = previous; result.valid = false; return false;
	}
	m_exprValid.setValue(false);
	emit expressionSourceChanged(key);
	return true;
}

Plugin::ExpressionResult Xpressive::refreshExpressionPreview(int index)
{
	ExpressionResult result;
	if (QThread::currentThread() != thread() || m_expressionChanging) { result.error = "Native preview refresh requires its owner thread outside an active edit"; return result; }
	if (index < 0 || index >= 5) { result.error = "Unknown native Xpressive expression index"; return result; }
	const auto source = index >= 3 ? m_outputExpression[index - 3] : m_wavesExpression[index];
	result = validateExpression(expressionKeys[index], QString::fromLatin1(source));
	if (index >= 3)
	{
		const auto graph = index == 3 ? &m_graphO1 : &m_graphO2;
		if (result.valid) { QString error; graph->setSamplesChecked(result.preview, error); }
		else { graph->clear(); }
	}
	m_exprValid.setValue(!result.valid); return result;
}

void Xpressive::saveSettings(QDomDocument & _doc, QDomElement & _this) {

	// Save plugin version
	_this.setAttribute("version", "0.1");
	_this.setAttribute("O1", QString(m_outputExpression[0]));
	_this.setAttribute("O2", QString(m_outputExpression[1]));
	_this.setAttribute("W1", QString(m_wavesExpression[0]));
	// Save sample shape base64-encoded
	QString sampleString;
	base64::encode( (const char*)m_rawgraphW1.samples(),
		 m_rawgraphW1.length() * sizeof(float), sampleString );
	_this.setAttribute( "W1sample", sampleString );

	_this.setAttribute("W2", QString(m_wavesExpression[1]));
	base64::encode( (const char*)m_rawgraphW2.samples(),
		 m_rawgraphW2.length() * sizeof(float), sampleString );
	_this.setAttribute( "W2sample", sampleString );
	_this.setAttribute("W3", QString(m_wavesExpression[2]));
	base64::encode( (const char*)m_rawgraphW3.samples(),
		 m_rawgraphW3.length() * sizeof(float), sampleString );
	_this.setAttribute( "W3sample", sampleString );
	m_smoothW1.saveSettings(_doc,_this,"smoothW1");
	m_smoothW2.saveSettings(_doc,_this,"smoothW2");
	m_smoothW3.saveSettings(_doc,_this,"smoothW3");
	m_interpolateW1.saveSettings(_doc,_this,"interpolateW1");
	m_interpolateW2.saveSettings(_doc,_this,"interpolateW2");
	m_interpolateW3.saveSettings(_doc,_this,"interpolateW3");
	m_parameterA1.saveSettings(_doc,_this,"A1");
	m_parameterA2.saveSettings(_doc,_this,"A2");
	m_parameterA3.saveSettings(_doc,_this,"A3");
	m_panning1.saveSettings(_doc,_this,"PAN1");
	m_panning2.saveSettings(_doc,_this,"PAN2");
	m_relTransition.saveSettings(_doc,_this,"RELTRANS");

}

void Xpressive::loadSettings(const QDomElement & _this) {
	if (QThread::currentThread() != thread() || m_expressionChanging) { collectErrorForUI("Cannot load Xpressive during an active native expression edit"); return; }
	QScopedValueRollback<bool> changing(m_expressionChanging, true);

	m_outputExpression[0]=_this.attribute( "O1").toLatin1();
	m_outputExpression[1]=_this.attribute( "O2").toLatin1();
	m_wavesExpression[0]=_this.attribute( "W1").toLatin1();
	m_wavesExpression[1]=_this.attribute( "W2").toLatin1();
	m_wavesExpression[2]=_this.attribute( "W3").toLatin1();

	m_smoothW1.loadSettings(_this,"smoothW1");
	m_smoothW2.loadSettings(_this,"smoothW2");
	m_smoothW3.loadSettings(_this,"smoothW3");
	m_interpolateW1.loadSettings(_this,"interpolateW1");
	m_interpolateW2.loadSettings(_this,"interpolateW2");
	m_interpolateW3.loadSettings(_this,"interpolateW3");
	m_parameterA1.loadSettings(_this,"A1");
	m_parameterA2.loadSettings(_this,"A2");
	m_parameterA3.loadSettings(_this,"A3");
	m_panning1.loadSettings(_this,"PAN1");
	m_panning2.loadSettings(_this,"PAN2");
	m_relTransition.loadSettings(_this,"RELTRANS");

	m_rawgraphW1.loadSampleAttribute(_this, "W1sample");
	m_rawgraphW2.loadSampleAttribute(_this, "W2sample");
	m_rawgraphW3.loadSampleAttribute(_this, "W3sample");
	for (int index = 0; index < 3; ++index) { refreshWave(index); }
	for (const auto& key : expressionKeys) { emit expressionSourceChanged(key); }
}


QString Xpressive::nodeName() const {
	return (xpressive_plugin_descriptor.name);
}

void Xpressive::playNote(NotePlayHandle* nph, SampleFrame* working_buffer) {
	m_A1=m_parameterA1.value();
	m_A2=m_parameterA2.value();
	m_A3=m_parameterA3.value();

	if (!nph->m_pluginData) {

		auto exprO1 = new ExprFront(m_outputExpression[0].constData(),
			Engine::audioEngine()->outputSampleRate()); // give the "last" function a whole second
		auto exprO2 = new ExprFront(m_outputExpression[1].constData(), Engine::audioEngine()->outputSampleRate());

		auto init_expression_step1 = [this, nph](ExprFront* e) { //lambda function to init exprO1 and exprO2
			//add the constants and the variables to the expression.
			e->add_constant("key", nph->key());//the key that was pressed.
			e->add_constant("bnote", nph->instrumentTrack()->baseNote()); // the base note
			e->add_constant("srate", Engine::audioEngine()->outputSampleRate());// sample rate of the audio engine
			e->add_constant("v", nph->getVolume() / 255.0); //volume of the note.
			e->add_constant("tempo", Engine::getSong()->getTempo());//tempo of the song.
			e->add_variable("A1", m_A1);//A1,A2,A3: general purpose input controls.
			e->add_variable("A2", m_A2);
			e->add_variable("A3", m_A3);
		};
		init_expression_step1(exprO1);
		init_expression_step1(exprO2);

		m_W1.setInterpolate(m_interpolateW1.value());//set interpolation according to the user selection.
		m_W2.setInterpolate(m_interpolateW2.value());
		m_W3.setInterpolate(m_interpolateW3.value());
		nph->m_pluginData = new ExprSynth(&m_W1, &m_W2, &m_W3, exprO1, exprO2, nph,
				Engine::audioEngine()->outputSampleRate(), &m_panning1, &m_panning2, m_relTransition.value());
	}

	auto ps = static_cast<ExprSynth*>(nph->m_pluginData);
	const f_cnt_t frames = nph->framesLeftForCurrentPeriod();
	const f_cnt_t offset = nph->noteOffset();

	ps->renderOutput(frames, working_buffer + offset);
}

void Xpressive::deleteNotePluginData(NotePlayHandle* nph) {
	delete static_cast<ExprSynth *>(nph->m_pluginData);
}

gui::PluginView* Xpressive::instantiateView(QWidget* parent) {
	return (new gui::XpressiveView(this, parent));
}


void Xpressive::smooth(float smoothness,const graphModel * in,graphModel * out)
{
	out->setSamples(in->samples());
	if (smoothness>0)
	{
		const int guass_size = (int)(smoothness * 5) | 1;
		const int guass_center = guass_size/2;
		const float delta = smoothness;
		constexpr float inv_sqrt2pi = std::numbers::inv_sqrtpi_v<float> / std::numbers::sqrt2_v<float>;
		const float a = inv_sqrt2pi / delta;
		auto const guassian = new float[guass_size];
		float sum = 0.0f;
		float temp = 0.0f;
		for (int i = 0; i < guass_size; i++)
		{
			temp = (i - guass_center) / delta;
			sum += guassian[i] = a * std::exp(-0.5f * temp * temp);
		}
		for (int i = 0; i < guass_size; i++)
		{
			guassian[i] = guassian[i] / sum;
		}
		out->convolve(guassian, guass_size, guass_center);
		delete [] guassian;
	}
}


namespace gui
{


class XpressiveKnob: public Knob {
public:
	void setStyle()
	{
		setFixedSize(29, 29);
		setCenterPointX(14.5);
		setCenterPointY(14.5);
		setInnerRadius(4);
		setOuterRadius(9);
		setTotalAngle(300.0);
		setLineWidth(3);
	}
	XpressiveKnob(QWidget * _parent, const QString & _name) :
		Knob(KnobType::Styled, _parent,_name) {
		setStyle();
	}
	XpressiveKnob(QWidget * _parent) :
		Knob(KnobType::Styled, _parent) {
		setStyle();
	}

};


XpressiveView::XpressiveView(Instrument * _instrument, QWidget * _parent) :
	InstrumentViewFixedSize(_instrument, _parent)

{
	const int COL_KNOBS = 191;
	const int BASE_START = 2;
	const int ROW_KNOBSA1 = BASE_START;
	const int ROW_KNOBSA2 = BASE_START + 32;
	const int ROW_KNOBSA3 = BASE_START + 64;
	const int ROW_KNOBSP1 = BASE_START + 100;
	const int ROW_KNOBSP2 = BASE_START + 100 + 32;
	const int ROW_KNOBREL = BASE_START + 100 + 64;
	const int ROW_BTN = BASE_START + 85;
	const int ROW_WAVEBTN = BASE_START + 233 - 26;
	const int EXPR_TEXT_Y = BASE_START + 102;
	const int EXPR_TEXT_H = 90;

	setAutoFillBackground(true);
	QPalette pal;

	pal.setBrush(backgroundRole(), PLUGIN_NAME::getIconPixmap("artwork"));
	setPalette(pal);

	m_graph = new Graph(this, Graph::Style::Linear, 180, 81);
	m_graph->move(3, BASE_START + 1);
	m_graph->setAutoFillBackground(true);
	m_graph->setGraphColor(QColor(255, 255, 255));
	m_graph->setEnabled(false);

	m_graph->setToolTip(tr("Draw your own waveform here "
			"by dragging your mouse on this graph."));

	pal = QPalette();
	pal.setBrush(backgroundRole(), PLUGIN_NAME::getIconPixmap("wavegraph"));
	m_graph->setPalette(pal);

	m_w1Btn = new PixmapButton(this, nullptr);
	m_w1Btn->move(3, ROW_BTN);
	m_w1Btn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("w1_active"));
	m_w1Btn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("w1_inactive"));
	m_w1Btn->setToolTip(tr("Select oscillator W1"));

	m_w2Btn = new PixmapButton(this, nullptr);
	m_w2Btn->move(26, ROW_BTN);
	m_w2Btn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("w2_active"));
	m_w2Btn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("w2_inactive"));
	m_w2Btn->setToolTip(tr("Select oscillator W2"));

	m_w3Btn = new PixmapButton(this, nullptr);
	m_w3Btn->move(49, ROW_BTN);
	m_w3Btn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("w3_active"));
	m_w3Btn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("w3_inactive"));
	m_w3Btn->setToolTip(tr("Select oscillator W3"));

	m_o1Btn = new PixmapButton(this, nullptr);
	m_o1Btn->move(79, ROW_BTN);
	m_o1Btn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("o1_active"));
	m_o1Btn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("o1_inactive"));
	m_o1Btn->setToolTip(tr("Select output O1"));

	m_o2Btn = new PixmapButton(this, nullptr);
	m_o2Btn->move(101, ROW_BTN);
	m_o2Btn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("o2_active"));
	m_o2Btn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("o2_inactive"));
	m_o2Btn->setToolTip(tr("Select output O2"));

	m_helpBtn = new PixmapButton(this, nullptr);
	m_helpBtn->move(133, ROW_BTN);
	m_helpBtn->setActiveGraphic(PLUGIN_NAME::getIconPixmap("help_active"));
	m_helpBtn->setInactiveGraphic(PLUGIN_NAME::getIconPixmap("help_inactive"));
	m_helpBtn->setToolTip(tr("Open help window"));

	m_selectedGraphGroup = new AutomatableButtonGroup(this);
	m_selectedGraphGroup->addButton(m_w1Btn);
	m_selectedGraphGroup->addButton(m_w2Btn);
	m_selectedGraphGroup->addButton(m_w3Btn);
	m_selectedGraphGroup->addButton(m_o1Btn);
	m_selectedGraphGroup->addButton(m_o2Btn);

	auto e = castModel<Xpressive>();
	m_selectedGraphGroup->setModel(&e->selectedGraph());

	m_sinWaveBtn = new PixmapButton(this, tr("Sine wave"));
	m_sinWaveBtn->move(4, ROW_WAVEBTN);
	m_sinWaveBtn->setActiveGraphic(embed::getIconPixmap("sin_wave_active"));
	m_sinWaveBtn->setInactiveGraphic(embed::getIconPixmap("sin_wave_inactive"));
	m_sinWaveBtn->setToolTip(tr("Sine wave"));

	m_moogWaveBtn = new PixmapButton(this, tr("Moog-saw wave"));
	m_moogWaveBtn->move(4, ROW_WAVEBTN-14);
	m_moogWaveBtn->setActiveGraphic(
		embed::getIconPixmap( "moog_saw_wave_active" ) );
	m_moogWaveBtn->setInactiveGraphic(embed::getIconPixmap("moog_saw_wave_inactive"));
	m_moogWaveBtn->setToolTip(tr("Moog-saw wave"));

	m_expWaveBtn = new PixmapButton(this, tr("Exponential wave"));
	m_expWaveBtn->move(4 +14, ROW_WAVEBTN-14);
	m_expWaveBtn->setActiveGraphic(embed::getIconPixmap( "exp_wave_active" ) );
	m_expWaveBtn->setInactiveGraphic(embed::getIconPixmap( "exp_wave_inactive" ) );
	m_expWaveBtn->setToolTip(tr("Exponential wave"));

	m_sawWaveBtn = new PixmapButton(this, tr("Saw wave"));
	m_sawWaveBtn->move(4 + 14 * 2, ROW_WAVEBTN-14);
	m_sawWaveBtn->setActiveGraphic(embed::getIconPixmap("saw_wave_active"));
	m_sawWaveBtn->setInactiveGraphic(embed::getIconPixmap("saw_wave_inactive"));
	m_sawWaveBtn->setToolTip(tr("Saw wave"));

	m_usrWaveBtn = new PixmapButton(this, tr("User-defined wave"));
	m_usrWaveBtn->move(4 + 14 * 3, ROW_WAVEBTN-14);
	m_usrWaveBtn->setActiveGraphic(embed::getIconPixmap("usr_wave_active"));
	m_usrWaveBtn->setInactiveGraphic(embed::getIconPixmap("usr_wave_inactive"));
	m_usrWaveBtn->setToolTip(tr("User-defined wave"));

	m_triangleWaveBtn = new PixmapButton(this, tr("Triangle wave"));
	m_triangleWaveBtn->move(4 + 14, ROW_WAVEBTN);
	m_triangleWaveBtn->setActiveGraphic(
		embed::getIconPixmap("triangle_wave_active"));
	m_triangleWaveBtn->setInactiveGraphic(
			embed::getIconPixmap("triangle_wave_inactive"));
	m_triangleWaveBtn->setToolTip(tr("Triangle wave"));

	m_sqrWaveBtn = new PixmapButton(this, tr("Square wave"));
	m_sqrWaveBtn->move(4 + 14 * 2, ROW_WAVEBTN);
	m_sqrWaveBtn->setActiveGraphic(embed::getIconPixmap("square_wave_active"));
	m_sqrWaveBtn->setInactiveGraphic(
			embed::getIconPixmap("square_wave_inactive"));
	m_sqrWaveBtn->setToolTip(tr("Square wave"));

	m_whiteNoiseWaveBtn = new PixmapButton(this, tr("White noise"));
	m_whiteNoiseWaveBtn->move(4 + 14 * 3, ROW_WAVEBTN);
	m_whiteNoiseWaveBtn->setActiveGraphic(
		embed::getIconPixmap("white_noise_wave_active"));
	m_whiteNoiseWaveBtn->setInactiveGraphic(
			embed::getIconPixmap("white_noise_wave_inactive"));
	m_whiteNoiseWaveBtn->setToolTip(tr("White noise"));


	m_waveInterpolate  = new LedCheckBox("Interpolate", this, tr("WaveInterpolate"),
										 LedCheckBox::LedColor::Green);
	m_waveInterpolate->move(2, 230);

	m_expressionValidToggle = new LedCheckBox("", this, tr("ExpressionValid"),
											  LedCheckBox::LedColor::Red);
	m_expressionValidToggle->move(168, EXPR_TEXT_Y+EXPR_TEXT_H-2);
	m_expressionValidToggle->setEnabled( false );

	m_expressionEditor = new QPlainTextEdit(this);
	m_expressionEditor->move(3, EXPR_TEXT_Y);
	m_expressionEditor->resize(180, EXPR_TEXT_H);

	m_generalPurposeKnob[0] = new XpressiveKnob(this,"A1");
	m_generalPurposeKnob[0]->setHintText(tr("General purpose 1:"), "");
	m_generalPurposeKnob[0]->move(COL_KNOBS, ROW_KNOBSA1);

	m_generalPurposeKnob[1] = new XpressiveKnob(this,"A2");
	m_generalPurposeKnob[1]->setHintText(tr("General purpose 2:"), "");
	m_generalPurposeKnob[1]->move(COL_KNOBS, ROW_KNOBSA2);

	m_generalPurposeKnob[2] = new XpressiveKnob(this,"A3");
	m_generalPurposeKnob[2]->setHintText(tr("General purpose 3:"), "");
	m_generalPurposeKnob[2]->move(COL_KNOBS, ROW_KNOBSA3);

	m_panningKnob[0] = new XpressiveKnob(this,"O1 panning");
	m_panningKnob[0]->setHintText(tr("O1 panning:"), "");
	m_panningKnob[0]->move(COL_KNOBS, ROW_KNOBSP1);

	m_panningKnob[1] = new XpressiveKnob(this,"O2 panning");
	m_panningKnob[1]->setHintText(tr("O2 panning:"), "");
	m_panningKnob[1]->move(COL_KNOBS, ROW_KNOBSP2);

	m_relKnob = new XpressiveKnob(this,"Release transition");
	m_relKnob->setHintText(tr("Release transition:"), "ms");
	m_relKnob->move(COL_KNOBS, ROW_KNOBREL);



	m_smoothKnob=new Knob(KnobType::Styled, this, "Smoothness");
	m_smoothKnob->setFixedSize(25, 25);
	m_smoothKnob->setCenterPointX(12.5);
	m_smoothKnob->setCenterPointY(12.5);
	m_smoothKnob->setInnerRadius(4);
	m_smoothKnob->setOuterRadius(9);
	m_smoothKnob->setTotalAngle(280.0);
	m_smoothKnob->setLineWidth(3);
	m_smoothKnob->setHintText(tr("Smoothness"), "");
	m_smoothKnob->move(66, EXPR_TEXT_Y + EXPR_TEXT_H + 4);

	connect(m_generalPurposeKnob[0], SIGNAL(sliderMoved(float)), this,
			SLOT(expressionChanged()));
	connect(m_generalPurposeKnob[1], SIGNAL(sliderMoved(float)), this,
			SLOT(expressionChanged()));
	connect(m_generalPurposeKnob[2], SIGNAL(sliderMoved(float)), this,
			SLOT(expressionChanged()));

	connect(m_expressionEditor, SIGNAL(textChanged()), this,
			SLOT(expressionChanged()));
	connect(m_smoothKnob, SIGNAL(sliderMoved(float)), this,
			SLOT(smoothChanged()));
	connect(m_graph, SIGNAL(drawn()), this,
			SLOT(graphDrawn()));

	connect(m_sinWaveBtn, SIGNAL(clicked()), this, SLOT(sinWaveClicked()));
	connect(m_triangleWaveBtn, SIGNAL(clicked()), this,
			SLOT(triangleWaveClicked()));
	connect(m_expWaveBtn, SIGNAL(clicked()), this, SLOT(expWaveClicked()));
	connect(m_moogWaveBtn, SIGNAL(clicked()), this,
			SLOT(moogSawWaveClicked()));
	connect(m_sawWaveBtn, SIGNAL(clicked()), this, SLOT(sawWaveClicked()));
	connect(m_sqrWaveBtn, SIGNAL(clicked()), this, SLOT(sqrWaveClicked()));
	connect(m_whiteNoiseWaveBtn, SIGNAL(clicked()), this,
			SLOT(noiseWaveClicked()));
	connect(m_usrWaveBtn, SIGNAL(clicked()), this, SLOT(usrWaveClicked()));
	connect(m_helpBtn, SIGNAL(clicked()), this, SLOT(helpClicked()));

	connect(m_w1Btn, SIGNAL(clicked()), this, SLOT(updateLayout()));
	connect(m_w2Btn, SIGNAL(clicked()), this, SLOT(updateLayout()));
	connect(m_w3Btn, SIGNAL(clicked()), this, SLOT(updateLayout()));
	connect(m_o1Btn, SIGNAL(clicked()), this, SLOT(updateLayout()));
	connect(m_o2Btn, SIGNAL(clicked()), this, SLOT(updateLayout()));

	updateLayout();
}


void XpressiveView::expressionChanged()
{
	auto instrument = castModel<Xpressive>();
	const int index = m_selectedGraphGroup->model()->value();
	if (index < 0 || index >= expressionKeys.size()) { return; }
	Plugin::ExpressionResult result;
	const auto source = m_expressionEditor->toPlainText();
	const bool accepted = instrument->setExpression(expressionKeys[index], source, result);
	instrument->exprValid().setValue(!accepted);
	m_expressionEditor->setToolTip(result.error);
	if (m_wave_expr) { m_graph->setEnabled(m_smoothKnob->model()->value() == 0 && source.trimmed().isEmpty()); }
	if (accepted) { Engine::getSong()->setModified(); }
}

void XpressiveView::nativeExpressionChanged(const QString& key)
{
	const int index = m_selectedGraphGroup->model()->value();
	if (index < 0 || index >= expressionKeys.size() || expressionKeys[index] != key) { return; }
	const auto instrument = castModel<Xpressive>();
	const auto source = QString::fromLatin1(index >= 3 ? instrument->outputExpression(index - 3) : instrument->wavesExpression(index));
	if (m_expressionEditor->toPlainText() != source)
	{
		const QSignalBlocker blocker(m_expressionEditor); m_expressionEditor->setPlainText(source);
	}
	m_expressionEditor->setToolTip({});
	if (m_wave_expr) { m_graph->setEnabled(m_smoothKnob->model()->value() == 0 && source.isEmpty()); }
}

void XpressiveView::smoothChanged()
{

	auto e = castModel<Xpressive>();
	e->refreshWave(m_selectedGraphGroup->model()->value());
	Engine::getSong()->setModified();
	m_graph->setEnabled(m_smoothKnob->model()->value() == 0 && m_expressionEditor->toPlainText().size() == 0);
}

void XpressiveView::graphDrawn()
{
	m_raw_graph->setSamples(m_graph->model()->samples());
	auto e = castModel<Xpressive>();
	switch (m_selectedGraphGroup->model()->value()) {
	case W1_EXPR:
		e->W1().copyFrom(m_graph->model());
		break;
	case W2_EXPR:
		e->W2().copyFrom(m_graph->model());
		break;
	case W3_EXPR:
		e->W3().copyFrom(m_graph->model());
		break;
	}
	Engine::getSong()->setModified();
}

void XpressiveView::modelChanged() {
	auto b = castModel<Xpressive>();
	connect(b, &Xpressive::expressionSourceChanged, this, &XpressiveView::nativeExpressionChanged, Qt::UniqueConnection);

	m_expressionValidToggle->setModel( &b->exprValid() );
	m_generalPurposeKnob[0]->setModel( &b->parameterA1() );
	m_generalPurposeKnob[1]->setModel( &b->parameterA2() );
	m_generalPurposeKnob[2]->setModel( &b->parameterA3() );

	m_panningKnob[0]->setModel( &b->panning1() );
	m_panningKnob[1]->setModel( &b->panning2() );
	m_relKnob->setModel( &b->relTransition() );
	m_selectedGraphGroup->setModel( &b->selectedGraph() );

	updateLayout();
}

void XpressiveView::updateLayout() {
	auto e = castModel<Xpressive>();
	const QSignalBlocker editorSignals(m_expressionEditor);
	m_output_expr=false;
	m_wave_expr=false;
	switch (m_selectedGraphGroup->model()->value()) {
	case W1_EXPR:
		m_wave_expr=true;
		m_graph->setModel(&e->graphW1(), true);
		m_raw_graph=&(e->rawgraphW1());
		m_expressionEditor->setPlainText(e->wavesExpression(0));
		m_smoothKnob->setModel(&e->smoothW1());
		m_graph->setEnabled((e->smoothW1().value() == 0 && e->wavesExpression(0).size() == 0));
		m_waveInterpolate->setModel(&e->interpolateW1());
		m_smoothKnob->show();
		m_usrWaveBtn->show();
		m_waveInterpolate->show();
		break;
	case W2_EXPR:
		m_wave_expr=true;
		m_graph->setModel(&e->graphW2(), true);
		m_raw_graph=&(e->rawgraphW2());
		m_expressionEditor->setPlainText(e->wavesExpression(1));
		m_smoothKnob->setModel(&e->smoothW2());
		m_graph->setEnabled((e->smoothW2().value() == 0 && e->wavesExpression(1).size() == 0));
		m_waveInterpolate->setModel(&e->interpolateW2());
		m_smoothKnob->show();
		m_usrWaveBtn->show();
		m_waveInterpolate->show();
		break;
	case W3_EXPR:
		m_wave_expr=true;
		m_graph->setModel(&e->graphW3(), true);
		m_raw_graph=&(e->rawgraphW3());
		m_expressionEditor->setPlainText(e->wavesExpression(2));
		m_smoothKnob->setModel(&e->smoothW3());
		m_graph->setEnabled((e->smoothW3().value() == 0 && e->wavesExpression(2).size() == 0));
		m_waveInterpolate->setModel(&e->interpolateW3());
		m_smoothKnob->show();
		m_usrWaveBtn->show();
		m_waveInterpolate->show();
		break;
	case O1_EXPR:
		m_output_expr=true;
		m_graph->setModel(&e->graphO1(), true);
		m_raw_graph=&(e->graphO1());
		m_expressionEditor->setPlainText(e->outputExpression(0));
		m_smoothKnob->hide();
		m_graph->setEnabled(false);
		m_usrWaveBtn->hide();
		m_waveInterpolate->hide();
		break;
	case O2_EXPR:
		m_output_expr=true;
		m_graph->setModel(&e->graphO2(), true);
		m_raw_graph=&(e->graphO2());
		m_expressionEditor->setPlainText(e->outputExpression(1));
		m_smoothKnob->hide();
		m_graph->setEnabled(false);
		m_usrWaveBtn->hide();
		m_waveInterpolate->hide();
		break;
	}
	const auto preview = e->refreshExpressionPreview(m_selectedGraphGroup->model()->value());
	m_expressionEditor->setToolTip(preview.error);
}

void XpressiveView::sinWaveClicked() {
	if (m_output_expr)
		m_expressionEditor->appendPlainText("sinew(integrate(f))");
	else
		m_expressionEditor->appendPlainText("sinew(t)");
	Engine::getSong()->setModified();
}

void XpressiveView::triangleWaveClicked() {
	if (m_output_expr)
		m_expressionEditor->appendPlainText("trianglew(integrate(f))");
	else
		m_expressionEditor->appendPlainText("trianglew(t)");
	Engine::getSong()->setModified();
}

void XpressiveView::sawWaveClicked() {
	if (m_output_expr)
		m_expressionEditor->appendPlainText("saww(integrate(f))");
	else
		m_expressionEditor->appendPlainText("saww(t)");
	Engine::getSong()->setModified();
}

void XpressiveView::sqrWaveClicked() {
	if (m_output_expr)
		m_expressionEditor->appendPlainText("squarew(integrate(f))");
	else
		m_expressionEditor->appendPlainText("squarew(t)");
	Engine::getSong()->setModified();
}

void XpressiveView::noiseWaveClicked() {
	m_expressionEditor->appendPlainText("randsv(t*srate,0)");
	Engine::getSong()->setModified();
}

void XpressiveView::moogSawWaveClicked()
{
	if (m_output_expr)
		m_expressionEditor->appendPlainText("moogsaww(integrate(f))");
	else
		m_expressionEditor->appendPlainText("moogsaww(t)");
	Engine::getSong()->setModified();
}
void XpressiveView::expWaveClicked()
{
	if (m_output_expr)
		m_expressionEditor->appendPlainText("expw(integrate(f))");
	else
		m_expressionEditor->appendPlainText("expw(t)");
	Engine::getSong()->setModified();
}

void XpressiveView::usrWaveClicked() {
	m_expressionEditor->setPlainText("");
	QString fileName = Graph::loadUserWaveform(m_raw_graph);
	smoothChanged();
	Engine::getSong()->setModified();
}

QString XpressiveHelpView::s_helpText=
"<b>O1, O2</b> - Two output waves. Panning is controlled by PN1 and PN2.<br>"
"<b>W1, W2, W3</b> - Wave samples evaluated by expression. In these samples, t variable ranges [0,1).<br>"
"These waves can be used as functions inside the output waves (O1, O2). The wave period is 1.<br>"
"<h4>Available variables:</h4><br>"
"<b>t</b> - Time in seconds.<br>"
"<b>f</b> - Note's pitched frequency. Available only in the output expressions.<br>"
"<b>key</b> - Note's keyboard key. 0 denotes C-1, 60 denotes C4, 127 denotes G9. Available only in the output expressions.<br>"
"<b>bnote</b> - Base note. By default it is 69 which means A4, unless you change it.<br>"
"<b>srate</b> - Sample rate. In wave expression it returns the wave's number of samples.<br>"
"<b>tempo</b> - Song's Tempo. Available only in the output expressions.<br>"
"<b>v</b> - Note's volume. Note that the output is already multiplied by the volume. Available only in the output expressions.<br>"
"<b>rel</b> - Gives 0.0 while the key is held, and 1.0 after the key release. Available only in the output expressions.<br>"
"<b>trel</b> - Time after release. While the note is held, it gives 0.0. Afterwards, it starts counting seconds.<br>"
"The time it takes to shift from 0.0 to 1.0 after key release is determined by the REL knob<br>"
"<b>seed</b> - A random value that remains consistent in the lifetime of a single wave. Meant to be used with <b>randsv</b><br>"
"<b>A1, A2, A3</b> - General purpose knobs. You can reference them only in O1 and O2. In range [-1,1].<br>"
"<h4>Available functions:</h4><br>"
"<b>W1, W2, W3</b> - As mentioned before. You can reference them only in O1 and O2.<br>"
"<b>cent(x)</b> - Gives pow(2,x/1200), so you can multiply it with the f variable to pitch the frequency.<br>"
"100 cents equals one semitone<br>"
"<b>semitone(x)</b> - Gives pow(2,x/12), so you can multiply it with the f variable to pitch the frequency.<br>"
"<b>last(n)</b> - Gives you the last n'th evaluated sample. In O1 and O2 it keeps a whole second. Thus the argument n must be in the range [1,srate], or else, it will return 0.<br>"
"<b>integrate(x)</b> - Integrates x by delta t (It sums values and divides them by sample rate).<br>"
"If you use notes with automated frequency, you should use:<br>"
"sinew(integrate(f)) instead of sinew(t*f)<br>"
"<b>randv(x)</b> - A random vector. Each cell is reference by an integer index in the range [0,2^31]<br>"
"Each evaluation of an expression results in different random vector.<br>"
"Although, it remains consistent in the lifetime of a single wave.<br>"
"If you want a single random values you can use randv(0),randv(1)... <br>"
"and every reference to randv(a) will give you the same value."
"If you want a random wave you can use randv(t*srate).<br>"
"Each random value is in the range [-1,1).<br>"
"<b>randsv(x,seed)</b> - works exactly like randv(x),<br>"
"except that it lets you to select the seed manualy,<br>"
"if you want to try different random values and make it consistent in each evaluation.<br>"
"<b>sinew(x)</b> - A sine wave with period of 1 (In contrast to real sine wave which have a period of 2*pi).<br>"
"<b>trianglew(x)</b> - A triangle wave with period of 1.<br>"
"<b>squarew(x)</b> - A square wave with period of 1.<br>"
"<b>saww(x)</b> - A saw wave with period of 1.<br>"
"<b>clamp(min_val,x,max_val)</b> - If x is in range of (min_val,max_val) it returns x. Otherwise if it's greater than max_val it returns max_val, else returns min_val.<br>"
"<b>abs, sin, cos, tan, cot, asin, acos, atan, atan2, sinh, cosh, tanh, asinh, acosh, atanh, sinc, "
"hypot, exp, log, log2, log10, logn, pow, sqrt, min, max, floor, ceil, round, trunc, frac, "
"avg, sgn, mod, etc. are also available.</b><br>"
"<b>Operands + - * / % ^ &gt; &lt; &gt;= &lt;= == != &amp; | are also available.</b><br>"
"<b>Amplitude Modulation</b> - W1(t*f)*(1+W2(t*f))<br>"
"<b>Ring Modulation</b> - W1(t * f)*W2(t * f)<br>"
"<b>Mix Modulation</b> - 0.5*( W1(t * f) + W2(t * f) )<br>"
"<b>Frequency Modulation</b> - [vol1]*W1( integrate( f + srate*[vol2]*W2( integrate(f) ) ) )<br>"
"<b>Phase Modulation</b> - [vol1]*W1( integrate(f) + [vol2]*W2( integrate(f) ) )<br>"
		;

XpressiveHelpView::XpressiveHelpView():QTextEdit(s_helpText)
{
	setWindowTitle ( "Xpressive Help" );
	setTextInteractionFlags ( Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse );
	getGUI()->mainWindow()->addWindowedWidget( this );
	parentWidget()->setAttribute( Qt::WA_DeleteOnClose, false );
	parentWidget()->setWindowIcon( PLUGIN_NAME::getIconPixmap( "logo" ) );
	parentWidget()->setFixedSize( 300, 500);
	
	// No maximize button
	Qt::WindowFlags flags = parentWidget()->windowFlags();
	flags &= ~Qt::WindowMaximizeButtonHint;
	parentWidget()->setWindowFlags( flags );
}

void XpressiveView::helpClicked() {
	XpressiveHelpView::getInstance()->show();

}


} // namespace gui

extern "C" {

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main(Model *m, void *) {
	return (new Xpressive(static_cast<InstrumentTrack *>(m)));
}

}


} // namespace lmms
