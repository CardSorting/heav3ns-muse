/* Native Hydrogen project import. SPDX-License-Identifier: GPL-2.0-or-later */
#include "HydrogenImport.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QThread>
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "LocalFileMng.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "Track.h"
#include "plugin_export.h"

namespace lmms
{
extern "C"
{
Plugin::Descriptor PLUGIN_EXPORT hydrogenimport_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME), "Hydrogen Import",
	QT_TRANSLATE_NOOP("PluginBrowser", "Filter for importing Hydrogen files into LMMS"),
	"frank mather", 0x0100, Plugin::Type::ImportFilter, nullptr, nullptr, nullptr,
};
PLUGIN_EXPORT Plugin* lmms_plugin_main(Model*, void* data)
{ return new HydrogenImport(QString::fromUtf8(static_cast<const char*>(data))); }
}

namespace
{
struct InstrumentData { QString id, name, path; float volume, pan; };
struct NoteData { int instrument, position, length, key, volume, pan; };
struct PatternData { QString name; int length; std::vector<NoteData> notes; };
struct GroupData { int position, length; std::vector<int> patterns; };
struct ImportData
{
	std::vector<InstrumentData> instruments;
	std::vector<PatternData> patterns;
	std::vector<GroupData> groups;
	QStringList warnings;
	float tempo = 0;
};
QString text(const QDomElement& node, const QString& name, const QString& fallback = {})
{ const auto child = node.firstChildElement(name); return child.isNull() ? fallback : child.text().trimmed(); }
[[noreturn]] void invalid(const QString& message) { throw std::invalid_argument(message.toStdString()); }
void warn(ImportData& data, const QString& warning) { if (!data.warnings.contains(warning)) { data.warnings.append(warning); } }
int integer(const QDomElement& node, const QString& name, int fallback, int minimum, int maximum)
{
	const auto child = node.firstChildElement(name); if (child.isNull()) { return fallback; }
	bool ok = false; const auto value = child.text().trimmed().toLongLong(&ok);
	if (!ok || value < minimum || value > maximum) { invalid("Hydrogen integer is invalid: " + name); } return static_cast<int>(value);
}
float number(const QDomElement& node, const QString& name, float fallback, float minimum, float maximum)
{
	const auto child = node.firstChildElement(name); if (child.isNull()) { return fallback; }
	bool ok = false; const auto value = QLocale::c().toDouble(child.text().trimmed(), &ok);
	if (!ok || !std::isfinite(value) || value < minimum || value > maximum) { invalid("Hydrogen number is invalid: " + name); } return static_cast<float>(value);
}
int noteKey(const QString& value)
{
	// Retain LMMS's legacy Hydrogen octave offset, with checked complete parsing.
	static const QHash<QString, int> keys{{"C", 0}, {"Cs", 1}, {"Db", 1}, {"D", 2}, {"Ds", 3}, {"Ef", 3}, {"Eb", 3}, {"E", 4}, {"F", 5}, {"Fs", 6}, {"Gb", 6}, {"G", 7}, {"Gs", 8}, {"Af", 8}, {"Ab", 8}, {"A", 9}, {"As", 10}, {"Bf", 10}, {"Bb", 10}, {"B", 11}};
	static const QRegularExpression syntax("^([A-G](?:s|f|b)?)(-?[0-9]+)?$"); const auto match = syntax.match(value);
	if (!match.hasMatch() || !keys.contains(match.captured(1))) { invalid("Hydrogen note key is invalid: " + value); }
	bool ok = true; const auto octave = match.captured(2).isEmpty() ? 0 : match.captured(2).toLongLong(&ok);
	if (!ok || octave < -3 || octave > 7) { invalid("Hydrogen note octave exceeds the native range"); }
	const auto key = (octave + 3) * 12 + keys.value(match.captured(1));
	if (key < 0 || key >= NumKeys) { invalid("Hydrogen note key exceeds the native range"); } return static_cast<int>(key);
}
ImportData parse(const QDomDocument& document, const QString& path, const std::function<void()>& poll)
{
	ImportData data; const auto songs = document.elementsByTagName("song");
	if (songs.size() != 1) { invalid("Hydrogen XML must contain one song"); } const auto song = songs.at(0).toElement();
	const auto instruments = song.firstChildElement("instrumentList"), patterns = song.firstChildElement("patternList");
	if (instruments.isNull() || patterns.isNull()) { invalid("Hydrogen song has no instrument or pattern list"); }
	const auto maximum = std::numeric_limits<tick_t>::max() - 2 * std::int64_t(TimePos::ticksPerBar());
	QHash<QString, int> instrumentIds, patternIds;
	for (auto element = instruments.firstChildElement("instrument"); !element.isNull(); element = element.nextSiblingElement("instrument"))
	{
		poll(); const QString id = text(element, "id");
		if (id.isEmpty() || instrumentIds.contains(id)) { invalid("Hydrogen instrument ID is missing or duplicated"); }
		QString sample = text(element, "filename");
		const auto component = element.firstChildElement("instrumentComponent"); const auto layerParent = component.isNull() ? element : component;
		const auto layer = layerParent.firstChildElement("layer");
		if (sample.isEmpty() && !layer.isNull()) { sample = text(layer, "filename"); }
		if ((!layer.isNull() && !layer.nextSiblingElement("layer").isNull()) || (!component.isNull() && !component.nextSiblingElement("instrumentComponent").isNull()))
		{ warn(data, "Only the legacy first sample layer/component is mapped; additional Hydrogen layers/components are not represented"); }
		if (!layer.isNull() && text(layer, "smode", "forward") != "forward") { warn(data, "Hydrogen sample direction/loop modes retain the legacy forward AudioFileProcessor mapping"); }
		if (sample.isEmpty()) { warn(data, "Hydrogen instrument has no sample filename; its native row remains silent and editable"); }
		else { if (QDir::isRelativePath(sample)) { sample = QFileInfo(path).absoluteDir().absoluteFilePath(sample); } sample = QDir::cleanPath(sample); }
		const auto volume = number(element, "volume", 1, 0, std::numeric_limits<float>::max() / 100);
		if (volume > 2) { warn(data, "Hydrogen instrument gain above the native track range is clamped by the legacy volume mapping"); }
		const auto pan = number(element, "pan_R", .5f, 0, 1) - number(element, "pan_L", .5f, 0, 1);
		if (data.instruments.size() >= std::size_t(std::numeric_limits<int>::max())) { invalid("Hydrogen instrument count exceeds the native index range"); }
		instrumentIds.insert(id, static_cast<int>(data.instruments.size())); data.instruments.push_back({id, text(element, "name", id), sample, std::min(200.f, volume * 100), pan * 100});
	}
	if (data.instruments.empty()) { invalid("Hydrogen song has no instruments"); }
	for (auto element = patterns.firstChildElement("pattern"); !element.isNull(); element = element.nextSiblingElement("pattern"))
	{
		poll(); const auto name = text(element, "name"); const int length = integer(element, "size", -1, 1, static_cast<int>(maximum));
		if (name.isEmpty() || length < 1 || patternIds.contains(name)) { invalid("Hydrogen pattern name/size is missing or ambiguous"); }
		if (data.patterns.size() >= std::size_t(std::numeric_limits<int>::max())) { invalid("Hydrogen pattern count exceeds the native index range"); }
		const int index = static_cast<int>(data.patterns.size()); patternIds.insert(name, index);
		const auto id = text(element, "id"); if (!id.isEmpty() && id != name) { if (patternIds.contains(id)) { invalid("Hydrogen pattern ID is ambiguous"); } patternIds.insert(id, index); }
		PatternData pattern{name, length, {}}; const auto notes = element.firstChildElement("noteList");
		for (auto note = notes.firstChildElement("note"); !note.isNull(); note = note.nextSiblingElement("note"))
		{
			if ((pattern.notes.size() % 256) == 0) { poll(); }
			const auto instrument = text(note, "instrument"); if (!instrumentIds.contains(instrument)) { invalid("Hydrogen note references an unknown instrument: " + instrument); }
			const int position = integer(note, "position", 0, 0, length - 1);
			if (text(note, "note_off", "false") != "false") { warn(data, "Hydrogen note-off/choke events retain the legacy ordinary-note approximation"); }
			const auto velocity = number(note, "velocity", .8f, 0, 1);
			const auto pan = number(note, "pan_R", .5f, 0, 1) - number(note, "pan_L", .5f, 0, 1);
			pattern.notes.push_back({instrumentIds.value(instrument), position, std::min(48, length - position), noteKey(text(note, "key", "C0")), static_cast<int>(velocity * 100), static_cast<int>(pan * 100)});
		}
		data.patterns.push_back(std::move(pattern));
	}
	if (data.patterns.empty()) { invalid("Hydrogen song has no patterns"); }
	std::int64_t position = 0; const auto sequence = song.firstChildElement("patternSequence");
	for (auto element = sequence.firstChildElement("group"); !element.isNull(); element = element.nextSiblingElement("group"))
	{
		poll(); GroupData group{static_cast<int>(position), 0, {}};
		for (auto pattern = element.firstChildElement("patternID"); !pattern.isNull(); pattern = pattern.nextSiblingElement("patternID"))
		{
			poll(); const auto id = pattern.text().trimmed(); if (!patternIds.contains(id)) { invalid("Hydrogen sequence references an unknown pattern: " + id); }
			const int index = patternIds.value(id); if (std::find(group.patterns.begin(), group.patterns.end(), index) == group.patterns.end()) { group.patterns.push_back(index); }
			group.length = std::max(group.length, data.patterns[index].length);
		}
		if (group.patterns.empty()) { group.length = DefaultTicksPerBar; warn(data, "Empty Hydrogen sequence groups are represented as one silent native 4/4 bar"); }
		if (position > maximum - group.length) { invalid("Hydrogen sequence exceeds the native timeline range"); }
		data.groups.push_back(std::move(group)); position += data.groups.back().length;
	}
	if (!song.firstChildElement("bpm").isNull()) { data.tempo = number(song, "bpm", 0, MinTempo, MaxTempo); }
	for (const auto& field : {QString("author"), QString("notes"), QString("license")}) { if (!text(song, field).isEmpty()) { warn(data, "Hydrogen author/notes/license metadata is not mapped into native project presentation"); } }
	poll(); return data;
}

class ImportState
{
public:
	ImportState(std::function<void()> context, std::function<void()> poll)
		: m_context(std::move(context)), m_poll(std::move(poll)), m_song(Engine::getSong()), m_store(Engine::patternStore()), m_generation(m_song->replacementGeneration()), m_selected(m_store->currentPattern()) { }
	~ImportState()
	{
		if (m_committed) { return; } const FileResourceIO::Suspension recovery; const FileResourceIO::EventSuspension events;
		for (auto it = m_tracks.rbegin(); it != m_tracks.rend(); ++it)
		{
			if (!m_song || Engine::getSong() != m_song.data() || !m_store || Engine::patternStore() != m_store.data() || m_song->replacementGeneration() != m_generation) { break; }
			if (it->track && it->owner && it->track->parent() == it->owner.data() && it->track->trackContainer() == it->owner.data()
				&& std::find(it->owner->tracks().begin(), it->owner->tracks().end(), it->track.data()) != it->owner->tracks().end()) { delete it->track.data(); }
		}
		if (m_song && Engine::getSong() == m_song.data() && m_store && Engine::patternStore() == m_store.data() && m_song->replacementGeneration() == m_generation)
		{ try { m_store->setCurrentPattern(m_selected); } catch (...) { if (m_song) { m_song->collectError("Hydrogen import could not restore pattern selection after failure"); } } }
	}
	void context() const
	{
		m_context();
		for (const auto& entry : m_tracks)
		{ if (!entry.track || !entry.owner || entry.track->parent() != entry.owner.data() || entry.track->trackContainer() != entry.owner.data() || std::find(entry.owner->tracks().begin(), entry.owner->tracks().end(), entry.track.data()) == entry.owner->tracks().end()) { throw std::runtime_error("Hydrogen imported track retired or detached"); } }
		for (const auto& clip : m_clips)
		{ if (!clip.clip || !clip.track || clip.clip->getTrack() != clip.track.data() || std::find(clip.track->getClips().begin(), clip.track->getClips().end(), clip.clip.data()) == clip.track->getClips().end()) { throw std::runtime_error("Hydrogen imported clip retired or detached"); } }
		for (const auto& entry : m_instruments) { if (!entry.track || !entry.instrument || entry.track->instrument() != entry.instrument.data()) { throw std::runtime_error("Hydrogen imported instrument changed"); } }
		for (const auto& column : m_columns) { if (!column.track || PatternTrack::findPatternTrack(column.index) != column.track.data()) { throw std::runtime_error("Hydrogen imported pattern column changed"); } }
	}
	void poll() const { context(); m_poll(); context(); }
	template<class Function> void apply(Function&& function) const { context(); function(); context(); }
	Track* track(Track::Type type, TrackContainer* owner)
	{
		context(); const QPointer<TrackContainer> container(owner); const QPointer<Track> candidate(Track::create(type, owner)); bool registered = false;
		const auto cleanup = qScopeGuard([&] { if (!registered && candidate && container && candidate->trackContainer() == container.data()) { delete candidate.data(); } });
		if (!candidate) { throw std::runtime_error("Hydrogen native track could not be created"); }
		m_tracks.push_back({candidate, container}); registered = true;
		if (auto column = dynamic_cast<PatternTrack*>(candidate.data())) { m_columns.push_back({column, column->patternIndex()}); }
		context(); for (auto clip : candidate->getClips()) { watch(clip); } return candidate.data();
	}
	Clip* watch(Clip* clip)
	{
		if (!clip) { throw std::runtime_error("Hydrogen native clip could not be created"); }
		if (std::none_of(m_clips.begin(), m_clips.end(), [clip](const auto& entry) { return entry.clip.data() == clip; })) { m_clips.push_back({clip, clip->getTrack()}); }
		context(); return clip;
	}
	Clip* clip(Track* track, TimePos position) { context(); return watch(track->createClip(position)); }
	MidiClip* cell(InstrumentTrack* row, int column)
	{
		context(); if (column < 0 || column >= row->numOfClips()) { throw std::runtime_error("Hydrogen native pattern cell is unavailable"); }
		auto result = dynamic_cast<MidiClip*>(watch(row->getClips()[column])); if (!result) { throw std::runtime_error("Hydrogen native pattern cell has the wrong type"); } return result;
	}
	Instrument* instrument(InstrumentTrack* row)
	{
		context(); const QPointer<Instrument> result(row->loadInstrument("audiofileprocessor")); context();
		if (!result || row->instrument() != result.data() || !result->descriptor() || QString::fromUtf8(result->descriptor()->name) != "audiofileprocessor") { throw std::runtime_error("Hydrogen import requires the native AudioFileProcessor instrument"); }
		m_instruments.push_back({row, result}); return result.data();
	}
	void commit() { apply([&] { Engine::patternStore()->setCurrentPattern(m_selected); }); poll(); m_committed = true; }
private:
	struct TrackEntry { QPointer<Track> track; QPointer<TrackContainer> owner; };
	struct ClipEntry { QPointer<Clip> clip; QPointer<Track> track; };
	struct InstrumentEntry { QPointer<InstrumentTrack> track; QPointer<Instrument> instrument; };
	struct ColumnEntry { QPointer<PatternTrack> track; int index; };
	std::function<void()> m_context, m_poll;
	QPointer<Song> m_song; QPointer<PatternStore> m_store; std::uint64_t m_generation; int m_selected; bool m_committed = false;
	std::vector<TrackEntry> m_tracks; std::vector<ClipEntry> m_clips; std::vector<InstrumentEntry> m_instruments; std::vector<ColumnEntry> m_columns;
};

void applyImport(const ImportData& data, ImportState& state)
{
	std::vector<QPointer<PatternTrack>> columns;
	for (const auto& pattern : data.patterns)
	{
		state.poll(); const QPointer<PatternTrack> column(dynamic_cast<PatternTrack*>(state.track(Track::Type::Pattern, Engine::getSong())));
		if (!column) { throw std::runtime_error("Hydrogen native pattern track is unavailable"); }
		columns.push_back(column); state.apply([&] { column->setName(pattern.name); });
	}
	std::vector<QPointer<InstrumentTrack>> rows;
	for (const auto& source : data.instruments)
	{
		state.poll(); const QPointer<InstrumentTrack> row(dynamic_cast<InstrumentTrack*>(state.track(Track::Type::Instrument, Engine::patternStore())));
		if (!row) { throw std::runtime_error("Hydrogen native instrument row is unavailable"); }
		rows.push_back(row); const QPointer<Instrument> instrument(state.instrument(row.data())); QString error; bool loaded = false;
		state.apply([&] { loaded = instrument->setFileResource("sample", source.path, error); });
		if (!loaded)
		{
			const auto resources = instrument->fileResources(); state.context();
			if (std::none_of(resources.begin(), resources.end(), [&](const auto& resource) { return resource.key == "sample" && resource.path == source.path; })) { throw std::runtime_error(("Hydrogen unresolved sample reference was not retained: " + source.path).toStdString()); }
			state.apply([&] { Engine::getSong()->collectError("Hydrogen import retained a silent unresolved sample: " + source.path + ": " + error); });
		}
		state.apply([&] { row->setName(source.name); }); state.apply([&] { row->volumeModel()->setValue(source.volume); }); state.apply([&] { row->panningModel()->setValue(source.pan); });
		for (std::size_t pattern = 0; pattern < data.patterns.size(); ++pattern)
		{
			state.poll(); const QPointer<MidiClip> cell(state.cell(row.data(), columns[pattern]->patternIndex()));
			state.apply([&] { cell->clearNotes(); }); cell->setType(MidiClip::Type::MelodyClip); cell->setAutoResize(false);
			state.apply([&] { cell->changeLength(TimePos(data.patterns[pattern].length)); });
		}
	}
	for (std::size_t pattern = 0; pattern < data.patterns.size(); ++pattern)
	{
		std::size_t count = 0;
		for (const auto& source : data.patterns[pattern].notes)
		{
			if ((count++ % 256) == 0) { state.poll(); }
			const QPointer<MidiClip> cell(state.cell(rows[source.instrument].data(), columns[pattern]->patternIndex()));
			Note note(source.length, source.position, source.key, source.volume, source.pan); state.apply([&] { cell->addNote(note, false); });
		}
	}
	for (const auto& group : data.groups)
	{
		state.poll();
		for (const int index : group.patterns)
		{
			const QPointer<PatternClip> clip(dynamic_cast<PatternClip*>(state.clip(columns[index].data(), TimePos(group.position))));
			if (!clip) { throw std::runtime_error("Hydrogen native arrangement clip is unavailable"); }
			clip->setAutoResize(false); state.apply([&] { clip->changeLength(TimePos(data.patterns[index].length)); });
		}
	}
	if (data.tempo > 0)
	{
		state.poll(); const QPointer<AutomationTrack> track(dynamic_cast<AutomationTrack*>(state.track(Track::Type::Automation, Engine::getSong())));
		if (!track) { throw std::runtime_error("Hydrogen tempo track is unavailable"); }
		state.apply([&] { track->setName("Hydrogen tempo"); });
		const QPointer<AutomationClip> clip(dynamic_cast<AutomationClip*>(state.clip(track.data(), TimePos(0))));
		if (!clip) { throw std::runtime_error("Hydrogen tempo clip is unavailable"); }
		state.apply([&] { clip->addObject(&Engine::getSong()->tempoModel()); }); state.apply([&] { clip->putValue(0, data.tempo, false); });
	}
	for (const auto& warning : data.warnings) { state.apply([&] { Engine::getSong()->collectError("Hydrogen import: " + warning); }); }
}
}

HydrogenImport::HydrogenImport(const QString& path) : ImportFilter(path, &hydrogenimport_plugin_descriptor) { }
bool HydrogenImport::readSong() { return tryImport(Engine::getSong()); }
bool HydrogenImport::tryImport(TrackContainer* target)
{
	if (!target || !Engine::getSong() || target != Engine::getSong() || QThread::currentThread() != target->thread()) { return false; }
	const QString requested(file().fileName()); const QPointer<Song> owner(Engine::getSong());
	try
	{
		WorkScope work(this, target, -1); work.check();
		const auto document = LocalFileMng::openXmlDocument(requested); work.checkContext();
		const bool expected = QFileInfo(requested).suffix().compare("h2song", Qt::CaseInsensitive) == 0;
		if (document.isNull()) { if (!expected) { return false; } throw std::runtime_error("Hydrogen XML could not be opened or parsed"); }
		const auto songs = document.elementsByTagName("song");
		if (!expected && (songs.isEmpty() || songs.at(0).firstChildElement("instrumentList").isNull() || songs.at(0).firstChildElement("patternList").isNull())) { return false; }
		const auto data = parse(document, requested, [&] { work.check(); }); work.checkContext();
		ImportState state([&] { work.checkContext(); }, [&] { work.check(); }); applyImport(data, state); state.commit(); return true;
	}
	catch (const std::exception& error) { if (owner && Engine::getSong() == owner.data()) { owner->collectError("Hydrogen import failed: " + QString::fromUtf8(error.what())); } }
	catch (...) { if (owner && Engine::getSong() == owner.data()) { owner->collectError("Hydrogen import failed with an unknown native exception"); } }
	return false;
}
} // namespace lmms
