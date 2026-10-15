/* Native selection serialization shared by headless callers and GUI adapters.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeClipTransfer.h"
#include <QCoreApplication>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "Clip.h"
#include "ControllerConnection.h"
#include "Controller.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NativeLinkedControls.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "PatternStore.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"

namespace lmms
{
namespace
{
void owner()
{
	if (!Engine::audioEngine() || !QCoreApplication::instance()
		|| QThread::currentThread() != QCoreApplication::instance()->thread())
	{ throw std::invalid_argument("Native clip transfer requires the model owner thread and audio engine"); }
	if (!Engine::getSong() || Engine::getSong()->isLoadingProject()) { throw std::invalid_argument("Native clip transfer requires a ready project"); }
}
void check() { QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } }
void connections(const QList<AutomatableModel*>& models)
{
	for (auto model : models)
	{
		check(); const auto connection = model->controllerConnection(); const auto controller = connection ? connection->getController() : nullptr;
		if (!connection) { continue; }
		if (!controller || controller->type() == Controller::ControllerType::Dummy)
		{ throw std::invalid_argument("Unresolved native clip controller binding"); }
		const auto& rack = Engine::getSong()->controllers();
		if (controller->type() != Controller::ControllerType::Midi && std::find(rack.begin(), rack.end(), controller) == rack.end())
		{ throw std::invalid_argument("Native clip controller binding is not in this project's controller rack"); }
	}
}
QString node(Track::Type type)
{
	switch (type)
	{
	case Track::Type::Instrument: return "midiclip";
	case Track::Type::Sample: return "sampleclip";
	case Track::Type::Automation: return "automationclip";
	case Track::Type::Pattern: return "patternclip";
	default: throw std::invalid_argument("Unsupported native clip transfer track type");
	}
}
}

bool NativeClipTransfer::save(const QList<Clip*>& clips, Clip* anchor, DataFile& document, QString& error)
{ return capture(clips, anchor, document, error, false); }

bool NativeClipTransfer::capture(const QList<Clip*>& clips, Clip* anchor, DataFile& document, QString& error, bool controlsOnly)
{
	try
	{
		owner(); const FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard(); check();
		if (!anchor || !anchor->getTrack() || !anchor->getTrack()->trackContainer() || clips.isEmpty() || !clips.contains(anchor))
		{ throw std::invalid_argument("Native clip transfer requires a selected anchor and live track container"); }
		if ((document.type() != DataFile::Type::DragNDropData && document.type() != DataFile::Type::ClipboardData)
			|| !document.content().firstChildElement().isNull() || !document.serializationError().isEmpty())
		{ throw std::invalid_argument("Native clip transfer requires an empty clipboard or drag document"); }
		const auto container = anchor->getTrack()->trackContainer(); const auto& tracks = container->tracks();
		QSet<Clip*> selected; QList<AutomatableModel*> models;
		for (auto clip : clips)
		{
			check();
			if (!clip || selected.contains(clip) || clip->thread() != QThread::currentThread() || !clip->getTrack()
				|| !clip->getTrack()->trackContainer() || (!controlsOnly && clip->getTrack()->trackContainer() != container)
				|| std::find(clip->getTrack()->trackContainer()->tracks().begin(), clip->getTrack()->trackContainer()->tracks().end(), clip->getTrack()) == clip->getTrack()->trackContainer()->tracks().end()
				|| std::find(clip->getTrack()->getClips().begin(), clip->getTrack()->getClips().end(), clip) == clip->getTrack()->getClips().end())
			{ throw std::invalid_argument("Native clip transfer selection has a duplicate, foreign or retired clip"); }
			clip->requireIdleForCopy();
			if (clip->nodeName() != node(clip->getTrack()->type())) { throw std::invalid_argument("Unsupported native clip transfer state type"); }
			selected.insert(clip); models.append(NativeLinkedControls::clipModels(clip));
		}
		connections(models);
		const auto generation = Engine::getSong()->errorGeneration();
		NativeLinkedControls::SaveScope links(document, models);
		auto parent = document.createElement("clips"); document.content().appendChild(parent);
		for (auto clip : clips)
		{
			check(); auto track = clip->getTrack(); const auto& sourceTracks = track->trackContainer()->tracks(); auto element = document.createElement("clip"); parent.appendChild(element);
			element.setAttribute("trackIndex", static_cast<int>(std::distance(sourceTracks.begin(), std::find(sourceTracks.begin(), sourceTracks.end(), track))));
			element.setAttribute("trackType", static_cast<int>(track->type())); element.setAttribute("trackName", track->name());
			if (controlsOnly)
			{
				auto state = document.createElement(clip->nodeName()); element.appendChild(state);
				clip->saveCopyControlSettings(document, state, track->type() == Track::Type::Automation ? "mute" : "muted");
			}
			else if (clip->saveState(document, element).isNull()) { throw std::runtime_error("Native clip transfer state was not serialized"); }
		}
		auto metadata = document.createElement("copyMetadata");
		metadata.setAttribute("initialTrackIndex", static_cast<int>(std::distance(tracks.begin(), std::find(tracks.begin(), tracks.end(), anchor->getTrack()))));
		metadata.setAttribute("trackContainerId", container->id()); metadata.setAttribute("grabbedClipPos", anchor->startPosition()); document.content().appendChild(metadata);
		if (!links.finish(document.content(), error)) { throw std::runtime_error(error.toStdString()); }
		if (!document.serializationError().isEmpty()) { throw std::runtime_error(document.serializationError().toStdString()); }
		if (Engine::getSong()->errorGeneration() != generation) { throw std::runtime_error("Native clip transfer serialization reported incomplete state"); }
		error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native clip transfer serialization failed with a nonstandard exception"; }
	DataFile::recordSerializationError(document, error); return false;
}

bool NativeClipTransfer::load(const DataFile& document, const QList<Target>& targets, QList<Clip*>& clips, QString& error)
{ return restore(document, targets, clips, error, nullptr); }

bool NativeClipTransfer::copy(const QList<Clip*>& sources, const QList<Target>& targets, QList<Clip*>& clips, QString& error)
{
	clips.clear();
	try
	{
		owner(); const FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard();
		if (sources.isEmpty() || sources.size() != targets.size()) { throw std::invalid_argument("Native selection copy source/target count is invalid"); }
		for (const auto& target : targets) { if (target.replacement && sources.contains(target.replacement)) { throw std::invalid_argument("Native selection copy cannot replace a selected source"); } }
		DataFile controls(DataFile::Type::DragNDropData);
		if (!capture(sources, sources.first(), controls, error, true)) { return false; }
		return restore(controls, targets, clips, error, &sources);
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native selection copy failed with a nonstandard exception"; }
	return false;
}

bool NativeClipTransfer::restore(const DataFile& document, const QList<Target>& targets, QList<Clip*>& clips, QString& error, const QList<Clip*>* sources)
{
	clips.clear();
	try
	{
		owner(); const FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard(); check();
		if ((document.type() != DataFile::Type::DragNDropData && document.type() != DataFile::Type::ClipboardData)
			|| !document.serializationError().isEmpty()) { throw std::invalid_argument("Invalid native clip transfer document"); }
		auto parent = document.content().firstChildElement("clips");
		if (parent.isNull() || !parent.nextSiblingElement("clips").isNull()) { throw std::invalid_argument("Native clip transfer requires exactly one clips container"); }
		std::vector<QDomElement> states;
		QHash<Track*, std::vector<Clip*>> original;
		QList<QPointer<Track>> watched; QSet<Clip*> replaced; QList<QPointer<Clip>> replacements;
		for (auto element = parent.firstChildElement(); !element.isNull(); element = element.nextSiblingElement())
		{
			check(); const auto index = states.size();
			if (element.tagName() != "clip" || index >= static_cast<std::size_t>(targets.size())) { throw std::invalid_argument("Native clip transfer target count or container is invalid"); }
			const auto& target = targets.at(static_cast<int>(index)); const auto state = element.firstChildElement(); bool valid = false; const auto type = element.attribute("trackType").toInt(&valid);
			if (!target.track || target.track->thread() != QThread::currentThread() || !target.track->trackContainer()
				|| !valid || type != static_cast<int>(target.track->type()) || state.isNull() || !state.nextSiblingElement().isNull()
				|| state.tagName() != node(target.track->type()) || target.position.getTicks() < 0 || target.position.getTicks() > MaxSongLength)
			{ throw std::invalid_argument("Native clip transfer target type, state or position is invalid"); }
			const auto& owned = target.track->trackContainer()->tracks();
			if (std::find(owned.begin(), owned.end(), target.track) == owned.end() || target.track->isNoteRecording()) { throw std::invalid_argument("Native clip transfer destination is unavailable or recording"); }
			if (dynamic_cast<SampleTrack*>(target.track)) { for (auto clip : target.track->getClips()) { if (clip) { clip->requireIdleForCopy(); } } }
			if (target.replacement)
			{
				const auto& cells = target.track->getClips(); const auto found = std::find(cells.begin(), cells.end(), target.replacement);
				if (target.track->trackContainer() != Engine::patternStore() || found == cells.end() || replaced.contains(target.replacement)
					|| target.track->numOfClips() != Engine::patternStore()->numOfPatterns() || std::find(cells.begin(), cells.end(), nullptr) != cells.end()
					|| target.position != TimePos(static_cast<int>(std::distance(cells.begin(), found)), 0))
				{ throw std::invalid_argument("Native selection replacement requires a unique existing pattern cell at its owned position"); }
				target.replacement->requireIdleForCopy(); replaced.insert(target.replacement);
			}
			replacements.append(target.replacement);
			if (!original.contains(target.track)) { original.insert(target.track, target.track->getClips()); watched.append(target.track); }
			states.push_back(state);
		}
		if (states.empty() || states.size() != static_cast<std::size_t>(targets.size())) { throw std::invalid_argument("Native clip transfer target count is invalid"); }
		const auto generation = Engine::getSong()->fatalErrorGeneration();
		const auto journal = Engine::projectJournal(); const bool wasJournalling = journal->isJournalling();
		journal->setJournalling(false); const auto restoreJournal = qScopeGuard([&] { journal->setJournalling(wasJournalling); });
		bool accepted = false;
		const auto cleanup = qScopeGuard([&] {
			if (accepted) { return; }
			for (const auto& track : watched)
			{
				if (!track) { continue; } const auto before = original.constFind(track.data());
				for (int index = track->numOfClips() - 1; index >= 0; --index)
				{
					auto clip = track->getClips().at(index);
					if (std::find(before->begin(), before->end(), clip) == before->end()) { delete clip; }
				}
			}
		});
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope links(document); DataFile::ResourceReadScope resources(document);
		std::vector<std::unique_ptr<Clip>> prepared; prepared.reserve(states.size()); clips.reserve(targets.size());
		for (std::size_t index = 0; index < states.size(); ++index)
		{
			check(); const auto& target = targets.at(static_cast<int>(index));
			prepared.push_back(sources ? sources->at(static_cast<int>(index))->constructCopy(target.track)
				: std::unique_ptr<Clip>(target.track->createClip(target.position)));
			if (!prepared.back()) { throw std::runtime_error("Native clip transfer constructor did not create a clip"); }
			if (sources) { prepared.back()->loadCopyControlSettings(states[index], target.track->type() == Track::Type::Automation ? "mute" : "muted"); }
			else { prepared.back()->restoreState(states[index]); }
			prepared.back()->movePosition(target.position);
			connections(NativeLinkedControls::clipModels(prepared.back().get()));
			if (prepared.back()->length().getTicks() < 0 || prepared.back()->length().getTicks() > MaxSongLength - target.position.getTicks())
			{ throw std::invalid_argument("Native transferred clip exceeds maximum song length"); }
		}
		if (!links.finish(document.content(), error)) { throw std::runtime_error(error.toStdString()); }
		AutomationClip::resolveAllIDs();
		if (Engine::getSong()->fatalErrorGeneration() != generation) { throw std::runtime_error("Native clip transfer load reported incomplete state"); }
		check();
		for (int index = 0; index < targets.size(); ++index)
		{
			if (!targets.at(index).replacement) { continue; }
			const auto& cells = targets.at(index).track->getClips();
			if (!replacements.at(index) || std::find(cells.begin(), cells.end(), replacements.at(index).data()) == cells.end())
			{ throw std::runtime_error("Native selection replacement retired while clips were prepared"); }
		}
		for (int index = 0; index < targets.size(); ++index)
		{
			const auto& target = targets.at(index); if (!target.replacement) { continue; }
			const auto& cells = target.track->getClips(); auto copied = prepared.at(index).get();
			const int oldIndex = static_cast<int>(std::distance(cells.begin(), std::find(cells.begin(), cells.end(), target.replacement)));
			const int newIndex = static_cast<int>(std::distance(cells.begin(), std::find(cells.begin(), cells.end(), copied)));
			target.track->swapPositionOfClips(oldIndex, newIndex); delete target.replacement; copied->movePosition(target.position);
			Engine::patternStore()->updatePatternTrack(copied);
		}
		Engine::getSong()->updateLength(); Engine::getSong()->setModified();
		for (auto& clip : prepared) { clips.append(clip.get()); } for (auto& clip : prepared) { clip.release(); }
		accepted = true;
		error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native clip transfer load failed with a nonstandard exception"; }
	clips.clear(); return false;
}
}
