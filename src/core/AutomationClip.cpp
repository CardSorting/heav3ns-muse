/*
 * AutomationClip.cpp - implementation of class AutomationClip which
 *                         holds dynamic values
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2006-2008 Javier Serrano Polo <jasp00/at/users.sourceforge.net>
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

#include "AutomationClip.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <QCoreApplication>
#include <QThread>

#include "AutomationNode.h"
#include "AutomationClipView.h"
#include "AutomationTrack.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "KeyboardShortcuts.h"
#include "LocaleHelper.h"
#include "Note.h"
#include "PatternStore.h"
#include "ProjectJournal.h"
#include "Song.h"


namespace lmms
{

int AutomationClip::s_quantization = 1;
const float AutomationClip::DEFAULT_MIN_VALUE = 0;
const float AutomationClip::DEFAULT_MAX_VALUE = 1;


AutomationClip::AutomationClip( AutomationTrack * _auto_track ) :
	Clip( _auto_track ),
	m_autoTrack( _auto_track ),
	m_objects(),
	m_tension( 1.0 ),
	m_progressionType( ProgressionType::Discrete ),
	m_dragging( false ),
	m_isRecording( false ),
	m_lastRecordedValue( 0 )
{
	changeLength( TimePos( 1, 0 ) );
}




AutomationClip::AutomationClip(const AutomationClip& other) : AutomationClip(other, other.m_autoTrack) { }

AutomationClip::AutomationClip(const AutomationClip& _clip_to_copy, AutomationTrack* destination) :
	Clip(_clip_to_copy, destination),
	m_autoTrack(destination),
	m_idsToResolve(_clip_to_copy.m_idsToResolve),
	m_objects( _clip_to_copy.m_objects ),
	m_tension( _clip_to_copy.m_tension ),
	m_progressionType(_clip_to_copy.m_progressionType),
	m_dragging(false),
	m_isRecording(_clip_to_copy.m_isRecording),
	m_lastRecordedValue(0)
{
	// Locks the mutex of the copied AutomationClip to make sure it
	// doesn't change while it's being copied
	QMutexLocker m(&_clip_to_copy.m_clipMutex);

	for( timeMap::const_iterator it = _clip_to_copy.m_timeMap.begin();
				it != _clip_to_copy.m_timeMap.end(); ++it )
	{
		// Copies the automation node (in/out values and in/out tangents)
		m_timeMap[POS(it)] = it.value();
		// Sets the node's clip to this one
		m_timeMap[POS(it)].setClip(this);
	}
	for (const auto& object : m_objects)
	{
		if (object)
		{
			connect(object, SIGNAL(destroyed(lmms::jo_id_t)), this, SLOT(objectDestroyed(lmms::jo_id_t)),
				static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
		}
	}
}

bool AutomationClip::addObject( AutomatableModel * _obj, bool _search_dup )
{
	assert(_obj != nullptr);
	QMutexLocker m(&m_clipMutex);

	if (_search_dup && std::find(m_objects.begin(), m_objects.end(), _obj) != m_objects.end())
	{
		return false;
	}

	// Seed interactive attachment, but preserve a serialized empty curve while
	// reconnecting its deferred target IDs during project/checkpoint loading.
	if (m_objects.empty() && hasAutomation() == false && m_idsToResolve.empty())
	{
		// then initialize first value
		putValue( TimePos(0), _obj->inverseScaledValue( _obj->value<float>() ), false );
	}

	m_objects.push_back(_obj);

	connect( _obj, SIGNAL(destroyed(lmms::jo_id_t)),
			this, SLOT(objectDestroyed(lmms::jo_id_t)),
			static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));

	emit dataChanged();

	return true;
}

bool AutomationClip::removeObject(AutomatableModel* object)
{
	if (!object) { return false; }
	QMutexLocker lock(&m_clipMutex);
	const auto size = m_objects.size();
	m_objects.erase(std::remove(m_objects.begin(), m_objects.end(), object), m_objects.end());
	const auto id = object->id();
	m_idsToResolve.erase(std::remove_if(m_idsToResolve.begin(), m_idsToResolve.end(), [id](jo_id_t pending) {
		return pending == id || ProjectJournal::idFromSave(pending) == id || ProjectJournal::idToSave(pending) == id;
	}), m_idsToResolve.end());
	if (size == m_objects.size()) { return false; }
	disconnect(object, SIGNAL(destroyed(lmms::jo_id_t)), this, SLOT(objectDestroyed(lmms::jo_id_t)));
	emit dataChanged();
	return true;
}

void AutomationClip::clearObjects()
{
	QMutexLocker lock(&m_clipMutex);
	for (const auto& object : m_objects)
	{
		if (object) { disconnect(object, SIGNAL(destroyed(lmms::jo_id_t)), this, SLOT(objectDestroyed(lmms::jo_id_t))); }
	}
	m_objects.clear();
	m_idsToResolve.clear();
	emit dataChanged();
}

void AutomationClip::bindInlineObject(AutomatableModel* model)
{
	QMutexLocker lock(&m_clipMutex);
	for (const auto& object : m_objects)
	{ if (object) { disconnect(object, SIGNAL(destroyed(lmms::jo_id_t)), this, SLOT(objectDestroyed(lmms::jo_id_t))); } }
	m_objects.clear(); m_idsToResolve.clear(); m_objects.push_back(model);
	connect(model, SIGNAL(destroyed(lmms::jo_id_t)), this, SLOT(objectDestroyed(lmms::jo_id_t)), Qt::DirectConnection);
	// Rebinding must preserve an empty native curve instead of inserting a point.
	emit dataChanged();
}




void AutomationClip::setProgressionType(
					ProgressionType _new_progression_type )
{
	QMutexLocker m(&m_clipMutex);

	if ( _new_progression_type == ProgressionType::Discrete ||
		_new_progression_type == ProgressionType::Linear ||
		_new_progression_type == ProgressionType::CubicHermite )
	{
		m_progressionType = _new_progression_type;
		emit dataChanged();
	}
}




void AutomationClip::setTension( QString _new_tension )
{
	QMutexLocker m(&m_clipMutex);

	bool ok;
	float nt = LocaleHelper::toFloat(_new_tension, & ok);

	if (ok) { setTension(nt); }
}

void AutomationClip::setTension(float tension)
{
	QMutexLocker lock(&m_clipMutex);
	if (std::isfinite(tension) && tension > -0.01f && tension < 1.01f && tension != m_tension)
	{
		m_tension = tension;
		emit dataChanged();
	}
}




const AutomatableModel * AutomationClip::firstObject() const
{
	QMutexLocker m(&m_clipMutex);

	AutomatableModel* model;
	if (!m_objects.empty() && (model = m_objects.front()) != nullptr)
	{
		return model;
	}

	static FloatModel fm(0, DEFAULT_MIN_VALUE, DEFAULT_MAX_VALUE, 0.001f);
	return &fm;
}

const AutomationClip::objectVector& AutomationClip::objects() const
{
	QMutexLocker m(&m_clipMutex);

	return m_objects;
}




TimePos AutomationClip::timeMapLength() const
{
	QMutexLocker m(&m_clipMutex);

	TimePos one_bar = TimePos(1, 0);
	if (m_timeMap.isEmpty()) { return one_bar; }

	timeMap::const_iterator it = m_timeMap.end();
	auto last_tick = static_cast<tick_t>(POS(std::prev(it)));
	// if last_tick is 0 (single item at tick 0)
	// return length as a whole bar to prevent disappearing Clip
	if (last_tick == 0) { return one_bar; }

	return TimePos(last_tick);
}




void AutomationClip::updateLength()
{
	// Technically it only matters if the clip has been resized from the right, but this
	// checks if it has been resized from either direction.
	if (getAutoResize())
	{
		if (m_autoTrack != nullptr && m_autoTrack->trackContainer() == Engine::patternStore())
		{
			// If inside a pattern, the clip is always the length of it.
			changeLength(TimePos::ticksPerBar() * Engine::patternStore()->lengthOfPattern(m_autoTrack->getClipNum(this)));
			return;
		}
		// Using 1 bar as the min length for an un-resized clip.
		// This does not prevent the user from resizing the clip to be less than a bar later on.
		changeLength(std::max(TimePos::ticksPerBar(), static_cast<tick_t>(timeMapLength())));
		setStartTimeOffset(TimePos(0));
	}
}




TimePos AutomationClip::putValue(
	const TimePos & time,
	const float value,
	const bool quantPos,
	const bool ignoreSurroundingPoints
)
{
	QMutexLocker m(&m_clipMutex);

	cleanObjects();

	TimePos newTime = quantPos ? Note::quantized(time, quantization()) : time;
	newTime = std::max(TimePos(0), newTime);

	// Create a node or replace the existing one on newTime
	m_timeMap[newTime] = AutomationNode(this, value, newTime);

	timeMap::iterator it = m_timeMap.find(newTime);

	// Remove control points that are covered by the new points
	// quantization value. Control Key to override
	if (!ignoreSurroundingPoints)
	{
		// We need to check that to avoid removing nodes from
		// newTime + 1 to newTime (removing the node we are adding)
		if (quantization() > 1)
		{
			// Remove nodes between the quantization points, them not
			// being included
			removeNodes(newTime + 1, newTime + quantization() - 1);
		}
	}
	if (it != m_timeMap.begin()) { --it; }
	generateTangents(it, 3);

	updateLength();

	emit dataChanged();

	return newTime;
}




TimePos AutomationClip::putValues(
	const TimePos & time,
	const float inValue,
	const float outValue,
	const bool quantPos,
	const bool ignoreSurroundingPoints
)
{
	QMutexLocker m(&m_clipMutex);

	cleanObjects();

	TimePos newTime = quantPos ? Note::quantized(time, quantization()) : time;
	newTime = std::max(TimePos(0), newTime);

	// Create a node or replace the existing one on newTime
	m_timeMap[newTime] = AutomationNode(this, inValue, outValue, newTime);

	timeMap::iterator it = m_timeMap.find(newTime);

	// Remove control points that are covered by the new points
	// quantization value. Control Key to override
	if (!ignoreSurroundingPoints)
	{
		// We need to check that to avoid removing nodes from
		// newTime + 1 to newTime (removing the node we are adding)
		if (quantization() > 1)
		{
			// Remove nodes between the quantization points, them not
			// being included
			removeNodes(newTime + 1, newTime + quantization() - 1);
		}
	}
	if (it != m_timeMap.begin()) { --it; }
	generateTangents(it, 3);

	updateLength();

	emit dataChanged();

	return newTime;
}




void AutomationClip::removeNode(const TimePos & time)
{
	QMutexLocker m(&m_clipMutex);

	cleanObjects();

	m_timeMap.remove( time );
	timeMap::iterator it = m_timeMap.lowerBound(time);
	if( it != m_timeMap.begin() )
	{
		--it;
	}
	generateTangents(it, 3);

	updateLength();

	emit dataChanged();
}




void AutomationClip::removeNodes(const int tick0, const int tick1)
{
	if (tick0 == tick1)
	{
		removeNode(TimePos(tick0));
		return;
	}

	auto start = TimePos(std::min(tick0, tick1));
	auto end = TimePos(std::max(tick0, tick1));

	// Make a list of TimePos with nodes to be removed
	// because we can't simply remove the nodes from
	// the timeMap while we are iterating it.
	std::vector<TimePos> nodesToRemove;

	for (auto it = m_timeMap.lowerBound(start), endIt = m_timeMap.upperBound(end); it != endIt; ++it)
	{
		nodesToRemove.push_back(POS(it));
	}

	for (auto node: nodesToRemove)
	{
		removeNode(node);
	}
}




void AutomationClip::resetNodes(const int tick0, const int tick1)
{
	if (tick0 == tick1)
	{
		auto it = m_timeMap.find(TimePos(tick0));
		if (it != m_timeMap.end()) { it.value().resetOutValue(); }
		return;
	}

	auto start = TimePos(std::min(tick0, tick1));
	auto end = TimePos(std::max(tick0, tick1));

	for (auto it = m_timeMap.lowerBound(start), endIt = m_timeMap.upperBound(end); it != endIt; ++it)
	{
		it.value().resetOutValue();
	}
}




void AutomationClip::resetTangents(const int tick0, const int tick1)
{
	if (tick0 == tick1)
	{
		auto it = m_timeMap.find(TimePos(tick0));
		if (it != m_timeMap.end())
		{
			it.value().setLockedTangents(false);
			generateTangents(it, 1);
		}
		return;
	}

	TimePos start = TimePos(std::min(tick0, tick1));
	TimePos end = TimePos(std::max(tick0, tick1));

	for (auto it = m_timeMap.lowerBound(start), endIt = m_timeMap.upperBound(end); it != endIt; ++it)
	{
		it.value().setLockedTangents(false);
		generateTangents(it, 1);
	}
}




bool AutomationClip::setNodeTangents(const TimePos& time, float inTangent, float outTangent, bool locked)
{
	if (!std::isfinite(inTangent) || !std::isfinite(outTangent)) { return false; }
	QMutexLocker lock(&m_clipMutex);
	auto node = m_timeMap.find(time);
	if (node == m_timeMap.end()) { return false; }
	node->setLockedTangents(locked);
	if (locked)
	{
		node->setInTangent(inTangent);
		node->setOutTangent(outTangent);
	}
	else { generateTangents(node, 1); }
	emit dataChanged();
	return true;
}

struct AutomationClip::RecordingSource
{
	struct Binding
	{
		QPointer<AutomatableModel> model;
		AutomatableModel* identity;
		float minimum, maximum, step;
		AutomatableModel::ScaleType scale;
		bool supportsAutomation;
	};
	timeMap points;
	std::vector<Binding> bindings;
	std::vector<jo_id_t> pendingIds;
	QPointer<Track> owner;
	TimePos position, size, offset;
	ProgressionType progression;
	float tension;
	int meter;
	bool automatic;
};

bool AutomationClip::recordingSourceMatches(const RecordingSource& source, bool contents) const
{
	if ((contents && m_timeMap != source.points) || m_idsToResolve != source.pendingIds
		|| m_objects.size() != source.bindings.size() || getTrack() != source.owner.data()
		|| startPosition() != source.position || length() != source.size || startTimeOffset() != source.offset
		|| getAutoResize() != source.automatic || m_progressionType != source.progression || m_tension != source.tension
		|| TimePos::ticksPerBar() != source.meter) { return false; }
	for (std::size_t index = 0; index < source.bindings.size(); ++index)
	{
		const auto& binding = source.bindings[index]; const auto model = binding.model.data();
		if (model != binding.identity || m_objects[index].data() != model) { return false; }
		if (model && (model->minValue<float>() != binding.minimum || model->maxValue<float>() != binding.maximum
			|| model->step<float>() != binding.step || model->scaleType() != binding.scale
			|| model->supportsAutomation() != binding.supportsAutomation)) { return false; }
	}
	return !source.owner || std::find(source.owner->getClips().begin(), source.owner->getClips().end(), this) != source.owner->getClips().end();
}

std::shared_ptr<AutomationClip::RecordingSource> AutomationClip::captureSource() const
{
	auto source = std::make_shared<RecordingSource>();
	// Caller holds the clip mutex. Native callers can retain mutable node
	// references, so the baseline must own an independent tree.
	source->points = m_timeMap; source->points.detach();
	source->pendingIds = m_idsToResolve; source->owner = getTrack();
	source->position = startPosition(); source->size = length(); source->offset = startTimeOffset();
	source->automatic = getAutoResize(); source->progression = m_progressionType; source->tension = m_tension; source->meter = TimePos::ticksPerBar();
	source->bindings.reserve(m_objects.size());
	for (const auto& model : m_objects)
	{
		source->bindings.push_back({model, model.data(), model ? model->minValue<float>() : 0.f,
			model ? model->maxValue<float>() : 0.f, model ? model->step<float>() : 0.f,
			model ? model->scaleType() : AutomatableModel::ScaleType::Linear, model && model->supportsAutomation()});
	}
	return source;
}

bool AutomationClip::editCurve(const CurveEdit& input, bool& changed, QString& error)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject())
		{ throw std::invalid_argument("Native curve editing requires a ready project on the model owner thread"); }
		const CurveEdit options = input;
		const bool updateGeometry = options.point.has_value() || options.removePoint.has_value();
		if (int(options.point.has_value()) + int(options.removePoint.has_value()) + int(options.clear) > 1)
		{ throw std::invalid_argument("Choose one native curve point, removal or clear action"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const QPointer<AutomationClip> receiver(this);
		std::shared_ptr<RecordingSource> source;
		{
			QMutexLocker lock(&m_clipMutex);
			if (m_isRecording || m_recordingTake || m_dragging) { throw std::invalid_argument("Complete native recording or curve dragging before editing its curve"); }
			if (TimePos::ticksPerBar() <= 0) { throw std::invalid_argument("Native curve editing requires a positive meter"); }
			if (options.progression && (*options.progression < ProgressionType::Discrete || *options.progression > ProgressionType::CubicHermite))
			{ throw std::invalid_argument("Invalid native curve progression"); }
			if (options.tension && (!std::isfinite(*options.tension) || *options.tension < 0 || *options.tension > 1))
			{ throw std::invalid_argument("Invalid native curve tension"); }
			if (options.point)
			{
				const auto& point = *options.point; const auto model = firstObject();
				if (point.position < 0 || point.position > MaxSongLength || !std::isfinite(point.value) || !std::isfinite(point.outValue)
					|| point.value < model->minValue<float>() || point.value > model->maxValue<float>()
					|| point.outValue < model->minValue<float>() || point.outValue > model->maxValue<float>()
					|| (point.inTangent && !std::isfinite(*point.inTangent)) || (point.outTangent && !std::isfinite(*point.outTangent))
					|| (point.locked && !*point.locked && (point.inTangent || point.outTangent)))
				{ throw std::invalid_argument("Invalid native curve point or tangents"); }
			}
			source = captureSource();
		}
		const auto validate = [&](bool contents) {
			if (!receiver) { throw std::runtime_error("Native curve retired during edit preparation"); }
			QMutexLocker lock(&receiver->m_clipMutex);
			if (receiver->m_isRecording || receiver->m_recordingTake || receiver->m_dragging || !receiver->recordingSourceMatches(*source, contents))
			{ throw std::runtime_error("Native curve changed during edit preparation"); }
		};
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } validate(false); };
		check();
		const auto progression = options.progression.value_or(source->progression); const auto tension = options.tension.value_or(source->tension);
		if (!updateGeometry && !options.clear && progression == source->progression && tension == source->tension) { validate(true); error.clear(); return true; }
		std::size_t count = 0;
		if (updateGeometry)
		{
			for (auto it = source->points.cbegin(); it != source->points.cend(); ++it)
			{
				if (count++ % 256 == 0) { check(); }
				if (options.removePoint && it.key() == *options.removePoint) { continue; }
				if (it.key() < 0 || it.key() > MaxSongLength) { throw std::invalid_argument("Native curve source points exceed the supported tick range"); }
			}
		}
		auto prepared = std::make_unique<AutomationClip>(nullptr); validate(false); prepared->setAutoResize(false);
		prepared->m_timeMap = source->points; prepared->m_progressionType = progression; prepared->m_tension = tension;
		count = 0;
		for (auto it = prepared->m_timeMap.begin(); it != prepared->m_timeMap.end(); ++it)
		{ if (count++ % 256 == 0) { check(); } it.value().setClip(prepared.get()); }
		if (options.point)
		{
			const auto& point = *options.point; prepared->putValues(point.position, point.value, point.outValue, false, true);
			if (point.locked || point.inTangent || point.outTangent)
			{
				const auto node = prepared->m_timeMap.value(point.position);
				if (!prepared->setNodeTangents(point.position, point.inTangent.value_or(node.getInTangent()),
					point.outTangent.value_or(node.getOutTangent()), point.locked.value_or(true)))
				{ throw std::runtime_error("Native curve tangent preparation failed"); }
			}
		}
		else if (options.removePoint) { prepared->removeNode(*options.removePoint); }
		else if (options.clear) { prepared->clear(); }
		count = 0;
		for (auto it = prepared->m_timeMap.begin(); it != prepared->m_timeMap.end(); ++it)
		{
			if (count++ % 256 == 0) { check(); }
			const auto& node = it.value();
			if (updateGeometry && (!std::isfinite(node.getInValue()) || !std::isfinite(node.getOutValue())
				|| !std::isfinite(node.getInTangent()) || !std::isfinite(node.getOutTangent())))
			{ throw std::invalid_argument("Native curve preparation produced nonfinite node data"); }
			it.value().setClip(receiver.data());
		}
		check();
		auto expected = *source; expected.points = prepared->m_timeMap; expected.progression = progression; expected.tension = tension;
		if (updateGeometry)
		{
			expected.bindings.erase(std::remove_if(expected.bindings.begin(), expected.bindings.end(), [](const auto& binding) { return !binding.model; }), expected.bindings.end());
			if (source->automatic)
			{
				if (source->meter <= 0) { throw std::invalid_argument("Native automatic curve sizing requires a positive meter"); }
				if (source->owner && source->owner->trackContainer() == Engine::patternStore())
				{
					const auto ticks = static_cast<qint64>(Engine::patternStore()->lengthOfPattern(source->owner->getClipNum(receiver.data()))) * source->meter;
					if (ticks < 0 || ticks > MaxSongLength) { throw std::invalid_argument("Native pattern curve length exceeds the song range"); }
					expected.size = static_cast<int>(ticks);
				}
				else { expected.size = std::max(source->meter, prepared->timeMapLength().getTicks()); expected.offset = 0; }
			}
		}
		if (expected.position < 0 || expected.size < 0 || expected.position > MaxSongLength - expected.size
			|| expected.offset < -MaxSongLength || expected.offset > MaxSongLength)
		{ throw std::invalid_argument("Native curve geometry exceeds the song range"); }
		{
			QMutexLocker lock(&receiver->m_clipMutex); validate(true);
			receiver->m_timeMap.swap(prepared->m_timeMap); receiver->m_progressionType = progression; receiver->m_tension = tension;
			if (updateGeometry) { receiver->cleanObjects(); }
		}
		// Point writes/removals refresh native sizing; clear/settings retain it.
		if (updateGeometry) { receiver->updateLength(); }
		const auto result = [&] {
			if (!receiver) { throw std::runtime_error("Native curve retired during edit notification"); }
			QMutexLocker lock(&receiver->m_clipMutex);
			if (!receiver->recordingSourceMatches(expected, true) || receiver->m_recordingTake || receiver->m_isRecording || receiver->m_dragging)
			{ throw std::runtime_error("Native curve result changed during edit notification"); }
		};
		result(); emit receiver->dataChanged(); result(); changed = true; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native curve edit failed with a nonstandard exception"; }
	return false;
}

bool AutomationClip::editBinding(AutomatableModel* object, bool connected, bool& changed, QString& error)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject())
		{ throw std::invalid_argument("Native binding editing requires a ready project on the model owner thread"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const QPointer<AutomationClip> receiver(this); const QPointer<AutomatableModel> target(object);
		if (!target || target->thread() != QThread::currentThread()) { throw std::invalid_argument("Native automation target must be live on the model owner thread"); }
		const RecordingSource::Binding dimension{target, target.data(), target->minValue<float>(), target->maxValue<float>(),
			target->step<float>(), target->scaleType(), target->supportsAutomation()};
		if (!receiver || !target) { throw std::runtime_error("Native binding owner or target retired during admission"); }
		if (!std::isfinite(dimension.minimum) || !std::isfinite(dimension.maximum) || !std::isfinite(dimension.step)
			|| dimension.minimum > dimension.maximum || (connected && !dimension.supportsAutomation))
		{ throw std::invalid_argument("Native parameter does not support this automation binding"); }
		std::shared_ptr<RecordingSource> source;
		{
			QMutexLocker lock(&m_clipMutex);
			if (m_isRecording || m_recordingTake || m_dragging) { throw std::invalid_argument("Complete native recording or curve dragging before editing its bindings"); }
			if (TimePos::ticksPerBar() <= 0) { throw std::invalid_argument("Native binding editing requires a positive meter"); }
			source = captureSource();
		}
		const auto validateTarget = [&] {
			if (!target || target->thread() != QThread::currentThread() || target->minValue<float>() != dimension.minimum
				|| target->maxValue<float>() != dimension.maximum || target->step<float>() != dimension.step
				|| target->scaleType() != dimension.scale || target->supportsAutomation() != dimension.supportsAutomation)
			{ throw std::runtime_error("Native automation target changed during binding editing"); }
			if (!target) { throw std::runtime_error("Native automation target retired during binding validation"); }
		};
		const auto validate = [&](bool contents) {
			if (!receiver) { throw std::runtime_error("Native curve retired during binding preparation"); }
			QMutexLocker lock(&receiver->m_clipMutex);
			if (receiver->m_isRecording || receiver->m_recordingTake || receiver->m_dragging || !receiver->recordingSourceMatches(*source, contents))
			{ throw std::runtime_error("Native curve changed during binding preparation"); }
		};
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } validateTarget(); validate(false); };
		check();
		const bool bound = std::any_of(source->bindings.begin(), source->bindings.end(), [&](const auto& binding) { return binding.model == target; });
		if (connected && bound) { validate(true); error.clear(); return true; }
		auto expected = *source;
		if (connected) { expected.bindings.push_back(dimension); }
		else
		{
			expected.bindings.erase(std::remove_if(expected.bindings.begin(), expected.bindings.end(), [&](const auto& binding) { return binding.model == target; }), expected.bindings.end());
			const auto id = target->id();
			expected.pendingIds.erase(std::remove_if(expected.pendingIds.begin(), expected.pendingIds.end(), [id](jo_id_t pending) {
				return pending == id || ProjectJournal::idFromSave(pending) == id || ProjectJournal::idToSave(pending) == id;
			}), expected.pendingIds.end());
		}
		const bool membershipChanged = connected || bound;
		if (!membershipChanged && expected.pendingIds == source->pendingIds) { validate(true); error.clear(); return true; }
		const bool seed = connected && source->bindings.empty() && source->points.isEmpty();
		std::unique_ptr<AutomationClip> prepared;
		if (seed)
		{
			// Reading a controller-backed value can call native code. Hold neither
			// the receiver mutex nor a raw target across that call's return.
			const float value = target->value<float>(); validateTarget(); validate(true);
			const float initial = target->inverseScaledValue(value);
			if (!std::isfinite(initial)) { throw std::invalid_argument("Native initial automation value is nonfinite"); }
			prepared = std::make_unique<AutomationClip>(nullptr); check(); prepared->setAutoResize(false);
			prepared->putValue(0, initial, false, true); check();
			expected.points = prepared->m_timeMap;
			for (auto it = expected.points.begin(); it != expected.points.end(); ++it) { it.value().setClip(receiver.data()); }
			prepared->m_timeMap = expected.points;
			if (source->automatic)
			{
				if (source->owner && source->owner->trackContainer() == Engine::patternStore())
				{
					const auto ticks = static_cast<qint64>(Engine::patternStore()->lengthOfPattern(source->owner->getClipNum(receiver.data()))) * source->meter;
					if (ticks < 0 || ticks > MaxSongLength) { throw std::invalid_argument("Native pattern binding length exceeds the song range"); }
					expected.size = static_cast<int>(ticks);
				}
				else { expected.size = source->meter; expected.offset = 0; }
			}
		}
		if (expected.position < 0 || expected.size < 0 || expected.position > MaxSongLength - expected.size
			|| expected.offset < -MaxSongLength || expected.offset > MaxSongLength)
		{ throw std::invalid_argument("Native binding geometry exceeds the song range"); }
		objectVector objects; objects.reserve(expected.bindings.size());
		std::size_t count = 0;
		for (const auto& binding : expected.bindings) { if (count++ % 256 == 0) { check(); } objects.push_back(binding.model); }
		auto pending = expected.pendingIds;
		auto published = expected; published.size = source->size; published.offset = source->offset;
		check();
		{
			QMutexLocker lock(&receiver->m_clipMutex); validate(true);
			if (seed) { receiver->m_timeMap.swap(prepared->m_timeMap); }
			receiver->m_objects.swap(objects); receiver->m_idsToResolve.swap(pending);
		}
		// Native signal wiring and sizing can call out. Data is already complete,
		// and no curve mutex is held while those callbacks or final observers run.
		if (connected)
		{
			connect(target.data(), SIGNAL(destroyed(lmms::jo_id_t)), receiver.data(), SLOT(objectDestroyed(lmms::jo_id_t)),
				static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
		}
		else if (bound) { disconnect(target.data(), SIGNAL(destroyed(lmms::jo_id_t)), receiver.data(), SLOT(objectDestroyed(lmms::jo_id_t))); }
		const auto result = [&](const RecordingSource& state) {
			validateTarget();
			if (!receiver) { throw std::runtime_error("Native curve retired during binding notification"); }
			QMutexLocker lock(&receiver->m_clipMutex);
			if (receiver->m_isRecording || receiver->m_recordingTake || receiver->m_dragging || !receiver->recordingSourceMatches(state, true))
			{ throw std::runtime_error("Native binding result changed during notification"); }
		};
		// Check returned signal wiring before calling another receiver method.
		result(published);
		if (seed) { receiver->updateLength(); }
		result(expected); if (membershipChanged) { emit receiver->dataChanged(); result(expected); }
		changed = membershipChanged; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native binding edit failed with a nonstandard exception"; }
	return false;
}

void AutomationClip::reserveRecordingTake(std::size_t maximumPoints, int quantization)
{
	const QPointer<AutomationClip> receiver(this);
	std::shared_ptr<RecordingSource> source;
	{
		QMutexLocker lock(&m_clipMutex);
		if (m_recordingTake || m_recordingSource || m_isRecording) { throw std::runtime_error("Native automation already has a recorder"); }
		if (!maximumPoints || static_cast<std::size_t>(m_timeMap.size()) > maximumPoints || quantization < 0)
		{ throw std::invalid_argument("Automation recording point budget or quantization is invalid"); }
		source = captureSource();
	}
	// Detached curve construction emits native length notifications. Do not hold
	// the receiver mutex across them, or publish against a changed source.
	auto take = std::make_unique<AutomationClip>(nullptr);
	take->m_timeMap = source->points;
	for (auto it = take->m_timeMap.begin(); it != take->m_timeMap.end(); ++it) { it.value().setClip(take.get()); }
	take->m_progressionType = source->progression; take->m_tension = source->tension;
	take->setAutoResize(false);
	take->m_lastRecordedValue = std::numeric_limits<float>::quiet_NaN();
	if (!receiver) { throw std::runtime_error("Native automation receiver retired during reservation"); }
	QMutexLocker lock(&receiver->m_clipMutex);
	if (receiver->m_recordingTake || receiver->m_recordingSource || receiver->m_isRecording || !receiver->recordingSourceMatches(*source, true))
	{ throw std::runtime_error("Native automation source changed during reservation"); }
	receiver->m_recordingMaximumPoints = maximumPoints; receiver->m_recordingQuantization = quantization;
	receiver->m_recordingSamples = 0; receiver->m_recordingError.clear();
	receiver->m_recordingSource = std::move(source); receiver->m_recordingTake = std::move(take);
}

void AutomationClip::commitRecordingTake(const std::function<void()>& checkpoint)
{
	const QPointer<AutomationClip> receiver(this);
	QPointer<AutomationClip> take;
	QPointer<Track> owner;
	std::shared_ptr<const RecordingSource> source;
	timeMap original, captured, prepared;
	objectVector objects;
	std::vector<AutomatableModel*> identities;
	TimePos position, size, offset;
	ProgressionType progression;
	float tension, lastValue;
	bool automatic;
	std::uint64_t samples;
	std::size_t maximumPoints;
	int quantization, meter;
	{
		QMutexLocker lock(&m_clipMutex);
		if (m_isRecording) { throw std::runtime_error("Disarm native automation before publishing its take"); }
		if (!m_recordingTake || !m_recordingSamples || !m_recordingError.isEmpty())
		{ throw std::runtime_error(m_recordingError.isEmpty() ? "No complete native automation take" : m_recordingError.toStdString()); }
		if (!m_recordingSource || !recordingSourceMatches(*m_recordingSource, true))
		{ throw std::runtime_error("Native automation source changed since reservation"); }
		source = m_recordingSource;
		take = m_recordingTake.get(); owner = getTrack(); original = m_timeMap; captured = take->m_timeMap; prepared = captured;
		objects = m_objects; identities.reserve(objects.size()); for (const auto& model : objects) { identities.push_back(model.data()); }
		position = startPosition(); size = length(); offset = startTimeOffset(); automatic = getAutoResize();
		progression = m_progressionType; tension = m_tension; meter = TimePos::ticksPerBar();
		lastValue = take->m_lastRecordedValue; samples = m_recordingSamples;
		maximumPoints = m_recordingMaximumPoints; quantization = m_recordingQuantization;
	}
	const auto unchanged = [&](bool contents) {
		if (!receiver || !take) { throw std::runtime_error("Native automation receiver or take retired before publication"); }
		QMutexLocker lock(&receiver->m_clipMutex);
		if (receiver->m_recordingTake.get() != take.data() || receiver->m_isRecording || !receiver->m_recordingError.isEmpty()
			|| receiver->m_recordingSource != source || !receiver->recordingSourceMatches(*source, contents)
			|| receiver->m_recordingSamples != samples || receiver->m_recordingMaximumPoints != maximumPoints
			|| receiver->m_recordingQuantization != quantization || take->m_lastRecordedValue != lastValue
			|| (contents && (receiver->m_timeMap != original || take->m_timeMap != captured))
			|| receiver->m_objects != objects || receiver->m_progressionType != progression || receiver->m_tension != tension
			|| receiver->startPosition() != position || receiver->length() != size || receiver->startTimeOffset() != offset
			|| receiver->getAutoResize() != automatic || TimePos::ticksPerBar() != meter || receiver->getTrack() != owner.data())
		{ throw std::runtime_error("Native automation state changed before take publication"); }
		for (std::size_t index = 0; index < objects.size(); ++index)
		{ if (objects[index].data() != identities[index]) { throw std::runtime_error("Native automation binding retired before take publication"); } }
		if (owner && std::find(owner->getClips().begin(), owner->getClips().end(), receiver.data()) == owner->getClips().end())
		{ throw std::runtime_error("Native automation clip detached before take publication"); }
	};
	// Cooperative checkpoints must not hold the receiver's mutex. Revalidate
	// both maps and take lifetime afterward, before exposing any captured data.
	std::size_t count = 0;
	for (auto it = prepared.begin(); it != prepared.end(); ++it)
	{
		if (checkpoint && count++ % 256 == 0) { checkpoint(); unchanged(false); }
		it.value().setClip(receiver.data());
	}
	if (checkpoint) { checkpoint(); }
	unchanged(true);
	{
		QMutexLocker lock(&receiver->m_clipMutex); unchanged(true);
		receiver->m_timeMap.swap(prepared); receiver->m_lastRecordedValue = lastValue;
		receiver->m_recordingTake.reset();
		receiver->m_recordingSource.reset();
	}
	// Keep native sizing and notifications, without retaining a mutex across
	// their callbacks. These remain observable post-publication boundaries.
	receiver->updateLength();
	if (!receiver) { throw std::runtime_error("Native automation receiver retired during take sizing"); }
	emit receiver->dataChanged();
	if (!receiver) { throw std::runtime_error("Native automation receiver retired during take notification"); }
}

void AutomationClip::discardRecordingTake()
{
	QMutexLocker lock(&m_clipMutex); m_recordingTake.reset(); m_recordingSource.reset(); m_recordingError.clear(); m_recordingSamples = 0;
}

void AutomationClip::recordValue(TimePos time, float value)
{
	QMutexLocker m(&m_clipMutex);
	if (m_recordingTake)
	{
		if (!m_recordingError.isEmpty()) { return; }
		try
		{
			if (!m_recordingSource || !recordingSourceMatches(*m_recordingSource, false))
			{ throw std::runtime_error("Native automation source changed during capture"); }
			const auto source = qint64(time.getTicks()) - startTimeOffset().getTicks();
			if (source < 0) { return; }
			if (!std::isfinite(value) || source > MaxSongLength)
			{
				throw std::runtime_error("Recorded automation value or source position is invalid");
			}
			const TimePos position = m_recordingQuantization ? Note::quantized(static_cast<int>(source), m_recordingQuantization) : TimePos(static_cast<int>(source));
			if (position > MaxSongLength) { throw std::runtime_error("Recorded automation exceeds the native song range"); }
			auto& take = *m_recordingTake;
			if (value != take.m_lastRecordedValue || take.m_timeMap.isEmpty())
			{
				if (!take.m_timeMap.contains(position.getTicks()) && static_cast<std::size_t>(take.m_timeMap.size()) >= m_recordingMaximumPoints)
				{
					throw std::runtime_error("Automation recording point limit reached");
				}
				// This take has its own grid. A GUI editor's static quantization
				// must not remove neighboring source nodes during headless capture.
				take.putValue(position, value, false, true); take.m_lastRecordedValue = value;
			}
			else if (take.valueAt(position) != value) { take.removeNode(position); }
			++m_recordingSamples;
		}
		catch (const std::exception& error)
		{
			m_recordingError = QString::fromUtf8(error.what()).left(1024);
			if (m_recordingError.isEmpty()) { m_recordingError = "Native automation capture failed"; }
		}
		catch (...) { m_recordingError = "Native automation capture failed"; }
		return;
	}

	if( value != m_lastRecordedValue || m_timeMap.isEmpty() )
	{
		putValue(time - startTimeOffset(), value, true);
		m_lastRecordedValue = value;
	}
	else if( valueAt(time - startTimeOffset()) != value )
	{
		removeNode(time - startTimeOffset());
	}
}




TimePos AutomationClip::setDragValue(
	const TimePos& time,
	const float value,
	const bool quantPos,
	const bool controlKey
) {
	QMutexLocker m(&m_clipMutex);

	if (m_dragging == false)
	{
		TimePos newTime = quantPos ? Note::quantized(time, quantization()) : time;

		// We will keep the same outValue only if it's different from the
		// inValue
		m_dragKeepOutValue = false;

		// We will set the tangents back to what they were if the node had
		// its tangents locked
		m_dragLockedTan = false;

		// Check if we already have a node on the position we are dragging
		// and if we do, store the outValue so the discrete jump can be kept
		// and information about the tangents
		timeMap::iterator it = m_timeMap.find(newTime);
		if (it != m_timeMap.end())
		{
			// If we don't have a discrete jump, the outValue will be the
			// same as the inValue
			if (OFFSET(it) != 0)
			{
				m_dragKeepOutValue = true;
				m_dragOutValue = OUTVAL(it);
			}
			// For the tangents, we will only keep them if the tangents were
			// locked
			if (LOCKEDTAN(it))
			{
				m_dragLockedTan = true;
				m_dragInTan = INTAN(it);
				m_dragOutTan = OUTTAN(it);
			}
		}

		this->removeNode(newTime);
		m_oldTimeMap = m_timeMap;
		m_dragging = true;
	}

	//Restore to the state before it the point were being dragged
	m_timeMap = m_oldTimeMap;

	generateTangents();

	TimePos returnedPos;

	if (m_dragKeepOutValue)
	{
		returnedPos = this->putValues(time, value, m_dragOutValue, quantPos, controlKey);
	}
	else
	{
		returnedPos = this->putValue(time, value, quantPos, controlKey);
	}

	// Set the tangents on the newly created node if they were locked
	// before dragging
	if (m_dragLockedTan)
	{
		timeMap::iterator it = m_timeMap.find(returnedPos);
		if (it != m_timeMap.end())
		{
			it.value().setInTangent(m_dragInTan);
			it.value().setOutTangent(m_dragOutTan);
			it.value().setLockedTangents(true);
		}
	}

	return returnedPos;
}




void AutomationClip::applyDragValue()
{
	QMutexLocker m(&m_clipMutex);

	m_dragging = false;
}




float AutomationClip::valueAt( const TimePos & _time ) const
{
	QMutexLocker m(&m_clipMutex);

	if( m_timeMap.isEmpty() )
	{
		return 0;
	}

	// If we have a node at that time, just return its value
	if (m_timeMap.contains(_time))
	{
		// When the time is exactly the node's time, we want the inValue
		return m_timeMap[_time].getInValue();
	}

	// lowerBound returns next value with equal or greater key. Since we already
	// checked if the key contains a node, we know the returned node has a greater
	// key than _time. Therefore we take the previous element to calculate the current value
	timeMap::const_iterator v = m_timeMap.lowerBound(_time);

	if( v == m_timeMap.begin() )
	{
		return 0;
	}

	const auto pv = std::prev(v);

	if( v == m_timeMap.end() )
	{
		// When the time is after the last node, we want the outValue of it
		return OUTVAL(pv);
	}

	return valueAt(pv, _time - POS(pv));
}




// This method will get the value at an offset from a node, so we use the outValue of
// that node and the inValue of the next node for the calculations.
float AutomationClip::valueAt( timeMap::const_iterator v, int offset ) const
{
	QMutexLocker m(&m_clipMutex);

	// We never use it with offset 0, but doesn't hurt to return a correct
	// value if we do
	if (offset == 0) { return INVAL(v); }

	if (m_progressionType == ProgressionType::Discrete)
	{
		return OUTVAL(v);
	}
	else if( m_progressionType == ProgressionType::Linear )
	{
		auto const nv = std::next(v);
		float slope =
			(INVAL(nv) - OUTVAL(v))
			/ (POS(nv) - POS(v));

		return OUTVAL(v) + offset * slope;
	}
	else /* ProgressionType::CubicHermite */
	{
		// Implements a Cubic Hermite spline as explained at:
		// http://en.wikipedia.org/wiki/Cubic_Hermite_spline#Unit_interval_.280.2C_1.29
		//
		// Note that we are not interpolating a 2 dimensional point over
		// time as the article describes.  We are interpolating a single
		// value: y.  To make this work we map the values of x that this
		// segment spans to values of t for t = 0.0 -> 1.0 and scale the
		// tangents _m1 and _m2
		auto const nv = std::next(v);

		int numValues = (POS(nv) - POS(v));
		float t = (float) offset / (float) numValues;
		float m1 = OUTTAN(v) * numValues * m_tension;
		float m2 = INTAN(nv) * numValues * m_tension;

		auto t2 = t * t, t3 = t2 * t;
		return (2 * t3 - 3 * t2 + 1) * OUTVAL(v)
			+ (t3 - 2 * t2 + t) * m1
			+ (-2 * t3 + 3 * t2) * INVAL(nv)
			+ (t3 - t2) * m2;
	}
}




float *AutomationClip::valuesAfter( const TimePos & _time ) const
{
	QMutexLocker m(&m_clipMutex);

	timeMap::const_iterator v = m_timeMap.lowerBound(_time);
	if (v == m_timeMap.end()) { return nullptr; }
	auto const nv = std::next(v);

	if (nv == m_timeMap.end())
	{
		return nullptr;
	}

	int numValues = POS(nv) - POS(v);
	auto ret = new float[numValues];

	for( int i = 0; i < numValues; i++ )
	{
		ret[i] = valueAt( v, i );
	}

	return ret;
}




void AutomationClip::flipY(int min, int max)
{
	QMutexLocker m(&m_clipMutex);

	bool changedTimeMap = false;

	for (auto it = m_timeMap.begin(); it != m_timeMap.end(); ++it)
	{
		// Get distance from IN/OUT values to max value
		float inValDist = max - INVAL(it);
		float outValDist = max - OUTVAL(it);

		// To flip, that will be the new distance between
		// the IN/OUT values and the min value
		it.value().setInValue(min + inValDist);
		it.value().setOutValue(min + outValDist);

		changedTimeMap = true;
	}

	if (changedTimeMap)
	{
		generateTangents();
		emit dataChanged();
	}
}




void AutomationClip::flipY()
{
	flipY(getMin(), getMax());
}




void AutomationClip::flipX(int start, int end)
{
	QMutexLocker m(&m_clipMutex);

	timeMap::const_iterator firstIterator = m_timeMap.lowerBound(0);

	if (firstIterator == m_timeMap.end()) { return; }

	if (start == -1 && end == -1) { start = 0; end = length() - startTimeOffset(); }
	else if (!(end >= 0 && start >= 0 && end > start)) { return; }

	// Temporary map where we will store the flipped version
	// of our clip
	timeMap tempMap;

	for (auto it = m_timeMap.begin(); it != m_timeMap.end(); ++it)
	{
		if (POS(it) < start || POS(it) > end)
		{
			tempMap[POS(it)] = *it;
		}
		else
		{
			// If the first node in the clip is not at the start, it can break things when clipping, so
			// we have to set its in value to 0.
			if (it == firstIterator && POS(firstIterator) > 0)
			{
				tempMap[end - (POS(it) - start)] = AutomationNode(this, OUTVAL(it), 0, end - (POS(it) - start));
			}
			else
			{
				tempMap[end - (POS(it) - start)] = AutomationNode(this, OUTVAL(it), INVAL(it), end - (POS(it) - start));
			}
		}
	}

	if (m_timeMap.contains(start) && m_timeMap.contains(end))
	{
		tempMap[start] = AutomationNode(this, m_timeMap[start].getInValue(), m_timeMap[end].getInValue(), start);
		tempMap[end] = AutomationNode(this, m_timeMap[start].getOutValue(), m_timeMap[end].getOutValue(), end);
	}
	else if (m_timeMap.contains(start))
	{
		tempMap[start] = AutomationNode(this, m_timeMap[start].getInValue(), valueAt(end), start);
		tempMap[end] = AutomationNode(this, m_timeMap[start].getOutValue(), valueAt(end), end);
	}
	else if (m_timeMap.contains(end))
	{
		tempMap[start] = AutomationNode(this, valueAt(start), m_timeMap[end].getInValue(), start);
		tempMap[end] = AutomationNode(this, valueAt(start), m_timeMap[end].getOutValue(), end);
	}
	else
	{
		tempMap[start] = AutomationNode(this, valueAt(start), valueAt(end), start);
		tempMap[end] = AutomationNode(this, valueAt(start), valueAt(end), end);
	}

	m_timeMap.clear();

	m_timeMap = tempMap;

	cleanObjects();

	generateTangents();
	emit dataChanged();
}




void AutomationClip::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	QMutexLocker m(&m_clipMutex);

	_this.setAttribute( "pos", startPosition() );
	_this.setAttribute( "len", length() );
	_this.setAttribute( "name", Clip::name() );
	_this.setAttribute( "prog", QString::number( static_cast<int>(progressionType()) ) );
	_this.setAttribute( "tens", QString::number( getTension() ) );
	saveControlSettings(_doc, _this, "mute");
	_this.setAttribute("off", startTimeOffset());
	_this.setAttribute("autoresize", QString::number(getAutoResize()));

	if (const auto& c = color())
	{
		_this.setAttribute("color", c->name());
	}

	for( timeMap::const_iterator it = m_timeMap.begin();
						it != m_timeMap.end(); ++it )
	{
		QDomElement element = _doc.createElement( "time" );
		element.setAttribute("pos", POS(it));
		element.setAttribute("value", INVAL(it));
		element.setAttribute("outValue", OUTVAL(it));
		element.setAttribute("inTan", INTAN(it));
		element.setAttribute("outTan", OUTTAN(it));
		element.setAttribute("lockedTan", static_cast<int>(LOCKEDTAN(it)));
		_this.appendChild( element );
	}

	for (const auto& object : m_objects)
	{
		if (object)
		{
			QDomElement element = _doc.createElement( "object" );
			element.setAttribute("id", ProjectJournal::idToSave(object->id()));
			_this.appendChild(element);
		}
	}
}




void AutomationClip::loadSettings( const QDomElement & _this )
{
	QMutexLocker m(&m_clipMutex);

	// Legacy compatibility: Previously tangents were not stored in
	// the project file. So if any node doesn't have tangent information
	// we will generate the tangents
	bool shouldGenerateTangents = false;

	clear();
	// Arrangement restore replaces its targets. Legacy global and inline curves
	// can already be bound to the model whose settings are being restored.
	if (m_autoTrack && m_autoTrack->type() == Track::Type::Automation) { clearObjects(); }
	else { m_idsToResolve.clear(); }

	movePosition( _this.attribute( "pos" ).toInt() );
	setName( _this.attribute( "name" ) );
	setProgressionType( static_cast<ProgressionType>( _this.attribute(
							"prog" ).toInt() ) );
	setTension( _this.attribute( "tens" ) );
	loadControlSettings(_this, "mute");
	setAutoResize(_this.attribute("autoresize", "1").toInt());
	setStartTimeOffset(_this.attribute("off").toInt());

	for( QDomNode node = _this.firstChild(); !node.isNull();
						node = node.nextSibling() )
	{
		QDomElement element = node.toElement();
		if( element.isNull()  )
		{
			continue;
		}
		if( element.tagName() == "time" )
		{
			int timeMapPos = element.attribute("pos").toInt();
			float timeMapInValue = LocaleHelper::toFloat(element.attribute("value"));
			float timeMapOutValue = LocaleHelper::toFloat(element.attribute("outValue"));

			m_timeMap[timeMapPos] = AutomationNode(this, timeMapInValue, timeMapOutValue, timeMapPos);

			// Load tangents if there is information about it (it's enough to check for either inTan or outTan)
			if (element.hasAttribute("inTan"))
			{
				float inTan = LocaleHelper::toFloat(element.attribute("inTan"));
				float outTan = LocaleHelper::toFloat(element.attribute("outTan"));
				bool lockedTan = static_cast<bool>(element.attribute("lockedTan", "0").toInt());

				m_timeMap[timeMapPos].setInTangent(inTan);
				m_timeMap[timeMapPos].setOutTangent(outTan);
				m_timeMap[timeMapPos].setLockedTangents(lockedTan);
			}
			else
			{
				shouldGenerateTangents = true;
			}
		}
		else if( element.tagName() == "object" )
		{
			m_idsToResolve.push_back(element.attribute("id").toInt());
		}
	}

	if (_this.hasAttribute("color"))
	{
		setColor(QColor{_this.attribute("color")});
	}

	int len = _this.attribute( "len" ).toInt();
	if( len <= 0 )
	{
		// TODO: Handle with an upgrade method
		updateLength();
	}
	else
	{
		changeLength( len );
	}

	if (shouldGenerateTangents) { generateTangents(); }
}




QString AutomationClip::name() const
{
	QMutexLocker m(&m_clipMutex);

	if( !Clip::name().isEmpty() )
	{
		return Clip::name();
	}
	if (!m_objects.empty() && m_objects.front() != nullptr)
	{
		return m_objects.front()->fullDisplayName();
	}
	return tr( "Drag a control while pressing <%1>" ).arg(UI_COPY_KEY);
}




gui::ClipView * AutomationClip::createView( gui::TrackView * _tv )
{
	QMutexLocker m(&m_clipMutex);

	return new gui::AutomationClipView( this, _tv );
}





bool AutomationClip::isConnected(const AutomatableModel* model)
{
	for (const auto track : combineAllTracks())
	{
		if (track->type() != Track::Type::Automation && track->type() != Track::Type::HiddenAutomation) { continue; }
		for (const auto clip : track->getClips())
		{
			const auto curve = dynamic_cast<const AutomationClip*>(clip);
			if (curve && std::find(curve->m_objects.begin(), curve->m_objects.end(), model) != curve->m_objects.end()) { return true; }
		}
	}
	return false;
}

bool AutomationClip::isAutomated( const AutomatableModel * _m )
{
	auto l = combineAllTracks();
	for (const auto track : l)
	{
		if (track->type() == Track::Type::Automation || track->type() == Track::Type::HiddenAutomation)
		{
			for (const auto& clip : track->getClips())
			{
				const auto a = dynamic_cast<const AutomationClip*>(clip);
				if( a && a->hasAutomation() )
				{
					for (const auto& object : a->m_objects)
					{
						if (object == _m)
						{
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}




std::vector<AutomationClip *> AutomationClip::clipsForModel(const AutomatableModel* _m)
{
	std::vector<AutomationClip *> clips;
	auto l = combineAllTracks();

	// go through all tracks...
	for (const auto track : l)
	{
		// we want only automation tracks...
		if (track->type() == Track::Type::Automation || track->type() == Track::Type::HiddenAutomation )
		{
			// go through all the clips...
			for (const auto& trackClip : track->getClips())
			{
				auto a = dynamic_cast<AutomationClip*>(trackClip);
				// check that the clip has automation
				if( a && a->hasAutomation() )
				{
					// now check is the clip is connected to the model we want by going through all the connections
					// of the clip
					bool has_object = false;
					for (const auto& object : a->m_objects)
					{
						if (object == _m)
						{
							has_object = true;
						}
					}
					// if the clips is connected to the model, add it to the list
					if (has_object) { clips.push_back(a); }
				}
			}
		}
	}
	return clips;
}



AutomationClip * AutomationClip::globalAutomationClip(
							AutomatableModel * _m )
{
	AutomationTrack * t = Engine::getSong()->globalAutomationTrack();
	for (const auto& clip : t->getClips())
	{
		auto a = dynamic_cast<AutomationClip*>(clip);
		if( a )
		{
			for (const auto& object : a->m_objects)
			{
				if (object == _m) { return a; }
			}
		}
	}

	auto a = new AutomationClip(t);
	a->addObject( _m, false );
	return a;
}




void AutomationClip::resolveAllIDs()
{
	auto l = combineAllTracks();
	for (const auto& track : l)
	{
		if (track->type() == Track::Type::Automation || track->type() == Track::Type::HiddenAutomation)
		{
			for (const auto& clip : track->getClips())
			{
				auto a = dynamic_cast<AutomationClip*>(clip);
				if( a )
				{
					for (const auto& id : a->m_idsToResolve)
					{
						JournallingObject* o = Engine::projectJournal()->journallingObject(id);
						if( o && dynamic_cast<AutomatableModel *>( o ) )
						{
							a->addObject(dynamic_cast<AutomatableModel*>(o));
						}
						else
						{
							// FIXME: Remove this block once the automation system gets fixed
							// This is a temporary fix for https://github.com/LMMS/lmms/issues/3781
							o = Engine::projectJournal()->journallingObject(ProjectJournal::idFromSave(id));
							if( o && dynamic_cast<AutomatableModel *>( o ) )
							{
								a->addObject(dynamic_cast<AutomatableModel*>(o));
							}
							else
							{
								// FIXME: Remove this block once the automation system gets fixed
								// This is a temporary fix for https://github.com/LMMS/lmms/issues/4781
								o = Engine::projectJournal()->journallingObject(ProjectJournal::idToSave(id));
								if( o && dynamic_cast<AutomatableModel *>( o ) )
								{
									a->addObject(dynamic_cast<AutomatableModel*>(o));
								}
							}
						}
					}
					a->m_idsToResolve.clear();
					a->dataChanged();
				}
			}
		}
	}
}




void AutomationClip::clear()
{
	QMutexLocker m(&m_clipMutex);

	m_timeMap.clear();

	emit dataChanged();
}




void AutomationClip::objectDestroyed( jo_id_t _id )
{
	QMutexLocker m(&m_clipMutex);

	// TODO: distict between temporary removal (e.g. LADSPA controls
	// when switching samplerate) and real deletions because in the latter
	// case we had to remove ourselves if we're the global automation
	// clip of the destroyed object
	if (std::find(m_idsToResolve.begin(), m_idsToResolve.end(), _id) == m_idsToResolve.end())
	{
		m_idsToResolve.push_back(_id);
	}
	// Old projects and copied clips can contain duplicate or already-null targets.
	m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [_id](const auto& object) {
		return !object || object->id() == _id;
	}), m_objects.end());

	emit dataChanged();
}




void AutomationClip::cleanObjects()
{
	QMutexLocker m(&m_clipMutex);

	for( objectVector::iterator it = m_objects.begin(); it != m_objects.end(); )
	{
		if( *it )
		{
			++it;
		}
		else
		{
			it = m_objects.erase( it );
		}
	}
}




void AutomationClip::generateTangents()
{
	generateTangents(m_timeMap.begin(), m_timeMap.size());
}




// We have two tangents, one for the left side of the node and one for the right side
// of the node (in case we have discrete value jumps in the middle of a curve).
// If the inValue and outValue of a node are the same, consequently the inTangent and
// outTangent values of the node will be the same too.
void AutomationClip::generateTangents(timeMap::iterator it, int numToGenerate)
{
	QMutexLocker m(&m_clipMutex);

	for (int i = 0; i < numToGenerate && it != m_timeMap.end(); ++i, ++it)
	{
		// Skip the node if it has locked tangents (were manually edited)
		if (LOCKEDTAN(it))
		{
			continue;
		}

		auto const nit = std::next(it);

		if (nit == m_timeMap.end())
		{
			// Previously, the last value's tangent was always set to 0. That logic was kept for both tangents
			// of the last node
			it.value().setInTangent(0);
			it.value().setOutTangent(0);
		}
		else if (it == m_timeMap.begin())
		{
			// On the first node there's no curve behind it, so we will only calculate the outTangent
			// and inTangent will be set to 0.
			float tangent = (INVAL(nit) - OUTVAL(it)) / (POS(nit) - POS(it));
			it.value().setInTangent(0);
			it.value().setOutTangent(tangent);
		}
		else
		{
			// When we are in a node that is in the middle of two other nodes, we need to check if we
			// have a discrete jump at this node. If we do not, then we can calculate the tangents normally.
			// If we do have a discrete jump, then we have to calculate the tangents differently for each side
			// of the curve.
			// TODO: This behavior means that a very small difference between the inValue and outValue can
			// result in a big change in the curve. In the future, allowing the user to manually adjust
			// the tangents would be better.

			auto const pit = std::prev(it);

			if (OFFSET(it) == 0)
			{
				float inTangent = (INVAL(nit) - OUTVAL(pit)) / (POS(nit) - POS(pit));
				it.value().setInTangent(inTangent);
				// inTangent == outTangent in this case
				it.value().setOutTangent(inTangent);
			}
			else
			{
				// Calculate the left side of the curve
				float inTangent = (INVAL(it) - OUTVAL(pit)) / (POS(it) - POS(pit));
				// Calculate the right side of the curve
				float outTangent = (INVAL(nit) - OUTVAL(it)) / (POS(nit) - POS(it));
				it.value().setInTangent(inTangent);
				it.value().setOutTangent(outTangent);
			}
		}
	}
}

std::vector<Track*> AutomationClip::combineAllTracks()
{
	std::vector<Track*> combinedTrackList;

	auto& songTracks = Engine::getSong()->tracks();
	auto& patternStoreTracks = Engine::patternStore()->tracks();

	combinedTrackList.insert(combinedTrackList.end(), songTracks.begin(), songTracks.end());
	combinedTrackList.insert(combinedTrackList.end(), patternStoreTracks.begin(), patternStoreTracks.end());
	combinedTrackList.push_back(Engine::getSong()->globalAutomationTrack());

	return combinedTrackList;
}

} // namespace lmms
