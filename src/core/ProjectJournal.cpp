/*
 * ProjectJournal.cpp - implementation of ProjectJournal
 *
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <cstdlib>
#include <QDomElement>
#include <QScopedValueRollback>
#include <exception>

#include "ProjectJournal.h"
#include "Engine.h"
#include "JournallingObject.h"
#include "JournalIdAllocator.h"
#include "Song.h"
#include "AutomationClip.h"
#include "AutomatableModel.h"
#include "NativeControlLinkJournal.h"

namespace lmms
{

//! Avoid clashes between loaded IDs (have the bit cleared)
//! and newly created IDs (have the bit set)
static const int EO_ID_MSB = 1 << 23;

const int ProjectJournal::MAX_UNDO_STATES = 100; // TODO: make this configurable in settings

ProjectJournal::ProjectJournal() :
	m_joIDs(),
	m_undoCheckPoints(),
	m_redoCheckPoints(),
	m_journalling( false )
{
}




void ProjectJournal::undo()
{
	restoreLatest(m_undoCheckPoints, m_redoCheckPoints, "undo");
}

void ProjectJournal::redo()
{
	restoreLatest(m_redoCheckPoints, m_undoCheckPoints, "redo");
}

void ProjectJournal::restoreLatest(CheckPointStack& source, CheckPointStack& destination, const QString& action)
{
	if (m_restoring)
	{
		Engine::getSong()->collectError("Native journal restoration cannot reenter"); return;
	}
	QScopedValueRollback<bool> restoring(m_restoring, true);
	while (!source.isEmpty())
	{
		const auto object = journallingObject(source.top().joID);
		if (!object) { source.pop(); continue; }
		try
		{
			auto checkpoint = source.top();
			DataFile current(DataFile::Type::JournalData);
			const auto previous = checkpoint.data.content().firstChildElement(); const bool links = previous.tagName() == "nativeControlLinkJournal";
			QString error;
			const auto saved = links ? NativeControlLinkJournal::counterpart(dynamic_cast<AutomatableModel*>(object), previous, current, current.content(), error)
				: object->saveJournalState(current, current.content());
			if (saved.isNull() || checkpoint.data.content().firstChildElement().isNull()
				|| !checkpoint.data.serializationError().isEmpty() || !current.serializationError().isEmpty())
			{
				Engine::getSong()->collectError("Native " + action + " state could not be serialized completely" + (error.isEmpty() ? QString() : ": " + error)); return;
			}
			// Allocate stack space before native changes. Rejected restoration
			// retains both history stacks and always restores the journal flag.
			destination.reserve(destination.size() + 1);
			{
				QScopedValueRollback<bool> journalling(m_journalling, false);
				const bool accepted = links ? NativeControlLinkJournal::restore(dynamic_cast<AutomatableModel*>(object), previous, error) : object->restoreJournalState(previous, error);
				if (!accepted)
				{
					Engine::getSong()->collectError("Native " + action + " was rejected: " + error); return;
				}
				if (!checkpoint.data.content().elementsByTagName("automationclip").isEmpty()) { AutomationClip::resolveAllIDs(); }
			}
			destination.push(CheckPoint(checkpoint.joID, current));
			source.pop();
			Engine::getSong()->setModified();
		}
		catch (const std::exception& exception)
		{
			Engine::getSong()->collectError("Native " + action + " failed: " + QString::fromUtf8(exception.what()));
		}
		catch (...) { Engine::getSong()->collectError("Native " + action + " failed with a nonstandard exception"); }
		return;
	}
}

bool ProjectJournal::canUndo() const
{
	return !m_undoCheckPoints.isEmpty();
}

bool ProjectJournal::canRedo() const
{
	return !m_redoCheckPoints.isEmpty();
}



void ProjectJournal::addJournalCheckPoint(JournallingObject* object)
{
	if (!isJournalling() || m_restoring || !object) { return; }
	object = object->journalTarget();
	if (!object) { return; }
	DataFile dataFile(DataFile::Type::JournalData);
	const auto state = object->saveJournalState(dataFile, dataFile.content());
	if (state.isNull() || !dataFile.serializationError().isEmpty())
	{
		Engine::getSong()->collectError("Native journal checkpoint could not be serialized completely"); return;
	}
	m_undoCheckPoints.push(CheckPoint(object->id(), dataFile));
	m_redoCheckPoints.clear();
	if (m_undoCheckPoints.size() > MAX_UNDO_STATES)
	{
		m_undoCheckPoints.remove(0, m_undoCheckPoints.size() - MAX_UNDO_STATES);
	}
}

bool ProjectJournal::addPreparedJournalCheckPoint(JournallingObject* object, const DataFile& state)
{
	if (!isJournalling() || m_restoring || !object || journallingObject(object->id()) != object
		|| state.content().firstChildElement().isNull() || !state.serializationError().isEmpty()) { return false; }
	m_undoCheckPoints.push(CheckPoint(object->id(), state)); m_redoCheckPoints.clear();
	if (m_undoCheckPoints.size() > MAX_UNDO_STATES) { m_undoCheckPoints.remove(0, m_undoCheckPoints.size() - MAX_UNDO_STATES); }
	return true;
}


jo_id_t ProjectJournal::allocID(JournallingObject* obj)
{
	// Loaded and newly allocated IDs share the same serialized namespace.
	// Reserving either form must prevent a later save from producing an alias.
	const auto id = nextJournalId(m_nextAllocationId, EO_ID_MSB,
		[this](jo_id_t value) { return m_joIDs.contains(value); });
	m_joIDs[id] = obj;
	return id;
}


void ProjectJournal::reallocID( const jo_id_t _id, JournallingObject * _obj )
{
	//printf("realloc %d %d\n", _id, _obj );
//	if( m_joIDs.contains( _id ) )
	{
		m_joIDs[_id] = _obj;
	}
}




jo_id_t ProjectJournal::idToSave( jo_id_t id )
{
	return id & ~EO_ID_MSB;
}

jo_id_t ProjectJournal::idFromSave( jo_id_t id )
{
	return id | EO_ID_MSB;
}




void ProjectJournal::clearJournal()
{
	if (m_restoring)
	{
		Engine::getSong()->collectError("Native journal cannot be cleared during restoration"); return;
	}
	m_undoCheckPoints.clear();
	m_redoCheckPoints.clear();

	for( JoIdMap::Iterator it = m_joIDs.begin(); it != m_joIDs.end(); )
	{
		if( it.value() == nullptr )
		{
			it = m_joIDs.erase( it );
		}
		else
		{
			++it;
		}
	}
}

void ProjectJournal::stopAllJournalling()
{
	for( JoIdMap::Iterator it = m_joIDs.begin(); it != m_joIDs.end(); ++it)
	{
		if( it.value() != nullptr )
		{
			it.value()->setJournalling(false);
		}
	}
	setJournalling(false);
}



} // namespace lmms
