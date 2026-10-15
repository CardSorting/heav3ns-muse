/*
 * ImportFilter.cpp - base-class for all import-filters
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


#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <QDebug>
#include <QMessageBox>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>

#include "ImportFilter.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "Song.h"
#include "TrackContainer.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Mixer.h"
#include "FileResourceIO.h"
#include "NativeTrackSaveSnapshot.h"
#include "AutomationTrack.h"


namespace lmms
{

class ImportFilter::Session
{
public:
	static thread_local Session* current;
	static thread_local QPointer<ImportFilter> activeFilter;
	bool matches(TrackContainer* target, int pattern) const { return m_target.data() == target && m_pattern == pattern; }
	Session(TrackContainer* target, int pattern)
		: m_song(Engine::getSong()), m_target(target), m_patterns(Engine::patternStore()), m_mixer(Engine::mixer()), m_hidden(m_song->globalAutomationTrack()),
		  m_journal(Engine::projectJournal()), m_journalling(m_journal->isJournalling()),
		  m_project(m_song->replacementGeneration()), m_routing(m_mixer->replacementGeneration()),
		  m_filename(m_song->projectFileName()), m_pattern(pattern), m_inPattern(target == m_patterns.data())
	{
		if (m_song->m_importingProject || m_song->m_creatingProject || m_song->m_loadingProject || m_song->m_loadingProjectFile || m_song->m_clearingProject || m_song->m_savingProject)
		{ throw std::logic_error("Native import cannot overlap a project transition"); }
		if (m_inPattern && m_patterns->numOfPatterns() > 0) { m_column = PatternTrack::findPatternTrack(pattern); m_columnPinned = true; if (!m_column) { throw std::runtime_error("Native import pattern column is unavailable"); } }
		for (auto owner : {static_cast<TrackContainer*>(m_song.data()), static_cast<TrackContainer*>(m_patterns.data()), target})
		{
			if (std::any_of(m_original.begin(), m_original.end(), [owner](const auto& roster) { return roster.owner.data() == owner; })) { continue; }
			Roster roster{owner, {}}; for (auto track : owner->tracks()) { roster.tracks.emplace_back(track); } m_original.push_back(std::move(roster));
		}
		checkContext(); m_changes.emplace(Engine::audioEngine()->requestChangesGuard());
		m_song->m_importingProject = true; m_journal->setJournalling(false); current = this;
	}
	~Session()
	{
		if (current == this) { current = nullptr; }
		if (m_song) { m_song->m_importingProject = false; }
		if (Engine::projectJournal() == m_journal) { m_journal->setJournalling(m_journalling); }
	}
	void checkContext()
	{
		if (!m_song || Engine::getSong() != m_song.data() || !m_target || !m_patterns || Engine::patternStore() != m_patterns.data()
			|| !m_mixer || Engine::mixer() != m_mixer.data() || Engine::projectJournal() != m_journal
			|| !m_hidden || m_song->globalAutomationTrack() != m_hidden.data() || m_song->thread() != QThread::currentThread() || m_target->thread() != QThread::currentThread()
			|| m_song->replacementGeneration() != m_project || m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename)
		{ throw std::runtime_error("Native import project or destination context changed"); }
		for (const auto& roster : m_original)
		{
			if (!roster.owner || roster.owner->tracks().size() < roster.tracks.size()) { throw std::runtime_error("Native import original tracks retired"); }
			for (std::size_t row = 0; row < roster.tracks.size(); ++row)
			{ if (!roster.tracks[row] || roster.owner->tracks()[row] != roster.tracks[row].data()) { throw std::runtime_error("Native import original track identity changed"); } }
		}
		if (m_inPattern)
		{
			if (m_columnPinned && (!m_column || PatternTrack::findPatternTrack(m_pattern) != m_column.data())) { throw std::runtime_error("Native import selected pattern column changed"); }
			if (!m_columnPinned && m_patterns->numOfPatterns() > 0) { m_column = PatternTrack::findPatternTrack(m_pattern); m_columnPinned = true; if (!m_column) { throw std::runtime_error("Native import pattern column is unavailable"); } }
		}
	}
	void check()
	{
		checkContext(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } checkContext();
	}
private:
	struct Roster { QPointer<TrackContainer> owner; std::vector<QPointer<Track>> tracks; };
	QPointer<Song> m_song;
	QPointer<TrackContainer> m_target;
	QPointer<PatternStore> m_patterns;
	QPointer<Mixer> m_mixer;
	QPointer<AutomationTrack> m_hidden;
	QPointer<PatternTrack> m_column;
	ProjectJournal* m_journal;
	bool m_journalling;
	std::uint64_t m_project, m_routing;
	QString m_filename;
	int m_pattern;
	bool m_inPattern;
	bool m_columnPinned = false;
	std::vector<Roster> m_original;
	std::optional<AudioEngine::RequestChangesGuard> m_changes;
	FileResourceIO::EventSuspension m_events;
};

thread_local ImportFilter::Session* ImportFilter::Session::current = nullptr;
thread_local QPointer<ImportFilter> ImportFilter::Session::activeFilter;

struct ImportFilter::WorkScope::State
{
	QPointer<ImportFilter> filter;
	std::unique_ptr<Session> owned;
	Session* session = nullptr;
	~State() { if (filter && session) { filter->m_working = false; } }
};

ImportFilter::WorkScope::WorkScope(ImportFilter* filter, TrackContainer* target, int pattern) : m_state(std::make_unique<State>())
{
	if (!filter || !target || !Engine::getSong() || !Engine::audioEngine() || !Engine::patternStore() || !Engine::mixer() || !Engine::projectJournal()
		|| filter->thread() != QThread::currentThread() || target->thread() != QThread::currentThread() || Engine::getSong()->thread() != QThread::currentThread() || filter->m_working)
	{ throw std::logic_error("Native import work requires live idle owner-thread models"); }
	m_state->filter = filter;
	if (Session::current && Session::activeFilter.data() == filter && Session::current->matches(target, pattern)) { m_state->session = Session::current; }
	else { m_state->owned = std::make_unique<Session>(target, pattern); m_state->session = m_state->owned.get(); }
	filter->m_working = true; checkContext();
}

ImportFilter::WorkScope::~WorkScope() = default;
void ImportFilter::WorkScope::checkContext()
{
	if (!m_state->filter || !m_state->filter->m_working) { throw std::runtime_error("Native import filter retired during work"); }
	m_state->session->checkContext();
}
void ImportFilter::WorkScope::check() { checkContext(); m_state->session->check(); checkContext(); }

ImportFilter::ImportFilter( const QString & _file_name,
							const Descriptor * _descriptor ) :
	Plugin( _descriptor, nullptr ),
	m_file( _file_name )
{
}





bool ImportFilter::import( const QString & _file_to_import,
							TrackContainer* tc, int pattern )
{
	if (!tc || !Engine::getSong() || !Engine::projectJournal() || !Engine::audioEngine() || !Engine::patternStore() || !Engine::mixer()
		|| QThread::currentThread() != Engine::getSong()->thread() || tc->thread() != Engine::getSong()->thread()) { return false; }
	const QString requested(_file_to_import); QStringList filters;
	for (const auto descriptor : getPluginFactory()->descriptors(Plugin::Type::ImportFilter)) { filters.append(QString::fromUtf8(descriptor->name)); }
	return importWithFilters(requested, tc, pattern, std::move(filters), [](const QString& name, void* path) { return Plugin::instantiate(name, nullptr, path); });
}

bool ImportFilter::importWithFilters(const QString& path, TrackContainer* target, int pattern, QStringList filters, Instantiate instantiate)
{
	const QString requested(path);
	if (!target || !Engine::getSong() || !Engine::projectJournal() || !Engine::audioEngine() || !Engine::patternStore() || !Engine::mixer()
		|| QThread::currentThread() != Engine::getSong()->thread() || target->thread() != Engine::getSong()->thread()) { return false; }
	const QPointer<Song> owner(Engine::getSong()); const QPointer<TrackContainer> destination(target);
	if (target == Engine::patternStore())
	{
		auto store = Engine::patternStore(); if (pattern == -1) { pattern = store->numOfPatterns() ? store->currentPattern() : 0; }
		if (pattern < 0 || pattern >= std::max(1, store->numOfPatterns())) { return false; }
	}
	else if (pattern != -1) { return false; }
	QString failure;
	try
	{
		Session session(target, pattern); session.check();
		for (const auto& name : filters)
		{
			session.check(); NativeProjectSaveSnapshot previous(owner.data()); const auto tracks = destination->tracks(); QByteArray encoded = requested.toUtf8();
			const QPointer<Plugin> candidate(instantiate(name, encoded.data()));
			const auto cleanup = qScopeGuard([candidate] { if (candidate && !candidate->parent()) { delete candidate.data(); } });
			session.check();
			if (!candidate || candidate->parent()) { throw std::runtime_error("Native import candidate retired or has another owner"); }
			bool successful = false;
			if (auto filter = dynamic_cast<ImportFilter*>(candidate.data()))
			{
				const auto previousFilter = Session::activeFilter; Session::activeFilter = filter;
				const auto active = qScopeGuard([previousFilter] { Session::activeFilter = previousFilter; });
				successful = filter->tryImport(destination.data(), pattern); session.check();
				if (!candidate || candidate->parent()) { throw std::runtime_error("Native import filter retired or changed ownership"); }
			}
			if (!successful) { previous.validate(); if (destination->tracks() != tracks) { throw std::runtime_error("Declined native import left destination tracks changed"); } }
			NativeProjectSaveSnapshot completed(owner.data());
			delete candidate.data(); session.check(); completed.validate();
			if (successful) { return true; }
		}
		failure = TrackContainer::tr("Couldn't find a filter for importing file %1.\nYou should convert this file into a format supported by LMMS using another software.").arg(requested);
	}
	catch (const std::exception& error) { failure = TrackContainer::tr("Native import failed: %1").arg(QString::fromUtf8(error.what())); }
	catch (...) { failure = TrackContainer::tr("Native import failed with an unknown native exception"); }
	if (owner && Engine::getSong() == owner.data()) { owner->collectError(failure); }
	if (gui::getGUI() != nullptr && owner) { QMessageBox::information(nullptr, TrackContainer::tr("Couldn't import file"), failure); }
	else { qWarning().noquote() << failure; }
	return false;
}




bool ImportFilter::openFile()
{
	return openFile(m_file);
}

bool ImportFilter::openFile(QFile& input)
{
	const QString requested(input.fileName()); const QPointer<Song> owner(Engine::getSong());
	if( input.open( QFile::ReadOnly ) == false )
	{
		const auto message = TrackContainer::tr( "Couldn't open file %1 "
						"for reading.\nPlease make "
						"sure you have read-"
						"permission to the file and "
						"the directory containing the "
						"file and try again!" ).arg(
							requested );
		if (gui::getGUI() != nullptr) { QMessageBox::critical(nullptr, TrackContainer::tr("Couldn't open file"), message); }
		else { if (owner && Engine::getSong() == owner.data()) { owner->collectError(message); } qWarning().noquote() << message; }
		return false;
	}
	return true;
}



} // namespace lmms
