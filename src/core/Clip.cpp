/*
 * Clip.cpp - implementation of Clip class
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Clip.h"

#include <QDomDocument>
#include <QCoreApplication>
#include <QThread>
#include <QScopeGuard>
#include <memory>
#include <limits>
#include <algorithm>
#include <stdexcept>

#include "AutomationEditor.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "ControllerConnection.h"
#include "FileResourceIO.h"
#include "NativeLinkedControls.h"
#include "Note.h"
#include <QSet>
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"


namespace lmms
{

Clip::Clip( Track * track ) :
	Model( track ),
	m_track( track ),
	m_startPosition(),
	m_length(),
	m_mutedModel( false, this, tr( "Mute" ) ),
	m_soloModel(false, this, tr("Solo")),
	m_selectViewOnCreate{false}
{
	auto rollback = qScopeGuard([this] { if (m_track) { m_track->removeClip(this); } });
	if( getTrack() )
	{
		getTrack()->addClip( this );
	}
	setJournalling( false );
	movePosition( 0 );
	changeLength( 0 );
	setJournalling( true );
	rollback.dismiss();
}


Clip::Clip(const Clip& other) : Clip(other, other.m_track) { }

Clip::Clip(const Clip& other, Track* destination):
	Model(destination),
	m_track(destination),
	m_name(other.m_name),
	m_startPosition(other.m_startPosition),
	m_length(other.m_length),
	m_startTimeOffset(other.m_startTimeOffset),
	m_mutedModel(other.m_mutedModel.storedValue(), this, tr( "Mute" )),
	m_soloModel(other.m_soloModel.storedValue(), this, tr("Solo")),
	m_autoResize(other.m_autoResize),
	m_selectViewOnCreate{other.m_selectViewOnCreate},
	m_color(other.m_color)
{
	auto rollback = qScopeGuard([this] { if (m_track) { m_track->removeClip(this); } });
	if (getTrack())
	{
		getTrack()->addClip(this);
	}
	rollback.dismiss();
}


Clip::~Clip()
{
	emit destroyedClip();

	if( getTrack() )
	{
		getTrack()->removeClip( this );
	}
}


QDomElement Clip::saveJournalState(QDomDocument& document, QDomElement& parent)
{
	if (!isJournalling()) { return {}; }
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	auto state = document.createElement("nativeClipJournal"); state.setAttribute("version", 1); state.setAttribute("receiver", id()); state.setAttribute("type", nodeName()); parent.appendChild(state);
	NativeLinkedControls::SaveScope links(document, NativeLinkedControls::clipModels(this));
	const auto body = JournallingObject::saveJournalState(document, state); QString error;
	if (body.isNull() || !links.finish(state, error)) { DataFile::recordSerializationError(document, error.isEmpty() ? "Native clip journal has no serialized state" : error); return {}; }
	return state;
}

bool Clip::restoreJournalState(const QDomElement& state, QString& error)
{
	if (state.tagName() != "nativeClipJournal") { return JournallingObject::restoreJournalState(state, error); }
	bool valid = false; const auto receiver = state.attribute("receiver").toUInt(&valid); const auto body = state.firstChildElement(nodeName());
	if (state.attribute("version") != "1" || state.attribute("type") != nodeName() || !valid || receiver != id() || body.isNull() || !body.nextSiblingElement(nodeName()).isNull())
	{ error = "Invalid native clip journal receiver or format"; return false; }
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; requireIdleForCopy();
	NativeLinkedControls::RestoreScope links(state.ownerDocument()); NativeLinkedControls::unlinkInternalGroups(NativeLinkedControls::clipModels(this));
	JournallingObject::restoreJournalState(body, error); if (!links.finish(state, error)) { return false; }
	AutomationClip::resolveAllIDs(); error.clear(); return true;
}

void Clip::movePosition( const TimePos & pos )
{
	TimePos newPos = std::max(0, pos.getTicks());
	if (m_startPosition != newPos)
	{
		Engine::audioEngine()->requestChangeInModel();
		m_startPosition = newPos;
		Engine::audioEngine()->doneChangeInModel();
		Engine::getSong()->updateLength();
		emit positionChanged();
	}
}


void Clip::changeLength( const TimePos & length )
{
	if (m_length == length) { return; }

	m_length = length;
	Engine::getSong()->updateLength();
	emit lengthChanged();
}


bool Clip::comparePosition(const Clip *a, const Clip *b)
{
	return a->startPosition() < b->startPosition();
}

Clip* Clip::splitAt(const TimePos& relativePosition)
{
	if (!m_track || isInPattern() || relativePosition <= 0 || relativePosition >= length()
		|| relativePosition.getTicks() > std::numeric_limits<int>::max() - startPosition().getTicks()) { return nullptr; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto originalEnd = endPosition();
	const auto splitPosition = startPosition() + relativePosition;
	m_track->addJournalCheckPoint();
	m_track->saveJournallingState(false);
	try
	{
		auto right = std::unique_ptr<Clip>(clone());
		if (!right) { m_track->restoreJournallingState(); return nullptr; }
		changeLength(relativePosition);
		setAutoResize(false);
		right->movePosition(splitPosition);
		right->changeLength(originalEnd - splitPosition);
		const auto offset = static_cast<qint64>(startTimeOffset().getTicks()) - length().getTicks();
		right->setStartTimeOffset(static_cast<int>(std::clamp(offset,
			static_cast<qint64>(std::numeric_limits<int>::min()), static_cast<qint64>(std::numeric_limits<int>::max()))));
		right->setAutoResize(false);
		m_track->restoreJournallingState();
		return right.release();
	}
	catch (...)
	{
		m_track->restoreJournallingState();
		throw;
	}
}


void Clip::copyStateTo( Clip *src, Clip *dst )
{
	if (!src || !dst || !QCoreApplication::instance() || !Engine::audioEngine()
		|| QThread::currentThread() != QCoreApplication::instance()->thread()
		|| QThread::currentThread() != src->thread() || QThread::currentThread() != dst->thread())
	{ throw std::invalid_argument("Native clip state copying requires live models on their owner thread"); }
	const FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	// If the node names match we copy the state
	if( src->nodeName() == dst->nodeName() ){
		src->requireIdleForCopy(); dst->requireIdleForCopy();
		QDomDocument doc;
		QDomElement parent = doc.createElement( "StateCopy" );
		doc.appendChild(parent);
		{
			NativeLinkedControls::SaveScope links(doc, NativeLinkedControls::clipModels(src));
			src->saveState(doc, parent);
			QString error; if (!links.finish(parent, error)) { throw std::runtime_error(error.toStdString()); }
			const auto serializationError = DataFile::serializationError(doc);
			if (!serializationError.isEmpty()) { throw std::runtime_error(serializationError.toStdString()); }
		}

		const TimePos pos = dst->startPosition();
		{
			JournallingObject::KeepIdentityScope identity;
			NativeLinkedControls::RestoreScope links(doc);
			NativeLinkedControls::unlinkInternalGroups(NativeLinkedControls::clipModels(dst));
			dst->restoreState(parent.firstChildElement());
			QString error; if (!links.finish(parent, error)) { throw std::runtime_error(error.toStdString()); }
		}
		dst->movePosition( pos );

		AutomationClip::resolveAllIDs();
		if (gui::getGUI() != nullptr)
		{
			gui::getGUI()->automationEditor()->m_editor->updateAfterClipChange();
		}
	}
}

void Clip::saveControlSettings(QDomDocument& document, QDomElement& element, const QString& muteKey)
{
	m_mutedModel.saveSettings(document, element, muteKey);
	m_soloModel.saveSettings(document, element, "solo");
	// Retain scalar fallbacks for legacy readers. Older MIDI-clip readers
	// treat unrecognized child nodes as notes unless marked as metadata.
	element.setAttribute(muteKey, m_mutedModel.storedValue());
	element.setAttribute("solo", m_soloModel.storedValue());
	for (const auto key : {muteKey, QString("solo"), QString("connection")})
	{ auto node = element.firstChildElement(key); if (!node.isNull()) { node.setAttribute("metadata", 1); node.setAttribute("native_state", 1); } }
}

void Clip::loadControlSettings(const QDomElement& element, const QString& muteKey)
{
	if (NativeLinkedControls::isRestoring(element.ownerDocument()))
	{ m_mutedModel.unlink(); m_soloModel.unlink(); }
	delete m_mutedModel.controllerConnection();
	delete m_soloModel.controllerConnection();
	m_mutedModel.loadSettings(element, muteKey);
	m_soloModel.loadSettings(element, "solo");
}

QDomDocument Clip::captureCopyControls() const
{
	QDomDocument document; auto parent = document.createElement("clipcontrols"); document.appendChild(parent);
	auto source = const_cast<Clip*>(this);
	NativeLinkedControls::SaveScope links(document, NativeLinkedControls::clipModels(source));
	source->saveCopyControlSettings(document, parent);
	QString error; if (!links.finish(parent, error)) { throw std::runtime_error(error.toStdString()); }
	const auto serializationError = DataFile::serializationError(document);
	if (!serializationError.isEmpty()) { throw std::runtime_error(serializationError.toStdString()); }
	return document;
}

void Clip::saveCopyControlSettings(QDomDocument& document, QDomElement& element, const QString& muteKey)
{
	saveControlSettings(document, element, muteKey);
	if (const auto midi = dynamic_cast<MidiClip*>(this))
	{
		if (midi->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::invalid_argument("Native inline control copy has too many note indices"); }
		auto controls = document.createElement("nativeInlineControls"); controls.setAttribute("metadata", 1); controls.setAttribute("native_state", 1); controls.setAttribute("version", 1); controls.setAttribute("count", static_cast<int>(midi->notes().size()));
		for (std::size_t index = 0; index < midi->notes().size(); ++index)
		{
			const auto note = midi->notes().at(index); if (!note || !note->detuning()) { continue; }
			auto state = document.createElement("note"); state.setAttribute("index", static_cast<int>(index)); state.setAttribute("key", note->key()); state.setAttribute("pos", note->pos()); state.setAttribute("len", note->length());
			note->detuning()->saveControlSettings(document, state); controls.appendChild(state);
		}
		if (controls.hasChildNodes()) { element.appendChild(controls); }
	}
}

void Clip::loadCopyControlSettings(const QDomElement& element, const QString& muteKey)
{
	const auto controls = element.firstChildElement("nativeInlineControls"); auto midi = dynamic_cast<MidiClip*>(this);
	if (!controls.isNull())
	{
		bool countOk = false; const auto count = controls.attribute("count").toULongLong(&countOk); QSet<int> seen;
		if (!midi || controls.attribute("version") != "1" || !countOk || count != midi->notes().size() || !controls.nextSiblingElement("nativeInlineControls").isNull())
		{ throw std::invalid_argument("Native inline control copy note roster changed"); }
		for (auto state = controls.firstChildElement(); !state.isNull(); state = state.nextSiblingElement())
		{
			bool valid = false; const int index = state.attribute("index").toInt(&valid);
			if (state.tagName() != "note" || !valid || index < 0 || static_cast<std::size_t>(index) >= midi->notes().size() || seen.contains(index))
			{ throw std::invalid_argument("Invalid native inline control copy index"); } seen.insert(index); const auto note = midi->notes().at(index);
			if (!note || !note->detuning() || state.attribute("key") != QString::number(note->key()) || state.attribute("pos") != QString::number(note->pos().getTicks())
				|| state.attribute("len") != QString::number(note->length().getTicks()) || state.firstChildElement("inlinecontrol").isNull())
			{ throw std::invalid_argument("Native inline control copy note identity changed"); }
		}
	}
	loadControlSettings(element, muteKey);
	if (!controls.isNull())
	{ for (auto state = controls.firstChildElement(); !state.isNull(); state = state.nextSiblingElement()) { midi->notes().at(state.attribute("index").toInt())->detuning()->loadControlSettings(state); } }
}

void Clip::requireIdleForCopy() const
{
	if (getTrack() && getTrack()->isNoteRecording())
	{
		throw std::invalid_argument("Complete native note recording before copying clips");
	}
	if (const auto sample = dynamic_cast<const SampleClip*>(this); sample &&
		(sample->isRecord() || sample->hasReservedRecording() || sample->hasActiveRecording()
			|| sample->hasPendingRecording() || sample->isFinalizingRecording()))
	{
		throw std::invalid_argument("Complete or disarm native sample recording before copying its clip");
	}
	if (const auto automation = dynamic_cast<const AutomationClip*>(this); automation &&
		(automation->isRecording() || automation->hasReservedRecording()))
	{
		throw std::invalid_argument("Complete or disarm native automation recording before copying its clip");
	}
}

Clip* Clip::copyTo(Track* destination, Clip* replacement) const
{ return copyToImpl(destination, replacement, false); }

Clip* Clip::cloneNative() const
{
	if (!QCoreApplication::instance() || QThread::currentThread() != thread()
		|| QThread::currentThread() != QCoreApplication::instance()->thread()) { return nullptr; }
	try { return copyToImpl(getTrack(), nullptr, true); }
	catch (const std::exception& exception)
	{ if (Engine::getSong()) { Engine::getSong()->collectError("Native clip cloning failed: " + QString::fromUtf8(exception.what())); } }
	catch (...) { if (Engine::getSong()) { Engine::getSong()->collectError("Native clip cloning failed with a nonstandard exception"); } }
	return nullptr;
}

Clip* Clip::copyToImpl(Track* destination, Clip* replacement, bool legacyClone) const
{
	if (!Engine::audioEngine() || !QCoreApplication::instance()
		|| QThread::currentThread() != QCoreApplication::instance()->thread()
		|| QThread::currentThread() != thread()
		|| (destination && QThread::currentThread() != destination->thread()))
	{
		throw std::invalid_argument("Native clip copying requires the model owner thread and audio engine");
	}
	if (!destination || !getTrack() || destination->type() != getTrack()->type())
	{
		throw std::invalid_argument("Native clip copy requires compatible source and destination tracks");
	}
	const FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (getTrack()->isNoteRecording() || destination->isNoteRecording())
	{
		throw std::invalid_argument("Complete native note recording before copying clips");
	}
	const bool patternCell = destination->trackContainer() == Engine::patternStore();
	const auto& destinationClips = destination->getClips();
	const auto replaced = std::find(destinationClips.begin(), destinationClips.end(), replacement);
	const int index = static_cast<int>(replaced - destinationClips.begin());
	if ((patternCell && !legacyClone && (!replacement || replacement == this || replaced == destinationClips.end()
		|| destination->numOfClips() != Engine::patternStore()->numOfPatterns()
		|| std::find(destinationClips.begin(), destinationClips.end(), nullptr) != destinationClips.end()))
		|| (!patternCell && replacement))
	{
		throw std::invalid_argument("A pattern copy must replace a different existing cell; arrangement copies append");
	}
	requireIdleForCopy();
	if (replacement) { replacement->requireIdleForCopy(); }
	if (dynamic_cast<SampleTrack*>(destination))
	{
		// Creating a sample clip refreshes every playback handle on its track.
		// A capture in a different destination clip must remain undisturbed.
		for (auto existing : destinationClips) { if (existing) { existing->requireIdleForCopy(); } }
	}
	const auto controls = captureCopyControls();
	const auto previousErrors = Engine::getSong()->errorSummary();
	auto result = constructCopy(destination);
	{
		JournallingObject::KeepIdentityScope identity;
		NativeLinkedControls::RestoreScope links(controls);
		result->loadCopyControlSettings(controls.documentElement());
		QString error; if (!links.finish(controls.documentElement(), error)) { throw std::runtime_error(error.toStdString()); }
	}
	if (Engine::getSong()->errorSummary() != previousErrors) { throw std::runtime_error("Native clip controls did not copy completely"); }
	if (replacement)
	{
		// Construction appends. Swap into the owned column before removing the
		// old cell, retaining every other column and its native identity.
		destination->swapPositionOfClips(index, destination->numOfClips() - 1);
		delete replacement;
		result->movePosition(TimePos(index, 0));
		Engine::patternStore()->updatePatternTrack(result.get());
	}
	Engine::getSong()->updateLength();
	Engine::getSong()->setModified();
	return result.release();
}

std::unique_ptr<Clip> Clip::constructCopy(Track* destination) const
{
	std::unique_ptr<Clip> result;
	if (const auto midi = dynamic_cast<const MidiClip*>(this))
	{
		auto track = dynamic_cast<InstrumentTrack*>(destination);
		if (track) { result.reset(new MidiClip(*midi, track)); }
	}
	else if (const auto sample = dynamic_cast<const SampleClip*>(this); sample && dynamic_cast<SampleTrack*>(destination))
	{
		result.reset(new SampleClip(*sample, destination));
	}
	else if (const auto automation = dynamic_cast<const AutomationClip*>(this))
	{
		auto track = dynamic_cast<AutomationTrack*>(destination);
		if (track) { result.reset(new AutomationClip(*automation, track)); }
	}
	else if (const auto pattern = dynamic_cast<const PatternClip*>(this); pattern && dynamic_cast<PatternTrack*>(destination))
	{
		result.reset(new PatternClip(*pattern, destination));
	}
	if (!result) { throw std::invalid_argument("Unsupported native clip copy type"); }
	return result;
}

bool Clip::hasTrackContainer() const
{
	return getTrack() != nullptr && getTrack()->trackContainer() != nullptr;
}

bool Clip::isInPattern() const
{
	return hasTrackContainer()
		&& getTrack()->trackContainer()->type() == TrackContainer::Type::Pattern;
}

bool Clip::manuallyResizable() const
{
	return !isInPattern();
}


void Clip::toggleMute()
{
	m_mutedModel.setValue( !m_mutedModel.value() );
	emit dataChanged();
}


TimePos Clip::startTimeOffset() const
{
	return m_startTimeOffset;
}


void Clip::setStartTimeOffset( const TimePos &startTimeOffset )
{
	m_startTimeOffset = startTimeOffset;
}

void Clip::setColor(const std::optional<QColor>& color)
{
	m_color = color;
	emit colorChanged();
}

} // namespace lmms
