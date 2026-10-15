/* Declared native project dependency observations. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioProjectResources.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
#include <utility>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "EnvelopeAndLfoParameters.h"
#include "InstrumentSoundShaping.h"
#include <array>
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "Mixer.h"
#include "Oscillator.h"
#include "PathUtil.h"
#include "PatternStore.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"

namespace lmms
{
namespace
{
[[noreturn]] void refuse(const char* message) { throw std::runtime_error(message); }
void work() { QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } }
int pageNumber(const QJsonObject& arguments, const char* key, int fallback, int minimum, int maximum)
{
	if (!arguments.contains(key)) { return fallback; } const auto value = arguments.value(key); const double number = value.toDouble(-1);
	if (!value.isDouble() || !std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number) { refuse("Invalid resource inventory page range"); }
	return static_cast<int>(number);
}
QString preview(const QString& text, int maximum)
{ auto out = text.left(maximum); if (out.size() < text.size() && !out.isEmpty() && out.at(out.size() - 1).isHighSurrogate()) { out.chop(1); } return out; }
void text(QJsonObject& object, const QString& key, const QString& value, bool names)
{
	if (names || value.size() <= 256) { object.insert(key, value); return; }
	object.insert(key + "Preview", preview(value, 256)); object.insert(key + "Length", static_cast<double>(value.size())); object.insert(key + "Truncated", value.size() > 256);
}
EffectChain* effects(Track* track)
{
	if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { return instrument->audioBusHandle()->effects(); }
	if (auto sample = dynamic_cast<SampleTrack*>(track)) { return sample->audioBusHandle()->effects(); }
	return nullptr;
}
template<class T, class Range> bool sameRoster(const std::vector<QPointer<T>>& before, const Range& after)
{
	if (before.size() != static_cast<std::size_t>(after.size())) { return false; }
	for (std::size_t index = 0; index < before.size(); ++index) { if (!before[index] || before[index].data() != after[index]) { return false; } }
	return true;
}
bool sameResource(const Plugin::FileResource& a, const Plugin::FileResource& b)
{ return a.key == b.key && a.name == b.name && a.path == b.path && a.fileTypes == b.fileTypes && a.selector == b.selector && a.selectionValue == b.selectionValue && a.requiredForRestore == b.requiredForRestore && a.identity == b.identity && a.unresolved == b.unresolved; }

class Inventory
{
	struct Container { QPointer<TrackContainer> owner; std::vector<QPointer<Track>> tracks; };
	struct TrackState
	{
		QPointer<Track> track; QPointer<TrackContainer> owner; QString name;
		QPointer<Instrument> instrument; Instrument* selected = nullptr;
		QPointer<EffectChain> chain; EffectChain* chainIdentity = nullptr; std::vector<QPointer<Clip>> clips;
	};
	struct Chain { QPointer<EffectChain> chain; std::vector<QPointer<Effect>> effects; };
	struct Actor { QPointer<Plugin> plugin; QJsonObject owner; QString name, pluginName, operation; std::vector<Plugin::FileResource> resources; std::vector<QPointer<AutomatableModel>> selectors; std::vector<float> selectorValues; std::vector<int> selectorIds; QPointer<Track> namedTrack; };
	struct SampleState { QPointer<SampleClip> clip; std::shared_ptr<const SampleBuffer> buffer; QString path, name; int id; bool unresolved; int rate; std::optional<std::pair<int, int>> window; };
	struct EnvelopeWave { QPointer<InstrumentTrack> track; QPointer<InstrumentSoundShaping> shaping; QPointer<EnvelopeAndLfoParameters> parameters; std::shared_ptr<const SampleBuffer> buffer; QString target, path; bool unresolved; float selection; int id, selectorId; };
	struct Wave { QPointer<LfoController> controller; std::shared_ptr<const SampleBuffer> buffer; float selection; QString name; int id, selectorId; QString path; bool unresolved; };
	struct Named { QPointer<QObject> owner; QString value; };
	struct Row { QPointer<QObject> nameOwner; QJsonObject owner, details, file; QString ownerName, plugin, operation; Plugin::FileResource resource; bool missingPlugin = false; std::optional<float> selectorValue; int selectorId = -1; };
public:
	Inventory() : m_song(Engine::getSong()), m_patterns(Engine::patternStore()), m_mixer(Engine::mixer())
	{
		if (!m_song || !m_patterns || !m_mixer || m_song->thread() != QThread::currentThread()) { refuse("Resource inventory requires live native roots on their owner thread"); }
		m_project = m_song->replacementGeneration(); m_routing = m_mixer->replacementGeneration(); m_filename = m_song->projectFileName(); roots(); work(); roots();
		for (auto owner : {static_cast<TrackContainer*>(m_song.data()), static_cast<TrackContainer*>(m_patterns.data())})
		{
			Container container; container.owner = owner; const QString location = owner == m_song.data() ? "song" : "pattern";
			int index = 0;
			for (auto track : owner->tracks())
			{
				if (!track || track->thread() != QThread::currentThread()) { refuse("Resource inventory contains a foreign or absent track"); }
				container.tracks.emplace_back(track); TrackState state; state.track = track; state.owner = owner; state.name = track->Track::name(); state.chain = effects(track); state.chainIdentity = state.chain.data();
				if (auto instrument = dynamic_cast<InstrumentTrack*>(track))
				{
					if (instrument->isRetiring() || instrument->isReplacingInstrument()) { refuse("Resource inventory instrument track is changing"); }
					auto shaping = instrument->soundShaping();
					const auto envelopes = std::array{std::pair{QString("volume"), &shaping->getVolumeParameters()}, std::pair{QString("cutoff"), &shaping->getCutoffParameters()}, std::pair{QString("resonance"), &shaping->getResonanceParameters()}};
					for (const auto& [target, parameters] : envelopes)
					{
						const auto buffer = parameters->getLfoUserWave(); const auto path = parameters->lfoUserWaveFile(); const bool missing = parameters->lfoUserWaveUnresolved(); const auto selection = parameters->getLfoWaveModel().storedValue();
						m_envelopes.push_back({instrument, shaping, parameters, buffer, target, path, missing, selection, static_cast<int>(parameters->id()), static_cast<int>(parameters->getLfoWaveModel().id())});
						// Empty native shaping controls are not file dependencies; still capture them to detect newly introduced references.
						if (path.isEmpty()) { continue; }
						Row row; row.owner = {{"kind", "instrumentShapingWave"}, {"container", location}, {"track", index}, {"target", target}, {"nativeId", static_cast<int>(parameters->id())}}; row.nameOwner = instrument; row.ownerName = state.name; row.operation = "instrument.shaping.waveform.set";
						row.resource = {"userwave", target + " envelope/LFO waveform", path, {}, &parameters->getLfoWaveModel(), static_cast<float>(EnvelopeAndLfoParameters::LfoShape::UserDefinedWave), true, buffer, missing}; row.selectorValue = selection; row.selectorId = static_cast<int>(parameters->getLfoWaveModel().id());
						row.details = {{"sourceFrames", static_cast<double>(buffer->size())}, {"sampleRate", static_cast<int>(buffer->sampleRate())}, {"retainedSource", !buffer->acceptedFile().isEmpty()}}; append(std::move(row));
					}
					state.instrument = instrument->instrument(); state.selected = state.instrument.data();
					if (state.instrument) { actor(state.instrument.data(), {{"kind", "instrument"}, {"container", location}, {"track", index}}, state.name, "instrument.resource.set", track); }
				}
				int clipIndex = 0;
				for (auto clip : track->getClips())
				{
					if (!clip || clip->thread() != QThread::currentThread()) { refuse("Resource inventory contains a foreign or absent clip"); } state.clips.emplace_back(clip);
					if (auto sample = dynamic_cast<SampleClip*>(clip))
					{
						SampleState source{sample, sample->sample().buffer(), sample->sampleFile(), sample->name(), static_cast<int>(sample->id()), sample->sampleUnresolved(), sample->sourceSampleRate(), sample->sourceSampleWindow()}; m_samples.push_back(source);
						Row row; row.owner = {{"kind", "sample"}, {"container", location}, {"track", index}, {"clip", clipIndex}, {"nativeId", static_cast<int>(sample->id())}}; row.ownerName = sample->name(); row.operation = "sample.load";
						row.resource = {"sample", "Sample source", source.path, {}, nullptr, 0, true, source.buffer, source.unresolved};
						row.details = {{"sourceFrames", static_cast<double>(source.buffer->size())}, {"sampleRate", source.rate}, {"retainedSource", !source.buffer->acceptedFile().isEmpty()},
							{"startFrame", source.window ? QJsonValue(source.window->first) : QJsonValue(QJsonValue::Null)}, {"endFrame", source.window ? QJsonValue(source.window->second) : QJsonValue(QJsonValue::Null)}}; append(std::move(row));
					}
					++clipIndex;
				}
				if (state.chain) { chain(state.chain.data(), {{"kind", "effect"}, {"scope", "track"}, {"container", location}, {"track", index}}, state.name, track); }
				m_tracks.push_back(std::move(state)); ++index;
			}
			m_containers.push_back(std::move(container));
		}
		for (int index = 0; index < m_mixer->numChannels(); ++index)
		{
			auto channel = m_mixer->mixerChannel(index); m_channels.push_back(channel); m_channelAnchors.emplace_back(&channel->m_fxChain); m_channelNames.push_back(channel->m_name);
			chain(&channel->m_fxChain, {{"kind", "effect"}, {"scope", "mixer"}, {"channel", index}}, channel->m_name);
		}
		int index = 0;
		for (auto controller : m_song->controllers())
		{
			if (!controller || controller->isRetiring() || controller->thread() != QThread::currentThread()) { refuse("Resource inventory rack is changing or foreign"); } m_controllers.emplace_back(controller);
			if (auto lfo = dynamic_cast<LfoController*>(controller))
			{
				const auto buffer = lfo->userWaveBuffer(); const auto selection = lfo->waveModel().storedValue(); m_waves.push_back({lfo, buffer, selection, lfo->Controller::name(), static_cast<int>(lfo->id()), static_cast<int>(lfo->waveModel().id()), lfo->userWaveFile(), lfo->userWaveUnresolved()});
				Row row; row.owner = {{"kind", "controllerWave"}, {"controller", index}, {"nativeId", static_cast<int>(lfo->id())}}; row.nameOwner = lfo; row.ownerName = lfo->Controller::name(); row.operation = "controller.waveform.set";
				row.resource = {"userwave", "LFO user waveform", lfo->userWaveFile(), {}, &lfo->waveModel(), static_cast<float>(Oscillator::WaveShape::UserDefined), true, buffer, lfo->userWaveUnresolved()}; row.selectorValue = selection; row.selectorId = static_cast<int>(lfo->waveModel().id());
				row.details = {{"sourceFrames", static_cast<double>(buffer->size())}, {"sampleRate", static_cast<int>(buffer->sampleRate())}, {"retainedSource", !buffer->acceptedFile().isEmpty()}}; append(std::move(row));
			}
			++index;
		}
		// All rosters and weak actors are captured before invoking any plugin getter.
		// Virtual name getters also run only after this complete capture.
		captureNames();
		for (auto& source : m_actors)
		{
			work(); roots(); if (!source.plugin || source.plugin->thread() != QThread::currentThread()) { refuse("Native resource plugin retired or changed threads during inspection"); }
			source.resources = source.plugin->fileResources(); roots(); if (!source.plugin) { refuse("Native resource plugin retired in its resource getter"); }
			for (const auto& resource : source.resources)
			{
				if (!std::isfinite(resource.selectionValue)) { refuse("Native resource selection is not finite"); }
				if (resource.selector && resource.selector->thread() != QThread::currentThread()) { refuse("Native resource selector belongs to another thread"); }
				source.selectors.emplace_back(resource.selector); const auto value = resource.selector ? resource.selector->storedValue() : 0.f; source.selectorValues.push_back(value); source.selectorIds.push_back(resource.selector ? static_cast<int>(resource.selector->id()) : -1);
				if (!std::isfinite(value)) { refuse("Native resource selector is not finite"); }
				Row row; row.owner = source.owner; row.ownerName = source.name; row.plugin = source.pluginName; row.operation = source.operation; row.resource = resource;
				if (resource.selector) { row.selectorValue = value; row.selectorId = static_cast<int>(resource.selector->id()); } append(std::move(row));
			}
		}
		validate(); applyNames();
	}
	void roots() const
	{
		if (!m_song || !m_patterns || !m_mixer || Engine::getSong() != m_song.data() || Engine::patternStore() != m_patterns.data() || Engine::mixer() != m_mixer.data()
			|| m_song->thread() != QThread::currentThread() || m_patterns->thread() != QThread::currentThread() || m_mixer->thread() != QThread::currentThread()
			|| m_song->isLoadingProject() || m_song->replacementGeneration() != m_project || m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename)
		{ refuse("Native project roots changed during resource inspection"); }
	}
	void validate() const
	{
		roots();
		for (const auto& container : m_containers) { if (!container.owner || !sameRoster(container.tracks, container.owner->tracks())) { refuse("Native resource track roster changed"); } }
		for (const auto& state : m_tracks)
		{
			if (!state.track || !state.owner || state.track->trackContainer() != state.owner.data() || state.track->Track::name() != state.name || !sameRoster(state.clips, state.track->getClips()) || effects(state.track.data()) != state.chainIdentity || state.chain.data() != state.chainIdentity) { refuse("Native resource track or clip context changed"); }
			if (auto instrument = dynamic_cast<InstrumentTrack*>(state.track.data()); instrument && (instrument->isRetiring() || instrument->isReplacingInstrument() || instrument->instrument() != state.selected || state.instrument.data() != state.selected)) { refuse("Native resource instrument selection changed"); }
		}
		if (m_channels.size() != static_cast<std::size_t>(m_mixer->numChannels())) { refuse("Native resource mixer roster changed"); }
		for (std::size_t index = 0; index < m_channels.size(); ++index) { if (!m_channelAnchors[index] || m_mixer->mixerChannel(static_cast<int>(index)) != m_channels[index] || &m_channels[index]->m_fxChain != m_channelAnchors[index].data() || m_channels[index]->m_name != m_channelNames[index]) { refuse("Native resource mixer channel retired or changed"); } }
		for (const auto& state : m_chains) { if (!state.chain || state.chain->isRetiring() || !sameRoster(state.effects, state.chain->effects())) { refuse("Native resource effect chain changed"); } }
		if (!sameRoster(m_controllers, m_song->controllers())) { refuse("Native resource controller rack changed"); }
		for (const auto& controller : m_controllers) { if (controller->isRetiring() || controller->thread() != QThread::currentThread()) { refuse("Native resource controller retired or changed threads"); } }
		for (const auto& state : m_samples) { if (!state.clip || state.clip->thread() != QThread::currentThread() || state.clip->name() != state.name || static_cast<int>(state.clip->id()) != state.id || state.clip->sample().buffer() != state.buffer || state.clip->sampleFile() != state.path || state.clip->sampleUnresolved() != state.unresolved || state.clip->sourceSampleRate() != state.rate || state.clip->sourceSampleWindow() != state.window) { refuse("Native sample resource changed during inspection"); } }
		for (const auto& state : m_envelopes)
		{
			if (!state.track || !state.shaping || !state.parameters || state.track->soundShaping() != state.shaping.data() || state.parameters->parent() != state.shaping.data()
				|| state.parameters->thread() != QThread::currentThread() || static_cast<int>(state.parameters->id()) != state.id || static_cast<int>(state.parameters->getLfoWaveModel().id()) != state.selectorId || state.parameters->getLfoUserWave() != state.buffer
				|| state.parameters->lfoUserWaveFile() != state.path || state.parameters->lfoUserWaveUnresolved() != state.unresolved || state.parameters->getLfoWaveModel().storedValue() != state.selection)
			{ refuse("Native shaping waveform changed during resource inspection"); }
		}
		for (const auto& state : m_waves) { if (!state.controller || state.controller->Controller::name() != state.name || static_cast<int>(state.controller->id()) != state.id || static_cast<int>(state.controller->waveModel().id()) != state.selectorId || state.controller->userWaveBuffer() != state.buffer || state.controller->userWaveFile() != state.path || state.controller->userWaveUnresolved() != state.unresolved || state.controller->waveModel().storedValue() != state.selection) { refuse("Native LFO resource changed during inspection"); } }
		for (const auto& state : m_actors)
		{
			if (!state.plugin || state.plugin->thread() != QThread::currentThread() || static_cast<int>(state.plugin->id()) != state.owner.value("nativeId").toInt()) { refuse("Native resource plugin retired or changed"); }
			for (std::size_t index = 0; index < state.resources.size(); ++index) { if (state.selectors[index].data() != state.resources[index].selector || (state.selectors[index] && (state.selectors[index]->thread() != QThread::currentThread() || state.selectors[index]->storedValue() != state.selectorValues[index] || static_cast<int>(state.selectors[index]->id()) != state.selectorIds[index]))) { refuse("Native resource selector changed"); } }
		}
	}
	QJsonObject page(int offset, int limit, bool names, const QString& expected)
	{
		QCryptographicHash digest(QCryptographicHash::Sha256); static const auto process = QUuid::createUuid().toString(QUuid::WithoutBraces);
		hashText(digest, process); hashText(digest, QString::number(m_project)); hashText(digest, QString::number(m_routing)); hashText(digest, m_filename);
		QJsonObject counts; QJsonArray items; qint64 bytes = 2; int index = 0, unresolved = 0, required = 0;
		for (auto& row : m_rows)
		{
			work(); roots(); row.file = probe(row); const auto status = row.file.value("status").toString(); counts.insert(status, counts.value(status).toInt() + 1);
			if (row.resource.unresolved || row.missingPlugin) { ++unresolved; }
			if (row.missingPlugin || (!row.resource.path.isEmpty() && row.resource.requiredForRestore)) { ++required; }
			auto hashed = project(row, false); hashed.remove("index"); digest.addData(QJsonDocument(hashed).toJson(QJsonDocument::Compact));
			for (const auto& value : {row.ownerName, row.plugin, row.operation, row.resource.key, row.resource.name, row.resource.path, row.file.value("resolved").toString()}) { hashText(digest, value); }
			for (const auto& type : row.resource.fileTypes) { hashText(digest, type); }
			if (index >= offset && items.size() < limit && !m_pageFull)
			{
				auto item = project(row, names); item.insert("index", index); const qint64 length = static_cast<qint64>(QJsonDocument(item).toJson(QJsonDocument::Compact).size()) + 1;
				if (bytes + length > 512 * 1024) { if (items.isEmpty()) { refuse("A resource inventory row exceeds 512 KiB; request names=false"); } m_pageFull = true; }
				else { items.append(item); bytes += length; }
			}
			++index;
		}
		// Re-run returned native getters after work/probes, then validate every weak
		// roster and source without dispatching further cooperative work.
		work(); roots();
		for (const auto& source : m_actors)
		{
			roots(); if (!source.plugin || source.plugin->thread() != QThread::currentThread()) { refuse("Native resource plugin retired or changed threads before final validation"); }
			const auto current = source.plugin->fileResources(); roots(); if (!source.plugin || current.size() != source.resources.size()) { refuse("Native declared resource inventory changed"); }
			for (std::size_t slot = 0; slot < current.size(); ++slot) { if (!sameResource(source.resources[slot], current[slot])) { refuse("Native declared resource metadata or backing changed"); } }
		}
		validateNames(); validate(); const auto generation = QString::fromLatin1(digest.result().toHex());
		if (!expected.isEmpty() && expected != generation) { refuse("Resource observation changed; restart paging without resourceGeneration"); }
		return {{"section", "resources"}, {"resources", items}, {"total", index}, {"offset", offset}, {"limit", limit}, {"nextOffset", static_cast<double>(static_cast<qint64>(offset) + items.size())},
			{"hasMore", static_cast<qint64>(offset) + items.size() < index}, {"resourceGeneration", generation}, {"fileObservations", counts}, {"declaredUnresolvedCount", unresolved}, {"requiredForRestoreCount", required}, {"completedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
			{"coverage", "declared native instrument/effect resources, sample sources, nonempty native instrument shaping waves, rack LFO waves and unavailable effects; opaque or undeclared dependencies are excluded"},
			{"verification", "filesystem metadata observations only; no byte hashing, decode, plugin load, portability or audible-output verification"},
			{"handleScope", "current process, native project and declared resource observations; keep resourceGeneration fixed while paging and rediscover after changes or restart"}};
	}
private:
	template<class T> QString readName(const QPointer<T>& actor) const
	{
		validate(); if (!actor) { refuse("Native name owner retired before its getter"); }
		const QString& returned = actor->name();
		// Do not copy or compare borrowed QString storage until its owner and all
		// captured native actors survive the returned virtual callback.
		validate(); if (!actor) { refuse("Native name owner retired in its getter"); }
		return QString(returned);
	}
	void captureNames()
	{
		for (const auto& state : m_tracks) { work(); roots(); m_names.push_back({state.track.data(), readName(state.track)}); }
		for (const auto& state : m_waves) { work(); roots(); m_names.push_back({state.controller.data(), readName(state.controller)}); }
		for (auto& source : m_actors) { if (source.namedTrack) { source.name = observedName(source.namedTrack.data()); } }
	}
	QString observedName(QObject* owner) const
	{ for (const auto& name : m_names) { if (name.owner.data() == owner) { return name.value; } } refuse("Native resource name owner was not captured"); }
	void applyNames()
	{ for (auto& row : m_rows) { if (row.nameOwner) { row.ownerName = observedName(row.nameOwner.data()); } } }
	void validateNames() const
	{
		for (const auto& state : m_tracks) { if (readName(state.track) != observedName(state.track.data())) { refuse("Native reported track name changed during resource inspection"); } }
		for (const auto& state : m_waves) { if (readName(state.controller) != observedName(state.controller.data())) { refuse("Native reported controller name changed during resource inspection"); } }
	}
	void actor(Plugin* plugin, QJsonObject owner, const QString& name, const QString& operation, Track* namedTrack = nullptr)
	{
		if (!plugin || plugin->thread() != QThread::currentThread()) { refuse("Native resource plugin is absent or foreign"); }
		owner.insert("nativeId", static_cast<int>(plugin->id())); const auto descriptor = plugin->descriptor();
		m_actors.push_back({plugin, owner, name, descriptor ? QString::fromUtf8(descriptor->name) : QString{}, operation, {}, {}, {}, {}, namedTrack});
	}
	void chain(EffectChain* chain, const QJsonObject& owner, const QString& name, Track* namedTrack = nullptr)
	{
		if (chain->isRetiring() || chain->thread() != QThread::currentThread()) { refuse("Native resource effect chain is changing or foreign"); } Chain state; state.chain = chain; int index = 0;
		for (auto effect : chain->effects())
		{
			if (!effect) { refuse("Native resource effect is absent"); } state.effects.emplace_back(effect); auto context = owner; context.insert("effect", index);
			if (auto missing = dynamic_cast<DummyEffect*>(effect))
			{ Row row; row.owner = context; row.owner.insert("nativeId", static_cast<int>(effect->id())); row.nameOwner = namedTrack; row.ownerName = name; row.plugin = missing->originalPluginData().attribute("name"); row.resource = {"plugin", "Unavailable effect", {}, {}}; row.resource.unresolved = true; row.missingPlugin = true; append(std::move(row)); }
			else { actor(effect, context, name, {}, namedTrack); }
			++index;
		}
		m_chains.push_back(std::move(state));
	}
	void append(Row row)
	{ if (m_rows.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max())) { refuse("Native resource inventory exceeds the page index range"); } m_rows.push_back(std::move(row)); }
	QJsonObject probe(const Row& row) const
	{
		if (row.missingPlugin) { return {{"status", "missingPlugin"}}; } const auto& path = row.resource.path;
		if (path.isEmpty()) { return {{"status", "noFileReference"}}; }
		if (path.contains(QChar::Null)) { return {{"status", "unresolvable"}}; }
		bool error = false; const auto resolved = PathUtil::toAbsolute(path, &error); if (error || resolved.isEmpty()) { return {{"status", "unresolvable"}}; }
		const QFileInfo file(resolved); QString status = !file.exists() ? "missing" : !file.isFile() ? "notFile" : !file.isReadable() ? "unreadable" : "readableFile";
		return {{"status", status}, {"resolved", resolved}, {"symbolicLink", file.isSymLink()}, {"bytes", file.isFile() ? QJsonValue(static_cast<double>(file.size())) : QJsonValue(QJsonValue::Null)}};
	}
	QJsonObject project(const Row& row, bool names) const
	{
		auto file = row.file; if (file.contains("resolved")) { const auto resolved = file.take("resolved").toString(); text(file, "resolved", resolved, names); }
		QJsonObject result{{"owner", row.owner}, {"requiredForRestore", row.missingPlugin || (!row.resource.path.isEmpty() && row.resource.requiredForRestore)}, {"unresolved", row.resource.unresolved}, {"nativeBackingPinned", static_cast<bool>(row.resource.identity)}, {"file", file}, {"native", row.details}};
		text(result, "ownerName", row.ownerName, names); text(result, "plugin", row.plugin, names); text(result, "key", row.resource.key, names); text(result, "name", row.resource.name, names); text(result, "path", row.resource.path, names);
		result.insert("repairOperation", row.operation.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(row.operation));
		QJsonArray types; for (int index = 0; index < 16 && index < row.resource.fileTypes.size(); ++index) { types.append(preview(row.resource.fileTypes[index], 64)); }
		result.insert("fileTypePreviews", types); result.insert("fileTypeCount", static_cast<double>(row.resource.fileTypes.size()));
		if (row.selectorValue) { result.insert("selectionParameter", row.selectorId); result.insert("selectionValue", row.resource.selectionValue); result.insert("selectorManualValue", *row.selectorValue); result.insert("selectedStored", *row.selectorValue == row.resource.selectionValue); }
		return result;
	}
	void hashText(QCryptographicHash& digest, const QString& value) const
	{
		digest.addData(QByteArray::number(static_cast<qulonglong>(value.size())) + ':');
		// Hash native UTF-16 chunks without making a full UTF-8 duplicate or
		// changing meaning at a surrogate boundary. Tokens are process-local.
		std::size_t offset = 0; const auto bytes = static_cast<std::size_t>(value.size()) * sizeof(QChar); const auto data = reinterpret_cast<const char*>(value.constData());
		while (offset < bytes) { work(); roots(); const auto count = std::min<std::size_t>(8192, bytes - offset); digest.addData(QByteArray::fromRawData(data + offset, static_cast<int>(count))); offset += count; }
	}
	QPointer<Song> m_song; QPointer<PatternStore> m_patterns; QPointer<Mixer> m_mixer;
	std::uint64_t m_project = 0, m_routing = 0; QString m_filename;
	std::vector<Container> m_containers; std::vector<TrackState> m_tracks; std::vector<Chain> m_chains; std::vector<Actor> m_actors;
	std::vector<MixerChannel*> m_channels; std::vector<QPointer<EffectChain>> m_channelAnchors; std::vector<QString> m_channelNames; std::vector<QPointer<Controller>> m_controllers;
	std::vector<Named> m_names; std::vector<EnvelopeWave> m_envelopes; std::vector<SampleState> m_samples; std::vector<Wave> m_waves; std::vector<Row> m_rows; bool m_pageFull = false;
};
}

QJsonObject StudioProjectResources::inspect(const QJsonObject& arguments)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()) { refuse("Resource inventory requires the application thread"); }
	if (!Engine::audioEngine() || !Engine::getSong() || !Engine::patternStore() || !Engine::mixer()) { refuse("Resource inventory requires an initialized native engine"); }
	for (auto it = arguments.begin(); it != arguments.end(); ++it) { if (it.key() != "section" && it.key() != "offset" && it.key() != "limit" && it.key() != "names" && it.key() != "resourceGeneration") { refuse("Unknown resource inventory argument"); } }
	if (arguments.contains("section") && arguments.value("section") != QJsonValue("resources")) { refuse("Resource inventory section must be resources"); }
	const int offset = pageNumber(arguments, "offset", 0, 0, std::numeric_limits<int>::max()), limit = pageNumber(arguments, "limit", 128, 1, 1024);
	if (arguments.contains("names") && !arguments.value("names").isBool()) { refuse("Resource inventory names must be boolean"); }
	const auto token = arguments.value("resourceGeneration");
	if (arguments.contains("resourceGeneration") && (!token.isString() || !QRegularExpression("^[0-9a-f]{64}$").match(token.toString()).hasMatch())) { refuse("Invalid native resource observation token"); }
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; Inventory inventory;
	return inventory.page(offset, limit, !arguments.contains("names") || arguments.value("names").toBool(), token.toString());
}
}
