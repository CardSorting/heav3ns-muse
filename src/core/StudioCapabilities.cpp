/* Native studio capabilities. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioCapabilities.h"
#include "StudioResources.h"
#include "StudioProjectResources.h"
#include "StudioAssets.h"
#include "EnvelopeAndLfoParameters.h"
#include "InstrumentSoundShaping.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QPointer>
#include <QSet>
#include <QScopeGuard>
#include <QHash>
#include <QThread>

#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AudioEngineWorkerThread.h"
#include "AudioDummy.h"
#include "AudioFileDevice.h"
#include "AutomatableModel.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Clip.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "ComboBoxModel.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "GraphModel.h"
#include "Instrument.h"
#include "ImportFilter.h"
#include "InstrumentTrack.h"
#include "InstrumentFunctions.h"
#include "Keymap.h"
#include "LfoController.h"
#include "Microtuner.h"
#include "MidiDummy.h"
#include "MidiPort.h"
#include "MidiController.h"
#include "MidiClip.h"
#include "Mixer.h"
#include "Note.h"
#include "NativeClipTransfer.h"
#include "NativeTrackSaveSnapshot.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PathUtil.h"
#include "Oscillator.h"
#include "PluginFactory.h"
#include "PluginHost.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "SampleClip.h"
#include "Scale.h"
#include "Song.h"
#include "NativeControlLinkJournal.h"
#include "NativeLinkedControls.h"
#include "NativeNoteTransfer.h"
#include "StudioAudio.h"
#include "RemotePlugin.h"
#include "StudioRecording.h"
#include "StudioMidiRecording.h"
#include "StudioMidiLearn.h"
#include "StudioAutomationRecording.h"
#include "Track.h"
#include "TempoSyncKnobModel.h"
#include "TuningFile.h"

namespace lmms
{
namespace
{

[[noreturn]] void fail(const QString& message)
{
	throw std::invalid_argument(message.toStdString());
}

QString string(const QJsonObject& a, const QString& key)
{
	if (!a.value(key).isString() || a.value(key).toString().isEmpty())
	{
		fail(key + " must be a nonempty string");
	}
	return a.value(key).toString();
}

double number(const QJsonObject& a, const QString& key, double min, double max)
{
	const auto value = a.value(key);
	const double n = value.toDouble();
	if (!value.isDouble() || !std::isfinite(n) || n < min || n > max)
	{
		fail(QString("%1 must be a number between %2 and %3").arg(key).arg(min).arg(max));
	}
	return n;
}

int integer(const QJsonObject& a, const QString& key, int min, int max)
{
	const double n = number(a, key, min, max);
	if (std::floor(n) != n) { fail(key + " must be an integer"); }
	return static_cast<int>(n);
}

MidiClip::NoteEdit noteEdit(const QJsonObject& a)
{
	MidiClip::NoteEdit edit;
	if (a.contains("position")) { edit.position = integer(a, "position", 0, MaxSongLength); }
	if (a.contains("length")) { edit.length = integer(a, "length", 1, MaxSongLength); }
	if (a.contains("key")) { edit.key = integer(a, "key", 0, NumKeys - 1); }
	// Preserve native integer-note conversion after admitting the full number.
	if (a.contains("volume")) { edit.volume = static_cast<int>(number(a, "volume", MinVolume, MaxVolume)); }
	if (a.contains("panning")) { edit.panning = static_cast<int>(number(a, "panning", PanningLeft, PanningRight)); }
	return edit;
}

MidiClip::NoteSpec noteSpec(const QJsonObject& a)
{
	static const QSet<QString> allowed{"position", "length", "key", "volume", "panning"};
	for (auto item = a.begin(); item != a.end(); ++item) { if (!allowed.contains(item.key())) { fail("Unknown note input field: " + item.key()); } }
	MidiClip::NoteSpec result; result.position = integer(a, "position", 0, MaxSongLength); result.length = integer(a, "length", 1, MaxSongLength - result.position); result.key = integer(a, "key", 0, NumKeys - 1);
	if (a.contains("volume")) { result.volume = static_cast<int>(number(a, "volume", MinVolume, MaxVolume)); }
	if (a.contains("panning")) { result.panning = static_cast<int>(number(a, "panning", PanningLeft, PanningRight)); }
	return result;
}

bool boolean(const QJsonObject& a, const QString& key)
{
	if (!a.value(key).isBool()) { fail(key + " must be a boolean"); }
	return a.value(key).toBool();
}

struct InspectionPage
{
	int offset, limit;
	static InspectionPage read(const QJsonObject& arguments, const char* offsetKey = "offset",
		const char* limitKey = "limit", int defaultLimit = 128, int maximumLimit = 1024)
	{
		return {arguments.contains(offsetKey) ? integer(arguments, offsetKey, 0, std::numeric_limits<int>::max()) : 0,
			arguments.contains(limitKey) ? integer(arguments, limitKey, 1, maximumLimit) : defaultLimit};
	}
	QJsonObject metadata(int total, int returned) const
	{
		const qint64 next = static_cast<qint64>(offset) + returned;
		return {{"total", total}, {"offset", offset}, {"limit", limit}, {"nextOffset", static_cast<double>(next)}, {"hasMore", next < total}};
	}
};

void checkInspectionWork()
{
	QString error; if (!FileResourceIO::check(error)) { fail(error); }
}

bool appendInspectionItem(QJsonArray& items, const QJsonObject& item, qint64& bytes, qint64 maximum)
{
	const qint64 length = QJsonDocument(item).toJson(QJsonDocument::Compact).size() + 1;
	if (bytes + length > maximum)
	{
		if (items.isEmpty()) { fail("A native inspection item exceeds its page byte budget; request previews or omit full names, details or choices"); }
		return false;
	}
	items.append(item); bytes += length; return true;
}

void validateSchemaValue(const QJsonObject& schema, const QJsonValue& value, const QString& path, std::size_t& work)
{
	if (work++ % 256 == 0) { QString error; if (!FileResourceIO::check(error)) { fail(error); } }
	const auto type = schema.value("type").toString();
	if (type != "object" && type != "array" && type != "string" && type != "boolean" && type != "integer" && type != "number") { fail("Unsupported native capability schema type: " + path); }
	const bool numeric = type == "integer" || type == "number";
	if ((type == "object" && !value.isObject()) || (type == "array" && !value.isArray()) || (type == "string" && !value.isString())
		|| (type == "boolean" && !value.isBool()) || (numeric && (!value.isDouble() || !std::isfinite(value.toDouble()) || (type == "integer" && std::floor(value.toDouble()) != value.toDouble()))))
	{ fail("Invalid type for argument: " + path); }
	if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) { fail("Argument is outside its declared enum: " + path); }
	if (numeric)
	{
		const auto number = value.toDouble();
		if ((schema.contains("minimum") && number < schema.value("minimum").toDouble()) || (schema.contains("maximum") && number > schema.value("maximum").toDouble())) { fail("Argument is outside its declared bounds: " + path); }
	}
	if (type == "string")
	{
		const auto string = value.toString();
		if (schema.contains("minLength") || schema.contains("maxLength"))
		{
			int length = 0; for (int index = 0; index < string.size(); ++index) { if (string[index].isHighSurrogate() && index + 1 < string.size() && string[index + 1].isLowSurrogate()) { ++index; } ++length; }
			if ((schema.contains("minLength") && length < schema.value("minLength").toInt()) || (schema.contains("maxLength") && length > schema.value("maxLength").toInt())) { fail("Argument string length is outside its declared bounds: " + path); }
		}
		if (schema.contains("pattern") && !QRegularExpression(schema.value("pattern").toString()).match(string).hasMatch()) { fail("Argument does not match its declared pattern: " + path); }
	}
	if (type == "object")
	{
		const auto object = value.toObject(), properties = schema.value("properties").toObject();
		for (const auto key : schema.value("required").toArray()) { if (!object.contains(key.toString())) { fail("Missing argument: " + path + "." + key.toString()); } }
		const auto additional = schema.value("additionalProperties");
		for (auto item = object.begin(); item != object.end(); ++item)
		{
			const auto child = path + "." + item.key();
			if (properties.contains(item.key())) { validateSchemaValue(properties.value(item.key()).toObject(), item.value(), child, work); }
			else if (additional.isBool() && !additional.toBool()) { fail("Unknown argument: " + child); }
			else if (additional.isObject()) { validateSchemaValue(additional.toObject(), item.value(), child, work); }
		}
	}
	if (type == "array")
	{
		const auto array = value.toArray();
		if ((schema.contains("minItems") && array.size() < schema.value("minItems").toInt()) || (schema.contains("maxItems") && array.size() > schema.value("maxItems").toInt())) { fail("Argument array size is outside its declared bounds: " + path); }
		const auto items = schema.value("items").toObject(); const bool unique = schema.value("uniqueItems").toBool(), integers = items.value("type") == QJsonValue("integer"); QSet<double> seen;
		for (int index = 0; index < array.size(); ++index)
		{
			const auto child = path + "[" + QString::number(index) + "]"; validateSchemaValue(items, array[index], child, work);
			if (unique && integers) { const auto number = array[index].toDouble(); if (seen.contains(number)) { fail("Duplicate argument array item: " + child); } seen.insert(number); }
			else if (unique) { for (int prior = 0; prior < index; ++prior) { if (work++ % 256 == 0) { QString error; if (!FileResourceIO::check(error)) { fail(error); } } if (array[prior] == array[index]) { fail("Duplicate argument array item: " + child); } } }
		}
	}
}

QJsonObject specification(const char* name, const char* description, bool mutates,
	std::initializer_list<const char*> fields, std::initializer_list<const char*> required)
{
	QJsonObject properties;
	for (const auto field : fields)
	{
		const QString key = field;
		QString type = "integer";
		if (key == "path" || key == "name" || key == "type" || key == "container"
			|| key == "instrument" || key == "plugin" || key == "direction" || key == "resource"
			|| key == "intervals" || key == "mapping" || key == "query" || key == "targetContainer"
			|| key == "playMode" || key == "port" || key == "format" || key == "binary"
			|| key == "label" || key == "uniqueId" || key == "discoveryTool" || key == "scan" || key == "outputSource"
			|| key == "graph" || key == "action" || key == "expression" || key == "source" || key == "section" || key == "sourceSha256" || key == "stereoMode"
			|| key == "diagnosticGeneration" || key == "resourceGeneration" || key == "root" || key == "kind" || key == "searchGeneration") { type = "string"; }
		if (key == "muted" || key == "solo" || key == "enabled" || key == "empty" || key == "subplugins"
			|| key == "withResources" || key == "autoResize" || key == "reversed" || key == "lockedTangents" || key == "preserveSettings" || key == "recursive" || key == "followSymlinks" || key == "hidden"
			|| key == "choices" || key == "select" || key == "details" || key == "linearFromMiddle"
			|| key == "estimateTempo" || key == "replace" || key == "paused" || key == "loop" || key == "playback"
			|| key == "readable" || key == "writable" || key == "active" || key == "recordPitch" || key == "preview"
			|| key == "summary" || key == "names" || key == "exportLoop" || key == "betweenMarkers" || key == "abandonFailedOutput") { type = "boolean"; }
		if (key == "value" || key == "amount" || key == "volume" || key == "panning" || key == "gain"
			|| key == "outValue" || key == "inTangent" || key == "outTangent" || key == "tension" || key == "baseFrequency" || key == "compressionLevel") { type = "number"; }
		if (key == "attributes") { type = "object"; }
		if (key == "values")
		{
			properties.insert(key, QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "number"}}}, {"minItems", 1}});
		}
		else if (key == "points")
		{
			properties.insert(key, QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
				{"minItems", 2}, {"maxItems", 129}});
		}
		else { properties.insert(key, QJsonObject{{"type", type}}); }
	}
	const QString operation = name;
	for (const auto key : {"container", "targetContainer"}) { if (!properties.contains(key)) { continue; } auto field = properties.value(key).toObject(); field.insert("enum", QJsonArray{"song", "pattern"}); properties.insert(key, field); }
	if (properties.contains("attributes")) { properties.insert("attributes", QJsonObject{{"type", "object"}, {"additionalProperties", QJsonObject{{"type", "string"}}}}); }
	if (operation == "song.set")
	{
		properties.insert("tempo", QJsonObject{{"type", "integer"}, {"minimum", MinTempo}, {"maximum", MaxTempo}});
		properties.insert("masterVolume", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 200}});
		properties.insert("masterPitch", QJsonObject{{"type", "integer"}, {"minimum", -12}, {"maximum", 12}});
	}
	if (operation == "track.add") { properties.insert("type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"instrument", "sample", "automation", "pattern"}}}); }
	if (operation == "clip.edit")
	{
		properties.insert("position", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaxSongLength}});
		properties.insert("length", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxSongLength}});
		properties.insert("offset", QJsonObject{{"type", "integer"}, {"minimum", -MaxSongLength}, {"maximum", MaxSongLength}});
	}
	if (operation == "track.add" || operation == "track.edit" || operation == "clip.add" || operation == "clip.edit" || operation == "controller.add" || operation == "controller.edit" || operation == "mixer.add" || operation == "mixer.edit" || operation == "host.plugin.rename")
	{ if (properties.contains("name")) { auto field = properties.value("name").toObject(); field.insert("minLength", 1); properties.insert("name", field); } }
	if (operation == "note.selection.copy" || operation == "note.selection.shift" || operation == "note.selection.quantize" || operation == "note.selection.reverse" || operation == "note.selection.length" || operation == "note.selection.glue" || operation == "note.selection.split" || operation == "note.selection.knife" || operation == "note.selection.remove" || operation == "note.selection.edit")
	{
		properties.insert("notes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 4096}, {"uniqueItems", true},
			{"items", QJsonObject{{"type", "integer"}, {"minimum", 0}}}});
		for (const auto key : {"container", "targetContainer"}) { if (!properties.contains(key)) { continue; } auto field = properties.value(key).toObject(); field.insert("enum", QJsonArray{"song", "pattern"}); properties.insert(key, field); }
		for (const auto key : {"track", "clip", "targetTrack", "targetClip", "position"})
		{ if (!properties.contains(key)) { continue; } auto field = properties.value(key).toObject(); field.insert("minimum", 0); if (QString(key) == "position") { field.insert("maximum", MaxSongLength); } properties.insert(key, field); }
		if (operation == "note.selection.shift")
		{
			properties.insert("semitones", QJsonObject{{"type", "integer"}, {"minimum", -NumKeys}, {"maximum", NumKeys}});
			properties.insert("ticks", QJsonObject{{"type", "integer"}, {"minimum", -MaxSongLength}, {"maximum", MaxSongLength}});
		}
		if (operation == "note.selection.quantize")
		{
			properties.insert("grid", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxSongLength}});
			properties.insert("mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"both", "positions", "lengths"}}, {"default", "both"}});
		}
		if (operation == "note.selection.length")
		{
			properties.insert("mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"maximum", "minimum", "trim", "fill"}}});
			properties.insert("length", QJsonObject{{"type", "integer"}, {"minimum", -MaxSongLength}, {"maximum", MaxSongLength}});
		}
		if (operation == "note.selection.knife")
		{
			for (const auto key : {"fromPosition", "fromKey", "toPosition", "toKey"}) { properties.insert(key, QJsonObject{{"type", "integer"}, {"minimum", std::numeric_limits<int>::min()}, {"maximum", std::numeric_limits<int>::max()}}); }
			properties.insert("keepLongest", QJsonObject{{"type", "boolean"}, {"default", false}});
		}
		if (operation == "note.selection.edit")
		{
			properties.insert("length", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxSongLength}});
			properties.insert("key", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", NumKeys - 1}});
			properties.insert("volume", QJsonObject{{"type", "number"}, {"minimum", MinVolume}, {"maximum", MaxVolume}});
			properties.insert("panning", QJsonObject{{"type", "number"}, {"minimum", PanningLeft}, {"maximum", PanningRight}});
		}
	}
	if (operation == "note.batch.add")
	{
		const QJsonObject fields{{"position", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaxSongLength}}},
			{"length", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxSongLength}}},
			{"key", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", NumKeys - 1}}},
			{"volume", QJsonObject{{"type", "number"}, {"minimum", MinVolume}, {"maximum", MaxVolume}, {"default", DefaultVolume}}},
			{"panning", QJsonObject{{"type", "number"}, {"minimum", PanningLeft}, {"maximum", PanningRight}, {"default", DefaultPanning}}}};
		properties.insert("items", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 4096},
			{"items", QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"position", "length", "key"}}, {"properties", fields}}}});
		properties.insert("container", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"song", "pattern"}}});
	}
	if (operation == "clip.selection.copy")
	{
		const QJsonObject index{{"type", "integer"}, {"minimum", 0}};
		const QJsonObject owner{{"type", "string"}, {"enum", QJsonArray{"song", "pattern"}}};
		properties.insert("items", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 4096},
			{"items", QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"track", "clip"}},
				{"properties", QJsonObject{{"container", owner}, {"track", index}, {"clip", index}, {"targetContainer", owner},
					{"targetTrack", index}, {"position", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaxSongLength}}},
					{"targetClip", index}, {"replace", QJsonObject{{"type", "boolean"}}}}}}}});
	}
	if (operation == "audio.render")
	{
		properties.insert("track", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"description", "Optional instrument/sample track for one native stem; must be unmuted. Omit for the full mix"}});
		properties.insert("container", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"song", "pattern"}}, {"default", "song"},
			{"description", "Requires track; pattern selects a PatternStore instrument/sample row across the song arrangement"}});
		QJsonArray sampleRates;
		for (const auto rate : SUPPORTED_SAMPLERATES) { sampleRates.append(rate); }
		properties.insert("sampleRate", QJsonObject{{"type", "integer"}, {"enum", sampleRates}, {"default", 44100}});
		properties.insert("bitDepth", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{16, 24, 32}}, {"default", 16}, {"description", "WAV: 16/24/32; FLAC: 16/24; omit for lossy formats"}});
		properties.insert("bitRate", QJsonObject{{"type", "integer"}, {"minimum", 64}, {"maximum", 384}, {"default", 160},
			{"description", "OGG/MP3 only, native CLI range; codec may reject or adjust it. MP3 uses CBR kbit/s; OGG uses VBR quality bitRate/320, not target kbit/s"}});
		properties.insert("stereoMode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"mono", "stereo", "jointStereo"}}, {"default", "stereo"}, {"description", "MP3 only"}});
		properties.insert("compressionLevel", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.625},
			{"description", "FLAC only; requires native libsndfile compression control; unsupported builds explicitly reject this option"}});
		properties.insert("exportLoop", QJsonObject{{"type", "boolean"}, {"default", false}, {"description", "Omit native one-bar tail padding on whole-song exports; marker exports already omit it"}});
		properties.insert("betweenMarkers", QJsonObject{{"type", "boolean"}, {"default", false}, {"description", "Render only the current song timeline loop markers, independently of playback loop enablement; configure markers with timeline.set"}});
		properties.insert("loopCount", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 2147483647}, {"default", 1},
			{"description", "Number of passes through song loop markers. Whole-song exports include the surrounding arrangement and may extend to the loop end"}});
	}
	if (operation == "audio.analyze")
	{
		properties.insert("sourceSha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"minLength", 64}, {"maxLength", 64},
			{"description", "Expected source digest from audio.render sha256 or a previous evaluation; mismatch fails before measuring samples"}});
	}
	if (operation == "parameter.list" || operation == "instrument.inspect" || operation == "effect.inspect" || operation == "parameter.inspect" || operation == "project.inspect"
		|| operation == "automation.inspect" || operation == "automation.global.inspect" || operation == "note.detuning.inspect")
	{
		for (const auto key : {"offset", "parameterOffset", "choiceOffset", "linkedOffset"})
		{
			if (properties.contains(key)) { properties.insert(key, QJsonObject{{"type", "integer"}, {"minimum", 0}, {"default", 0}}); }
		}
		for (const auto key : {"limit", "parameterLimit", "choiceLimit", "linkedLimit"})
		{
			const bool effects = operation == "effect.inspect" && QString(key) == "limit";
			if (properties.contains(key))
			{ properties.insert(key, QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", effects ? 128 : 1024}, {"default", effects ? 16 : 128}}); }
		}
	}
	if (operation == "project.inspect")
	{
		properties.insert("section", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"full", "summary", "tracks", "clips", "notes", "channels", "routes", "controllers", "globalAutomations", "warnings", "resources"}},
			{"default", "full"}, {"description", "Use summary for a bounded overview, then a named inventory section to page native contents"}});
		properties.insert("names", QJsonObject{{"type", "boolean"}, {"default", true}, {"description", "Full names and sample paths; false returns bounded previews in paged inventories"}});
	}
	if (operation == "instrument.shaping.waveform.set")
	{ properties.insert("target", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"volume", "cutoff", "resonance"}}, {"description", "Native instrument sound-shaping envelope/LFO group"}}); }
	if (operation == "asset.search")
	{
		properties.insert("root", QJsonObject{{"type", "string"}, {"minLength", 1}, {"description", "Native alias from asset.roots or an explicit absolute/prefixed directory; no implicit whole-library scan"}});
		properties.insert("query", QJsonObject{{"type", "string"}, {"maxLength", 4096}, {"default", ""}, {"description", "Case-insensitive filename tokens; double quotes keep phrases together. Empty query lists matches"}});
		properties.insert("extensions", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}, {"default", QJsonArray{"*"}}});
		properties.insert("kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"files", "directories", "all"}}, {"default", "files"}});
		properties.insert("maximumEntries", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}, {"default", 100000}});
		properties.insert("offset", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<int>::max()}, {"default", 0}});
		properties.insert("limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}, {"default", 128}});
		properties.insert("recursive", QJsonObject{{"type", "boolean"}, {"default", true}});
		for (const auto key : {"hidden", "followSymlinks"}) { properties.insert(key, QJsonObject{{"type", "boolean"}, {"default", false}}); }
		properties.insert("searchGeneration", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"description", "Observed metadata/order token; keep search scope and filters fixed when paging, omit after changes"}});
	}
	if (operation == "project.import")
	{
		properties.insert("container", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"song", "pattern"}}, {"default", "song"},
			{"description", "Native import destination. Pattern imports require a compatible filter (MIDI); append rows within one shared pattern column"}});
		properties.insert("pattern", QJsonObject{{"type", "integer"}, {"minimum", 0},
			{"description", "Pattern destination only: existing column index, or 0 for an empty PatternStore; omit to use its current column. Does not change the current selection"}});
	}
	if (operation == "automation.inspect" || operation == "automation.global.inspect" || operation == "note.detuning.inspect")
	{
		QJsonArray sections{"full", "summary", "points"}; if (operation != "note.detuning.inspect") { sections.append("targets"); }
		properties.insert("section", QJsonObject{{"type", "string"}, {"enum", sections}, {"default", "full"},
			{"description", "summary omits arrays; points pages by native tick using fromPosition/nextPosition; targets pages native target slots with offset/nextOffset"}});
		properties.insert("fromPosition", QJsonObject{{"type", "integer"}, {"minimum", std::numeric_limits<int>::min()},
			{"maximum", std::numeric_limits<int>::max()}, {"description", "Inclusive native point tick; omit for the first point, then use nextPosition"}});
		if (properties.contains("names")) { properties.insert("names", QJsonObject{{"type", "boolean"}, {"default", true}, {"description", "Full target names; false returns bounded previews"}}); }
	}
	if (properties.contains("summary"))
	{ properties.insert("summary", QJsonObject{{"type", "boolean"}, {"default", false}, {"description", "Return bounded native metadata after the operation instead of expanding project, curve or rack contents"}}); }
	if (operation == "midi.backend.select")
	{ properties.insert("abandonFailedOutput", QJsonObject{{"type", "boolean"}, {"default", false}, {"description", "Explicitly discard unconfirmed output only when the current backend reports abandonmentEligible; preserves old ledgers on failed preparation; successful replacement does not verify physical note cleanup"}}); }
	QJsonArray requiredKeys;
	for (const auto key : required) { requiredKeys.append(key); }
	return {{"name", name}, {"description", description}, {"mutatesProject", mutates},
		{"inputSchema", QJsonObject{{"type", "object"}, {"properties", properties},
			{"required", requiredKeys}, {"additionalProperties", false}}}};
}

TrackContainer* container(const QJsonObject& a)
{
	const auto name = a.contains("container") ? string(a, "container") : QString("song");
	if (name == "song") { return Engine::getSong(); }
	if (name == "pattern") { return Engine::patternStore(); }
	fail("container must be song or pattern");
}

Track* track(const QJsonObject& a)
{
	const auto& tracks = container(a)->tracks();
	if (tracks.empty()) { fail("Container has no tracks"); }
	return tracks.at(integer(a, "track", 0, static_cast<int>(tracks.size()) - 1));
}

AudioBusHandle* trackAudioBus(const QJsonObject& arguments)
{
	const auto selected = track(arguments);
	if (const auto instrument = dynamic_cast<InstrumentTrack*>(selected)) { return instrument->audioBusHandle(); }
	if (const auto sample = dynamic_cast<SampleTrack*>(selected)) { return sample->audioBusHandle(); }
	fail("Native track audio ports require an instrument or sample track");
}

Clip* clip(const QJsonObject& a)
{
	const auto& clips = track(a)->getClips();
	if (clips.empty()) { fail("Track has no clips"); }
	return clips.at(integer(a, "clip", 0, static_cast<int>(clips.size()) - 1));
}

InstrumentTrack* instrumentTrack(const QJsonObject& a)
{
	auto t = dynamic_cast<InstrumentTrack*>(track(a));
	if (!t) { fail("Operation requires an instrument track"); }
	return t;
}

int currentInstrumentTrackIndex(const QPointer<Song>& song, const QPointer<TrackContainer>& owner,
	const QPointer<InstrumentTrack>& target, const QJsonObject& arguments, bool allowRecording = false)
{
	if (!song || Engine::getSong() != song.data() || song->isLoadingProject() || !owner || container(arguments) != owner.data()
		|| !target || target->isRetiring() || target->isReplacingInstrument() || target->thread() != QThread::currentThread()
		|| target->trackContainer() != owner.data() || (!allowRecording && target->isNoteRecording()))
	{ fail("Native instrument track owner changed, is recording or is replacing its instrument"); }
	const auto& tracks = owner->tracks(); const auto found = std::find(tracks.begin(), tracks.end(), target.data());
	if (found == tracks.end()) { fail("Native instrument track is no longer in its container"); }
	return static_cast<int>(std::distance(tracks.begin(), found));
}

MidiClip* midiClip(const QJsonObject& a)
{
	auto c = dynamic_cast<MidiClip*>(clip(a));
	if (!c) { fail("Operation requires a MIDI clip"); }
	return c;
}

Note* note(const QJsonObject& a)
{
	const auto c = midiClip(a);
	if (c->notes().empty()) { fail("Clip has no notes"); }
	return c->notes().at(integer(a, "note", 0, static_cast<int>(c->notes().size()) - 1));
}

AutomationClip* automationClip(const QJsonObject& a)
{
	auto c = dynamic_cast<AutomationClip*>(clip(a));
	if (!c) { fail("Operation requires an automation clip"); }
	return c;
}

QJsonObject inspectNode(int position, const AutomationNode& node)
{
	return {{"position", position}, {"value", node.getInValue()}, {"outValue", node.getOutValue()},
		{"inTangent", node.getInTangent()}, {"outTangent", node.getOutTangent()}, {"lockedTangents", node.lockedTangents()}};
}

QJsonObject inspectCurveMetadata(const AutomationClip* curve, bool includeTargets = true)
{
	QJsonObject result{{"exists", curve != nullptr}, {"pointCount", curve ? static_cast<int>(curve->getTimeMap().size()) : 0},
		{"progression", curve ? static_cast<int>(curve->progressionType()) : 0},
		{"tension", curve ? curve->getTension() : 1.f}, {"hasAutomation", curve && curve->hasAutomation()}};
	if (includeTargets) { result.insert("targetCount", curve ? static_cast<int>(curve->objects().size()) : 0); }
	return result;
}

QJsonObject inspectCurve(const AutomationClip* curve, bool includeTargets = true)
{
	QJsonArray points;
	QJsonArray targets;
	if (curve)
	{
		for (auto it = curve->getTimeMap().cbegin(); it != curve->getTimeMap().cend(); ++it) { points.append(inspectNode(it.key(), it.value())); }
		if (includeTargets)
		{
			for (const auto& model : curve->objects())
			{
				if (model) { targets.append(QJsonObject{{"parameter", static_cast<int>(model->id())},
					{"name", model->fullDisplayName()}, {"minimum", model->minValue<float>()}, {"maximum", model->maxValue<float>()}}); }
			}
		}
	}
	QJsonObject result{{"points", points}, {"progression", curve ? static_cast<int>(curve->progressionType()) : 0},
		{"tension", curve ? curve->getTension() : 1.0f}, {"hasAutomation", curve && curve->hasAutomation()}};
	if (includeTargets) { result.insert("targets", targets); }
	return result;
}

QJsonObject inspectDetuning(const Note* note)
{
	const auto curve = note->detuning() ? note->detuning()->existingAutomationClip() : nullptr;
	auto result = inspectCurve(curve, false);
	if (!curve) { result.insert("progression", 1); }
	result.insert("hasAutomation", note->hasDetuningInfo());
	result.insert("parameter", note->detuning() ? QJsonValue(static_cast<int>(note->detuning()->id())) : QJsonValue(QJsonValue::Null));
	if (curve) { result.insert("curveControls", QJsonObject{{"mute", static_cast<int>(curve->mutedModel()->id())}, {"solo", static_cast<int>(curve->soloModel()->id())}}); }
	result.insert("units", "semitones; positions are relative to note start");
	return result;
}

using CurveSettings = AutomationClip::CurveEdit;

CurveSettings curveSettings(const QJsonObject& a)
{
	CurveSettings result;
	if (a.contains("progression")) { result.progression = static_cast<AutomationClip::ProgressionType>(integer(a, "progression", 0, 2)); }
	if (a.contains("tension")) { result.tension = static_cast<float>(number(a, "tension", 0, 1)); }
	return result;
}

void applyCurveSettings(AutomationClip* curve, const CurveSettings& settings)
{
	bool changed = false; QString error;
	if (!curve->editCurve(settings, changed, error)) { fail(error); }
}

using CurvePoint = AutomationClip::PointEdit;

CurvePoint curvePoint(const QJsonObject& a, float minimum, float maximum)
{
	CurvePoint point{integer(a, "position", 0, MaxSongLength), static_cast<float>(number(a, "value", minimum, maximum)), 0, {}, {}, {}};
	point.outValue = a.contains("outValue") ? static_cast<float>(number(a, "outValue", minimum, maximum)) : point.value;
	if (a.contains("inTangent")) { point.inTangent = static_cast<float>(number(a, "inTangent", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max())); }
	if (a.contains("outTangent")) { point.outTangent = static_cast<float>(number(a, "outTangent", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max())); }
	if (a.contains("lockedTangents")) { point.locked = boolean(a, "lockedTangents"); }
	if (point.locked && !*point.locked && (point.inTangent || point.outTangent)) { fail("Manual tangents require lockedTangents true; false regenerates native tangents"); }
	return point;
}

QJsonObject writeCurvePoint(AutomationClip* curve, const CurvePoint& point, const CurveSettings& settings)
{
	auto options = settings; options.point = point; bool changed = false; QString error;
	if (!curve->editCurve(options, changed, error)) { fail(error); }
	return {{"point", inspectNode(point.position, curve->getTimeMap().value(point.position))},
		{"progression", static_cast<int>(curve->progressionType())}, {"tension", curve->getTension()}, {"changed", changed}};
}

bool removeCurvePoint(AutomationClip* curve, int position)
{
	const bool removed = curve->getTimeMap().contains(position);
	AutomationClip::CurveEdit options; options.removePoint = position; bool changed = false; QString error;
	if (!curve->editCurve(options, changed, error)) { fail(error); }
	return removed;
}

void clearCurve(AutomationClip* curve)
{
	AutomationClip::CurveEdit options; options.clear = true; bool changed = false; QString error;
	if (!curve->editCurve(options, changed, error)) { fail(error); }
}

SampleClip* sampleClip(const QJsonObject& a)
{
	auto c = dynamic_cast<SampleClip*>(clip(a));
	if (!c) { fail("Operation requires a sample clip"); }
	return c;
}

QJsonObject inspectSample(const SampleClip* clip)
{
	const auto& sample = clip->sample();
	const auto window = clip->sourceSampleWindow();
	return {{"path", clip->sampleFile()}, {"sampleRate", clip->sourceSampleRate()}, {"unresolved", clip->sampleUnresolved()},
		{"sourceFrames", static_cast<double>(sample.sampleSize())}, {"startFrame", window ? QJsonValue(window->first) : QJsonValue(QJsonValue::Null)},
		{"endFrame", window ? QJsonValue(window->second) : QJsonValue(QJsonValue::Null)}, {"gain", sample.amplification()}, {"reversed", sample.reversed()},
		{"frameCoordinates", "playback order, half-open [startFrame,endFrame)"},
		{"sampleLengthTicks", static_cast<int>(clip->sampleLength())}};
}

MixerChannel* channel(const QJsonObject& a)
{
	return Engine::mixer()->mixerChannel(integer(a, "channel", 0, Engine::mixer()->numChannels() - 1));
}

EffectChain* chain(const QJsonObject& a)
{
	if (a.contains("channel"))
	{
		if (a.contains("track") || a.contains("container")) { fail("Select an effect chain by channel or track/container, not both"); }
		return &channel(a)->m_fxChain;
	}
	auto t = track(a);
	if (auto instrument = dynamic_cast<InstrumentTrack*>(t)) { return instrument->audioBusHandle()->effects(); }
	if (auto sample = dynamic_cast<SampleTrack*>(t)) { return sample->audioBusHandle()->effects(); }
	fail("Effects require an instrument, sample track or mixer channel");
}

EffectChain* trackEffects(Track* track)
{
	if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { return instrument->audioBusHandle()->effects(); }
	if (auto sample = dynamic_cast<SampleTrack*>(track)) { return sample->audioBusHandle()->effects(); }
	return nullptr;
}

void currentEffectChain(const QPointer<Song>& song, const QPointer<EffectChain>& effects, const QJsonObject& arguments)
{
	if (!song || Engine::getSong() != song.data() || song->isLoadingProject() || !effects || effects->isRetiring()
		|| effects->thread() != QThread::currentThread() || chain(arguments) != effects.data())
	{ fail("Native effect chain owner or selector changed"); }
}

int currentEffectIndex(const QPointer<Song>& song, const QPointer<EffectChain>& effects,
	const QPointer<Effect>& effect, const QJsonObject& arguments)
{
	currentEffectChain(song, effects, arguments);
	if (!effect || effect->effectChain() != effects.data()) { fail("Native effect retired or changed owners"); }
	const auto& roster = effects->effects(); const auto found = std::find(roster.begin(), roster.end(), effect.data());
	if (found == roster.end()) { fail("Native effect is no longer in the selected chain"); }
	return static_cast<int>(std::distance(roster.begin(), found));
}

bool isActiveEffectParameter(const AutomatableModel* model)
{
	for (const auto* parent = model->parent(); parent; parent = parent->parent())
	{
		if (const auto controller = dynamic_cast<const Controller*>(parent); controller && controller->isRetiring()) { return false; }
		if (const auto effect = dynamic_cast<const Effect*>(parent))
		{
			const auto owner = effect->effectChain();
			// Missing-plugin controls are dummy UI models, not the preserved
			// plugin's serialized settings. Detached legacy QObject children also
			// no longer belong to the native processing chain.
			return !dynamic_cast<const DummyEffect*>(effect) && owner && !owner->isRetiring()
				&& std::find(owner->effects().begin(), owner->effects().end(), effect) != owner->effects().end();
		}
	}
	return true;
}

QList<AutomatableModel*> modelsUnder(const QList<QObject*>& roots)
{
	FileResourceIO::EventSuspension events;
	QList<AutomatableModel*> result;
	std::vector<QPointer<QObject>> owners; for (auto root : roots) { if (root) { owners.emplace_back(root); } }
	std::vector<QPointer<AutomatableModel>> retained;
	QSet<AutomatableModel*> seen;
	// Refresh before collecting any pointers: a plugin can replace controls.
	for (const auto& root : owners)
	{
		checkInspectionWork();
		if (!root) { fail("Native parameter root retired during refresh"); }
		if (auto native = dynamic_cast<Plugin*>(root.data())) { native->refreshParameterModels(); }
		if (!root) { fail("Native parameter root retired during refresh"); }
	}
	const auto append = [&](const QPointer<AutomatableModel>& weak) {
		if (result.size() % 64 == 0) { checkInspectionWork(); }
		if (!weak) { fail("Native child parameter retired during discovery"); } const auto model = weak.data();
		if (!seen.contains(model) && isActiveEffectParameter(model)) { seen.insert(model); result.append(model); retained.push_back(weak); }
	};
	for (const auto& root : owners)
	{
		if (!root) { fail("Native parameter root retired during discovery"); }
		std::vector<QPointer<AutomatableModel>> children;
		if (auto model = dynamic_cast<AutomatableModel*>(root.data())) { children.emplace_back(model); }
		for (auto model : root->findChildren<AutomatableModel*>()) { children.emplace_back(model); }
		for (const auto& model : children) { append(model); }
	}
	for (const auto& root : owners) { if (!root) { fail("Native parameter root retired during discovery"); } }
	for (const auto& model : retained) { if (!model) { fail("Native child parameter retired during discovery"); } }
	return result;
}

QList<AutomatableModel*> models()
{
	QList<QObject*> roots{Engine::getSong(), Engine::patternStore(), Engine::mixer()};
	const auto addChain = [&](EffectChain* effects) {
		roots.append(effects); roots.append(effects->enabledModel());
		for (const auto effect : effects->effects()) { roots.append(effect); }
	};
	for (const auto tc : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (auto t : tc->tracks())
		{
			for (auto clip : t->getClips()) { checkInspectionWork(); for (auto model : NativeLinkedControls::clipModels(clip)) { roots.append(model); } }
			if (const auto effects = trackEffects(t)) { addChain(effects); }
			if (auto instrument = dynamic_cast<InstrumentTrack*>(t))
			{
				if (instrument->instrument()) { roots.append(instrument->instrument()); }
			}
		}
	}
	for (int index = 0; index < Engine::mixer()->numChannels(); ++index) { addChain(&Engine::mixer()->mixerChannel(index)->m_fxChain); }
	for (const auto route : Engine::mixer()->m_mixerRoutes) { roots.append(route->amount()); }
	return modelsUnder(roots);
}

AutomatableModel* parameter(const QJsonObject& a)
{
	const auto id = integer(a, "parameter", 1, 2147483647);
	for (auto model : models())
	{
		if (model->id() == static_cast<jo_id_t>(id)) { return model; }
	}
	fail("Parameter does not exist in the current project; rediscover after restore");
}

void requireWritableGroup(AutomatableModel* selected)
{
	for (auto model = selected; ; model = const_cast<AutomatableModel*>(model->nextLinkedModel()))
	{
		checkInspectionWork(); if (!model->isWritable()) { fail("Native linked group contains an output or disabled parameter"); }
		if (model->nextLinkedModel() == selected) { break; }
	}
}

void requireLinkableGroup(AutomatableModel* selected, const QHash<AutomatableModel*, QPointer<AutomatableModel>>& current)
{
	for (auto model = selected; ; model = const_cast<AutomatableModel*>(model->nextLinkedModel()))
	{
		checkInspectionWork();
		if (current.value(model).data() != model) { fail("Native linked group contains a retired control or a control outside current project parameter discovery"); }
		if (!model->isWritable() || !model->supportsAutomation()) { fail("Native linked group contains a disabled, output or unsupported input control"); }
		if (model->scaleType() != AutomatableModel::ScaleType::Linear && model->scaleType() != AutomatableModel::ScaleType::Logarithmic)
		{ fail("Native linked controller DSP does not support this parameter scale"); }
		if (!std::isfinite(model->storedValue())) { fail("Native linked group contains a nonfinite stored value"); }
		if (model->nextLinkedModel() == selected) { break; }
	}
}

MidiPort* midiPort(const QJsonObject& a)
{
	if (a.contains("parameter"))
	{
		if (a.contains("container") || a.contains("track")) { fail("Select MIDI by parameter or track, not both"); }
		const auto connection = parameter(a)->controllerConnection();
		const auto midi = connection ? dynamic_cast<MidiController*>(connection->getController()) : nullptr;
		if (!midi) { fail("Parameter has no direct native MIDI controller connection"); }
		return midi->midiPort();
	}
	if (!a.contains("track")) { fail("Select an instrument track or a parameter with a direct MIDI controller connection"); }
	return instrumentTrack(a)->midiPort();
}

AutomationClip* existingGlobalAutomation(const AutomatableModel* model)
{
	for (const auto clip : Engine::getSong()->globalAutomationTrack()->getClips())
	{
		auto automation = dynamic_cast<AutomationClip*>(clip);
		if (automation && std::find(automation->objects().begin(), automation->objects().end(), model) != automation->objects().end()) { return automation; }
	}
	return nullptr;
}

QJsonObject parameterLocation(const AutomatableModel* model)
{
	QSet<const QObject*> ancestors;
	for (const QObject* parent = model; parent; parent = parent->parent()) { ancestors.insert(parent); }
	const auto inChain = [&](const EffectChain* effects, QJsonObject location) -> QJsonObject {
		if (model == effects->enabledModel()) { location.insert("scope", "effectChain"); return location; }
		if (!ancestors.contains(effects)) { return {}; }
		int index = 0;
		for (const auto effect : effects->effects())
		{
			if (ancestors.contains(effect)) { location.insert("scope", "effect"); location.insert("effect", index); return location; }
			++index;
		}
		return {};
	};
	for (int index = 0; index < Engine::mixer()->numChannels(); ++index)
	{
		const auto channel = Engine::mixer()->mixerChannel(index); const QJsonObject location{{"channel", index}};
		const auto effectLocation = inChain(&channel->m_fxChain, location); if (!effectLocation.isEmpty()) { return effectLocation; }
		if (model == &channel->m_volumeModel || model == &channel->m_muteModel || model == &channel->m_soloModel) { return location; }
	}
	for (const auto route : Engine::mixer()->m_mixerRoutes)
	{
		if (model == route->amount()) { return {{"scope", "mixerRoute"}, {"from", static_cast<int>(route->senderIndex())}, {"to", static_cast<int>(route->receiverIndex())}}; }
	}
	for (const auto tc : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		int index = 0;
		for (const auto track : tc->tracks())
		{
			if (const auto effects = trackEffects(track))
			{
				const auto location = inChain(effects, {{"container", tc == Engine::patternStore() ? "pattern" : "song"}, {"track", index}});
				if (!location.isEmpty()) { return location; }
			}
			++index;
		}
	}
	for (const auto* parent = model->parent(); parent; parent = parent->parent())
	{
		if (const auto owner = dynamic_cast<const Controller*>(parent))
		{
			const auto& controllers = Engine::getSong()->controllers();
			const auto it = std::find(controllers.begin(), controllers.end(), owner);
			if (it != controllers.end()) { return {{"controller", static_cast<int>(it - controllers.begin())}}; }
		}
		if (const auto owner = dynamic_cast<const Track*>(parent))
		{
			const auto tc = owner->trackContainer();
			const auto& tracks = tc->tracks();
			const auto it = std::find(tracks.begin(), tracks.end(), owner);
			if (it == tracks.end()) { return {{"scope", "globalAutomation"}}; }
			return {{"container", tc == Engine::patternStore() ? "pattern" : "song"},
				{"track", static_cast<int>(it - tracks.begin())}};
		}
	}
	// Instruments also need not be QObject children of their native tracks.
	for (const auto tc : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		int index = 0;
		for (const auto track : tc->tracks())
		{
			if (const auto instrument = dynamic_cast<InstrumentTrack*>(track))
			{
				if (instrument->instrument() && ancestors.contains(instrument->instrument()))
				{
					return {{"container", tc == Engine::patternStore() ? "pattern" : "song"}, {"track", index}};
				}
			}
			int clipIndex = 0;
			for (auto clip : track->getClips())
			{
				if (const auto midi = dynamic_cast<MidiClip*>(clip))
				{
					for (std::size_t noteIndex = 0; noteIndex < midi->notes().size(); ++noteIndex)
					{
						checkInspectionWork(); const auto note = midi->notes().at(noteIndex); if (!note || !note->detuning()) { continue; }
						const auto curve = note->detuning()->existingAutomationClip();
						if (ancestors.contains(note->detuning().get()) || (curve && ancestors.contains(curve)))
						{ return {{"container", tc == Engine::patternStore() ? "pattern" : "song"}, {"track", index}, {"clip", clipIndex}, {"note", static_cast<int>(noteIndex)}, {"scope", "noteDetuning"}}; }
					}
				}
				++clipIndex;
			}
			++index;
		}
	}
	return {{"scope", "project"}};
}

// Inspection must retain a repair handle when a live controller cannot supply a
// usable value. This is observational only: native typed getters still refuse
// invalid conversions, and inspection never changes a binding or stored value.
QJsonObject parameterReadback(AutomatableModel* model)
{
	FileResourceIO::EventSuspension events;
	const QPointer<AutomatableModel> selected(model);
	const QPointer<Song> owner(Engine::getSong());
	if (!selected || !owner) { fail("Native parameter inspection has no live project or target"); }
	const auto generation = owner->replacementGeneration();
	const auto alive = [&] {
		if (!selected || !owner || Engine::getSong() != owner.data() || owner->replacementGeneration() != generation)
		{ fail("Native parameter inspection target or project retired during readback"); }
	};
	const auto current = [&] { alive(); checkInspectionWork(); alive(); };
	QJsonObject result{{"value", QJsonValue::Null}, {"effectiveValue", QJsonValue::Null}, {"state", "unavailable"}};
	float observed = 0;
	bool available = false;
	current();
	try { observed = model->value<float>(); available = true; }
	catch (const std::exception& exception)
	{
		const auto message = exception.what(); int length = 0;
		while (length < 1024 && message[length]) { ++length; }
		result.insert("error", QString::fromUtf8(message, length));
	}
	catch (...) { result.insert("error", "Native parameter getter threw a nonstandard exception"); }
	// Retirement/replacement and cancellation remain failures of the entire
	// inspection, including when the getter also threw an ordinary exception.
	current();
	if (!available) { return result; }
	if (!std::isfinite(observed)) { result.insert("state", "nonfinite"); return result; }
	result.insert("value", observed);
	double effective = observed;
	try
	{
		// A hosted FloatModel may advertise integer choices without using the
		// native IntModel conversion. Do not truncate those models by metadata.
		if (dynamic_cast<IntModel*>(model)) { effective = AutomatableModel::castValue<int>(observed); result.insert("effectiveValue", effective); }
		else if (dynamic_cast<BoolModel*>(model))
		{ const auto value = AutomatableModel::castValue<bool>(observed); effective = value; result.insert("effectiveValue", value); }
		else { result.insert("effectiveValue", observed); }
	}
	catch (const std::out_of_range&) { result.insert("state", "unrepresentable"); return result; }
	result.insert("state", effective < model->minValue<float>() || effective > model->maxValue<float>() ? "outside-range" : "valid");
	return result;
}

QJsonObject tempoReadback(Song* song)
{
	auto& model = song->tempoModel(); auto result = parameterReadback(&model);
	result.insert("parameter", static_cast<int>(model.id()));
	result.insert("manualValue", model.storedValue());
	// A linked peer may own the effective controller instead. Do not imply that
	// disconnecting this parameter always removes the source of a bad value.
	result.insert("directControllerConnected", model.controllerConnection() != nullptr);
	return result;
}

QJsonObject inspectParameter(AutomatableModel* model, bool withChoices = true, InspectionPage choicePage = {0, 128})
{
	FileResourceIO::EventSuspension events;
	const auto readback = parameterReadback(model);
	QString kind = "number";
	if (model->valueType() == AutomatableModel::ValueType::Boolean) { kind = "boolean"; }
	else if (model->valueType() == AutomatableModel::ValueType::Integer) { kind = "integer"; }
	QJsonArray path;
	for (const auto* parent = model->parent(); parent; parent = parent->parent())
	{
		if (const auto native = dynamic_cast<const Model*>(parent))
		{
			path.prepend(QJsonObject{{"class", native->metaObject()->className()}, {"name", native->displayName()}});
		}
	}
	QJsonObject result{{"id", static_cast<int>(model->id())}, {"name", model->fullDisplayName()},
		{"value", readback.value("value")}, {"readback", readback}, {"manualValue", model->storedValue()},
		{"minimum", model->minValue<float>()}, {"maximum", model->maxValue<float>()},
		{"step", model->step<float>()}, {"kind", kind}, {"scale", static_cast<int>(model->scaleType())},
		{"writable", model->isWritable()}, {"automatable", model->supportsAutomation()},
		{"linkedParametersPresent", model->nextLinkedModel() != model}, {"linkedPersistence", "checked-native-project"},
		{"linkedPersistenceScope", "complete projects/checkpoints, closed track/clip/selected-note copies and native clipboard/drag selections require every member to serialize; native clip/value/topology journals retain their own scopes; standalone Note clones and plugin-only copies remain outside portable graph scopes"},
		{"location", parameterLocation(model)}, {"modelPath", path}};
	if (withChoices)
	{
		QJsonArray choices;
		const auto labels = model->valueLabels();
		int total = 0;
		qint64 bytes = 2;
		bool full = false;
		for (auto it = labels.cbegin(); it != labels.cend(); ++it)
		{
			if (total % 64 == 0) { checkInspectionWork(); }
			if (it.key() >= model->minValue<float>() && it.key() <= model->maxValue<float>())
			{
				if (total >= choicePage.offset && choices.size() < choicePage.limit && !full)
				{ full = !appendInspectionItem(choices, {{"value", it.key()}, {"name", it.value()}}, bytes, 64 * 1024); }
				++total;
			}
		}
		if (total || dynamic_cast<ComboBoxModel*>(model))
		{
			result.insert("choices", choices);
			result.insert("choicePage", choicePage.metadata(total, choices.size()));
		}
	}
	if (auto connection = model->controllerConnection())
	{
		const auto native = connection->getController();
		const auto& controllers = Engine::getSong()->controllers();
		const auto it = std::find(controllers.begin(), controllers.end(), native);
		result.insert("controller", QJsonObject{{"index", it == controllers.end() ? QJsonValue(QJsonValue::Null)
			: QJsonValue(static_cast<int>(it - controllers.begin()))}, {"name", native->name()}, {"type", static_cast<int>(native->type())}});
		if (dynamic_cast<MidiController*>(native))
		{
			auto metadata = result.value("controller").toObject();
			metadata.insert("midiSelector", QJsonObject{{"parameter", static_cast<int>(model->id())}});
			metadata.insert("ownership", "parameter connection"); result.insert("controller", metadata);
		}
	}
	if (auto sync = dynamic_cast<TempoSyncKnobModel*>(model))
	{
		result.insert("syncMode", static_cast<int>(sync->syncMode()));
		result.insert("customNumerator", sync->getCustomMeterModel().getNumerator());
		result.insert("customDenominator", sync->getCustomMeterModel().getDenominator());
	}
	return result;
}

QJsonObject inspectParameterPage(const QList<AutomatableModel*>& models, const InspectionPage& page, bool choices)
{
	FileResourceIO::EventSuspension events;
	std::vector<QPointer<AutomatableModel>> snapshot; snapshot.reserve(models.size());
	for (auto model : models) { snapshot.emplace_back(model); }
	QJsonArray parameters;
	qint64 bytes = 2;
	for (int index = page.offset; index < models.size() && parameters.size() < page.limit; ++index)
	{
		checkInspectionWork();
		if (!snapshot[index]) { fail("Native parameter retired during paged inspection"); }
		auto item = inspectParameter(snapshot[index].data(), choices);
		if (!snapshot[index]) { fail("Native parameter retired during paged inspection"); }
		if (!appendInspectionItem(parameters, item, bytes, 512 * 1024)) { break; }
	}
	checkInspectionWork();
	for (const auto& model : snapshot) { if (!model) { fail("Native parameter retired during paged inspection"); } }
	auto result = page.metadata(models.size(), parameters.size());
	result.insert("parameters", parameters);
	return result;
}

QString effectPluginName(const Effect* effect)
{
	if (const auto missing = dynamic_cast<const DummyEffect*>(effect)) { return missing->originalPluginData().attribute("name"); }
	return QString::fromUtf8(effect->descriptor()->name);
}

QJsonArray effectNames(const EffectChain* effects)
{
	QJsonArray result;
	for (const auto effect : effects->effects()) { result.append(effectPluginName(effect)); }
	return result;
}

bool waveformRange(const graphModel* graph)
{
	return graph->minValue() <= -1.f && graph->maxValue() >= 1.f && graph->step() == 0.f;
}

QJsonObject inspectGraph(const Plugin::GraphResource& resource, bool samples = false)
{
	const auto graph = resource.model;
	QJsonArray actions{"clear", "invert", "normalize", "smooth", "smooth-noncyclic", "shift"};
	if (waveformRange(graph)) { for (const auto name : {"sine", "triangle", "saw", "square", "noise"}) { actions.append(name); } }
	QJsonObject result{{"key", resource.key}, {"name", resource.name}, {"units", resource.units},
		{"length", graph->length()}, {"min", graph->minValue()}, {"max", graph->maxValue()}, {"step", graph->step()},
		{"writable", resource.writable}, {"actions", actions}, {"fileImport", resource.writable && waveformRange(graph)}};
	if (samples)
	{
		QJsonArray values;
		for (int index = 0; index < graph->length(); ++index) { values.append(graph->samples()[index]); }
		result.insert("values", values);
	}
	return result;
}

QJsonArray inspectGraphs(Plugin* plugin)
{
	QJsonArray result;
	if (plugin)
	{
		for (const auto& graph : plugin->graphResources())
		{
			if (graph.model) { result.append(inspectGraph(graph)); }
		}
	}
	return result;
}

Plugin* editablePlugin(const QJsonObject& arguments)
{
	if (arguments.contains("effect"))
	{
		const auto effects = chain(arguments);
		const auto effect = effects->effects().at(integer(arguments, "effect", 0, static_cast<int>(effects->effects().size()) - 1));
		if (dynamic_cast<DummyEffect*>(effect)) { fail("Missing plugins have no editable native sound state"); }
		return effect;
	}
	if (arguments.contains("channel")) { fail("A mixer plugin selector requires an effect index"); }
	const auto instrument = instrumentTrack(arguments)->instrument();
	if (!instrument) { fail("Load a native instrument before accessing its sound-design state"); }
	return instrument;
}

QJsonObject inspectNativePrograms(Plugin* plugin, int offset, int limit)
{
	if (!plugin->supportsNativePrograms()) { fail("This plugin does not expose indexed native programs"); }
	const auto page = plugin->nativePrograms(offset, limit);
	if (page.total < 0 || page.offset != offset || page.programs.size() > static_cast<std::size_t>(limit)
		|| page.programs.size() != static_cast<std::size_t>(offset >= page.total ? 0 : std::min(limit, page.total - offset))
		|| (page.total == 0 ? page.current != -1 : page.current < 0 || page.current >= page.total))
	{ fail("Native plugin returned an inconsistent program page"); }
	QJsonArray programs;
	for (const auto& program : page.programs)
	{
		if (program.index != offset + programs.size() || program.index >= page.total) { fail("Native plugin returned an inconsistent program index"); }
		programs.append(QJsonObject{{"program", program.index}, {"name", program.name}, {"nameAvailable", program.nameAvailable}});
	}
	return {{"programs", programs}, {"total", page.total}, {"current", page.current}, {"offset", offset}, {"limit", limit},
		{"hasMore", static_cast<qint64>(offset) + programs.size() < page.total}, {"numbering", "zero-based native plugin index"}};
}

QJsonObject nativePresetResult(const Plugin::NativePresetInfo& preset, const QString& path)
{
	if ((preset.format != "fxp" && preset.format != "fxb") || preset.bytes < 56 || preset.programCount < 0
		|| preset.parameterCount < -1 || preset.currentProgram < -1
		|| !QRegularExpression("^[0-9a-f]{8}$").match(preset.pluginId).hasMatch()
		|| !QRegularExpression("^[0-9a-f]{64}$").match(preset.sha256).hasMatch())
	{ fail("Native plugin returned inconsistent preset metadata"); }
	return {{"path", path}, {"format", preset.format}, {"encoding", preset.encoding}, {"name", preset.name},
		{"pluginId", preset.pluginId}, {"pluginVersion", static_cast<double>(preset.pluginVersion)},
		{"formatVersion", preset.formatVersion}, {"programCount", preset.programCount}, {"parameterCount", preset.parameterCount},
		{"currentProgram", preset.currentProgram}, {"bytes", static_cast<double>(preset.bytes)}, {"sha256", preset.sha256}};
}

QJsonObject editGraph(const QString& operation, const QJsonObject& arguments)
{
	const auto plugin = editablePlugin(arguments);
	if (operation == "graph.list") { return {{"graphs", inspectGraphs(plugin)}}; }
	const auto key = string(arguments, "graph");
	const auto resources = plugin->graphResources();
	const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto& resource) { return resource.key == key && resource.model; });
	if (found == resources.end()) { fail("Native graph key was not found; rediscover with graph.list"); }
	const auto& resource = *found;
	const auto graph = resource.model;
	if (operation == "graph.inspect") { return {{"graph", inspectGraph(resource, true)}}; }
	if (!resource.writable) { fail("This native graph is read only"); }
	QString error;
	if (operation == "graph.set")
	{
		if (!arguments.value("values").isArray()) { fail("values must be an array of native graph samples"); }
		const auto input = arguments.value("values").toArray();
		if (input.size() != graph->length()) { fail("values must contain exactly the discovered native graph length"); }
		QVector<float> values(graph->length());
		for (int index = 0; index < input.size(); ++index)
		{
			const auto value = input.at(index);
			if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < graph->minValue() || value.toDouble() > graph->maxValue())
			{
				fail("Native graph values must be finite numbers within the discovered range");
			}
			values[index] = static_cast<float>(value.toDouble());
		}
		if (!graph->setSamplesChecked({values.constData(), static_cast<std::size_t>(values.size())}, error, true)) { fail(error); }
	}
	else if (operation == "graph.load")
	{
		if (!waveformRange(graph)) { fail("Waveform file import requires a continuous native graph containing the amplitude range -1 to 1"); }
		bool invalid = false; const auto path = PathUtil::toAbsolute(string(arguments, "path"), &invalid);
		if (invalid) { fail("Waveform path could not be resolved"); }
		if (!graph->loadWaveform(path, error)) { fail(error); }
	}
	else if (operation == "graph.transform")
	{
		const auto action = string(arguments, "action");
		if (!inspectGraph(resource).value("actions").toArray().contains(action)) { fail("Unsupported native graph action; use a discovered action"); }
		if (arguments.contains("degrees") && action != "shift") { fail("degrees is only valid for the shift action"); }
		const int degrees = action == "shift" ? integer(arguments, "degrees", -360000, 360000) : 0;
		graphModel staged(graph->minValue(), graph->maxValue(), graph->length(), nullptr, false, graph->step());
		if (!staged.setSamplesChecked({graph->samples(), static_cast<std::size_t>(graph->length())}, error)) { fail(error); }
		if (action == "clear") { staged.clear(); }
		else if (action == "invert") { staged.invert(); }
		else if (action == "normalize") { staged.normalize(); }
		else if (action == "smooth") { staged.smooth(); }
		else if (action == "smooth-noncyclic") { staged.smoothNonCyclic(); }
		else if (action == "shift") { staged.shiftPhase(degrees); }
		else if (action == "sine") { staged.setWaveToSine(); }
		else if (action == "triangle") { staged.setWaveToTriangle(); }
		else if (action == "saw") { staged.setWaveToSaw(); }
		else if (action == "square") { staged.setWaveToSquare(); }
		else if (action == "noise") { staged.setWaveToNoise(); }
		// Native transforms keep their established amplitude semantics. Check
		// finiteness before publishing; manual graph.set also constrains range.
		if (!graph->setSamplesChecked({staged.samples(), static_cast<std::size_t>(staged.length())}, error)) { fail(error); }
	}
	else { fail("Unknown native graph operation"); }
	plugin->graphEdited(key);
	return {{"graph", inspectGraph(resource, true)}};
}

QJsonObject inspectExpression(const Plugin::ExpressionResource& expression)
{
	QJsonArray symbols; for (const auto& symbol : expression.symbols) { symbols.append(symbol); }
	return {{"key", expression.key}, {"name", expression.name}, {"source", expression.source}, {"kind", expression.kind},
		{"symbols", symbols}, {"semantics", expression.semantics}, {"writable", expression.writable}};
}

QJsonArray inspectExpressions(const Plugin* plugin)
{
	QJsonArray result;
	if (plugin) { for (const auto& expression : plugin->expressionResources()) { result.append(inspectExpression(expression)); } }
	return result;
}

QJsonObject expressionResult(const Plugin::ExpressionResult& result, bool preview)
{
	QJsonObject response{{"valid", result.valid}, {"error", result.error}, {"sampleRate", result.sampleRate},
		{"frames", static_cast<int>(result.preview.size())}, {"nonfiniteSamples", result.nonfiniteSamples},
		{"scope", "native plugin preview context; live notes and automation can produce different results"}};
	if (preview)
	{
		QJsonArray samples; for (const auto value : result.preview) { samples.append(value); }
		response.insert("preview", samples);
	}
	return response;
}

QJsonObject editExpression(const QString& operation, const QJsonObject& arguments)
{
	const auto plugin = editablePlugin(arguments);
	if (operation == "expression.list") { return {{"expressions", inspectExpressions(plugin)}}; }
	const auto key = string(arguments, "expression");
	const auto resources = plugin->expressionResources();
	const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto& item) { return item.key == key; });
	if (found == resources.end()) { fail("Native expression key was not found; rediscover with expression.list"); }
	const auto source = arguments.value("source").toString(); // Empty source has native plugin semantics.
	const bool preview = arguments.contains("preview") && boolean(arguments, "preview");
	if (operation == "expression.validate") { return expressionResult(plugin->validateExpression(key, source), preview); }
	if (operation != "expression.set") { fail("Unknown native expression operation"); }
	if (!found->writable) { fail("This native expression is read only"); }
	Plugin::ExpressionResult result;
	if (!plugin->setExpression(key, source, result)) { fail(result.error.isEmpty() ? "Native expression edit was rejected" : result.error); }
	auto response = expressionResult(result, preview);
	for (const auto& expression : plugin->expressionResources())
	{
		if (expression.key == key) { response.insert("expression", inspectExpression(expression)); break; }
	}
	return response;
}

QJsonObject inspectEffectChain(EffectChain* effects, const QJsonObject& arguments)
{
	FileResourceIO::EventSuspension events;
	const QPointer<Song> song(Engine::getSong()); const QPointer<EffectChain> owner(effects);
	currentEffectChain(song, owner, arguments);
	std::vector<QPointer<Effect>> roster; for (auto effect : effects->effects()) { roster.emplace_back(effect); }
	const auto current = [&](std::size_t index) {
		currentEffectChain(song, owner, arguments);
		if (owner->effects().size() != roster.size() || !roster[index] || owner->effects()[index] != roster[index].data())
		{ fail("Native effect roster changed during inspection"); }
	};
	const bool details = !arguments.contains("details") || boolean(arguments, "details");
	const bool choices = !arguments.contains("choices") || boolean(arguments, "choices");
	const int selected = arguments.contains("effect") ? integer(arguments, "effect", 0, static_cast<int>(effects->effects().size()) - 1) : -1;
	const auto page = InspectionPage::read(arguments, "offset", "limit", 16, 128);
	const auto parameterPage = InspectionPage::read(arguments, "parameterOffset", "parameterLimit");
	const int total = selected >= 0 ? 1 : effects->effects().size();
	QJsonArray result;
	qint64 bytes = 2;
	for (int position = page.offset; position < total && result.size() < page.limit; ++position)
	{
		checkInspectionWork();
		const int index = selected >= 0 ? selected : position;
		current(index);
		const auto effect = effects->effects()[index]; const auto missing = dynamic_cast<DummyEffect*>(effect);
		const auto key = missing ? EffectKey(missing->originalPluginData().firstChildElement("key")) : effect->key();
		QJsonObject attributes; for (auto it = key.attributes.cbegin(); it != key.attributes.cend(); ++it) { attributes.insert(it.key(), it.value()); }
		QJsonObject item{{"index", index}, {"plugin", effectPluginName(effect)}, {"name", effect->displayName()}, {"attributes", attributes},
			{"missing", missing != nullptr}};
		if (!missing)
		{
			item.insert("nativeProgramsSupported", effect->supportsNativePrograms());
			current(index);
			item.insert("nativePresetsSupported", effect->supportsNativePresets());
			current(index);
			item.insert("enabled", effect->isEnabled()); item.insert("wet", effect->wetLevel());
			item.insert("autoQuitEnabled", effect->autoQuitEnabled()); item.insert("corrupted", effect->isCorrupted());
		}
		if (details)
		{
			auto controls = inspectParameterPage(missing ? QList<AutomatableModel*>{} : modelsUnder({effect}), parameterPage, choices);
			current(index);
			item.insert("parameters", controls.take("parameters"));
			item.insert("parameterPage", controls);
			item.insert("graphs", missing ? QJsonArray{} : inspectGraphs(effect));
			current(index);
			item.insert("expressions", missing ? QJsonArray{} : inspectExpressions(effect));
			current(index);
		}
		if (!appendInspectionItem(result, item, bytes, 1024 * 1024)) { break; }
	}
	checkInspectionWork();
	currentEffectChain(song, owner, arguments);
	if (owner->effects().size() != roster.size()) { fail("Native effect roster changed during inspection"); }
	for (std::size_t index = 0; index < roster.size(); ++index) { current(index); }
	auto response = page.metadata(total, result.size());
	response.insert("enabled", effects->enabledModel()->value());
	response.insert("enabledParameter", inspectParameter(effects->enabledModel(), choices));
	currentEffectChain(song, owner, arguments);
	if (owner->effects().size() != roster.size()) { fail("Native effect roster changed during inspection"); }
	for (std::size_t index = 0; index < roster.size(); ++index) { current(index); }
	response.insert("effects", result); response.insert("handleScope", "current project revision");
	return response;
}

QJsonArray inspectResources(const Plugin* plugin)
{
	QJsonArray result;
	if (!plugin) { return result; }
	const QPointer<Plugin> owner(const_cast<Plugin*>(plugin));
	const auto resources = plugin->fileResources();
	if (!owner) { fail("Native resource owner retired during inspection"); }
	std::vector<QPointer<AutomatableModel>> selectors;
	for (const auto& resource : resources) { selectors.emplace_back(resource.selector); }
	const auto ready = [&] {
		if (!owner) { fail("Native resource owner retired during inspection"); }
		for (std::size_t index = 0; index < resources.size(); ++index)
		{ if (selectors[index].data() != resources[index].selector) { fail("Native resource selector retired during inspection"); } }
	};
	for (std::size_t index = 0; index < resources.size(); ++index)
	{
		ready(); const auto& resource = resources[index]; const auto& selector = selectors[index];
		QJsonArray types;
		for (const auto& type : resource.fileTypes) { if (!type.isEmpty()) { types.append(type); } }
		QJsonObject item{{"key", resource.key}, {"name", resource.name}, {"path", resource.path}, {"fileTypes", types},
			{"requiredForRestore", !resource.path.isEmpty() && resource.requiredForRestore}, {"unresolved", resource.unresolved}};
		if (selector)
		{
			const auto id = static_cast<int>(selector->id());
			const auto value = selector->value<float>(); ready();
			item.insert("selectionParameter", id);
			item.insert("selectionValue", resource.selectionValue);
			item.insert("selected", value == resource.selectionValue);
		}
		result.append(item);
	}
	ready(); return result;
}

QJsonObject inspectScale(int slot, bool details = true)
{
	const auto scale = Engine::getSong()->getScale(slot);
	QJsonObject result{{"slot", slot}, {"name", scale->getDescription()}, {"degreeCount", static_cast<int>(scale->getIntervals().size()) - 1}};
	if (details)
	{
		QJsonArray intervals; QString text;
		int degree = 0;
		for (const auto& interval : scale->getIntervals())
		{
			QJsonObject item{{"degree", degree}, {"text", interval.getString()}, {"ratio", interval.getRatio()}, {"form", interval.isRatio() ? "ratio" : "cents"}};
			if (interval.isRatio()) { item.insert("numerator", static_cast<double>(interval.numerator())); item.insert("denominator", static_cast<double>(interval.denominator())); }
			else { item.insert("cents", interval.cents()); }
			intervals.append(item);
			if (degree++ > 0) { text += interval.getString() + '\n'; }
		}
		result.insert("intervals", intervals); result.insert("intervalText", text);
	}
	return result;
}

QJsonObject inspectKeymap(int slot, bool details = true)
{
	const auto keymap = Engine::getSong()->getKeymap(slot);
	QJsonObject result{{"slot", slot}, {"name", keymap->getDescription()}, {"size", static_cast<int>(keymap->getSize())},
		{"firstKey", keymap->getFirstKey()}, {"lastKey", keymap->getLastKey()}, {"middleKey", keymap->getMiddleKey()},
		{"baseKey", keymap->getBaseKey()}, {"baseFrequency", keymap->getBaseFreq()}, {"octaveDegree", keymap->octaveDegree()},
		{"linearFromMiddle", keymap->linearFromMiddle()}, {"referenceMapped", keymap->isMapped(keymap->getBaseKey())}};
	if (details)
	{
		QJsonArray entries; QString text;
		for (std::size_t i = 0; i < keymap->getSize(); ++i)
		{
			const bool mapped = keymap->isDegreeMapped(i);
			entries.append(mapped ? QJsonValue(keymap->getMap()[i]) : QJsonValue(QJsonValue::Null));
			text += (mapped ? QString::number(keymap->getMap()[i]) : QString("x")) + '\n';
		}
		result.insert("entries", entries); result.insert("mappingText", text);
	}
	return result;
}

QJsonObject inspectTuningLibrary(bool details = false)
{
	QJsonArray scales, keymaps;
	for (unsigned int i = 0; i < MaxScaleCount; ++i) { scales.append(inspectScale(i, details)); }
	for (unsigned int i = 0; i < MaxKeymapCount; ++i) { keymaps.append(inspectKeymap(i, details)); }
	return {{"scales", scales}, {"keymaps", keymaps}, {"maximumEntries", TuningFile::MaximumEntries}, {"maximumTextBytes", TuningFile::MaximumTextBytes}};
}

QJsonObject inspectInstrumentTuning(InstrumentTrack* track)
{
	const auto tuner = track->microtuner();
	const auto baseKey = track->keyRangeImport()
		? Engine::getSong()->getKeymap(tuner->currentKeymap())->getBaseKey() : track->baseNoteModel()->value();
	return {{"enabled", tuner->enabled()}, {"importKeyRange", tuner->keyRangeImport()},
		{"scale", tuner->currentScale()}, {"keymap", tuner->currentKeymap()},
		{"firstKey", track->firstKey()}, {"lastKey", track->lastKey()}, {"baseKey", baseKey}, {"baseFrequency", track->baseFreq()}};
}

QJsonObject inspectProgramSelection(Instrument* instrument)
{
	if (!instrument) { return {}; }
	const auto current = instrument->midiPatch();
	if (!current) { return {}; }
	QJsonObject result{{"bank", current->bank}, {"program", current->program}, {"available", false}};
	for (const auto& program : instrument->programs())
	{
		if (program.bank == current->bank && program.program == current->program)
		{
			result.insert("name", program.name); result.insert("available", true); break;
		}
	}
	return result;
}

QJsonObject inspectSlices(const Instrument::SliceLayout& layout)
{
	QJsonArray points, keys;
	int addressable = 0;
	for (std::size_t index = 0; index < layout.points.size(); ++index)
	{
		points.append(layout.points[index]);
		if (index + 1 < layout.points.size())
		{
			const int key = layout.baseKey + static_cast<int>(index) + 1;
			const bool validKey = key >= 0 && key < NumKeys;
			keys.append(validKey ? QJsonValue(key) : QJsonValue(QJsonValue::Null));
			if (validKey) { ++addressable; }
		}
	}
	const int count = layout.points.empty() ? 0 : static_cast<int>(layout.points.size()) - 1;
	return {{"loaded", layout.sourceFrames > 1}, {"sourceFrames", layout.sourceFrames}, {"sampleRate", layout.sampleRate},
		{"originalBpm", layout.originalBpm}, {"tempoSync", layout.tempoSync}, {"baseKey", layout.baseKey},
		{"points", points}, {"sliceKeys", keys}, {"sliceCount", count},
		{"addressableSlices", addressable}, {"coordinates", "normalized source positions; endpoints 0 and 1"},
		{"wholeSampleKey", layout.baseKey >= 0 && layout.baseKey < NumKeys ? QJsonValue(layout.baseKey) : QJsonValue(QJsonValue::Null)}};
}

QJsonObject inspectHostedParameter(const PluginHost::Parameter& parameter)
{
	const char* kind = parameter.kind == PluginHost::Parameter::Kind::Boolean ? "boolean"
		: parameter.kind == PluginHost::Parameter::Kind::Integer ? "integer" : "number";
	return {{"index", parameter.index}, {"nativeIndex", parameter.nativeIndex}, {"name", parameter.name},
		{"symbol", parameter.symbol}, {"unit", parameter.unit}, {"comment", parameter.comment}, {"group", parameter.group},
		{"kind", kind}, {"value", parameter.value}, {"text", parameter.text}, {"default", parameter.defaultValue},
		{"minimum", parameter.minimum}, {"maximum", parameter.maximum}, {"step", parameter.step},
		{"smallStep", parameter.smallStep}, {"largeStep", parameter.largeStep}, {"enabled", parameter.enabled},
		{"output", parameter.output}, {"writable", parameter.writable}, {"automatable", parameter.automatable},
		{"logarithmic", parameter.logarithmic}, {"saved", parameter.saved}, {"scalePointCount", parameter.scalePointCount},
		{"handleScope", "current hosted plugin and parameter indices; rediscover after native layout changes or restore"}};
}

QJsonObject inspectDiscovery(const PluginHost::DiscoveryPage& page)
{
	QJsonArray plugins;
	for (const auto& item : page.plugins)
	{
		QJsonObject load{{"format", item.plugin.format}, {"binary", item.plugin.binary}, {"name", item.plugin.name},
			{"path", item.plugin.path}, {"label", item.plugin.label}, {"uniqueId", QString::number(item.plugin.uniqueId)}};
		plugins.append(QJsonObject{{"load", load}, {"maker", item.maker}, {"hash", item.hash},
			{"category", item.category}, {"instrument", item.instrument},
			{"audioInputs", static_cast<double>(item.audioInputs)}, {"audioOutputs", static_cast<double>(item.audioOutputs)},
			{"midiInputs", static_cast<double>(item.midiInputs)}, {"midiOutputs", static_cast<double>(item.midiOutputs)},
			{"cvInputs", static_cast<double>(item.cvInputs)}, {"cvOutputs", static_cast<double>(item.cvOutputs)},
			{"parameterInputs", static_cast<double>(item.parameterInputs)}, {"parameterOutputs", static_cast<double>(item.parameterOutputs)}});
	}
	return {{"scan", page.scan}, {"state", page.state}, {"error", page.error}, {"plugins", plugins},
		{"offset", page.offset}, {"total", page.total}, {"hasMore", page.hasMore}, {"elapsedMs", page.elapsedMs},
		{"scope", "current instrument instance; discovery metadata does not prove plugin load or audio compatibility"}};
}

QJsonObject inspectPluginHost(PluginHost* host)
{
	QJsonObject result{{"available", host && host->available()},
		{"handleScope", "current hosted plugin order; rediscover after add, remove, swap or restore"}};
	if (!host || !host->available())
	{
		result.insert("reason", host ? host->unavailableReason() : "This instrument does not expose native hosted-plugin management");
		return result;
	}
	result.insert("formats", QJsonArray::fromStringList(host->formats()));
	QJsonObject parameters{{"available", host->parametersAvailable()}};
	if (!host->parametersAvailable()) { parameters.insert("reason", host->parametersUnavailableReason()); }
	result.insert("parameterAccess", parameters);
	QJsonObject discovery{{"available", host->discoveryAvailable()}};
	if (!host->discoveryAvailable()) { discovery.insert("reason", host->discoveryUnavailableReason()); }
	result.insert("discovery", discovery);
	QJsonObject patchbay{{"available", host->patchbayAvailable()}};
	if (!host->patchbayAvailable()) { patchbay.insert("reason", host->patchbayUnavailableReason()); }
	result.insert("patchbay", patchbay);
	QJsonArray plugins;
	for (const auto& plugin : host->plugins())
	{
		plugins.append(QJsonObject{{"index", plugin.index}, {"format", plugin.format}, {"name", plugin.name},
			{"label", plugin.label}, {"path", plugin.path}, {"maker", plugin.maker}, {"uniqueId", plugin.uniqueId}, {"active", plugin.active}});
	}
	result.insert("plugins", plugins);
	return result;
}

QJsonObject inspectPatchbay(const PluginHost::PatchbayGraph& graph)
{
	QJsonArray clients, ports, connections;
	for (const auto& client : graph.clients)
	{
		clients.append(QJsonObject{{"group", static_cast<double>(client.id)}, {"hostedPlugin", client.plugin},
			{"icon", client.icon}, {"name", client.name}});
	}
	for (const auto& port : graph.ports)
	{
		const char* signal = port.signal == PluginHost::PatchbayPort::Signal::Audio ? "audio"
			: port.signal == PluginHost::PatchbayPort::Signal::Midi ? "midi" : "cv";
		ports.append(QJsonObject{{"group", static_cast<double>(port.endpoint.group)}, {"port", static_cast<double>(port.endpoint.port)},
			{"portGroup", port.portGroup}, {"hints", static_cast<double>(port.hints)}, {"direction", port.input ? "input" : "output"},
			{"signal", signal}, {"name", port.name}});
	}
	for (const auto& connection : graph.connections)
	{
		connections.append(QJsonObject{{"id", static_cast<double>(connection.id)},
			{"sourceGroup", static_cast<double>(connection.route.source.group)}, {"sourcePort", static_cast<double>(connection.route.source.port)},
			{"destinationGroup", static_cast<double>(connection.route.destination.group)}, {"destinationPort", static_cast<double>(connection.route.destination.port)}});
	}
	return {{"clients", clients}, {"ports", ports}, {"connections", connections}, {"external", false},
		{"refreshMayRemoveIllegalConnections", true},
		{"handleScope", "current internal native graph; rediscover endpoints after structural edits or restore; connection IDs may change on every refresh"}};
}

QJsonObject inspectInstrument(InstrumentTrack* track, const QJsonObject& arguments)
{
	const auto page = InspectionPage::read(arguments);
	const bool withChoices = !arguments.contains("choices") || boolean(arguments, "choices");
	const auto effects = track->audioBusHandle()->effects();
	QList<QObject*> roots{track, effects, effects->enabledModel(), track->instrument()};
	for (const auto effect : effects->effects()) { roots.append(effect); }
	auto controls = inspectParameterPage(modelsUnder(roots), page, withChoices);
	QJsonObject result{{"name", track->name()}, {"parameters", controls.take("parameters")}, {"parameterPage", controls},
		{"resources", inspectResources(track->instrument())}, {"graphs", inspectGraphs(track->instrument())},
		{"expressions", inspectExpressions(track->instrument())}, {"arpeggioEnabled", track->isArpeggioEnabled()}, {"tuning", inspectInstrumentTuning(track)}, {"handleScope", "current project revision"}};
	if (const auto instrument = track->instrument())
	{
		result.insert("instrument", instrument->descriptor()->name);
		auto* host = instrument->pluginHost();
		result.insert("hostManagement", host && host->available());
		result.insert("singleStreamed", instrument->isSingleStreamed());
		result.insert("midiBased", instrument->isMidiBased());
		result.insert("bendable", instrument->isBendable());
		result.insert("programCount", static_cast<int>(instrument->programs().size()));
		result.insert("nativeProgramsSupported", instrument->supportsNativePrograms());
		result.insert("nativePresetsSupported", instrument->supportsNativePresets());
		result.insert("programSelection", inspectProgramSelection(instrument));
		result.insert("slicingSupported", instrument->sliceLayout().has_value());
		result.insert("midiOutputSupported", instrument->supportsMidiOutput());
		if (const auto patch = instrument->midiPatch()) { result.insert("midiPatch", QJsonObject{{"bank", patch->bank}, {"program", patch->program}}); }
	}
	else { result.insert("instrument", QJsonValue::Null); }
	if (withChoices)
	{
		QJsonArray tables;
		const auto& chords = InstrumentFunctionNoteStacking::ChordTable::getInstance().chords();
		int index = 0;
		for (const auto& chord : chords)
		{
			QJsonArray intervals;
			for (int i = 0; i < chord.size(); ++i) { intervals.append(chord[i]); }
			tables.append(QJsonObject{{"index", index++}, {"name", chord.getName()}, {"semitones", intervals}});
		}
		result.insert("nativeChordTables", tables);
	}
	return result;
}

QString resourcePath(const QJsonObject& args)
{
	if (!args.value("path").isString()) { fail("Resource path must be a string; empty clears the native resource"); }
	const auto path = args.value("path").toString();
	if (path.isEmpty()) { return {}; }
	bool error = false;
	const auto absolute = PathUtil::toAbsolute(path, &error);
	const QFileInfo file(absolute);
	if (error || !file.isFile() || !file.isReadable()) { fail("Resource path must resolve to a readable file"); }
	return absolute;
}

QJsonObject inspectLfoWave(const LfoController* controller)
{
	const auto buffer = controller->userWaveBuffer();
	return {{"path", controller->userWaveFile()}, {"unresolved", controller->userWaveUnresolved()}, {"frames", static_cast<double>(buffer->size())}, {"sampleRate", static_cast<double>(buffer->sampleRate())}};
}

Controller* controller(const QJsonObject& a)
{
	const auto& controllers = Engine::getSong()->controllers();
	if (controllers.empty()) { fail("Project has no controllers"); }
	const auto target = controllers.at(integer(a, "controller", 0, static_cast<int>(controllers.size()) - 1));
	if (!target || target->isRetiring()) { fail("Native rack controller is retiring"); }
	return target;
}

int currentControllerIndex(const QPointer<Song>& owner, const QPointer<Controller>& target)
{
	if (!owner || Engine::getSong() != owner.data() || owner->isLoadingProject() || !target || target->isRetiring() || target->thread() != QThread::currentThread())
	{ fail("Native rack controller or project retired during editing"); }
	const auto found = std::find(owner->controllers().begin(), owner->controllers().end(), target.data());
	if (found == owner->controllers().end()) { fail("Native controller left the project rack during editing"); }
	return static_cast<int>(std::distance(owner->controllers().begin(), found));
}

void currentMixer(const QPointer<Song>& owner, const QPointer<Mixer>& mixer, std::uint64_t generation, std::uint64_t project)
{
	if (!owner || Engine::getSong() != owner.data() || owner->isLoadingProject() || !mixer || Engine::mixer() != mixer.data()
		|| mixer->replacementGeneration() != generation || owner->replacementGeneration() != project) { fail("Native mixer or project was replaced during editing"); }
}
int currentMixerChannelIndex(const QPointer<Song>& owner, const QPointer<Mixer>& mixer,
	std::uint64_t generation, std::uint64_t project, MixerChannel* identity, const QPointer<EffectChain>& anchor)
{
	currentMixer(owner, mixer, generation, project);
	if (!anchor) { fail("Native mixer channel retired during editing"); }
	for (int index = 0; index < mixer->numChannels(); ++index) { if (mixer->mixerChannel(index) == identity) { return index; } }
	fail("Native mixer channel left the project during editing");
}

QJsonArray inspectControllers()
{
	FileResourceIO::EventSuspension events;
	const QPointer<Song> owner(Engine::getSong()); std::vector<QPointer<Controller>> roster;
	for (auto native : owner->controllers()) { roster.emplace_back(native); }
	const auto current = [&](std::size_t index) {
		if (!owner || Engine::getSong() != owner.data() || owner->controllers().size() != roster.size()
			|| !roster[index] || roster[index]->isRetiring() || owner->controllers()[index] != roster[index].data())
		{ fail("Native controller rack changed during inspection"); }
		return roster[index].data();
	};
	QJsonArray result;
	for (std::size_t index = 0; index < roster.size(); ++index)
	{
		checkInspectionWork(); const QPointer<Controller> native(current(index));
		std::vector<QPointer<AutomatableModel>> children;
		for (auto model : native->findChildren<AutomatableModel*>()) { children.emplace_back(model); }
		QJsonArray parameters;
		for (const auto& model : children)
		{
			checkInspectionWork(); current(index);
			if (!model) { fail("Native controller parameter retired during inspection"); }
			auto parameter = inspectParameter(model.data()); current(index);
			if (!model) { fail("Native controller parameter retired during inspection"); }
			parameter.insert("parameter", static_cast<int>(model->id()));
			parameters.append(parameter);
		}
		current(index);
		QJsonObject item{{"index", static_cast<int>(index)}, {"name", native->name()},
			{"type", static_cast<int>(native->type())}, {"connections", native->connectionCount()},
			{"sampleExact", native->isSampleExact()}, {"parameters", parameters}};
		if (const auto lfo = dynamic_cast<LfoController*>(native.data())) { item.insert("userWave", inspectLfoWave(lfo)); }
		current(index);
		result.append(item);
	}
	for (std::size_t index = 0; index < roster.size(); ++index) { current(index); }
	return result;
}

Plugin::Descriptor::SubPluginFeatures::Key pluginKey(const QJsonObject& a, Plugin::Type type)
{
	const auto name = string(a, a.contains("instrument") ? "instrument" : "plugin");
	const auto info = getPluginFactory()->pluginInfo(name.toUtf8().constData());
	if (info.isNull() || !info.descriptor || info.descriptor->type != type)
	{
		fail("Required native plugin is unavailable: " + name);
	}
	Plugin::Descriptor::SubPluginFeatures::Key::AttributeMap attributes;
	if (a.contains("attributes"))
	{
		if (!a.value("attributes").isObject()) { fail("attributes must be an object of strings"); }
		const auto object = a.value("attributes").toObject();
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			if (!it.value().isString()) { fail("Subplugin attributes must be strings"); }
			attributes.insert(it.key(), it.value().toString());
		}
	}
	return {info.descriptor, name, attributes};
}

QJsonObject inspectMidi(MidiPort* port);

QJsonObject inspectNativeNote(const Note* note, int index, bool details)
{
	QJsonObject result{{"index", index}, {"position", static_cast<int>(note->pos())}, {"length", static_cast<int>(note->length())},
		{"key", note->key()}, {"volume", note->getVolume()}, {"panning", note->getPanning()}, {"type", static_cast<int>(note->type())}};
	if (note->hasDetuningInfo())
	{
		if (details) { result.insert("detuning", inspectDetuning(note)); }
		else
		{
			const auto curve = note->detuning() ? note->detuning()->existingAutomationClip() : nullptr;
			result.insert("detuning", QJsonObject{{"pointCount", curve ? static_cast<int>(curve->getTimeMap().size()) : 0},
				{"progression", curve ? static_cast<int>(curve->progressionType()) : 1}, {"tension", curve ? curve->getTension() : 1.f},
				{"hasAutomation", true}, {"units", "semitones; positions are relative to note start"}});
		}
	}
	return result;
}

QString projectNamePreview(const QString& text)
{
	// Bound the substring before converting it, even for a very long native name.
	int characters = text.size() > 256 ? 256 : static_cast<int>(text.size());
	if (characters && text.at(characters - 1).isHighSurrogate()) { --characters; }
	const auto bytes = text.left(characters).toUtf8(); int end = std::min(256, static_cast<int>(bytes.size()));
	while (end < bytes.size() && (static_cast<unsigned char>(bytes.at(end)) & 0xc0) == 0x80) { --end; }
	return QString::fromUtf8(bytes.constData(), end);
}

void projectText(QJsonObject& object, const QString& key, const QString& value, bool full)
{
	if (full) { object.insert(key, value); }
	else
	{
		const auto preview = projectNamePreview(value); object.insert(key + "Preview", preview);
		object.insert(key + "Truncated", preview.size() != value.size());
	}
}

QJsonObject inspectCurveSection(const AutomationClip* curve, const QJsonObject& arguments, bool targets = true)
{
	FileResourceIO::EventSuspension events; checkInspectionWork();
	const auto section = arguments.contains("section") ? string(arguments, "section") : QString("full");
	if (section != "full" && section != "summary" && section != "points" && !(targets && section == "targets"))
	{ fail("Unknown native automation inspection section"); }
	for (const auto key : {"offset", "limit", "fromPosition", "names"})
	{
		const bool allowed = section == "points" ? QString(key) == "limit" || QString(key) == "fromPosition"
			: section == "targets" ? QString(key) != "fromPosition" : false;
		if (arguments.contains(key) && !allowed) { fail("Argument is not applicable to this automation section: " + QString(key)); }
	}
	if (section == "full") { return inspectCurve(curve, targets); }
	auto result = inspectCurveMetadata(curve, targets); result.insert("section", section);
	result.insert("handleScope", "current project revision; rediscover after edits or restoration");
	if (section == "summary") { return result; }
	const auto page = InspectionPage::read(arguments); QJsonArray items; qint64 bytes = 2;
	if (section == "points")
	{
		const bool hasStart = arguments.contains("fromPosition");
		const int start = hasStart ? integer(arguments, "fromPosition", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) : 0;
		QJsonValue next = QJsonValue::Null;
		if (curve)
		{
			const auto& points = curve->getTimeMap(); auto it = hasStart ? points.lowerBound(start) : points.cbegin();
			while (it != points.cend() && items.size() < page.limit)
			{
				checkInspectionWork(); if (!appendInspectionItem(items, inspectNode(it.key(), it.value()), bytes, 512 * 1024)) { break; }
				++it;
			}
			if (it != points.cend()) { next = it.key(); }
		}
		result.insert("points", items); result.insert("total", result.value("pointCount")); result.insert("limit", page.limit);
		result.insert("fromPosition", hasStart ? QJsonValue(start) : QJsonValue(QJsonValue::Null));
		result.insert("nextPosition", next); result.insert("hasMore", !next.isNull());
	}
	else
	{
		const bool names = !arguments.contains("names") || boolean(arguments, "names");
		const int total = curve ? static_cast<int>(curve->objects().size()) : 0;
		for (int index = page.offset; index < total && items.size() < page.limit; ++index)
		{
			checkInspectionWork(); const auto model = curve->objects().at(index).data();
			QJsonObject item{{"index", index}, {"available", model != nullptr}};
			if (model)
			{
				item.insert("parameter", static_cast<int>(model->id())); projectText(item, "name", model->fullDisplayName(), names);
				item.insert("minimum", model->minValue<float>()); item.insert("maximum", model->maxValue<float>());
			}
			if (!appendInspectionItem(items, item, bytes, 512 * 1024)) { break; }
		}
		const auto metadata = page.metadata(total, items.size());
		for (auto it = metadata.begin(); it != metadata.end(); ++it) { result.insert(it.key(), it.value()); }
		result.insert("targets", items);
	}
	checkInspectionWork(); return result;
}

QJsonObject inspectDetuningSection(const Note* note, const QJsonObject& arguments)
{
	const auto curve = note->detuning() ? note->detuning()->existingAutomationClip() : nullptr;
	auto result = inspectCurveSection(curve, arguments, false);
	if (!curve) { result.insert("progression", 1); }
	result.insert("hasAutomation", note->hasDetuningInfo()); result.insert("units", "semitones; positions are relative to note start");
	result.insert("parameter", note->detuning() ? QJsonValue(static_cast<int>(note->detuning()->id())) : QJsonValue(QJsonValue::Null));
	if (curve) { result.insert("curveControls", QJsonObject{{"mute", static_cast<int>(curve->mutedModel()->id())}, {"solo", static_cast<int>(curve->soloModel()->id())}}); }
	return result;
}

QJsonObject inspectControllerInventory(Controller* controller, int index, bool names)
{
	if (!controller || controller->isRetiring()) { fail("Native rack controller is retiring during inspection"); }
	QJsonObject result{{"index", index}, {"type", static_cast<int>(controller->type())},
		{"connections", controller->connectionCount()}, {"sampleExact", controller->isSampleExact()}};
	projectText(result, "name", controller->name(), names);
	if (const auto lfo = dynamic_cast<LfoController*>(controller))
	{
		auto wave = inspectLfoWave(lfo);
		if (!names) { const auto path = wave.take("path").toString(); projectText(wave, "path", path, false); }
		result.insert("userWave", wave);
	}
	return result;
}

QJsonObject inspectChannelInventory(MixerChannel* channel, int index, bool names)
{
	QJsonObject result{{"index", index}, {"volume", channel->m_volumeModel.value()}, {"muted", channel->m_muteModel.value()},
		{"solo", channel->m_soloModel.value()}, {"muteBeforeSolo", Engine::mixer()->soloChannel() >= 0 ? channel->m_muteBeforeSolo : channel->m_muteModel.value()},
		{"effectCount", static_cast<int>(channel->m_fxChain.effects().size())}, {"effectsEnabled", channel->m_fxChain.enabledModel()->value()},
		{"sendCount", static_cast<int>(channel->m_sends.size())}, {"receiveCount", static_cast<int>(channel->m_receives.size())},
		{"parameters", QJsonObject{{"volume", static_cast<int>(channel->m_volumeModel.id())}, {"muted", static_cast<int>(channel->m_muteModel.id())},
			{"solo", static_cast<int>(channel->m_soloModel.id())}, {"effectsEnabled", static_cast<int>(channel->m_fxChain.enabledModel()->id())}}}};
	projectText(result, "name", channel->m_name, names); return result;
}

QJsonObject inspectRouteInventory(MixerRoute* route, int index)
{
	return {{"index", index}, {"from", static_cast<int>(route->senderIndex())}, {"to", static_cast<int>(route->receiverIndex())},
		{"amount", route->amount()->value()}, {"parameter", static_cast<int>(route->amount()->id())}};
}

QJsonObject inspectTrackInventory(Track* track, int index, bool names)
{
	QJsonObject result{{"index", index}, {"type", static_cast<int>(track->type())}, {"muted", track->isMuted()},
		{"solo", track->isSolo()}, {"clipCount", static_cast<int>(track->getClips().size())}};
	projectText(result, "name", track->name(), names);
	if (const auto pattern = dynamic_cast<PatternTrack*>(track)) { result.insert("patternIndex", pattern->patternIndex()); }
	if (const auto effects = trackEffects(track))
	{
		result.insert("effectCount", static_cast<int>(effects->effects().size())); result.insert("effectsEnabled", effects->enabledModel()->value());
		if (const auto instrument = dynamic_cast<InstrumentTrack*>(track)) { result.insert("mixerChannel", instrument->mixerChannelModel()->value()); }
		else if (const auto sample = dynamic_cast<SampleTrack*>(track)) { result.insert("mixerChannel", sample->mixerChannelModel()->value()); }
	}
	if (const auto instrument = dynamic_cast<InstrumentTrack*>(track); instrument && instrument->instrument())
	{ projectText(result, "instrument", QString::fromUtf8(instrument->instrument()->descriptor()->name), names); }
	return result;
}

QJsonObject inspectClipInventory(Clip* clip, int index, bool names)
{
	QJsonObject result{{"index", index}, {"type", clip->nodeName()}, {"position", static_cast<int>(clip->startPosition())},
		{"length", static_cast<int>(clip->length())}, {"muted", clip->isMuted()}, {"offset", static_cast<int>(clip->startTimeOffset())},
		{"autoResize", clip->getAutoResize()}};
	projectText(result, "name", clip->name(), names);
	if (const auto midi = dynamic_cast<MidiClip*>(clip))
	{ result.insert("noteCount", static_cast<int>(midi->notes().size())); result.insert("stepCount", midi->stepCount()); }
	if (const auto curve = dynamic_cast<AutomationClip*>(clip))
	{
		result.insert("automation", QJsonObject{{"pointCount", static_cast<int>(curve->getTimeMap().size())}, {"targetCount", static_cast<int>(curve->objects().size())},
			{"progression", static_cast<int>(curve->progressionType())}, {"tension", curve->getTension()}, {"hasAutomation", curve->hasAutomation()}});
	}
	if (const auto sample = dynamic_cast<SampleClip*>(clip))
	{
		auto metadata = inspectSample(sample);
		if (!names) { const auto path = metadata.take("path").toString(); projectText(metadata, "path", path, false); }
		result.insert("sample", metadata);
	}
	return result;
}

QJsonObject inspectProjectSection(const QJsonObject& arguments)
{
	FileResourceIO::EventSuspension events; checkInspectionWork();
	const auto section = arguments.contains("section") ? string(arguments, "section") : QString("full");
	const bool arrangement = section == "tracks" || section == "clips" || section == "notes";
	const bool state = section == "channels" || section == "routes" || section == "controllers" || section == "globalAutomations";
	const bool warnings = section == "warnings";
	const bool resources = section == "resources";
	QSet<QString> allowed{"section"};
	if (arrangement || state || warnings || resources) { allowed.unite({"offset", "limit"}); }
	if (resources) { allowed.unite({"names", "resourceGeneration"}); }
	if (warnings) { allowed.unite({"messageOffset", "messageLimit", "diagnosticGeneration"}); }
	if (arrangement) { allowed.insert("container"); }
	if (section == "clips" || section == "notes") { allowed.insert("track"); }
	if (section == "notes") { allowed.insert("clip"); }
	if ((arrangement || state) && section != "notes" && section != "routes") { allowed.insert("names"); }
	for (auto it = arguments.begin(); it != arguments.end(); ++it) { if (!allowed.contains(it.key())) { fail("Argument is not applicable to this project section: " + it.key()); } }
	if (section == "full") { return StudioCapabilities::inspect(); }
	if (section == "summary") { return StudioCapabilities::inspectSummary(); }
	if (resources) { return StudioProjectResources::inspect(arguments); }
	if (warnings)
	{
		const auto page = InspectionPage::read(arguments);
		const auto textPage = InspectionPage::read(arguments, "messageOffset", "messageLimit", 1024, 8192);
		if (textPage.limit < 2) { fail("messageLimit must be at least 2 UTF-16 code units"); }
		const auto song = Engine::getSong();
		const auto generationValue = song->errorGeneration();
		const auto generation = QString::number(static_cast<qulonglong>(generationValue));
		const auto checkGeneration = [&] {
			if (song->errorGeneration() != generationValue) { fail("Native diagnostics changed; restart warning inspection with the current generation"); }
		};
		if (arguments.contains("diagnosticGeneration") && string(arguments, "diagnosticGeneration") != generation)
		{ fail("Native diagnostics changed; restart warning inspection with the current generation"); }
		const auto& errors = song->nativeErrors(); QJsonArray items; qint64 bytes = 2; int index = 0;
		for (auto it = errors.cbegin(); it != errors.cend(); ++it, ++index)
		{
			checkInspectionWork(); checkGeneration(); if (index < page.offset) { continue; } if (items.size() >= page.limit) { break; }
			const auto& message = it.key();
			if (message.size() > std::numeric_limits<int>::max()) { fail("Native warning exceeds the supported UTF-16 offset range"); }
			const int total = static_cast<int>(message.size());
			if (textPage.offset > 0 && textPage.offset < total && message.at(textPage.offset).isLowSurrogate()
				&& message.at(textPage.offset - 1).isHighSurrogate()) { fail("messageOffset splits a UTF-16 surrogate pair"); }
			int length = textPage.offset < total ? std::min(textPage.limit, total - textPage.offset) : 0;
			if (length && textPage.offset + length < total && message.at(textPage.offset + length - 1).isHighSurrogate()
				&& message.at(textPage.offset + length).isLowSurrogate()) { --length; }
			QJsonObject item{{"index", index}, {"message", message.mid(textPage.offset, length)}, {"occurrences", it.value()},
				{"occurrencesSaturated", it.value() == std::numeric_limits<int>::max()}, {"messagePage", textPage.metadata(total, length)}};
			if (!appendInspectionItem(items, item, bytes, 512 * 1024)) { break; }
		}
		checkInspectionWork(); checkGeneration(); auto result = page.metadata(static_cast<int>(errors.size()), items.size());
		result.insert("section", section); result.insert("warnings", items); result.insert("diagnosticGeneration", generation);
		result.insert("handleScope", "current process and native diagnostics generation; rediscover after restart or warnings change; message offsets use UTF-16 code units");
		return result;
	}
	if (!arrangement && !state) { fail("Unknown native project inspection section"); }
	const auto page = InspectionPage::read(arguments); const auto owner = arrangement ? container(arguments) : nullptr;
	const auto names = arguments.contains("names") ? boolean(arguments, "names") : true;
	int total = 0; Track* selectedTrack = nullptr; MidiClip* selectedClip = nullptr;
	if (section == "tracks") { total = static_cast<int>(owner->tracks().size()); }
	else if (section == "channels") { total = Engine::mixer()->numChannels(); }
	else if (section == "routes") { total = static_cast<int>(Engine::mixer()->m_mixerRoutes.size()); }
	else if (section == "controllers") { total = static_cast<int>(Engine::getSong()->controllers().size()); }
	else if (section == "globalAutomations") { total = Engine::getSong()->globalAutomationTrack()->numOfClips(); }
	else
	{
		selectedTrack = track(arguments);
		if (section == "clips") { total = static_cast<int>(selectedTrack->getClips().size()); }
		else { selectedClip = midiClip(arguments); total = static_cast<int>(selectedClip->notes().size()); }
	}
	QJsonArray items; qint64 bytes = 2;
	for (int index = page.offset; index < total && items.size() < page.limit; ++index)
	{
		checkInspectionWork(); QJsonObject item;
		if (section == "tracks") { item = inspectTrackInventory(owner->tracks().at(index), index, names); }
		else if (section == "clips") { item = inspectClipInventory(selectedTrack->getClips().at(index), index, names); }
		else if (section == "channels") { item = inspectChannelInventory(Engine::mixer()->mixerChannel(index), index, names); }
		else if (section == "routes") { item = inspectRouteInventory(Engine::mixer()->m_mixerRoutes.at(index), index); }
		else if (section == "controllers") { item = inspectControllerInventory(Engine::getSong()->controllers().at(index), index, names); }
		else if (section == "globalAutomations") { item = inspectClipInventory(Engine::getSong()->globalAutomationTrack()->getClip(index), index, names); }
		else { item = inspectNativeNote(selectedClip->notes().at(index), index, false); }
		if (!appendInspectionItem(items, item, bytes, 512 * 1024)) { break; }
	}
	checkInspectionWork(); auto result = page.metadata(total, items.size());
	result.insert("section", section); result.insert(section, items);
	if (arrangement) { result.insert("container", arguments.value("container").toString("song")); }
	for (const auto field : {"track", "clip"}) { if (arguments.contains(field)) { result.insert(field, arguments.value(field)); } }
	result.insert("handleScope", "current project revision; rediscover indices after structural edits or restoration");
	return result;
}

QJsonArray inspectTracks(TrackContainer* tc)
{
	QJsonArray result;
	int index = 0;
	for (auto t : tc->tracks())
	{
		QJsonArray clips;
		int ci = 0;
		for (auto c : t->getClips())
		{
			QJsonObject item{{"index", ci++}, {"name", c->name()}, {"type", c->nodeName()},
				{"position", static_cast<int>(c->startPosition())}, {"length", static_cast<int>(c->length())},
				{"muted", c->isMuted()}, {"offset", static_cast<int>(c->startTimeOffset())},
				{"autoResize", c->getAutoResize()}};
			if (auto sample = dynamic_cast<SampleClip*>(c)) { item.insert("sample", inspectSample(sample)); }
			if (auto automation = dynamic_cast<AutomationClip*>(c)) { item.insert("automation", inspectCurve(automation)); }
			if (auto midi = dynamic_cast<MidiClip*>(c))
			{
				QJsonArray notes;
				int ni = 0;
				for (auto n : midi->notes())
				{
					notes.append(inspectNativeNote(n, ni++, true));
				}
				item.insert("notes", notes);
			}
			clips.append(item);
		}
		QJsonObject item{{"index", index++}, {"name", t->name()}, {"type", static_cast<int>(t->type())},
			{"muted", t->isMuted()}, {"solo", t->isSolo()}, {"clips", clips}};
		if (const auto pattern = dynamic_cast<PatternTrack*>(t)) { item.insert("patternIndex", pattern->patternIndex()); }
		if (const auto instrument = dynamic_cast<InstrumentTrack*>(t)) { item.insert("midi", inspectMidi(instrument->midiPort())); }
		if (const auto effects = trackEffects(t))
		{
			item.insert("effects", effectNames(effects)); item.insert("effectsEnabled", effects->enabledModel()->value());
			if (const auto instrument = dynamic_cast<InstrumentTrack*>(t)) { item.insert("mixerChannel", instrument->mixerChannelModel()->value()); }
			else if (const auto sample = dynamic_cast<SampleTrack*>(t)) { item.insert("mixerChannel", sample->mixerChannelModel()->value()); }
		}
		if (auto instrument = dynamic_cast<InstrumentTrack*>(t); instrument && instrument->instrument())
		{
			item.insert("instrument", instrument->instrument()->descriptor()->name);
			item.insert("resources", inspectResources(instrument->instrument()));
		}
		result.append(item);
	}
	return result;
}

QString playModeName(Song::PlayMode mode)
{
	switch (mode)
	{
	case Song::PlayMode::Song: return "song";
	case Song::PlayMode::Pattern: return "pattern";
	case Song::PlayMode::MidiClip: return "midiClip";
	case Song::PlayMode::AutomationClip: return "automationClip";
	default: return "none";
	}
}

Song::PlayMode transportMode(const QJsonObject& a, Song::PlayMode fallback)
{
	if (!a.contains("playMode")) { return fallback; }
	const auto mode = string(a, "playMode");
	if (mode == "song") { return Song::PlayMode::Song; }
	if (mode == "pattern") { return Song::PlayMode::Pattern; }
	if (mode == "midiClip") { return Song::PlayMode::MidiClip; }
	fail("playMode must be song, pattern or midiClip");
}

QJsonObject recordingState()
{
	int armed = 0, active = 0, pending = 0, finalizing = 0, automationArmed = 0, reserved = 0, noteRecording = 0, automationReserved = 0;
	const auto inspectTrack = [&](const Track* track) {
		noteRecording += track->isNoteRecording();
		for (const auto clip : track->getClips())
		{
			if (const auto sample = dynamic_cast<SampleClip*>(clip))
			{
				armed += sample->isRecord(); active += sample->hasActiveRecording();
				pending += sample->hasPendingRecording(); finalizing += sample->isFinalizingRecording();
				reserved += sample->hasReservedRecording();
			}
			if (const auto curve = dynamic_cast<AutomationClip*>(clip))
			{
				automationArmed += curve->isRecording(); automationReserved += curve->hasReservedRecording();
			}
		}
	};
	for (const auto owner : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (const auto track : owner->tracks()) { inspectTrack(track); }
	}
	inspectTrack(Engine::getSong()->globalAutomationTrack());
	return {{"songRecording", Engine::getSong()->isRecording()}, {"armedSampleClips", armed},
		{"activeSampleClips", active}, {"pendingSampleClips", pending}, {"finalizingSampleClips", finalizing}, {"reservedSampleClips", reserved},
		{"armedAutomationClips", automationArmed}, {"midiRecordingTracks", noteRecording}, {"reservedAutomationClips", automationReserved}};
}

QJsonObject inspectMidiInputBackend(const MidiClient* client)
{
	const auto state = client ? client->inputBackendStatus() : MidiClient::InputBackendStatus{};
	return {{"observationAvailable", state.observed}, {"active", state.active}, {"nativeErrorCode", state.nativeErrorCode},
		{"overrunObservationAvailable", state.overrunObservationAvailable},
		{"reportedOverrunBytes", QString::number(static_cast<qulonglong>(state.reportedOverrunBytes))},
		{"overrunScope", "last native input byte-loss sample; separate from dropped application events; zero is not proof of loss-free capture"}};
}

QJsonObject inspectMidiNativeRetirement()
{
	const auto state = MidiClient::nativeRetirementStatus();
	return {{"observationAvailable", state.observationAvailable}, {"retrying", state.retrying},
		{"pendingInputHandles", static_cast<int>(state.pendingInputHandles)}, {"pendingOutputHandles", static_cast<int>(state.pendingOutputHandles)},
		{"cleanupPendingChannels", static_cast<int>(state.cleanupPendingChannels)},
		{"nativeErrorCode", state.nativeErrorCode}, {"lastNativeErrorCode", state.lastNativeErrorCode},
		{"retryOnBackendSelection", state.retryOnBackendSelection},
		{"scope", "process-local retired native handles, including clients replaced by another backend; no physical silence acknowledgment"}};
}

QJsonObject inspectMidiOutputBackend(const MidiClient* client)
{
	const auto state = client ? client->outputBackendStatus() : MidiClient::OutputBackendStatus{};
	return {{"observed", state.observed}, {"active", state.active},
		{"pendingEvents", static_cast<int>(state.pendingEvents)}, {"queueLimit", static_cast<int>(state.queueLimit)},
		{"cleanupPendingChannels", static_cast<int>(state.cleanupPendingChannels)},
		{"nativeErrorCode", state.nativeErrorCode}, {"lastNativeErrorCode", state.lastNativeErrorCode},
		{"abandonmentSupported", state.abandonmentSupported}, {"abandonmentEligible", state.abandonmentEligible},
		{"scope", "client output queue, retained channel cleanup and native buffer submission; external routing and device receipt are not acknowledged"}};
}

QJsonArray inspectMidiStartupFailures(const AudioEngine* engine)
{
	QJsonArray result;
	for (const auto& failure : engine->midiBackendStartupFailures())
	{
		result.append(QJsonObject{{"name", failure.name}, {"nativeCodeAvailable", failure.nativeCodeAvailable}, {"nativeErrorCode", failure.nativeErrorCode}});
	}
	return result;
}

QJsonObject inspectAudioProcessing()
{
	const auto engine = Engine::audioEngine(); const auto state = engine->dspStatus();
	const auto workers = AudioEngineWorkerThread::schedulerStatus();
	return {{"failed", state.failure != AudioEngine::DspFailure::None},
		{"failure", AudioEngine::dspFailureName(state.failure)}, {"failureCount", QString::number(static_cast<qulonglong>(state.failures))},
		{"resetEligible", engine->canResetDspFailure()},
		{"counterExhausted", state.failures == (std::numeric_limits<std::uint64_t>::max() >> 8)},
		{"workers", QJsonObject{{"registered", workers.registered}, {"ready", workers.ready},
			{"queued", workers.queued}, {"scanning", workers.scanning}, {"batchActive", workers.batchActive}}}};
}

QJsonObject inspectAudioDeviceRestore()
{
	const auto engine = Engine::audioEngine(); const auto failure = engine->deviceRestoreFailure();
	const auto cleanup = Engine::getSong()->hasPendingExportCleanup();
	return {{"pending", engine->hasStoredAudioDevice() || cleanup}, {"exportCleanupPending", cleanup}, {"restoring", engine->isRestoringAudioDevice()},
		{"inProgress", engine->isChangingAudioDevice()}, {"failed", failure != AudioEngine::DeviceRestoreFailure::None || cleanup},
		{"failure", AudioEngine::deviceRestoreFailureName(cleanup && failure == AudioEngine::DeviceRestoreFailure::None ? AudioEngine::DeviceRestoreFailure::ExportStopException : failure)},
		{"retryEligible", (engine->hasStoredAudioDevice() || cleanup) && !engine->isChangingAudioDevice()
			&& (!Engine::getSong()->isExporting() || cleanup) && !RemotePlugin::hasRunningRenderExecution()
			&& (!RemotePlugin::hasActiveRenderContext() || engine->isRestoringAudioDevice())}};
}

QJsonObject inspectTransport()
{
	const auto song = Engine::getSong(); const auto engine = Engine::audioEngine(); const auto device = engine->audioDev();
	const auto deviceStatus = device ? device->processingStatus()
		: AudioDevice::ProcessingStatus{false, AudioDevice::ProcessingFailure::None, 0};
	QString kind = "unavailable";
	if (device) { kind = dynamic_cast<AudioDummy*>(device) ? "dummy" : dynamic_cast<AudioFileDevice*>(device) ? "file" : "device"; }
	QJsonArray positions;
	for (const auto mode : {Song::PlayMode::None, Song::PlayMode::Song, Song::PlayMode::Pattern, Song::PlayMode::MidiClip})
	{
		const auto& timeline = song->getTimeline(mode);
		positions.append(QJsonObject{{"playMode", playModeName(mode)}, {"position", timeline.ticks()},
			{"frameOffset", timeline.frameOffset()}, {"elapsedSeconds", timeline.getElapsedSeconds()},
			{"loop", QJsonObject{{"begin", timeline.loopBegin().getTicks()}, {"end", timeline.loopEnd().getTicks()},
				{"enabled", timeline.loopEnabled()}}}, {"stopBehaviour", static_cast<int>(timeline.stopBehaviour())}});
	}
	QJsonObject selection;
	if (const auto playing = song->playingMidiClip())
	{
		for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(Engine::patternStore())})
		{
			int trackIndex = 0;
			for (const auto track : owner->tracks())
			{
				const auto& clips = track->getClips(); const auto found = std::find(clips.begin(), clips.end(), playing);
				if (found != clips.end())
				{
					selection = {{"container", owner == song ? "song" : "pattern"}, {"track", trackIndex},
						{"clip", static_cast<int>(std::distance(clips.begin(), found))}, {"loop", song->midiClipLoopEnabled()}};
				}
				++trackIndex;
			}
		}
	}
	return {{"playMode", playModeName(song->playMode())}, {"playing", song->isPlaying()}, {"paused", song->isPaused()},
		{"midiOutputCleanupError", song->midiOutputCleanupError()},
		{"stopped", song->isStopped()}, {"exporting", song->isExporting()}, {"exportCleanupPending", song->hasPendingExportCleanup()}, {"position", song->getPlayPos().getTicks()},
		{"positions", positions}, {"pattern", Engine::patternStore()->currentPattern()}, {"midiClip", selection},
		{"recording", recordingState()}, {"backend", QJsonObject{{"kind", kind}, {"engineReportedName", engine->audioDevName()},
			{"dsp", inspectAudioProcessing()},
			{"restoration", inspectAudioDeviceRestore()},
			{"requestedName", engine->requestedAudioBackend()},
			{"running", deviceStatus.running}, {"supportsCapture", device && device->supportsCapture()},
			{"processingFailure", AudioDevice::processingFailureName(deviceStatus.failure)}, {"nativeError", deviceStatus.nativeError},
			{"inputErrorEpoch", QString::number(engine->inputErrorEpoch())},
			{"renderOnly", engine->renderOnly()}, {"reserved", engine->hasStoredAudioDevice()},
			{"startFailed", engine->audioDevStartFailed()}, {"sampleRate", static_cast<int>(engine->outputSampleRate())},
			{"framesPerPeriod", static_cast<int>(engine->framesPerPeriod())}}},
		{"midiBackend", QJsonObject{{"kind", !engine->midiClient() ? "unavailable" : dynamic_cast<MidiDummy*>(engine->midiClient()) ? "dummy" : "device"},
			{"engineReportedName", engine->midiClientName()}, {"requestedName", engine->requestedMidiBackend()},
			{"inputBackend", inspectMidiInputBackend(engine->midiClient())}, {"outputBackend", inspectMidiOutputBackend(engine->midiClient())},
			{"nativeRetirement", inspectMidiNativeRetirement()},
			{"startupFailures", inspectMidiStartupFailures(engine)}}}};
}

void requirePlaybackTransport()
{
	const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
	if (song->isExporting() || song->hasPendingExportCleanup() || engine->hasStoredAudioDevice() || dynamic_cast<AudioFileDevice*>(engine->audioDev()))
	{
		fail("Native transport is reserved for export");
	}
	const auto recording = recordingState();
	for (auto it = recording.begin(); it != recording.end(); ++it)
	{
		if (it.value().toBool() || it.value().toInt() != 0)
		{
			fail("Playback controls require recording to be disarmed and native takes to be completed");
		}
	}
}

QJsonObject audioBackendGraphResult(const AudioDevice::BackendGraph& graph, bool input, const QJsonObject& arguments)
{
	const auto page = InspectionPage::read(arguments); checkInspectionWork();
	QJsonArray channels, peers; qint64 bytes = 2;
	for (const auto& channel : graph.channels)
	{
		channels.append(QJsonObject{{"index", static_cast<int>(channel.index)}, {"localPort", channel.localPort},
			{"startupPeer", channel.startupPeer}, {"startupPeerTruncated", channel.startupPeerTruncated}});
	}
	for (int index = page.offset; index < static_cast<int>(graph.endpoints.size()) && peers.size() < page.limit; ++index)
	{
		checkInspectionWork(); const auto& endpoint = graph.endpoints[index]; QJsonArray connected;
		for (const auto channel : endpoint.connectedChannels) { connected.append(static_cast<int>(channel)); }
		if (!appendInspectionItem(peers, {{"name", endpoint.name}, {"connectedChannels", connected}}, bytes, 512 * 1024)) { break; }
	}
	auto result = page.metadata(static_cast<int>(graph.endpoints.size()), peers.size());
	result.insert("supported", graph.supported); result.insert("observed", graph.observed);
	result.insert("scope", "master"); result.insert("direction", input ? "input" : "output");
	result.insert("channels", channels); result.insert("ports", peers); result.insert("nativeErrorCode", graph.nativeErrorCode);
	result.insert("connectionVerified", graph.observed); result.insert("deviceReceiptVerified", false);
	result.insert("persistence", "current native master graph; loaded startup settings may reconnect ports after client restart; receipt replay does not change connections");
	return result;
}

QJsonObject inspectAudioBackendGraph(bool input, const QJsonObject& arguments)
{
	const auto device = Engine::audioEngine()->audioDev();
	return audioBackendGraphResult(device ? device->inspectBackendGraph(input) : AudioDevice::BackendGraph{}, input, arguments);
}

QJsonObject inspectTrackAudioBus(AudioBusHandle* bus, const QJsonObject& arguments)
{
	const auto device = Engine::audioEngine()->audioDev();
	const auto state = device ? device->inspectBusGraph(bus) : AudioDevice::BusGraph{};
	auto result = audioBackendGraphResult(state.graph, false, arguments);
	result.insert("scope", "track"); result.insert("track", arguments.value("track"));
	result.insert("container", arguments.value("container").toString("song"));
	result.insert("busId", state.id); result.insert("label", state.label); result.insert("labelTruncated", state.labelTruncated);
	result.insert("enabled", state.enabled); result.insert("published", state.published); result.insert("pending", state.pending);
	result.insert("portErrorCode", state.nativeErrorCode); result.insert("portError", state.error);
	result.insert("signalStage", "after native track gain, pan and effects; before mixer sends, mixer effects and master gain");
	result.insert("persistence", "process-local native track ports; stable names across track rename; client recreation loses connections; project and receipt replay do not restore routing");
	return result;
}

QJsonObject inspectMidiBackendGraph(bool input, const QJsonObject& arguments)
{
	const auto page = InspectionPage::read(arguments); checkInspectionWork();
	const auto client = Engine::audioEngine()->midiClient();
	const auto graph = client ? client->inspectBackendGraph(input) : MidiClient::BackendGraph{};
	QJsonArray peers; qint64 bytes = 2;
	for (int index = page.offset; index < static_cast<int>(graph.endpoints.size()) && peers.size() < page.limit; ++index)
	{
		checkInspectionWork(); const auto& endpoint = graph.endpoints[index];
		if (!appendInspectionItem(peers, {{"name", endpoint.name}, {"connected", endpoint.connected}, {"available", endpoint.available},
			{"reconnect", endpoint.reconnect}, {"pending", endpoint.pending}, {"nativeErrorCode", endpoint.nativeErrorCode}}, bytes, 512 * 1024)) { break; }
	}
	auto result = page.metadata(static_cast<int>(graph.endpoints.size()), peers.size());
	result.insert("supported", graph.supported); result.insert("observed", graph.observed);
	result.insert("reconnectSupported", graph.reconnectSupported); result.insert("reconnectDeferred", graph.reconnectDeferred);
	result.insert("scope", "client"); result.insert("direction", input ? "input" : "output");
	result.insert("localPort", graph.localPort); result.insert("ports", peers); result.insert("nativeErrorCode", graph.nativeErrorCode);
	result.insert("connectionVerified", graph.observed); result.insert("deviceReceiptVerified", false);
	result.insert("persistence", "process-local graph and opt-in reconnect preferences; not project-saved or recreated by receipt replay; exact names are not physical device identities");
	return result;
}

QJsonObject inspectMidi(MidiPort* port)
{
	const auto endpoints = [](const MidiPort::Map& available, const QStringList& requested) {
		QSet<QString> names; for (auto it = available.begin(); it != available.end(); ++it) { names.insert(it.key()); }
		for (const auto& name : requested) { names.insert(name); }
		auto sorted = names.values(); std::sort(sorted.begin(), sorted.end()); QJsonArray result;
		for (const auto& name : sorted)
		{
			result.append(QJsonObject{{"name", name}, {"available", available.contains(name)}, {"selected", requested.contains(name)}, {"connectionVerified", false}});
		}
		return result;
	};
	const auto client = port->client();
	const auto delivery = client ? client->inputDeliveryStatus(port) : MidiClient::InputDeliveryStatus{};
	const auto backend = client ? client->inputBackendStatus() : MidiClient::InputBackendStatus{};
	const auto clock = client ? client->outputClockStatus() : MidiClient::OutputClockStatus{};
	const bool queued = client && client->hasQueuedInputDelivery();
	const bool backendFault = delivery.requiresBackendObservation;
	const bool counterSpace = !delivery.failed || (delivery.failureVersion != std::numeric_limits<std::uint64_t>::max() && delivery.recoveryCount != std::numeric_limits<std::uint64_t>::max());
	const bool backendAllowsReset = (!backend.observed || backend.active) && (!backendFault || backend.observed);
	const bool eligible = queued && delivery.registered && port->isInputEnabled() && counterSpace && backendAllowsReset && !delivery.processing && !delivery.recovering;
	return {{"name", port->displayName()}, {"mode", static_cast<int>(port->mode())},
		{"outputSource", port->outputSource() == MidiPort::OutputSource::Track ? "track" : "instrument"},
		{"trackedOutputNotes", static_cast<int>(port->trackedOutputNotes())}, {"outputNoteLimit", static_cast<int>(MidiPort::OutputNoteLimit)},
		{"outputTiming", "native backend timing; sample offsets are reduced to whole MIDI ticks; device receipt is not acknowledged"},
		{"outputBackend", inspectMidiOutputBackend(client)},
		{"outputClock", QJsonObject{{"observed", clock.observed}, {"configurationVerified", clock.configurationVerified},
			{"ticksPerQuarterNote", clock.ticksPerQuarterNote}, {"microsecondsPerQuarterNote", static_cast<int>(clock.microsecondsPerQuarterNote)},
			{"nativeErrorCode", clock.nativeErrorCode}, {"scope", "last verified native queue configuration; no device latency measurement"}}},
		{"readable", port->isReadable()}, {"writable", port->isWritable()},
		{"inputChannel", port->inputChannel()}, {"outputChannel", port->outputChannel()},
		{"inputController", port->inputController()}, {"outputController", port->outputController()},
		{"fixedInputVelocity", port->fixedInputVelocity()}, {"fixedOutputVelocity", port->fixedOutputVelocity()},
		{"fixedOutputNote", port->fixedOutputNote()}, {"outputProgram", port->outputProgram()}, {"baseVelocity", port->baseVelocity()},
		{"rawBackend", !client || client->isRaw()}, {"namedPortSubscriptions", client && !client->isRaw()},
		{"inputDelivery", QJsonObject{{"queuedDelivery", queued}, {"registered", delivery.registered}, {"pendingEvents", static_cast<int>(delivery.pendingEvents)},
			{"queueLimit", static_cast<int>(MidiClient::InputQueueLimit)}, {"failed", delivery.failed},
			{"failureCode", static_cast<int>(delivery.failure)},
			{"lastFailureCode", static_cast<int>(delivery.lastFailure)},
			{"failureVersion", QString::number(static_cast<qulonglong>(delivery.failureVersion))},
			{"recoveryCount", QString::number(static_cast<qulonglong>(delivery.recoveryCount))},
			{"processing", delivery.processing}, {"recovering", delivery.recovering},
			{"requiresBackendObservation", delivery.requiresBackendObservation},
			{"retirementCleanupPending", delivery.retirementCleanupPending},
			{"backend", inspectMidiInputBackend(client)},
			{"droppedEvents", QString::number(static_cast<qulonglong>(delivery.droppedEvents))}, {"resetEligible", eligible},
			{"restartRequired", delivery.failed && (!queued || !delivery.registered || !counterSpace || (backend.observed && !backend.active))}}},
		{"readablePorts", endpoints(port->readablePorts(), port->requestedReadablePorts())},
		{"writablePorts", endpoints(port->writablePorts(), port->requestedWritablePorts())},
		{"connectionVerified", false}};
}

QJsonObject dispatch(const QString& op, const QJsonObject& a)
{
	auto song = Engine::getSong();
	if (op == "studio.capabilities") { return {{"capabilities", StudioCapabilities::schema()}}; }
	if (op == "project.inspect") { return inspectProjectSection(a); }
	if (op == "midi.inspect") { return inspectMidi(midiPort(a)); }
	if (op == "midi.input.reset")
	{
		requirePlaybackTransport();
		if (song->isPlaying() || song->isPaused() || !song->isStopped()) { fail("Native MIDI input recovery requires stopped transport"); }
		const QPointer<MidiPort> port(midiPort(a)); QString error; bool changed = false;
		if (!port->resetInputDelivery(error, changed)) { fail(error); }
		if (!port) { fail("Native MIDI input port retired during recovery"); }
		auto result = inspectMidi(port); result.insert("recoveryChanged", changed); return result;
	}
	if (op == "midi.configure")
	{
		auto port = midiPort(a); QJsonObject values;
		if (a.contains("parameter"))
		{
			for (auto it = a.begin(); it != a.end(); ++it)
			{
				if (it.key() != "parameter" && it.key() != "readable" && it.key() != "inputChannel" && it.key() != "inputController")
				{
					fail("MIDI controller configuration supports readable, inputChannel and inputController only");
				}
			}
		}
		const auto checked = [&](const char* key, int low, int high) { if (a.contains(key)) { values.insert(key, integer(a, key, low, high)); } };
		checked("inputChannel", 0, MidiChannelCount); checked("outputChannel", 0, MidiChannelCount);
		checked("inputController", MidiController::NONE, MidiControllerCount - 1); checked("outputController", MidiController::NONE, MidiControllerCount - 1);
		checked("fixedInputVelocity", -1, MidiMaxVelocity); checked("fixedOutputVelocity", -1, MidiMaxVelocity);
		checked("fixedOutputNote", -1, MidiMaxKey); checked("outputProgram", 1, MidiProgramCount); checked("baseVelocity", 1, MidiMaxVelocity);
		if (a.contains("readable")) { values.insert("readable", boolean(a, "readable")); }
		if (a.contains("writable")) { values.insert("writable", boolean(a, "writable")); }
		if (a.contains("outputSource"))
		{
			const auto source = string(a, "outputSource");
			if (source != "track" && source != "instrument") { fail("outputSource must be track or instrument"); }
			const auto instrument = instrumentTrack(a)->instrument();
			if (source == "instrument" && (!instrument || !instrument->supportsMidiOutput()))
			{
				fail("The current instrument does not produce native MIDI output");
			}
			values.insert("outputSource", source);
		}
		if (values.contains("outputSource"))
		{
			port->setOutputSource(values.value("outputSource") == "track" ? MidiPort::OutputSource::Track : MidiPort::OutputSource::Instrument);
		}
		if (values.contains("inputChannel")) { port->setInputChannel(values.value("inputChannel").toInt()); }
		if (values.contains("outputChannel")) { port->setOutputChannel(values.value("outputChannel").toInt()); }
		if (values.contains("inputController")) { port->setInputController(values.value("inputController").toInt()); }
		if (values.contains("outputController")) { port->setOutputController(values.value("outputController").toInt()); }
		if (values.contains("fixedInputVelocity")) { port->setFixedInputVelocity(values.value("fixedInputVelocity").toInt()); }
		if (values.contains("fixedOutputVelocity")) { port->setFixedOutputVelocity(values.value("fixedOutputVelocity").toInt()); }
		if (values.contains("fixedOutputNote")) { port->setFixedOutputNote(values.value("fixedOutputNote").toInt()); }
		if (values.contains("baseVelocity")) { port->setBaseVelocity(values.value("baseVelocity").toInt()); }
		if (values.contains("readable")) { port->setReadable(values.value("readable").toBool()); }
		if (values.contains("writable")) { port->setWritable(values.value("writable").toBool()); }
		if (values.contains("outputProgram")) { port->setOutputProgram(values.value("outputProgram").toInt()); }
		if (const auto midi = dynamic_cast<MidiController*>(port->parent())) { midi->updateName(); }
		return inspectMidi(port);
	}
	if (op == "midi.subscribe")
	{
		auto port = midiPort(a); const auto direction = string(a, "direction"); const auto name = string(a, "port");
		const bool enabled = boolean(a, "enabled"); if (direction != "input" && direction != "output") { fail("direction must be input or output"); }
		if (a.contains("parameter") && direction != "input") { fail("Native MIDI controllers support input subscriptions only"); }
		const auto& available = direction == "input" ? port->readablePorts() : port->writablePorts();
		const auto requested = direction == "input" ? port->requestedReadablePorts() : port->requestedWritablePorts();
		if (enabled && (!port->client() || port->client()->isRaw())) { fail("The current raw MIDI backend has no named port subscriptions"); }
		if ((enabled && !available.contains(name)) || (!enabled && !available.contains(name) && !requested.contains(name))) { fail("Native MIDI port is unavailable or unknown"); }
		if (direction == "input") { port->subscribeReadablePort(name, enabled); } else { port->subscribeWritablePort(name, enabled); }
		return inspectMidi(port);
	}
	if (op == "transport.inspect") { return inspectTransport(); }
	if (op == "audio.processing.inspect") { return inspectAudioProcessing(); }
	if (op == "audio.processing.reset")
	{
		requirePlaybackTransport();
		if (!song->isStopped() || !Engine::audioEngine()->resetDspFailure())
		{
			fail("Native DSP retry requires stopped/disarmed transport, completed takes, no export reservation and an available failure counter");
		}
		return inspectAudioProcessing();
	}
	if (op == "audio.backend.list")
	{
		return {{"audioBackends", QJsonArray::fromStringList(AudioEngine::availableAudioDeviceNames())},
			{"midiBackends", QJsonArray::fromStringList(AudioEngine::availableMidiClientNames())},
			{"configuredAudioBackend", ConfigManager::inst()->value("audioengine", "audiodev")},
			{"configuredMidiBackend", ConfigManager::inst()->value("audioengine", "mididev")},
			{"current", inspectTransport()}};
	}
	if (op == "transport.play")
	{
		requirePlaybackTransport();
		if (Engine::audioEngine()->dspStatus().failure != AudioEngine::DspFailure::None)
		{
			fail("Native DSP has a latched failure; stop playback, repair the project and explicitly reset audio processing before playing");
		}
		const auto mode = transportMode(a, Song::PlayMode::Song);
		if (!Engine::audioEngine()->audioDev() || !Engine::audioEngine()->audioDev()->isRunning())
		{
			fail("Native audio backend is not running");
		}
		if (mode == Song::PlayMode::MidiClip)
		{
			if (a.contains("pattern")) { fail("pattern applies to pattern playback"); }
			const auto midi = dynamic_cast<MidiClip*>(clip(a));
			if (!midi) { fail("Selected clip is not a native MIDI clip"); }
			const auto begin = -static_cast<qint64>(midi->startTimeOffset());
			const auto end = static_cast<qint64>(midi->length()) - midi->startTimeOffset();
			if (begin < -MaxSongLength || begin > MaxSongLength || end > MaxSongLength || end <= begin)
			{
				fail("MIDI clip playback window exceeds the native song tick range");
			}
			song->playMidiClip(midi, a.contains("loop") ? boolean(a, "loop") : true);
		}
		else
		{
			if (a.contains("container") || a.contains("track") || a.contains("clip") || a.contains("loop"))
			{
				fail("container, track, clip and loop apply to MIDI clip playback");
			}
			if (mode == Song::PlayMode::Pattern)
			{
				const auto store = Engine::patternStore(); const int count = store->numOfPatterns();
				if (count == 0) { fail("Project has no native patterns"); }
				const int selected = a.contains("pattern") ? integer(a, "pattern", 0, count - 1) : store->currentPattern();
				if (selected < 0 || selected >= count) { fail("Native pattern selection is invalid"); }
				if (!song->isStopped()) { song->stop(); }
				store->setCurrentPattern(selected); song->playPattern();
			}
			else
			{
				if (a.contains("pattern")) { fail("pattern applies to pattern playback"); }
				song->playSong();
			}
		}
		return inspectTransport();
	}
	if (op == "transport.pause")
	{
		requirePlaybackTransport(); const bool paused = boolean(a, "paused");
		if (song->isStopped()) { fail("Native transport is stopped; play before pausing or resuming"); }
		if (!paused && (!Engine::audioEngine()->audioDev() || !Engine::audioEngine()->audioDev()->isRunning()))
		{
			fail("Native audio backend is not running");
		}
		if (!paused && Engine::audioEngine()->dspStatus().failure != AudioEngine::DspFailure::None)
		{
			fail("Native DSP has a latched failure; stop playback and explicitly reset audio processing before resuming");
		}
		if (song->isPaused() != paused) { song->togglePause(); }
		else if (paused) { song->releaseMidiOutput(); }
		if (paused && !song->midiOutputCleanupError().isEmpty()) { fail("Native playback paused, but MIDI output cleanup failed: " + song->midiOutputCleanupError()); }
		return inspectTransport();
	}
	if (op == "transport.stop")
	{
		requirePlaybackTransport(); song->stop();
		if (!song->midiOutputCleanupError().isEmpty()) { fail("Native playback stopped, but MIDI output cleanup failed: " + song->midiOutputCleanupError()); }
		return inspectTransport();
	}
	if (op == "transport.seek")
	{
		requirePlaybackTransport();
		const auto mode = transportMode(a, song->isStopped() ? Song::PlayMode::Song : song->playMode());
		const int position = integer(a, "position", 0, MaxSongLength);
		if (!song->isStopped() && mode != song->playMode()) { fail("Seek playMode must match the active native transport"); }
		song->setPlayPos(position, mode); return inspectTransport();
	}
	if (op == "project.new")
	{
		const QPointer<Song> owner(song);
		if (a.contains("empty") && boolean(a, "empty")) { song->createEmptyProject(); }
		else { song->createNewProject(); }
		if (!owner || Engine::getSong() != owner.data()) { fail("Native project creation owner retired"); }
		if (owner->hasErrors()) { fail("Native project was not created completely: " + owner->errorSummary()); }
		return {};
	}
	if (op == "project.load")
	{
		const QPointer<Song> owner(song);
		const auto path = string(a, "path");
		DataFile file(path);
		if (!owner || Engine::getSong() != owner.data()) { fail("Native project load owner retired during input validation"); }
		if (file.type() != DataFile::Type::SongProject && file.type() != DataFile::Type::SongProjectTemplate)
		{
			fail("Path is not a valid LMMS project or template");
		}
		if (!song->loadProject(path)) { fail("Native project load was rejected"); }
		if (!owner || Engine::getSong() != owner.data()) { fail("Native project load owner retired during restoration"); }
		if (song->hasFatalErrors()) { fail("Native project did not load completely: " + song->errorSummary()); }
		return a.contains("summary") && boolean(a, "summary") ? StudioCapabilities::inspectSummary() : StudioCapabilities::inspect();
	}
	if (op == "project.save")
	{
		const auto path = string(a, "path");
		const bool resources = a.contains("withResources") && boolean(a, "withResources");
		DataFile naming(DataFile::Type::SongProject);
		const auto info = QFileInfo(path);
		const auto actual = resources ? naming.nameWithExtension(info.path() + "/"
			+ info.fileName().section('.', 0, 0) + "/" + info.fileName()) : naming.nameWithExtension(path);
		const auto committedPath = QFileInfo(actual).absoluteFilePath();
		if (!song->saveProjectFile(path, resources)) { fail("Native project save failed"); }
		return {{"committed", true}, {"path", committedPath}};
	}
	if (op == "project.import")
	{
		auto destination = container(a);
		int pattern = -1;
		if (destination == Engine::patternStore())
		{
			const int count = Engine::patternStore()->numOfPatterns();
			pattern = a.contains("pattern") ? integer(a, "pattern", 0, std::max(1, count) - 1)
				: (count ? Engine::patternStore()->currentPattern() : 0);
		}
		else if (a.contains("pattern")) { fail("pattern requires container=pattern for import"); }
		if (!ImportFilter::import(string(a, "path"), destination, pattern)) { fail("Native import failed"); }
		return {};
	}
	if (op == "project.exportMidi")
	{
		const auto path = QFileInfo(string(a, "path")).absoluteFilePath();
#ifdef Q_OS_WIN
		const std::filesystem::path nativePath(path.toStdWString());
#else
		const std::filesystem::path nativePath(path.toStdString());
#endif
		if (!song->exportProjectMidi(nativePath)) { fail("Native MIDI export failed"); }
		return {{"path", path}};
	}
	if (op == "song.set")
	{
		const auto tempo = a.contains("tempo") ? integer(a, "tempo", MinTempo, MaxTempo) : song->getTempo();
		const auto volume = a.contains("masterVolume") ? integer(a, "masterVolume", 0, 200) : song->masterVolume();
		const auto pitch = a.contains("masterPitch") ? integer(a, "masterPitch", -12, 12) : song->masterPitch();
		if (a.contains("tempo")) { song->setTempo(tempo); }
		if (a.contains("masterVolume")) { song->setMasterVolume(volume); }
		if (a.contains("masterPitch")) { song->setMasterPitch(pitch); }
		return {{"tempo", song->getTempo()}, {"masterVolume", song->masterVolume()}, {"masterPitch", song->masterPitch()}};
	}
	if (op == "timeline.set")
	{
		const int begin = integer(a, "begin", 0, MaxSongLength);
		const int end = integer(a, "end", begin + 1, MaxSongLength);
		const bool enabled = boolean(a, "enabled");
		auto& timeline = song->getTimeline(Song::PlayMode::Song);
		timeline.setLoopPoints(TimePos(begin), TimePos(end));
		timeline.setLoopEnabled(enabled);
		return {{"begin", begin}, {"end", end}, {"enabled", enabled}};
	}
	if (op == "track.add")
	{
		const auto type = string(a, "type");
		Track::Type nativeType;
		if (type == "instrument") { nativeType = Track::Type::Instrument; }
		else if (type == "sample") { nativeType = Track::Type::Sample; }
		else if (type == "automation") { nativeType = Track::Type::Automation; }
		else if (type == "pattern" && container(a) == song) { nativeType = Track::Type::Pattern; }
		else { fail("Unsupported native track type or container"); }
		auto tc = container(a);
		const int index = static_cast<int>(tc->tracks().size());
		auto t = Track::create(nativeType, tc);
		if (!t) { fail("Native track creation failed"); }
		if (a.contains("name")) { t->setName(string(a, "name")); }
		return {{"track", index}};
	}
	if (op == "track.edit")
	{
		const QPointer<Track> t(track(a)); const QPointer<TrackContainer> owner(t->trackContainer());
		const auto check = [&] {
			if (!t || !owner || t->trackContainer() != owner.data()
				|| std::find(owner->tracks().begin(), owner->tracks().end(), t.data()) == owner->tracks().end())
			{ fail("Native track retired or detached during editing"); }
		};
		check();
		const auto name = a.contains("name") ? string(a, "name") : t->name();
		const bool muted = a.contains("muted") ? boolean(a, "muted") : t->isMuted();
		const bool solo = a.contains("solo") ? boolean(a, "solo") : t->isSolo();
		QJsonObject result;
		if (a.contains("to"))
		{
			const int destination = integer(a, "to", 0, static_cast<int>(owner->tracks().size()) - 1);
			const int from = static_cast<int>(std::find(owner->tracks().begin(), owner->tracks().end(), t.data()) - owner->tracks().begin());
			if (!owner->moveTrack(t.data(), destination)) { fail("Native track move rejected; container or pattern layout is inconsistent"); }
			check();
			result = {{"from", from}, {"track", destination}, {"changed", from != destination}};
		}
		if (a.contains("name")) { t->setName(name); check(); }
		if (a.contains("muted")) { t->setMuted(muted); check(); }
		if (a.contains("solo")) { t->setSolo(solo); check(); }
		if (a.contains("to"))
		{
			const int current = static_cast<int>(std::find(owner->tracks().begin(), owner->tracks().end(), t.data()) - owner->tracks().begin());
			result.insert("track", current); result.insert("changed", result.value("from").toInt() != current);
			if (const auto pattern = dynamic_cast<PatternTrack*>(t.data())) { result.insert("patternIndex", pattern->patternIndex()); }
		}
		result.insert("name", t->name()); result.insert("muted", t->isMuted()); result.insert("solo", t->isSolo());
		return result;
	}
	if (op == "track.remove")
	{
		const QPointer<Track> target(track(a)); const QPointer<TrackContainer> owner(target->trackContainer());
		const auto admit = [&] {
			if (!target || !owner || target->trackContainer() != owner.data()
				|| std::find(owner->tracks().begin(), owner->tracks().end(), target.data()) == owner->tracks().end())
			{ fail("Native track retired or detached before removal"); }
			target->requireIdleForRemoval();
		};
		admit(); checkInspectionWork(); admit();
		// Do recoverable solo notifications while the complete object is alive,
		// before entering native destructors whose exception contract is noexcept.
		if (target->isSolo()) { target->setSolo(false); admit(); }
		if (target->isSolo()) { fail("Native solo remained active before track removal"); }
		delete target.data(); return {};
	}
	if (op == "track.clone")
	{
		auto source = track(a); const QPointer<TrackContainer> owner(source->trackContainer());
		const QPointer<Track> copy(source->clone());
		if (!copy || !owner || copy->trackContainer() != owner.data()) { fail("Native track cloning failed"); }
		const auto found = std::find(owner->tracks().begin(), owner->tracks().end(), copy.data());
		if (found == owner->tracks().end()) { fail("Native cloned track detached before completion"); }
		return {{"track", static_cast<int>(found - owner->tracks().begin())}};
	}
	if (op == "clip.add")
	{
		auto t = track(a);
		if (t->trackContainer() == Engine::patternStore())
		{
			fail("Pattern clips are owned by PatternStore; add a song pattern track to create a pattern");
		}
		const int position = integer(a, "position", 0, MaxSongLength);
		const int index = t->numOfClips();
		auto c = t->createClip(TimePos(position));
		if (!c) { fail("Native clip creation failed"); }
		if (a.contains("name")) { c->setName(string(a, "name")); }
		return {{"clip", index}};
	}
	if (op == "clip.edit")
	{
		Clip::EditOptions options;
		if (a.contains("position")) { options.position = integer(a, "position", 0, MaxSongLength); }
		if (a.contains("length")) { options.length = integer(a, "length", 1, MaxSongLength); }
		if (a.contains("offset")) { options.offset = integer(a, "offset", -MaxSongLength, MaxSongLength); }
		if (a.contains("autoResize")) { options.autoResize = boolean(a, "autoResize"); }
		if (a.contains("name")) { options.name = string(a, "name"); }
		if (a.contains("muted")) { options.muted = boolean(a, "muted"); }
		const QPointer<Clip> c(clip(a)); bool changed = false; QString error; if (!c->edit(options, changed, error)) { fail(error); }
		if (!c) { fail("Native edited clip retired before receipt construction"); }
		return {{"position", static_cast<int>(c->startPosition())}, {"length", static_cast<int>(c->length())},
			{"offset", static_cast<int>(c->startTimeOffset())}, {"autoResize", c->getAutoResize()}, {"changed", changed}};
	}
	if (op == "clip.split")
	{
		auto c = clip(a);
		const auto position = integer(a, "position", 1, c->length().getTicks() - 1);
		const int index = c->getTrack()->numOfClips();
		if (!c->splitAt(TimePos(position))) { fail("Native clip split rejected; PatternStore clips cannot be split"); }
		return {{"clip", index}, {"position", static_cast<int>(c->startPosition()) + position}};
	}
	if (op == "clip.remove")
	{
		auto c = clip(a);
		if (c->isInPattern()) { fail("Remove the owning song pattern track instead"); }
		delete c;
		return {};
	}
	if (op == "clip.clone")
	{
		auto c = clip(a);
		if (a.contains("targetContainer") && !a.contains("targetTrack")) { fail("targetContainer requires targetTrack"); }
		QJsonObject target{{"container", a.contains("targetContainer") ? string(a, "targetContainer")
			: a.contains("container") ? string(a, "container") : QString("song")},
			{"track", a.contains("targetTrack") ? a.value("targetTrack") : a.value("track")}};
		auto destination = track(target);
		const bool patternCell = destination->trackContainer() == Engine::patternStore();
		Clip* replacement = nullptr;
		int index = destination->numOfClips();
		int position = 0;
		if (patternCell)
		{
			if (!a.contains("targetClip") || !a.contains("replace") || !boolean(a, "replace"))
			{
				fail("PatternStore copies require targetClip and replace:true for an existing cell");
			}
			if (a.contains("position")) { fail("PatternStore owns clip positions; omit position"); }
			index = integer(a, "targetClip", 0, destination->numOfClips() - 1);
			replacement = destination->getClips().at(index);
			position = TimePos(index, 0).getTicks();
		}
		else
		{
			if (a.contains("targetClip") || a.contains("replace")) { fail("Arrangement copies append; omit targetClip and replace"); }
			position = integer(a, "position", 0, MaxSongLength);
		}
		if (c->length().getTicks() < 0 || position > MaxSongLength - c->length().getTicks())
		{
			fail("Copied clip end exceeds maximum song length");
		}
		auto copy = c->copyTo(destination, replacement);
		if (!patternCell) { copy->movePosition(TimePos(position)); }
		return {{"container", target.value("container")}, {"track", target.value("track")},
			{"clip", index}, {"position", position}, {"replaced", patternCell}};
	}
	if (op == "clip.selection.copy")
	{
		const auto items = a.value("items").toArray();
		if (items.isEmpty() || items.size() > 4096) { fail("items must contain between 1 and 4096 source/destination objects"); }
		const QSet<QString> fields{"container", "track", "clip", "targetContainer", "targetTrack", "position", "targetClip", "replace"};
		QList<Clip*> sources; QList<NativeClipTransfer::Target> targets; QJsonArray receipts;
		for (const auto& value : items)
		{
			checkInspectionWork(); if (!value.isObject()) { fail("Every selection item must be an object"); } const auto item = value.toObject();
			for (auto it = item.begin(); it != item.end(); ++it) { if (!fields.contains(it.key())) { fail("Unknown selection item argument: " + it.key()); } }
			auto source = clip(item); if (!source) { fail("Selection source clip is missing"); }
			if (item.contains("targetContainer") && !item.contains("targetTrack")) { fail("targetContainer requires targetTrack in each selection item"); }
			QJsonObject target{{"container", item.contains("targetContainer") ? string(item, "targetContainer")
				: item.contains("container") ? string(item, "container") : QString("song")}, {"track", item.contains("targetTrack") ? item.value("targetTrack") : item.value("track")}};
			auto destination = track(target); const bool pattern = destination->trackContainer() == Engine::patternStore(); Clip* replacement = nullptr; int position;
			if (pattern)
			{
				if (!item.contains("targetClip") || !item.contains("replace") || !boolean(item, "replace") || item.contains("position"))
				{ fail("Pattern selection destinations require targetClip and replace:true, and omit position"); }
				const int index = integer(item, "targetClip", 0, destination->numOfClips() - 1); replacement = destination->getClips().at(index); position = TimePos(index, 0).getTicks();
				if (!replacement) { fail("Selection destination pattern cell is missing"); }
			}
			else
			{
				if (item.contains("targetClip") || item.contains("replace")) { fail("Arrangement selection destinations append; omit targetClip and replace"); }
				position = integer(item, "position", 0, MaxSongLength);
			}
			if (source->length().getTicks() < 0 || position > MaxSongLength - source->length().getTicks()) { fail("Copied selection clip exceeds maximum song length"); }
			sources.append(source); targets.append({destination, TimePos(position), replacement});
			receipts.append(QJsonObject{{"container", target.value("container")}, {"track", target.value("track")}, {"position", position}, {"replaced", pattern}});
		}
		QList<Clip*> copies; QString error;
		if (!NativeClipTransfer::copy(sources, targets, copies, error)) { fail("Native selection copy failed: " + error); }
		for (int index = 0; index < copies.size(); ++index)
		{
			const auto& owned = targets.at(index).track->getClips(); auto receipt = receipts.at(index).toObject();
			receipt.insert("clip", static_cast<int>(std::distance(owned.begin(), std::find(owned.begin(), owned.end(), copies.at(index))))); receipts[index] = receipt;
		}
		return {{"clips", receipts}, {"copied", static_cast<int>(copies.size())}};
	}
	if (op == "sample.load")
	{
		const QPointer<SampleClip> c(sampleClip(a)); const QPointer<Track> track(c->getTrack()); const QPointer<Song> owner(song);
		const auto ready = [&] { if (!c || !track || !owner || Engine::getSong() != owner.data() || c->getTrack() != track.data() || clip(a) != c.data()) { fail("Native sample clip retired or changed during loading"); } };
		if (!a.value("path").isString()) { fail("Sample path must be a string; an empty path clears the source"); }
		const QString requested = a.value("path").toString(); const auto path = QFileInfo(requested);
		const bool preserve = a.contains("preserveSettings") ? boolean(a, "preserveSettings") : c->sampleUnresolved();
		if (!requested.isEmpty() && (!path.isFile() || !path.isReadable())) { fail("Sample path is not a readable file"); }
		QString error; const bool loaded = c->loadSampleFile(requested.isEmpty() ? QString{} : path.absoluteFilePath(), error, preserve); ready();
		if (!loaded) { fail(error); } const auto result = inspectSample(c.data()); ready(); return result;
	}
	if (op == "sample.edit")
	{
		auto c = sampleClip(a);
		auto& sample = c->sample();
		if (sample.sampleSize() == 0 || sample.sampleSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			fail("Load a usable sample before editing its playback settings");
		}
		const auto size = static_cast<int>(sample.sampleSize());
		const bool reverse = a.contains("reversed") && boolean(a, "reversed") != sample.reversed();
		const auto start = a.contains("startFrame") ? integer(a, "startFrame", 0, size - 1)
			: (reverse ? size - sample.endFrame() : sample.startFrame());
		const auto end = a.contains("endFrame") ? integer(a, "endFrame", 1, size)
			: (reverse ? size - sample.startFrame() : sample.endFrame());
		if (start >= end) { fail("Sample startFrame must precede endFrame"); }
		const auto gain = a.contains("gain") ? number(a, "gain", -std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()) : sample.amplification();
		if (reverse) { c->reverseSample(); }
		if (a.contains("startFrame") || a.contains("endFrame"))
		{
			if (!c->setSampleWindow(start, end)) { fail("Native sample source window was rejected"); }
		}
		sample.setAmplification(static_cast<float>(gain));
		emit c->sampleChanged();
		emit c->dataChanged();
		return inspectSample(c);
	}
	if (op == "note.selection.split" || op == "note.selection.knife")
	{
		const bool knife = op == "note.selection.knife", keepLongest = knife && a.contains("keepLongest") && boolean(a, "keepLongest");
		const auto indices = a.value("notes").toArray(); if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> owner(midiClip(a)); const int position = knife ? 0 : integer(a, "position", 0, MaxSongLength);
		const auto endpoint = [&](const char* key) { return knife ? integer(a, key, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) : 0; };
		const int fromPosition = endpoint("fromPosition"), fromKey = endpoint("fromKey"), toPosition = endpoint("toPosition"), toKey = endpoint("toKey");
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - indices.size())) { fail("Native split roster exceeds the receipt index range"); }
		NoteVector selected; std::vector<int> sourceIndices; std::vector<std::shared_ptr<const void>> identities; QSet<int> seen;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!owner) { fail("Native split clip retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(owner->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = owner->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); sourceIndices.push_back(index); identities.push_back(note->selectionIdentity());
		}
		checkInspectionWork(); if (!owner) { fail("Native split clip retired during admission"); }
		std::vector<int> starts, lengths;
		for (std::size_t index = 0; index < selected.size(); ++index)
		{
			if (static_cast<std::size_t>(sourceIndices[index]) >= owner->notes().size() || owner->notes().at(sourceIndices[index]) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native split selection changed during admission"); }
			starts.push_back(selected[index]->pos().getTicks()); lengths.push_back(selected[index]->length().getTicks());
		}
		NoteVector rights; std::vector<int> cuts; QString error;
		if (knife)
		{
			MidiClip::KnifeResult result; if (!owner->splitNotesAlongLine(selected, fromPosition, fromKey, toPosition, toKey, keepLongest, result, error)) { fail(error); }
			rights = std::move(result.rights); cuts = std::move(result.positions);
		}
		else if (!owner->splitNotes(selected, position, rights, error)) { fail(error); }
		if (!owner || rights.size() != selected.size() || (knife && cuts.size() != selected.size())) { fail("Native split result roster changed before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts; int count = 0;
		for (std::size_t index = 0; index < selected.size(); ++index)
		{
			if (!current.contains(selected[index]) || selected[index]->selectionIdentity() != identities[index]) { fail("Native split source changed before receipt construction"); }
			const bool cut = knife ? cuts[index] >= 0 : rights[index] != nullptr;
			QJsonObject receipt{{"sourceNote", sourceIndices[index]}, {"note", current.value(selected[index])}, {"position", selected[index]->pos().getTicks()}, {"length", selected[index]->length().getTicks()}, {"key", selected[index]->key()}, {knife ? "cut" : "split", cut}};
			if (cut)
			{
				++count;
				if (knife)
				{
					const qint64 left = static_cast<qint64>(cuts[index]) - starts[index], right = static_cast<qint64>(lengths[index]) - left;
					receipt.insert("cutPosition", cuts[index]); receipt.insert("kept", QString(!keepLongest ? "both" : right > left ? "right" : "left"));
				}
			}
			if (rights[index])
			{
				if (!current.contains(rights[index])) { fail("Native split fragment retired before receipt construction"); }
				receipt.insert("rightNote", current.value(rights[index])); receipt.insert("rightPosition", rights[index]->pos().getTicks()); receipt.insert("rightLength", rights[index]->length().getTicks());
			}
			receipts.append(receipt);
		}
		if (knife) { return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"fromPosition", fromPosition}, {"fromKey", fromKey}, {"toPosition", toPosition}, {"toKey", toKey}, {"keepLongest", keepLongest}, {"cut", count}, {"changed", count != 0}, {"notes", receipts}}; }
		return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"position", position}, {"split", count}, {"changed", count != 0}, {"notes", receipts}};
	}
	if (op == "note.selection.glue")
	{
		const auto indices = a.value("notes").toArray(); if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> owner(midiClip(a));
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native note roster exceeds the receipt index range"); }
		NoteVector selected; std::vector<int> sourceIndices; std::vector<std::shared_ptr<const void>> identities; QSet<int> seen; QHash<const Note*, int> original;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!owner) { fail("Native glue clip retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(owner->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = owner->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); sourceIndices.push_back(index); identities.push_back(note->selectionIdentity()); original.insert(note, index);
		}
		checkInspectionWork(); if (!owner) { fail("Native glue clip retired during admission"); }
		for (std::size_t index = 0; index < selected.size(); ++index)
		{ if (static_cast<std::size_t>(sourceIndices[index]) >= owner->notes().size() || owner->notes().at(sourceIndices[index]) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native glue selection changed during admission"); } }
		MidiClip::GlueResult glue; QString error; if (!owner->glueNotes(selected, glue, error)) { fail(error); }
		if (!owner) { fail("Native glue clip retired before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts; QSet<const Note*> survivors;
		for (std::size_t index = 0; index < glue.survivors.size(); ++index)
		{
			auto survivor = glue.survivors[index]; if (!current.contains(survivor) || !original.contains(survivor)) { fail("Native glue survivor changed before receipt construction"); } survivors.insert(survivor);
			receipts.append(QJsonObject{{"sourceNote", sourceIndices[index]}, {"survivingSourceNote", original.value(survivor)}, {"note", current.value(survivor)},
				{"merged", survivor != selected[index]}, {"position", survivor->pos().getTicks()}, {"key", survivor->key()}, {"length", survivor->length().getTicks()}});
		}
		return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"removed", glue.removed},
			{"survivors", static_cast<int>(survivors.size())}, {"changed", glue.removed != 0}, {"notes", receipts}};
	}
	if (op == "note.selection.shift" || op == "note.selection.quantize" || op == "note.selection.reverse" || op == "note.selection.length")
	{
		const bool quantize = op == "note.selection.quantize", reverse = op == "note.selection.reverse";
		const bool lengths = op == "note.selection.length";
		if (!quantize && !reverse && !lengths && !a.contains("semitones") && !a.contains("ticks")) { fail("Native note shift requires semitones or ticks"); }
		const auto indices = a.value("notes").toArray(); if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> owner(midiClip(a));
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native note roster exceeds the receipt index range"); }
		const int semitones = a.contains("semitones") ? integer(a, "semitones", -NumKeys, NumKeys) : 0;
		const int ticks = a.contains("ticks") ? integer(a, "ticks", -MaxSongLength, MaxSongLength) : 0;
		const int grid = quantize ? integer(a, "grid", 1, MaxSongLength) : 1; const auto mode = lengths || (quantize && a.contains("mode")) ? string(a, "mode") : QString("both");
		if (quantize && mode != "both" && mode != "positions" && mode != "lengths") { fail("Native note quantization mode must be both, positions or lengths"); }
		if (lengths && mode != "maximum" && mode != "minimum" && mode != "trim" && mode != "fill") { fail("Native note length mode must be maximum, minimum, trim or fill"); }
		const bool bounded = lengths && (mode == "maximum" || mode == "minimum");
		if (lengths && a.contains("length") != bounded) { fail("length is required only for maximum/minimum note length modes"); }
		const int bound = bounded ? integer(a, "length", -MaxSongLength, MaxSongLength) : 0;
		NoteVector selected; std::vector<int> sourceIndices; std::vector<std::shared_ptr<const void>> identities; QSet<int> seen;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!owner) { fail("Native note edit clip retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(owner->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = owner->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); sourceIndices.push_back(index); identities.push_back(note->selectionIdentity());
		}
		checkInspectionWork(); if (!owner) { fail("Native note edit clip retired during admission"); }
		for (std::size_t index = 0; index < selected.size(); ++index)
		{ if (static_cast<std::size_t>(sourceIndices[index]) >= owner->notes().size() || owner->notes().at(sourceIndices[index]) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native note edit selection changed during admission"); } }
		MidiClip::ShiftResult shift; QString error;
		if (lengths)
		{
			const auto nativeMode = mode == "maximum" ? MidiClip::LengthMode::Maximum : mode == "minimum" ? MidiClip::LengthMode::Minimum : mode == "trim" ? MidiClip::LengthMode::Trim : MidiClip::LengthMode::Fill;
			if (!owner->adjustNoteLengths(selected, nativeMode, bound, shift.changed, error)) { fail(error); }
		}
		else if (quantize)
		{
			const auto nativeMode = mode == "positions" ? MidiClip::QuantizeMode::Positions : mode == "lengths" ? MidiClip::QuantizeMode::Lengths : MidiClip::QuantizeMode::Both;
			if (!owner->quantizeNotes(selected, grid, nativeMode, shift.changed, error)) { fail(error); }
		}
		else if (reverse) { if (!owner->reverseNotes(selected, shift.changed, error)) { fail(error); } }
		else if (!owner->shiftNotes(selected, semitones, ticks, shift, error)) { fail(error); }
		if (!owner) { fail("Native note edit clip retired before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts;
		for (std::size_t index = 0; index < selected.size(); ++index)
		{
			if (!current.contains(selected[index]) || selected[index]->selectionIdentity() != identities[index]) { fail("Edited native note changed before receipt construction"); }
			auto receipt = QJsonObject{{"sourceNote", sourceIndices[index]}, {"note", current.value(selected[index])}, {"position", selected[index]->pos().getTicks()}, {"key", selected[index]->key()}};
			if (quantize || reverse || lengths) { receipt.insert("length", selected[index]->length().getTicks()); } receipts.append(receipt);
		}
		if (lengths)
		{
			QJsonObject result{{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"mode", mode}, {"changed", shift.changed}, {"notes", receipts}};
			if (bounded) { result.insert("length", bound); } return result;
		}
		if (reverse) { return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"changed", shift.changed}, {"notes", receipts}}; }
		if (quantize) { return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"grid", grid}, {"mode", mode}, {"changed", shift.changed}, {"notes", receipts}}; }
		return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")},
			{"semitones", semitones}, {"ticks", ticks}, {"appliedTicks", shift.appliedTicks}, {"changed", shift.changed}, {"notes", receipts}};
	}
	if (op == "note.selection.copy")
	{
		const auto indices = a.value("notes").toArray();
		if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> source(midiClip(a));
		QJsonObject targetArgs{{"container", a.contains("targetContainer") ? string(a, "targetContainer") : a.contains("container") ? string(a, "container") : QString("song")},
			{"track", a.value("targetTrack")}, {"clip", a.value("targetClip")}};
		const QPointer<MidiClip> target(midiClip(targetArgs)); const int position = integer(a, "position", 0, MaxSongLength);
		if (source->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || target->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - indices.size()))
		{ fail("Native note selection exceeds the receipt index range"); }
		QSet<int> seen; NoteVector selected; std::vector<std::shared_ptr<const void>> identities; std::vector<int> sourceIndices;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!source || !target) { fail("Native note selection owner retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(source->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = source->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); identities.push_back(note->selectionIdentity()); sourceIndices.push_back(index);
		}
		checkInspectionWork(); if (!source || !target) { fail("Native note selection owner retired during admission"); } int origin = MaxSongLength;
		for (std::size_t index = 0; index < selected.size(); ++index)
		{
			const auto sourceIndex = static_cast<std::size_t>(sourceIndices[index]);
			if (sourceIndex >= source->notes().size() || source->notes().at(sourceIndex) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native source note selection changed during admission"); }
			origin = std::min(origin, selected[index]->pos().getTicks());
		}
		NoteVector inserted; QString error;
		if (!NativeNoteTransfer::copy(source.data(), selected, TimePos(origin), target.data(), TimePos(position), inserted, error)) { fail(error); }
		if (!target) { fail("Native note destination retired before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < target->notes().size(); ++index) { current.insert(target->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts;
		for (std::size_t index = 0; index < inserted.size(); ++index)
		{
			if (!current.contains(inserted[index])) { fail("Copied native note retired before receipt construction"); }
			receipts.append(QJsonObject{{"sourceNote", sourceIndices[index]}, {"note", current.value(inserted[index])}, {"position", inserted[index]->pos().getTicks()}});
		}
		return {{"container", targetArgs.value("container")}, {"track", targetArgs.value("track")}, {"clip", targetArgs.value("clip")},
			{"position", position}, {"sourceOrigin", origin}, {"copied", static_cast<int>(inserted.size())}, {"notes", receipts}};
	}
	if (op == "note.selection.remove")
	{
		const auto indices = a.value("notes").toArray(); if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> owner(midiClip(a));
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native note roster exceeds the receipt index range"); }
		NoteVector selected; std::vector<int> sourceIndices; std::vector<std::shared_ptr<const void>> identities; QSet<int> seen;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!owner) { fail("Native removal clip retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(owner->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = owner->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); sourceIndices.push_back(index); identities.push_back(note->selectionIdentity());
		}
		checkInspectionWork(); if (!owner) { fail("Native removal clip retired during admission"); }
		for (std::size_t index = 0; index < selected.size(); ++index)
		{ if (static_cast<std::size_t>(sourceIndices[index]) >= owner->notes().size() || owner->notes().at(sourceIndices[index]) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native removal selection changed during admission"); } }
		int removed = 0; QString error; if (!owner->removeNotes(selected, removed, error)) { fail(error); }
		QJsonArray receipts; for (const int index : sourceIndices) { receipts.append(QJsonObject{{"sourceNote", index}}); }
		return {{"removed", removed}, {"notes", receipts}};
	}
	if (op == "note.selection.edit" || op == "note.edit")
	{
		const auto edit = noteEdit(a); const bool batch = op == "note.selection.edit";
		const auto indices = batch ? a.value("notes").toArray() : QJsonArray{a.value("note")};
		if (indices.isEmpty() || indices.size() > 4096) { fail("notes must contain 1..4096 distinct source note indices"); }
		const QPointer<MidiClip> owner(midiClip(a));
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native note roster exceeds the receipt index range"); }
		NoteVector selected; std::vector<int> sourceIndices; std::vector<std::shared_ptr<const void>> identities; QSet<int> seen;
		for (const auto& value : indices)
		{
			checkInspectionWork(); if (!owner) { fail("Native property edit clip retired during admission"); }
			const int index = integer(QJsonObject{{"index", value}}, "index", 0, static_cast<int>(owner->notes().size()) - 1);
			if (seen.contains(index)) { fail("Source note indices must be distinct"); } seen.insert(index); auto note = owner->notes().at(index);
			if (!note) { fail("Native source note is missing"); } selected.push_back(note); sourceIndices.push_back(index); identities.push_back(note->selectionIdentity());
		}
		checkInspectionWork(); if (!owner) { fail("Native property edit clip retired during admission"); }
		for (std::size_t index = 0; index < selected.size(); ++index)
		{ if (static_cast<std::size_t>(sourceIndices[index]) >= owner->notes().size() || owner->notes().at(sourceIndices[index]) != selected[index] || selected[index]->selectionIdentity() != identities[index]) { fail("Native property edit selection changed during admission"); } }
		bool changed = false; QString error; if (!owner->editNotes(selected, edit, changed, error)) { fail(error); }
		if (!owner) { fail("Native property edit clip retired before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts;
		for (std::size_t index = 0; index < selected.size(); ++index)
		{
			auto note = selected[index]; if (!current.contains(note) || note->selectionIdentity() != identities[index]) { fail("Native property edit note changed before receipt construction"); }
			receipts.append(QJsonObject{{"sourceNote", sourceIndices[index]}, {"note", current.value(note)}, {"position", note->pos().getTicks()}, {"length", note->length().getTicks()}, {"key", note->key()}, {"volume", note->volume()}, {"panning", note->panning()}});
		}
		if (!batch) { auto receipt = receipts.at(0).toObject(); receipt.insert("changed", changed); return receipt; }
		return {{"changed", changed}, {"notes", receipts}};
	}
	if (op == "note.add" || op == "note.batch.add")
	{
		const bool batch = op == "note.batch.add"; QJsonArray items;
		if (batch) { items = a.value("items").toArray(); }
		else { auto item = a; item.remove("track"); item.remove("clip"); item.remove("container"); items.append(item); }
		if (items.isEmpty() || items.size() > 4096) { fail("items must contain 1..4096 native note input objects"); }
		const QPointer<MidiClip> owner(midiClip(a)); std::vector<MidiClip::NoteSpec> specs; specs.reserve(items.size());
		for (const auto& item : items) { checkInspectionWork(); if (!owner) { fail("Native insertion clip retired during admission"); } if (!item.isObject()) { fail("Each note input must be an object"); } specs.push_back(noteSpec(item.toObject())); }
		if (!owner) { fail("Native insertion clip retired during admission"); }
		NoteVector inserted; QString error; if (!owner->addNotes(specs, inserted, error)) { fail(error); }
		if (!owner) { fail("Native insertion clip retired before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts;
		for (std::size_t index = 0; index < inserted.size(); ++index)
		{
			auto note = inserted[index]; if (!current.contains(note)) { fail("Inserted native note retired before receipt construction"); }
			receipts.append(QJsonObject{{"input", static_cast<int>(index)}, {"note", current.value(note)}, {"position", note->pos().getTicks()}, {"length", note->length().getTicks()}, {"key", note->key()}, {"volume", note->volume()}, {"panning", note->panning()}});
		}
		if (!batch) { return {{"note", receipts.at(0).toObject().value("note")}}; }
		return {{"added", static_cast<int>(inserted.size())}, {"notes", receipts}};
	}
	if (op == "note.remove")
	{
		auto c = midiClip(a);
		if (c->notes().empty()) { fail("Clip has no notes"); }
		if (c->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native note roster exceeds the receipt index range"); }
		auto n = c->notes().at(integer(a, "note", 0, static_cast<int>(c->notes().size()) - 1));
		int removed = 0; QString error; if (!c->removeNotes(NoteVector{n}, removed, error)) { fail(error); } return {{"removed", removed}};
	}
	if (op == "note.clear") { int removed = 0; QString error; if (!midiClip(a)->clearNotes(removed, error)) { fail(error); } return {{"removed", removed}}; }
	if (op == "note.detuning.inspect" || op == "note.detuning.point" || op == "note.detuning.edit"
		|| op == "note.detuning.removePoint" || op == "note.detuning.clear")
	{
		auto n = note(a);
		if (op == "note.detuning.inspect")
		{
			auto result = inspectDetuningSection(n, a);
			if (a.contains("position"))
			{
				const auto position = integer(a, "position", 0, MaxSongLength);
				const auto curve = n->detuning() ? n->detuning()->existingAutomationClip() : nullptr;
				result.insert("position", position); result.insert("value", curve ? curve->valueAt(TimePos(position)) : 0.f);
			}
			return result;
		}
		if (op == "note.detuning.point")
		{
			const auto point = curvePoint(a, -MaxDetuning, MaxDetuning);
			const auto settings = curveSettings(a);
			const QPointer<MidiClip> owner(midiClip(a)); const auto identity = n->selectionIdentity();
			if (owner->getTrack()->isNoteRecording()) { fail("Complete native note recording before editing detuning"); }
			n->createDetuning(); const auto helperOwner = n->detuning();
			const QPointer<DetuningHelper> helper(helperOwner.get());
			const auto valid = [&] {
				if (!owner || std::find(owner->notes().begin(), owner->notes().end(), n) == owner->notes().end()
					|| n->selectionIdentity() != identity || !helper || n->detuning().get() != helper.data())
				{ fail("Native detuning note or curve changed during editing"); }
			};
			const auto curve = helperOwner->automationClip(); valid();
			const auto result = writeCurvePoint(curve, point, settings);
			valid(); emit owner->dataChanged(); valid();
			return result;
		}
		if (op == "note.detuning.edit")
		{
			const auto settings = curveSettings(a);
			if (!n->hasDetuningInfo()) { fail("Add a detuning change before editing its interpolation"); }
			const QPointer<MidiClip> owner(midiClip(a)); const auto identity = n->selectionIdentity();
			if (owner->getTrack()->isNoteRecording()) { fail("Complete native note recording before editing detuning"); }
			const auto helper = n->detuning();
			const auto valid = [&] {
				if (!owner || std::find(owner->notes().begin(), owner->notes().end(), n) == owner->notes().end()
					|| n->selectionIdentity() != identity || n->detuning() != helper)
				{ fail("Native detuning note or curve changed during editing"); }
			};
			const auto curve = helper->automationClip(); valid(); applyCurveSettings(curve, settings);
			valid(); emit owner->dataChanged(); valid();
			return a.contains("summary") && boolean(a, "summary") ? inspectDetuningSection(n, {{"section", "summary"}}) : inspectDetuning(n);
		}
		const int position = op == "note.detuning.removePoint" ? integer(a, "position", 0, MaxSongLength) : 0;
		const QPointer<MidiClip> owner(midiClip(a)); const auto identity = n->selectionIdentity(); const auto helper = n->detuning();
		if (owner->getTrack()->isNoteRecording()) { fail("Complete native note recording before editing detuning"); }
		const auto valid = [&] {
			if (!owner || std::find(owner->notes().begin(), owner->notes().end(), n) == owner->notes().end()
				|| n->selectionIdentity() != identity || n->detuning() != helper)
			{ fail("Native detuning note or curve changed during editing"); }
		};
		if (helper)
		{
			const QPointer<AutomationClip> curve(helper->automationClip()); valid();
			if (op == "note.detuning.removePoint") { removeCurvePoint(curve.data(), position); valid(); }
			else
			{
				clearCurve(curve.data()); valid(); helper->setValue(helper->defaultValue()); valid();
				if (!curve || !curve->getTimeMap().isEmpty() || helper->storedValue() != helper->defaultValue())
				{ fail("Native detuning curve or value changed during default reset"); }
			}
		}
		valid(); emit owner->dataChanged(); valid();
		return a.contains("summary") && boolean(a, "summary") ? inspectDetuningSection(n, {{"section", "summary"}}) : inspectDetuning(n);
	}
	if (op == "step.set")
	{
		auto c = midiClip(a);
		const int step = integer(a, "step", 0, std::numeric_limits<int>::max());
		const bool enabled = boolean(a, "enabled");
		MidiClip::StepSetResult result; QString error; if (!c->setStep(step, enabled, result, error, 4096)) { fail(error); }
		QJsonObject receipt{{"step", step}, {"enabled", enabled}, {"changed", result.changed}, {"removed", result.removed}};
		if (result.note)
		{
			const auto found = std::find(c->notes().begin(), c->notes().end(), result.note); if (found == c->notes().end()) { fail("Native enabled step retired before receipt construction"); }
			receipt.insert("note", static_cast<int>(std::distance(c->notes().begin(), found))); receipt.insert("position", result.note->pos().getTicks());
		}
		return receipt;
	}
	if (op == "step.clone")
	{
		const QPointer<MidiClip> owner(midiClip(a)); QHash<const Note*, int> original;
		if (owner->notes().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { fail("Native step roster exceeds receipt index range"); }
		for (std::size_t index = 0; index < owner->notes().size(); ++index) { original.insert(owner->notes().at(index), static_cast<int>(index)); }
		MidiClip::StepCloneResult clone; QString error; if (!owner->cloneSteps(clone, error, 4096)) { fail(error); }
		if (!owner || clone.sources.size() != clone.copies.size() || clone.sources.size() != clone.steps.size()) { fail("Native step clone result changed before receipt construction"); }
		QHash<const Note*, int> current; for (std::size_t index = 0; index < owner->notes().size(); ++index) { current.insert(owner->notes().at(index), static_cast<int>(index)); }
		QJsonArray receipts;
		for (std::size_t index = 0; index < clone.copies.size(); ++index)
		{
			if (!original.contains(clone.sources[index]) || !current.contains(clone.sources[index]) || !current.contains(clone.copies[index])) { fail("Native step clone source or copy retired before receipt construction"); }
			receipts.append(QJsonObject{{"sourceNote", original.value(clone.sources[index])}, {"sourceStep", clone.steps[index]}, {"step", clone.fromCount + clone.steps[index]},
				{"note", current.value(clone.copies[index])}, {"position", clone.copies[index]->pos().getTicks()}, {"key", clone.copies[index]->key()}, {"length", clone.copies[index]->length().getTicks()}});
		}
		return {{"container", a.value("container").toString("song")}, {"track", a.value("track")}, {"clip", a.value("clip")}, {"fromCount", clone.fromCount}, {"count", owner->stepCount()}, {"copied", static_cast<int>(clone.copies.size())}, {"notes", receipts}};
	}
	if (op == "step.resize")
	{
		auto c = midiClip(a);
		const auto count = integer(a, "count", 1, 4096);
		if (TimePos::ticksPerBar() < 1 || TimePos::stepsPerBar() < 1) { fail("Native step meter is invalid"); }
		if (count % TimePos::stepsPerBar() != 0) { fail("Step count must be a whole number of native bars"); }
		if (c->stepCount() % TimePos::stepsPerBar() != 0) { fail("Existing native step count is not aligned to bars"); }
		MidiClip::StepResizeResult resized; QString error; if (!c->resizeSteps(count, resized, error, 4096)) { fail(error); }
		return {{"fromCount", resized.fromCount}, {"count", count}, {"removed", resized.removed}, {"changed", resized.changed}};
	}
	if (op == "instrument.load")
	{
		const QPointer<Song> owner(song); const QPointer<TrackContainer> tracks(container(a)); const QPointer<InstrumentTrack> t(instrumentTrack(a));
		auto key = pluginKey(a, Plugin::Type::Instrument);
		checkInspectionWork(); currentInstrumentTrackIndex(owner, tracks, t, a);
		const QPointer<Instrument> native(t->loadInstrument(key.desc->name, &key));
		currentInstrumentTrackIndex(owner, tracks, t, a);
		if (!native || t->instrument() != native.data() || native->descriptor() != key.desc) { fail("Native instrument load failed or selected instrument changed"); }
		return {{"instrument", key.desc->name}, {"track", currentInstrumentTrackIndex(owner, tracks, t, a)},
			{"container", a.value("container").toString("song")}};
	}
	if (op == "tuning.list") { return inspectTuningLibrary(a.contains("details") && boolean(a, "details")); }
	if (op.startsWith("tuning.scale."))
	{
		const int slot = integer(a, "slot", 0, MaxScaleCount - 1);
		if (op == "tuning.scale.inspect") { return inspectScale(slot); }
		QString error; std::shared_ptr<Scale> scale;
		if (op == "tuning.scale.set")
		{
			if (!a.value("intervals").isString()) { fail("intervals must contain native Scala pitch lines, excluding implicit 1/1"); }
			if (!a.value("name").isString()) { fail("Scale name must be a string"); }
			if (!TuningFile::parseScale(a.value("name").toString(), a.value("intervals").toString(), scale, error)) { fail(error); }
		}
		else if (op == "tuning.scale.load") { if (!TuningFile::loadScale(resourcePath(a), scale, error)) { fail(error); } }
		else if (op == "tuning.scale.save")
		{
			const auto path = QFileInfo(string(a, "path")).absoluteFilePath();
			if (!TuningFile::saveScale(path, *song->getScale(slot), error)) { fail(error); }
			return {{"path", path}};
		}
		else { fail("Unknown native scale operation"); }
		song->setScale(slot, scale); return inspectScale(slot);
	}
	if (op.startsWith("tuning.keymap."))
	{
		const int slot = integer(a, "slot", 0, MaxKeymapCount - 1);
		if (op == "tuning.keymap.inspect") { return inspectKeymap(slot); }
		QString error; std::shared_ptr<Keymap> keymap;
		if (op == "tuning.keymap.set")
		{
			const auto current = song->getKeymap(slot);
			if (!a.value("mapping").isString() || !a.value("name").isString()) { fail("Keymap name and mapping text must be strings"); }
			const auto first = a.contains("firstKey") ? integer(a, "firstKey", 0, NumKeys - 1) : current->getFirstKey();
			const auto last = a.contains("lastKey") ? integer(a, "lastKey", 0, NumKeys - 1) : current->getLastKey();
			const auto middle = a.contains("middleKey") ? integer(a, "middleKey", 0, NumKeys - 1) : current->getMiddleKey();
			const auto reference = a.contains("baseKey") ? integer(a, "baseKey", 0, NumKeys - 1) : current->getBaseKey();
			const auto frequency = a.contains("baseFrequency") ? number(a, "baseFrequency", std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()) : current->getBaseFreq();
			const auto octave = a.contains("octaveDegree") ? integer(a, "octaveDegree", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) : current->octaveDegree();
			const bool origin = a.contains("linearFromMiddle") ? boolean(a, "linearFromMiddle") : current->linearFromMiddle();
			if (!TuningFile::parseKeymap(a.value("name").toString(), a.value("mapping").toString(), first, last, middle,
				reference, frequency, octave, keymap, error, origin)) { fail(error); }
		}
		else if (op == "tuning.keymap.load") { if (!TuningFile::loadKeymap(resourcePath(a), keymap, error)) { fail(error); } }
		else if (op == "tuning.keymap.save")
		{
			const auto path = QFileInfo(string(a, "path")).absoluteFilePath();
			if (!TuningFile::saveKeymap(path, *song->getKeymap(slot), error)) { fail(error); }
			return {{"path", path}};
		}
		else { fail("Unknown native keymap operation"); }
		song->setKeymap(slot, keymap); return inspectKeymap(slot);
	}
	if (op == "instrument.tuning.inspect")
	{
		const auto native = instrumentTrack(a);
		auto result = inspectInstrumentTuning(native);
		const auto first = a.contains("key") ? integer(a, "key", 0, NumKeys - 1) : 0;
		const auto last = a.contains("key") ? first : NumKeys - 1;
		QJsonArray keys;
		for (int key = first; key <= last; ++key) { keys.append(QJsonObject{{"key", key}, {"mapped", native->isKeyMapped(key)}, {"frequency", native->nominalFrequency(key)}}); }
		result.insert("keys", keys);
		result.insert("frequencyScope", "native nominal frequency before master/instrument pitch and per-note detuning");
		return result;
	}
	if (op == "instrument.inspect")
	{
		return inspectInstrument(instrumentTrack(a), a);
	}
	if (op == "host.inspect" || op.startsWith("host.plugin.") || op.startsWith("host.parameter.")
		|| op.startsWith("host.catalog.") || op.startsWith("host.patchbay."))
	{
		auto* instrument = instrumentTrack(a)->instrument();
		auto* host = instrument ? instrument->pluginHost() : nullptr;
		if (op == "host.inspect") { return inspectPluginHost(host); }
		if (!host || !host->available())
		{
			fail(host ? host->unavailableReason() : "Load an instrument with native hosted-plugin management first");
		}
		if (op.startsWith("host.patchbay."))
		{
			if (!host->patchbayAvailable()) { fail(host->patchbayUnavailableReason()); }
			if (op == "host.patchbay.inspect") { return inspectPatchbay(host->patchbay()); }
			const auto endpointId = [&](const char* field)
			{
				const auto value = number(a, field, 0, std::numeric_limits<quint32>::max());
				if (std::floor(value) != value) { fail(QString(field) + " must be an integer"); }
				return static_cast<quint32>(value);
			};
			const PluginHost::PatchbayRoute route{{endpointId("sourceGroup"), endpointId("sourcePort")},
				{endpointId("destinationGroup"), endpointId("destinationPort")}};
			if (op == "host.patchbay.connect") { return inspectPatchbay(host->connectPatchbay(route)); }
			if (op == "host.patchbay.disconnect") { return inspectPatchbay(host->disconnectPatchbay(route)); }
			fail("Unknown native patchbay operation");
		}
		if (op.startsWith("host.catalog."))
		{
			if (op == "host.catalog.start")
			{
				PluginHost::DiscoveryQuery query; query.format = string(a, "format");
				if (a.contains("binary")) { query.binary = string(a, "binary"); }
				for (const auto field : {"path", "discoveryTool"})
				{
					if (!a.contains(field) || a.value(field).toString().isEmpty()) { continue; }
					bool invalid = false; const auto path = PathUtil::toAbsolute(string(a, field), &invalid);
					if (invalid) { fail("Discovery path could not be resolved"); }
					if (QString(field) == "path") { query.path = path; } else { query.discoveryTool = path; }
				}
				if (a.contains("maximumPlugins")) { query.maximumPlugins = integer(a, "maximumPlugins", 1, 65536); }
				if (a.contains("timeoutMs")) { query.timeoutMs = integer(a, "timeoutMs", 1, 600000); }
				return inspectDiscovery(host->startDiscovery(query));
			}
			const auto scan = string(a, "scan");
			if (op == "host.catalog.stop") { return inspectDiscovery(host->stopDiscovery(scan)); }
			if (op == "host.catalog.poll")
			{
				const int offset = a.contains("offset") ? integer(a, "offset", 0, std::numeric_limits<int>::max()) : 0;
				const int limit = a.contains("limit") ? integer(a, "limit", 1, 1024) : 128;
				return inspectDiscovery(host->pollDiscovery(scan, offset, limit));
			}
			fail("Unknown native plugin discovery operation");
		}
		if (op.startsWith("host.parameter."))
		{
			if (!host->parametersAvailable()) { fail(host->parametersUnavailableReason()); }
			const int plugin = integer(a, "hostedPlugin", 0, std::numeric_limits<int>::max());
			if (op == "host.parameter.list")
			{
				const int offset = a.contains("offset") ? integer(a, "offset", 0, std::numeric_limits<int>::max()) : 0;
				const int limit = a.contains("limit") ? integer(a, "limit", 1, 1024) : 128;
				const int total = host->parameterCount(plugin), start = std::min(offset, total);
				const int count = std::min(limit, total - start);
				QJsonArray parameters;
				for (int i = 0; i < count; ++i) { parameters.append(inspectHostedParameter(host->parameter(plugin, start + i))); }
				return {{"parameters", parameters}, {"offset", offset}, {"total", total}, {"hasMore", start + count < total}};
			}
			const int index = integer(a, "hostedParameter", 0, std::numeric_limits<int>::max());
			if (op == "host.parameter.set")
			{
				const auto value = number(a, "value", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
				const auto parameter = host->setParameter(plugin, index, value);
				instrument->refreshParameterModels();
				auto result = inspectHostedParameter(parameter);
				const auto nativeRequest = static_cast<float>(value);
				result.insert("requestedValue", value); result.insert("nativeRequestValue", nativeRequest);
				result.insert("matchesNativeRequest", parameter.value == nativeRequest);
				result.insert("confirmation", "native value readback; the native setter provides no acceptance status");
				return result;
			}
			if (op == "host.parameter.inspect")
			{
				const int offset = a.contains("choiceOffset") ? integer(a, "choiceOffset", 0, std::numeric_limits<int>::max()) : 0;
				const int limit = a.contains("choiceLimit") ? integer(a, "choiceLimit", 1, 1024) : 128;
				const auto parameter = host->parameter(plugin, index);
				auto result = inspectHostedParameter(parameter);
				QJsonArray choices;
				for (const auto& choice : host->parameterChoices(plugin, index, offset, limit))
				{
					choices.append(QJsonObject{{"value", choice.value}, {"label", choice.label}});
				}
				result.insert("choices", choices); result.insert("choiceOffset", offset);
				result.insert("hasMoreChoices", std::min(offset, parameter.scalePointCount) + choices.size() < parameter.scalePointCount);
				return result;
			}
			fail("Unknown native hosted-parameter operation");
		}
		int added = -1;
		if (op == "host.plugin.add")
		{
			PluginHost::Specification plugin;
			plugin.format = string(a, "format");
			plugin.name = a.value("name").toString(); plugin.label = a.value("label").toString();
			plugin.binary = a.contains("binary") ? string(a, "binary") : "native";
			if (a.contains("path") && !a.value("path").toString().isEmpty())
			{
				bool invalid = false;
				plugin.path = PathUtil::toAbsolute(string(a, "path"), &invalid);
				if (invalid || !QFileInfo::exists(plugin.path)) { fail("Hosted plugin path does not exist"); }
			}
			if (a.contains("uniqueId"))
			{
				const auto encoded = string(a, "uniqueId"); bool valid = false;
				plugin.uniqueId = encoded.toLongLong(&valid);
				if (!valid || !QRegularExpression("\\A-?(0|[1-9][0-9]*)\\z").match(encoded).hasMatch())
				{
					fail("Hosted plugin uniqueId must be a signed 64-bit decimal string");
				}
			}
			added = host->add(plugin);
		}
		else
		{
			const int index = integer(a, "hostedPlugin", 0, std::numeric_limits<int>::max());
			if (op == "host.plugin.remove") { host->remove(index); }
			else if (op == "host.plugin.rename") { host->rename(index, string(a, "name")); }
			else if (op == "host.plugin.swap") { host->move(index, integer(a, "targetPlugin", 0, std::numeric_limits<int>::max())); }
			else if (op == "host.plugin.active") { host->setActive(index, boolean(a, "active")); }
			else { fail("Unknown native hosted-plugin operation"); }
		}
		instrument->refreshParameterModels();
		auto result = inspectPluginHost(host);
		if (added >= 0) { result.insert("added", added); }
		return result;
	}
	if (op == "instrument.resource.set")
	{
		const QPointer<Song> owner(song); const QPointer<TrackContainer> tracks(container(a)); const QPointer<InstrumentTrack> track(instrumentTrack(a));
		const QPointer<Instrument> native(track->instrument());
		if (!native) { fail("Load a native instrument before setting a resource"); }
		const auto ready = [&] { currentInstrumentTrackIndex(owner, tracks, track, a); if (!native || track->instrument() != native.data()) { fail("Native resource instrument retired or changed"); } };
		const auto key = string(a, "resource");
		const auto resources = native->fileResources(); ready();
		const auto resource = std::find_if(resources.begin(), resources.end(), [&key](const auto& item) { return item.key == key; });
		if (resource == resources.end()) { fail("The current instrument does not expose this checked file resource; inspect native resources first"); }
		const bool select = a.contains("select") && boolean(a, "select");
		const QPointer<AutomatableModel> selector(resource->selector); AutomatableModel* const selectorIdentity = resource->selector; const auto selection = resource->selectionValue;
		const auto path = resourcePath(a); ready();
		QString error;
		const bool loaded = native->setFileResource(key, path, error); ready();
		if (!loaded) { fail(error.isEmpty() ? "Native resource load failed without a diagnostic" : error); }
		const auto current = native->fileResources(); ready(); const auto updated = std::find_if(current.begin(), current.end(), [&key](const auto& item) { return item.key == key; });
		if (updated == current.end() || selector.data() != selectorIdentity || updated->selector != selectorIdentity || updated->selectionValue != selection) { fail("Native resource selector changed during loading"); }
		if (select && selector) { selector->setValue(selection); ready(); if (!selector) { fail("Native resource selector retired during activation"); } }
		const auto result = inspectResources(native.data()); ready(); return {{"instrument", native->descriptor()->name}, {"resources", result}};
	}
	if (op == "instrument.program.list")
	{
		const auto native = instrumentTrack(a)->instrument();
		if (!native) { fail("Load a native instrument before inspecting programs"); }
		const int offset = a.contains("offset") ? integer(a, "offset", 0, std::numeric_limits<int>::max()) : 0;
		const int limit = a.contains("limit") ? integer(a, "limit", 1, 1024) : 128;
		const int bank = a.contains("bank") ? integer(a, "bank", 0, 16383) : -1;
		if (a.contains("query") && !a.value("query").isString()) { fail("Program query must be a string"); }
		const auto query = a.value("query").toString();
		const auto& available = native->programs();
		QJsonArray programs; int total = 0;
		for (const auto& item : available)
		{
			if ((bank >= 0 && item.bank != bank) || !item.name.contains(query, Qt::CaseInsensitive)) { continue; }
			if (total++ >= offset && programs.size() < limit) { programs.append(QJsonObject{{"bank", item.bank}, {"program", item.program}, {"name", item.name}}); }
		}
		return {{"programs", programs}, {"total", total}, {"offset", offset}, {"limit", limit},
			{"hasMore", static_cast<qint64>(offset) + programs.size() < total}, {"selection", inspectProgramSelection(native)}};
	}
	if (op == "instrument.program.select")
	{
		const auto native = instrumentTrack(a)->instrument();
		if (!native) { fail("Load a native instrument before selecting a program"); }
		const int bank = integer(a, "bank", 0, 16383), program = integer(a, "program", 0, 127);
		QString error;
		if (!native->selectProgram(bank, program, error)) { fail(error.isEmpty() ? "Native program selection failed without a diagnostic" : error); }
		return inspectProgramSelection(native);
	}
	if (op.startsWith("instrument.slices.") || op == "clip.fromSlices")
	{
		const auto native = instrumentTrack(a)->instrument();
		if (!native || !native->sliceLayout()) { fail("The current native instrument does not expose sample slicing"); }
		QString error;
		if (op == "instrument.slices.analyze")
		{
			const bool estimate = a.contains("estimateTempo") && boolean(a, "estimateTempo");
			if (!native->analyzeSlices(estimate, error)) { fail(error); }
		}
		else if (op == "instrument.slices.edit")
		{
			if (!a.value("points").isArray()) { fail("Slice points must be an array of normalized source positions"); }
			const auto input = a.value("points").toArray();
			if (input.size() < 2 || input.size() > 129) { fail("Slice points must contain 2 through 129 values including endpoints"); }
			std::vector<float> points;
			for (const auto item : input)
			{
				if (!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble() < 0 || item.toDouble() > 1)
				{
					fail("Slice points must be finite numbers within 0 through 1");
				}
				points.push_back(static_cast<float>(item.toDouble()));
			}
			if (!native->setSlicePoints(points, error)) { fail(error); }
		}
		else if (op == "instrument.slices.midi" || op == "clip.fromSlices")
		{
			std::vector<Note> notes;
			if (!native->sliceMidi(notes, error)) { fail(error); }
			const int offset = op == "clip.fromSlices" && a.contains("position") ? integer(a, "position", 0, MaxSongLength) : 0;
			for (auto& note : notes)
			{
				const int start = note.pos().getTicks(), length = note.length().getTicks();
				if (start < 0 || length <= 0 || length > MaxSongLength || start > MaxSongLength - length || offset > MaxSongLength - start - length
					|| note.key() < 0 || note.key() >= NumKeys) { fail("Native slice MIDI exceeds supported note/timeline bounds"); }
				note.setPos(TimePos(start + offset));
			}
			if (op == "clip.fromSlices")
			{
				auto target = a;
				if (a.contains("targetContainer")) { target.insert("container", string(a, "targetContainer")); }
				if (a.contains("targetTrack")) { target.insert("track", a.value("targetTrack")); }
				const auto clip = midiClip(target);
				const bool replace = a.contains("replace") && boolean(a, "replace");
				if (replace) { clip->clearNotes(); }
				for (const auto& note : notes) { clip->addNote(note, false); }
			}
			QJsonArray result;
			for (const auto& note : notes)
			{
				result.append(QJsonObject{{"position", note.pos().getTicks()}, {"length", note.length().getTicks()},
					{"key", note.key()}, {"volume", note.getVolume()}, {"panning", note.getPanning()}});
			}
			return {{"notes", result}, {"inserted", op == "clip.fromSlices" ? static_cast<int>(notes.size()) : 0},
				{"timing", "native source duration, tempo sync and instrument pitch; integer ticks"}};
		}
		else if (op != "instrument.slices.inspect") { fail("Unknown native slicing operation"); }
		return inspectSlices(*native->sliceLayout());
	}
	if (op == "preset.load")
	{
		const QPointer<Song> owner(song); const QPointer<TrackContainer> tracks(container(a)); const QPointer<InstrumentTrack> t(instrumentTrack(a));
		checkInspectionWork(); currentInstrumentTrackIndex(owner, tracks, t, a);
		DataFile file(string(a, "path"));
		checkInspectionWork(); currentInstrumentTrackIndex(owner, tracks, t, a);
		if (file.type() != DataFile::Type::InstrumentTrackSettings || file.hasLocalPlugins())
		{
			fail("Path is not a safe native instrument preset");
		}
		auto element = file.content().firstChildElement("instrumenttrack");
		if (element.isNull()) { fail("Preset has no instrumenttrack node"); }
		owner->clearErrors();
		t->replaceInstrument(file);
		const int index = currentInstrumentTrackIndex(owner, tracks, t, a);
		if (owner->hasFatalErrors()) { fail("Native preset did not load completely: " + owner->errorSummary()); }
		return {{"track", index}, {"container", a.value("container").toString("song")}};
	}
	if (op == "preset.save")
	{
		const QPointer<Song> owner(song); const QPointer<TrackContainer> tracks(container(a)); const QPointer<InstrumentTrack> t(instrumentTrack(a));
		checkInspectionWork(); int index = currentInstrumentTrackIndex(owner, tracks, t, a, true);
		NativeTrackSaveSnapshot source(t.data(), false);
		DataFile file(DataFile::Type::InstrumentTrackSettings);
		t->savePreset(file, file.content());
		source.validate(); const QString path = QFileInfo(file.nameWithExtension(string(a, "path"))).absoluteFilePath();
		if (!file.writeFile(path, false, [&] { source.validate(); index = currentInstrumentTrackIndex(owner, tracks, t, a, true); })) { fail("Native preset save failed"); }
		return {{"committed", true}, {"path", path}, {"track", index}, {"container", a.value("container").toString("song")}};
	}
	if (op == "plugin.list")
	{
		QJsonArray plugins;
		const bool subplugins = !a.contains("subplugins") || boolean(a, "subplugins");
		for (const auto d : getPluginFactory()->descriptors())
		{
			QJsonArray keys;
			if (subplugins && d->subPluginFeatures)
			{
				Plugin::Descriptor::SubPluginFeatures::KeyList nativeKeys;
				d->subPluginFeatures->listSubPluginKeys(d, nativeKeys);
				for (const auto& key : nativeKeys)
				{
					QJsonObject attributes;
					for (auto it = key.attributes.begin(); it != key.attributes.end(); ++it) { attributes.insert(it.key(), it.value()); }
					keys.append(QJsonObject{{"name", key.name}, {"attributes", attributes}});
				}
			}
			plugins.append(QJsonObject{{"name", d->name}, {"displayName", d->displayName},
				{"type", static_cast<int>(d->type)}, {"description", d->description},
				{"fileTypes", QString(d->supportedFileTypes)}, {"subplugins", keys}});
		}
		return {{"plugins", plugins}};
	}
	if (op == "parameter.list")
	{
		FileResourceIO::EventSuspension events;
		const auto page = InspectionPage::read(a);
		const bool choices = !a.contains("choices") || boolean(a, "choices");
		const bool trackFilter = a.contains("track") || a.contains("container");
		const bool channelFilter = a.contains("channel"), controllerFilter = a.contains("controller");
		const bool routeFilter = a.contains("from") || a.contains("to");
		if (int(trackFilter) + int(channelFilter) + int(controllerFilter) + int(routeFilter) > 1)
		{
			fail("Filter parameters by one of track/container, mixer channel, mixer route from/to, or controller");
		}
		QString targetContainer;
		int targetTrack = -1, targetController = -1, targetChannel = -1, targetEffect = -1, from = -1, to = -1;
		if (trackFilter)
		{
			targetContainer = container(a) == Engine::patternStore() ? "pattern" : "song";
			if (a.contains("track")) { track(a); targetTrack = a.value("track").toInt(); }
		}
		if (controllerFilter) { controller(a); targetController = a.value("controller").toInt(); }
		if (channelFilter) { targetChannel = channel(a)->index(); }
		if (a.contains("effect"))
		{
			if (!channelFilter && !a.contains("track")) { fail("An effect filter requires a mixer channel or a specific track"); }
			const auto effects = chain(a); targetEffect = integer(a, "effect", 0, static_cast<int>(effects->effects().size()) - 1);
		}
		if (a.contains("from")) { from = integer(a, "from", 0, Engine::mixer()->numChannels() - 1); }
		if (a.contains("to")) { to = integer(a, "to", 0, Engine::mixer()->numChannels() - 1); }
		QList<AutomatableModel*> selected;
		for (auto m : models())
		{
			checkInspectionWork();
			const auto location = parameterLocation(m);
			if (trackFilter && (location.value("container").toString() != targetContainer
				|| (targetTrack >= 0 && location.value("track").toInt(-1) != targetTrack))) { continue; }
			if (targetController >= 0 && location.value("controller").toInt(-1) != targetController) { continue; }
			if (targetChannel >= 0 && location.value("channel").toInt(-1) != targetChannel) { continue; }
			if (targetEffect >= 0 && location.value("effect").toInt(-1) != targetEffect) { continue; }
			if (routeFilter && (location.value("scope").toString() != "mixerRoute"
				|| (from >= 0 && location.value("from").toInt(-1) != from) || (to >= 0 && location.value("to").toInt(-1) != to))) { continue; }
			selected.append(m);
		}
		auto result = inspectParameterPage(selected, page, choices);
		result.insert("handleScope", "current project revision");
		return result;
	}
	if (op == "parameter.inspect")
	{
		FileResourceIO::EventSuspension events;
		const auto choices = InspectionPage::read(a, "choiceOffset", "choiceLimit");
		const auto links = InspectionPage::read(a, "linkedOffset", "linkedLimit");
		const auto model = parameter(a); auto result = inspectParameter(model, !a.contains("choices") || boolean(a, "choices"), choices);
		QSet<const AutomatableModel*> readable;
		for (const auto native : models()) { checkInspectionWork(); readable.insert(native); }
		QJsonArray peers; qint64 bytes = 2; int total = 0; bool full = false;
		for (auto peer = model->nextLinkedModel(); peer != model; peer = peer->nextLinkedModel())
		{
			checkInspectionWork(); if (total == std::numeric_limits<int>::max()) { fail("Linked group exceeds native inspection offset range"); }
			if (total >= links.offset && peers.size() < links.limit && !full)
			{
				full = !appendInspectionItem(peers, {{"id", static_cast<int>(peer->id())}, {"scale", static_cast<int>(peer->scaleType())},
					{"writable", peer->isWritable()}, {"automatable", peer->supportsAutomation()}, {"inspectable", readable.contains(peer)}}, bytes, 512 * 1024);
			}
			++total;
		}
		result.insert("linkedParameters", peers); result.insert("linkedPage", links.metadata(total, peers.size()));
		result.insert("linkedParameterCount", total); result.insert("linkedHandleScope", "current native group and project revision; rediscover after links change or restoration");
		return result;
	}
	if (op == "parameter.set")
	{
		const QPointer<AutomatableModel> receiver(parameter(a)); auto m = receiver.data();
		requireWritableGroup(m);
		const auto value = number(a, "value", m->minValue<float>(), m->maxValue<float>());
		if (m->valueType() != AutomatableModel::ValueType::Number && std::floor(value) != value)
		{
			fail("Discrete native parameters require an integer value");
		}
		bool programSelected = false;
		for (auto parent = m->parent(); parent; parent = parent->parent())
		{
			const auto native = dynamic_cast<Instrument*>(parent);
			if (!native) { continue; }
			const auto current = native->midiPatch();
			if (!current) { break; }
			const bool bank = m == native->childModel("bank"), program = m == native->childModel("patch");
			if (!bank && !program) { break; }
			QString error;
			if (!native->selectProgram(bank ? static_cast<int>(value) : current->bank,
				program ? static_cast<int>(value) : current->program, error)) { fail(error); }
			programSelected = true; break;
		}
		if (!programSelected) { m->setValue(value); }
		if (!receiver) { fail("Native parameter retired during its value write"); }
		return {{"value", m->value<float>()}, {"manualValue", m->storedValue()}};
	}
	if (op == "parameter.link")
	{
		const auto id = integer(a, "parameter", 1, 2147483647), peerId = integer(a, "peer", 1, 2147483647);
		if (id == peerId) { fail("Linking requires two distinct current parameter handles"); }
		// Refresh/discover once before retaining either receiver. A second
		// plugin refresh must not retire the first resolved parameter.
		const auto discovered = models(); QHash<AutomatableModel*, QPointer<AutomatableModel>> current; QPointer<AutomatableModel> model, peer;
		for (auto candidate : discovered)
		{
			current.insert(candidate, candidate); if (candidate->id() == static_cast<jo_id_t>(id)) { model = candidate; }
			if (candidate->id() == static_cast<jo_id_t>(peerId)) { peer = candidate; }
		}
		if (!model || !peer) { fail("Link parameter or peer is not a current project handle; rediscover after restoration"); }
		requireLinkableGroup(model.data(), current); requireLinkableGroup(peer.data(), current); checkInspectionWork();
		const bool changed = !model->isLinkedToModel(peer.data());
		QString linkError;
		if (changed && !NativeControlLinkJournal::link(model.data(), peer.data(), linkError)) { fail(linkError); }
		if (!model || !peer || !model->isLinkedToModel(peer.data())) { fail("Native parameter link receivers changed during callbacks"); }
		requireLinkableGroup(model.data(), current);
		return {{"parameter", id}, {"peer", peerId}, {"linked", true}, {"changed", changed}, {"manualValue", model->storedValue()},
			{"linkedPeers", static_cast<double>(model->countLinks())}};
	}
	if (op == "parameter.unlink")
	{
		auto model = parameter(a); if (!model->isWritable()) { fail("Native parameter is an output or is currently disabled"); }
		checkInspectionWork(); const bool changed = model->isLinked(); QString linkError;
		if (!NativeControlLinkJournal::unlink(model, linkError)) { fail(linkError); }
		return {{"parameter", static_cast<int>(model->id())}, {"linked", false}, {"changed", changed}, {"manualValue", model->storedValue()}};
	}
	if (op == "parameter.sync")
	{
		const QPointer<TempoSyncKnobModel> receiver(dynamic_cast<TempoSyncKnobModel*>(parameter(a))); auto model = receiver.data();
		if (!model) { fail("Parameter does not support native tempo synchronization"); }
		if (!model->isWritable()) { fail("Native parameter is an output or is currently disabled"); }
		const auto mode = static_cast<TempoSyncKnobModel::SyncMode>(integer(a, "mode", 0, 8));
		auto& meter = model->getCustomMeterModel();
		const auto numerator = a.contains("customNumerator") ? integer(a, "customNumerator",
			std::max(1, meter.numeratorModel().minValue()), meter.numeratorModel().maxValue()) : meter.getNumerator();
		const auto denominator = a.contains("customDenominator") ? integer(a, "customDenominator",
			std::max(1, meter.denominatorModel().minValue()), meter.denominatorModel().maxValue()) : meter.getDenominator();
		if (mode != TempoSyncKnobModel::SyncMode::Custom && (a.contains("customNumerator") || a.contains("customDenominator")))
		{
			fail("Custom meter arguments require Custom sync mode (8)");
		}
		if (mode == TempoSyncKnobModel::SyncMode::Custom && (numerator < 1 || denominator < 1))
		{
			fail("Custom tempo sync requires a positive numerator and denominator");
		}
		meter.setNumerator(numerator);
		if (!receiver) { fail("Native tempo-sync parameter retired during meter update"); }
		meter.setDenominator(denominator);
		if (!receiver) { fail("Native tempo-sync parameter retired during meter update"); }
		model->setTempoSync(mode);
		if (!receiver) { fail("Native tempo-sync parameter retired during synchronization"); }
		return {{"mode", static_cast<int>(model->syncMode())}, {"value", model->value()},
			{"customNumerator", numerator}, {"customDenominator", denominator}};
	}
	if (op == "controller.list") { return {{"controllers", inspectControllers()}}; }
	if (op == "controller.waveform.set")
	{
		const QPointer<Song> owner(song); const QPointer<LfoController> native(dynamic_cast<LfoController*>(controller(a)));
		const QPointer<Controller> target(native.data());
		if (!native) { fail("Waveform resources require a native LFO controller"); }
		const bool select = a.contains("select") && boolean(a, "select");
		const auto path = resourcePath(a);
		QString error;
		if (!native->setUserWave(path, error)) { fail(error); }
		currentControllerIndex(owner, target);
		const auto sample = native->userWaveBuffer(); const QString source = native->userWaveFile(); const bool pending = native->userWaveUnresolved();
		if (select) { native->waveModel().setValue(static_cast<int>(Oscillator::WaveShape::UserDefined)); }
		currentControllerIndex(owner, target);
		if (native->userWaveBuffer() != sample || native->userWaveFile() != source || native->userWaveUnresolved() != pending || (select && native->waveModel().storedValue() != static_cast<int>(Oscillator::WaveShape::UserDefined))) { fail("Native LFO waveform selection changed during notification"); }
		return {{"userWave", inspectLfoWave(native.data())}, {"waveParameter", static_cast<int>(native->waveModel().id())},
			{"wave", native->waveModel().value()}};
	}
	if (op == "controller.add")
	{
		if (string(a, "type") != "lfo") { fail("Rack creation supports native LFO controllers; peak controllers belong to their effects and MIDI controllers to their connections"); }
		const auto name = a.contains("name") ? string(a, "name") : QString();
		const QPointer<Song> owner(song);
		if (owner->isLoadingProject() || owner->controllers().size() >= std::numeric_limits<int>::max()) { fail("Native controller rack is loading or exceeds its index range"); }
		checkInspectionWork();
		if (!owner || Engine::getSong() != owner.data() || owner->isLoadingProject()) { fail("Native project changed before controller creation"); }
		const QPointer<Controller> native(Controller::create(Controller::ControllerType::Lfo, owner.data()));
		const auto cleanup = qScopeGuard([&] { if (native && (!owner || !owner->containsController(native.data()))) { delete native.data(); } });
		if (!native) { fail("Native controller creation failed"); }
		if (!owner || Engine::getSong() != owner.data()) { fail("Native project retired during controller creation"); }
		if (!name.isEmpty()) { native->setName(name); }
		checkInspectionWork();
		if (!owner || Engine::getSong() != owner.data() || owner->isLoadingProject()) { fail("Native project changed during controller preparation"); }
		owner->addController(native.data());
		const int index = currentControllerIndex(owner, native);
		QJsonObject receipt{{"controller", index}};
		if (a.contains("summary") && boolean(a, "summary")) { receipt.insert("summary", inspectControllerInventory(native.data(), index, false)); }
		else { receipt.insert("controllers", inspectControllers()); }
		if (currentControllerIndex(owner, native) != index) { fail("Native controller index changed during creation receipt inspection"); }
		return receipt;
	}
	if (op == "controller.edit")
	{
		const auto name = string(a, "name"); const QPointer<Song> owner(song); const QPointer<Controller> native(controller(a));
		checkInspectionWork(); currentControllerIndex(owner, native);
		native->setName(name);
		currentControllerIndex(owner, native); const auto actual = native->name();
		return {{"name", actual}, {"controller", currentControllerIndex(owner, native)}};
	}
	if (op == "controller.remove")
	{
		const QPointer<Song> owner(song); const QPointer<Controller> native(controller(a));
		if (native->type() == Controller::ControllerType::Peak) { fail("Remove the owning peak-controller effect through effect.remove"); }
		checkInspectionWork(); currentControllerIndex(owner, native);
		owner->removeController(native.data());
		if (!owner || Engine::getSong() != owner.data() || (native && owner->containsController(native.data())))
		{ fail("Native controller removal did not complete in the current project"); }
		return {};
	}
	if (op == "controller.midi.connect")
	{
		auto model = parameter(a);
		const int channel = a.contains("inputChannel") ? integer(a, "inputChannel", 0, MidiChannelCount) : 0;
		const int control = integer(a, "inputController", 0, MidiControllerCount - 1);
		const bool readable = a.contains("readable") ? boolean(a, "readable") : true;
		bool changed = false; QString error;
		if (!model->connectMidiController(channel, control, readable, changed, error)) { fail(error); }
		return {{"parameter", static_cast<int>(model->id())}, {"midi", inspectMidi(midiPort(a))}, {"ownership", "parameter connection"}, {"changed", changed}};
	}
	if (op == "controller.connect")
	{
		const QPointer<Controller> native(controller(a));
		auto model = parameter(a);
		bool changed = false; QString error;
		if (!model->connectController(native.data(), changed, error)) { fail(error); }
		if (!native) { fail("Native rack controller retired before its receipt"); }
		const auto found = std::find(song->controllers().begin(), song->controllers().end(), native.data());
		if (found == song->controllers().end()) { fail("Native controller left the rack before its receipt"); }
		return {{"controller", static_cast<int>(std::distance(song->controllers().begin(), found))},
			{"parameter", static_cast<int>(model->id())}, {"connections", native->connectionCount()}, {"changed", changed}};
	}
	if (op == "controller.disconnect")
	{
		auto model = parameter(a);
		bool changed = false; QString error;
		if (!model->disconnectController(changed, error)) { fail(error); }
		return {{"manualValue", model->storedValue()}, {"changed", changed}};
	}
	if (op == "automation.global.inspect")
	{
		FileResourceIO::EventSuspension events;
		if (a.contains("parameter") == a.contains("global")) { fail("Select exactly one native global automation parameter or global index"); }
		AutomatableModel* model = nullptr; AutomationClip* curve = nullptr;
		if (a.contains("parameter")) { model = parameter(a); curve = existingGlobalAutomation(model); }
		else
		{
			const auto global = song->globalAutomationTrack();
			curve = dynamic_cast<AutomationClip*>(global->getClip(integer(a, "global", 0, global->numOfClips() - 1)));
			if (!curve) { fail("Native global clip is not an automation curve"); }
		}
		auto result = inspectCurveSection(curve, a);
		if (model) { result.insert("parameter", static_cast<int>(model->id())); }
		else { result.insert("global", a.value("global")); }
		if (a.contains("position"))
		{
			const auto position = integer(a, "position", 0, MaxSongLength);
			result.insert("position", position);
			const auto evaluatedModel = model ? model : curve ? curve->firstObject() : nullptr;
			result.insert("value", curve && curve->hasAutomation() ? curve->valueAt(TimePos(position))
				: evaluatedModel ? evaluatedModel->inverseScaledValue(evaluatedModel->storedValue()) : 0.f);
		}
		return result;
	}
	if (op == "automation.global.point" || op == "automation.global.edit"
		|| op == "automation.global.removePoint" || op == "automation.global.clear")
	{
		auto model = parameter(a);
		auto curve = existingGlobalAutomation(model);
		if (op == "automation.global.point")
		{
			if (!model->supportsAutomation()) { fail("Native parameter does not support automation"); }
			const auto point = curvePoint(a, model->minValue<float>(), model->maxValue<float>());
			const auto settings = curveSettings(a);
			if (!curve) { curve = AutomationClip::globalAutomationClip(model); }
			return writeCurvePoint(curve, point, settings);
		}
		if (op == "automation.global.edit")
		{
			if (!model->supportsAutomation()) { fail("Native parameter does not support automation"); }
			const auto settings = curveSettings(a);
			if (!curve) { fail("Parameter has no native global automation curve"); }
			applyCurveSettings(curve, settings);
		}
		else if (op == "automation.global.removePoint")
		{
			const auto position = integer(a, "position", 0, MaxSongLength);
			if (curve) { removeCurvePoint(curve, position); }
		}
		else if (curve) { clearCurve(curve); }
		return a.contains("summary") && boolean(a, "summary") ? inspectCurveMetadata(curve) : inspectCurve(curve);
	}
	if (op == "automation.connect")
	{
		auto c = automationClip(a);
		auto m = parameter(a);
		bool connected = false; QString error;
		if (!c->editBinding(m, true, connected, error)) { fail(error); }
		return {{"connected", connected}, {"automation", a.contains("summary") && boolean(a, "summary") ? inspectCurveMetadata(c) : inspectCurve(c)}};
	}
	if (op == "automation.disconnect")
	{
		auto c = automationClip(a);
		bool disconnected = false; QString error;
		if (!c->editBinding(parameter(a), false, disconnected, error)) { fail(error); }
		return {{"disconnected", disconnected}, {"automation", a.contains("summary") && boolean(a, "summary") ? inspectCurveMetadata(c) : inspectCurve(c)}};
	}
	if (op == "automation.inspect")
	{
		auto c = automationClip(a);
		auto result = inspectCurveSection(c, a);
		if (a.contains("position"))
		{
			const auto position = integer(a, "position", 0, MaxSongLength);
			result.insert("position", position); result.insert("value", c->valueAt(TimePos(position)));
		}
		return result;
	}
	if (op == "automation.edit")
	{
		const auto settings = curveSettings(a);
		auto c = automationClip(a);
		applyCurveSettings(c, settings);
		return {{"progression", static_cast<int>(c->progressionType())}, {"tension", c->getTension()}};
	}
	if (op == "automation.point")
	{
		auto c = automationClip(a);
		const auto model = c->firstObject();
		const auto point = curvePoint(a, model->minValue<float>(), model->maxValue<float>());
		const auto settings = curveSettings(a);
		return writeCurvePoint(c, point, settings);
	}
	if (op == "automation.removePoint")
	{
		auto c = automationClip(a);
		const auto position = integer(a, "position", 0, MaxSongLength);
		const auto removed = removeCurvePoint(c, position);
		return {{"removed", removed}, {"position", position}};
	}
	if (op == "automation.clear")
	{
		clearCurve(automationClip(a));
		return {};
	}
	if (op == "mixer.add")
	{
		const QPointer<Song> owner(song); const QPointer<Mixer> mixer(Engine::mixer()); const auto generation = mixer->replacementGeneration(); const auto project = owner->replacementGeneration();
		const auto name = a.contains("name") ? string(a, "name") : QString{};
		const auto index = mixer->createChannel(); currentMixer(owner, mixer, generation, project);
		if (a.contains("name")) { mixer->mixerChannel(index)->setName(name); currentMixer(owner, mixer, generation, project); }
		return {{"channel", index}};
	}
	if (op == "mixer.edit")
	{
		const QPointer<Song> owner(song); const QPointer<Mixer> mixer(Engine::mixer()); const auto generation = mixer->replacementGeneration(); const auto project = owner->replacementGeneration();
		auto ch = channel(a);
		const QPointer<EffectChain> anchor(&ch->m_fxChain);
		const auto ready = [&] { return currentMixerChannelIndex(owner, mixer, generation, project, ch, anchor); };
		const auto name = a.contains("name") ? string(a, "name") : QString{};
		const auto volume = a.contains("volume") ? number(a, "volume", ch->m_volumeModel.minValue(), ch->m_volumeModel.maxValue()) : 0.;
		const auto muted = a.contains("muted") ? boolean(a, "muted") : false;
		const auto solo = a.contains("solo") ? boolean(a, "solo") : false;
		const auto destination = a.contains("to") ? integer(a, "to", 1, Engine::mixer()->numChannels() - 1) : ch->index();
		if (a.contains("to") && !Engine::mixer()->moveChannel(ch->index(), destination)) { fail("Native mixer channel could not be moved; master must stay at zero"); }
		ready();
		if (a.contains("name")) { ch->setName(name); ready(); }
		if (a.contains("volume")) { ch->m_volumeModel.setValue(volume); ready(); }
		if (a.contains("muted")) { ch->m_muteModel.setValue(muted); ready(); }
		if (a.contains("solo")) { ch->m_soloModel.setValue(solo); ready(); }
		return {{"channel", ready()}};
	}
	if (op == "mixer.remove" || op == "mixer.reset")
	{
		const QPointer<Song> owner(song); const QPointer<Mixer> mixer(Engine::mixer()); const auto generation = mixer->replacementGeneration(); const auto project = owner->replacementGeneration();
		const int index = channel(a)->index();
		const bool removed = op == "mixer.remove";
		if (removed ? !mixer->deleteChannel(index) : !mixer->clearChannel(index))
		{
			fail("Native mixer channel could not be removed or reset; master cannot be removed");
		}
		currentMixer(owner, mixer, generation, project);
		return {{"channel", index}, {"removed", removed}, {"channels", mixer->numChannels()}};
	}
	if (op == "mixer.route")
	{
		auto mixer = Engine::mixer();
		const int from = integer(a, "from", 1, mixer->numChannels() - 1);
		const int to = integer(a, "to", 0, mixer->numChannels() - 1);
		const double amount = number(a, "amount", 0, 1);
		if (mixer->isInfiniteLoop(from, to)) { fail("Route would create a mixer feedback cycle"); }
		if (!mixer->createChannelSend(from, to, amount)) { fail("Native mixer routing failed"); }
		return {};
	}
	if (op == "mixer.unroute")
	{
		auto mixer = Engine::mixer();
		const int from = integer(a, "from", 1, mixer->numChannels() - 1);
		const int to = integer(a, "to", 0, mixer->numChannels() - 1);
		mixer->deleteChannelSend(from, to);
		return {};
	}
	if (op.startsWith("graph.")) { return editGraph(op, a); }
	if (op.startsWith("expression.")) { return editExpression(op, a); }
	if (op == "plugin.program.list")
	{
		const int offset = a.contains("offset") ? integer(a, "offset", 0, std::numeric_limits<int>::max()) : 0;
		const int limit = a.contains("limit") ? integer(a, "limit", 1, 1024) : 128;
		return inspectNativePrograms(editablePlugin(a), offset, limit);
	}
	if (op == "plugin.preset.inspect" || op == "plugin.preset.load" || op == "plugin.preset.save")
	{
		auto plugin = editablePlugin(a);
		if (!plugin->supportsNativePresets()) { fail("This plugin does not expose native preset files"); }
		const auto path = string(a, "path");
		if (path.isEmpty() || path.contains(QChar(0))) { fail("Native preset requires a valid file path"); }
		if (op == "plugin.preset.save")
		{
			const auto format = string(a, "format");
			if (format != "fxp" && format != "fxb") { fail("Native preset format must be fxp (current program) or fxb (bank)"); }
			return nativePresetResult(plugin->saveNativePreset(path, format == "fxb"), path);
		}
		const auto source = resourcePath(a);
		return nativePresetResult(op == "plugin.preset.load" ? plugin->loadNativePreset(source) : plugin->inspectNativePreset(source), source);
	}
	if (op == "plugin.program.select")
	{
		auto plugin = editablePlugin(a);
		if (!plugin->supportsNativePrograms()) { fail("This plugin does not expose indexed native programs"); }
		const int index = integer(a, "program", 0, std::numeric_limits<int>::max());
		QString error;
		if (!plugin->selectNativeProgram(index, error)) { fail(error.isEmpty() ? "Native plugin refused program selection" : error); }
		auto result = inspectNativePrograms(plugin, index, 1);
		if (result.value("current").toInt() != index) { fail("Native plugin did not retain the requested program"); }
		return result;
	}
	if (op == "effect.inspect") { return inspectEffectChain(chain(a), a); }
	if (op == "effect.add")
	{
		const QPointer<Song> owner(song); const QPointer<EffectChain> effects(chain(a));
		auto key = pluginKey(a, Plugin::Type::Effect);
		checkInspectionWork(); currentEffectChain(owner, effects, a);
		const QPointer<Effect> native(Effect::instantiate(key.desc->name, effects.data(), &key));
		// The native factory associates a prepared effect with this chain before
		// appendEffect publishes it in the roster. Preserve effects already
		// adopted elsewhere, but clean up a refused, still-unattached candidate.
		const auto unattached = [&] {
			if (!native || (native->parent() && native->parent() != effects.data())
				|| (native->effectChain() && native->effectChain() != effects.data())) { return false; }
			return !effects || std::find(effects->effects().begin(), effects->effects().end(), native.data()) == effects->effects().end();
		};
		const auto cleanup = qScopeGuard([&] { if (unattached()) { delete native.data(); } });
		checkInspectionWork(); currentEffectChain(owner, effects, a);
		if (!native || !native->isOkay() || !unattached()) { fail("Native effect initialization or prepared ownership failed"); }
		std::unique_ptr<Effect> candidate(native.data());
		if (!effects->appendEffect(candidate)) { fail("Native effect could not be attached to the chain"); }
		return {{"effect", currentEffectIndex(owner, effects, native, a)}};
	}
	if (op == "effect.clear")
	{
		const QPointer<Song> owner(song); const QPointer<EffectChain> effects(chain(a));
		checkInspectionWork(); currentEffectChain(owner, effects, a);
		const int removed = static_cast<int>(effects->effects().size());
		if (!effects->clear()) { fail("Native effect chain could not be cleared during an active transition"); }
		currentEffectChain(owner, effects, a);
		if (!effects->effects().empty() || effects->enabledModel()->value()) { fail("Native effect chain did not remain empty and disabled"); }
		return {{"removed", removed}};
	}
	if (op == "effect.remove" || op == "effect.move")
	{
		const QPointer<Song> owner(song); const QPointer<EffectChain> effects(chain(a));
		if (effects->effects().empty()) { fail("Effect chain is empty"); }
		const auto index = integer(a, "effect", 0, static_cast<int>(effects->effects().size()) - 1);
		const QPointer<Effect> effect(effects->effects().at(index));
		if (op == "effect.remove")
		{
			checkInspectionWork(); currentEffectIndex(owner, effects, effect, a);
			const bool alive = [&] { auto detached = effects->takeEffect(effect.data()); return static_cast<bool>(detached); }();
			currentEffectChain(owner, effects, a);
			if (!alive && effect) { fail("Native effect could not be removed during an active transition or changed owners"); }
			if (effect) { fail("Native removed effect did not retire"); }
		}
		else
		{
			if (a.contains("direction") == a.contains("to")) { fail("Supply exactly one of direction or to"); }
			int destination;
			if (a.contains("to")) { destination = integer(a, "to", 0, static_cast<int>(effects->effects().size()) - 1); }
			else
			{
				const auto direction = string(a, "direction");
				if (direction == "up") { destination = std::max(index - 1, 0); }
				else if (direction == "down") { destination = std::min(index + 1, static_cast<int>(effects->effects().size()) - 1); }
				else { fail("direction must be up or down"); }
			}
			checkInspectionWork();
			if (currentEffectIndex(owner, effects, effect, a) != index) { fail("Native selected effect index changed before moving"); }
			if (!effects->moveEffect(index, destination)) { fail("Native effect could not be moved during an active transition"); }
			const int actual = currentEffectIndex(owner, effects, effect, a);
			if (actual != destination) { fail("Native moved effect did not retain the requested index"); }
			return {{"effect", actual}};
		}
		return {};
	}
	fail("Unknown native studio operation: " + op);
}

} // namespace

bool StudioCapabilities::Selection::changed(const Selection& current) const
{
	for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it)
	{
		// A deleted QObject clears its QPointer even if its address is reused.
		if (it.value().isNull() || it.value() != current.m_objects.value(it.key())) { return true; }
	}
	for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) { if (it.value() != current.m_notes.value(it.key())) { return true; } }
	return m_note && m_note != current.m_note;
}

StudioCapabilities::Selection StudioCapabilities::selection(const QString& op, const QJsonObject& a)
{
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	Selection result;
	// Resolve each selector separately, without refreshing plugins, creating
	// automation, validating a future call, or walking whole project inventories.
	// Invalid/unresolved targets are left to the normal operation validator.
	const auto bind = [&](const QString& key, const auto& resolve) {
		try { if (QObject* object = resolve()) { result.m_objects.insert(key, object); } }
		catch (const std::invalid_argument&) {}
	};
	if (op == "note.selection.copy" || op == "note.selection.shift" || op == "note.selection.quantize" || op == "note.selection.reverse" || op == "note.selection.length" || op == "note.selection.glue" || op == "note.selection.split" || op == "note.selection.knife" || op == "note.selection.remove" || op == "note.selection.edit")
	{
		bind("sourceTrack", [&] { return track(a); }); bind("sourceClip", [&] { return clip(a); });
		if (op == "note.selection.copy")
		{
			auto target = a; if (a.contains("targetContainer")) { target.insert("container", a.value("targetContainer")); }
			target.insert("track", a.value("targetTrack")); target.insert("clip", a.value("targetClip"));
			bind("targetTrack", [&] { return track(target); }); bind("targetClip", [&] { return clip(target); });
		}
		const auto notes = a.value("notes").toArray();
		for (int index = 0; index < notes.size() && index < 4096; ++index)
		{
			checkInspectionWork(); auto selected = a; selected.insert("note", notes.at(index));
			try { result.m_notes.insert(QString("notes/%1").arg(index), note(selected)->selectionIdentity()); } catch (const std::invalid_argument&) {}
		}
		return result;
	}
	if (op == "clip.selection.copy")
	{
		const auto items = a.value("items").toArray();
		for (int index = 0; index < items.size() && index < 4096; ++index)
		{
			checkInspectionWork(); if (!items.at(index).isObject()) { continue; } const auto item = items.at(index).toObject(); const auto key = QString("items/%1/").arg(index);
			bind(key + "sourceTrack", [&] { return track(item); }); bind(key + "sourceClip", [&] { return clip(item); });
			auto target = item; if (item.contains("targetContainer")) { target.insert("container", item.value("targetContainer")); }
			if (item.contains("targetTrack")) { target.insert("track", item.value("targetTrack")); }
			bind(key + "targetTrack", [&] { return track(target); });
			if (item.contains("targetClip")) { target.insert("clip", item.value("targetClip")); bind(key + "targetClip", [&] { return clip(target); }); }
		}
		return result;
	}
	if (op == "audio.backend.route")
	{
		// Native master channel indices are unrelated to project mixer indices.
		// JACK is a QObject: replacement clears this identity and invalidates a
		// queued route instead of applying an old intent to the new device.
		bind("audioDevice", [&] { return dynamic_cast<QObject*>(Engine::audioEngine()->audioDev()); });
		return result;
	}
	if (op.startsWith("audio.bus."))
	{
		bind("audioDevice", [&] { return dynamic_cast<QObject*>(Engine::audioEngine()->audioDev()); });
		bind("track", [&] { return track(a); });
		return result; // A bus's stereo channel index is not a mixer channel selector.
	}
	if (a.contains("track")) { bind("track", [&] { return track(a); }); }
	if (a.contains("clip") && op != "clip.fromSlices") { bind("clip", [&] { return clip(a); }); }
	if (a.contains("note"))
	{
		try { result.m_note = note(a)->selectionIdentity(); }
		catch (const std::invalid_argument&) {}
	}
	if (op == "clip.clone" || op == "clip.fromSlices")
	{
		auto target = a;
		if (a.contains("targetContainer")) { target.insert("container", a.value("targetContainer")); }
		if (a.contains("targetTrack")) { target.insert("track", a.value("targetTrack")); }
		bind("targetTrack", [&] { return track(target); });
		if (op == "clip.fromSlices") { bind("targetClip", [&] { return clip(target); }); }
		else if (a.contains("targetClip"))
		{
			target.insert("clip", a.value("targetClip"));
			bind("targetClip", [&] { return clip(target); });
		}
	}
	if (a.contains("channel")) { bind("channel", [&] { return &channel(a)->m_volumeModel; }); }
	if (a.contains("effect"))
	{
		bind("effect", [&] {
			const auto& effects = chain(a)->effects();
			return effects.at(integer(a, "effect", 0, static_cast<int>(effects.size()) - 1));
		});
	}
	if (a.contains("controller")) { bind("controller", [&] { return controller(a); }); }
	if (a.contains("parameter"))
	{
		bind("parameter", [&]() -> AutomatableModel* {
			const auto id = integer(a, "parameter", 1, 2147483647);
			auto model = dynamic_cast<AutomatableModel*>(Engine::projectJournal()->journallingObject(id));
			// Saved aliases are not current parameter handles. Actual membership
			// and editability are still checked by the operation's native resolver.
			return model && model->id() == static_cast<jo_id_t>(id) ? model : nullptr;
		});
	}
	if (op == "parameter.link" && a.contains("peer"))
	{
		bind("peer", [&]() -> AutomatableModel* {
			const auto id = integer(a, "peer", 1, 2147483647);
			auto model = dynamic_cast<AutomatableModel*>(Engine::projectJournal()->journallingObject(id));
			return model && model->id() == static_cast<jo_id_t>(id) ? model : nullptr;
		});
	}
	if (op == "mixer.route" || op == "mixer.unroute" || op == "parameter.list")
	{
		for (const auto key : {"from", "to"})
		{
			if (a.contains(key)) { bind(key, [&] { return &channel({{"channel", a.value(key)}})->m_volumeModel; }); }
		}
		if (a.contains("from") && a.contains("to"))
		{
			bind("route", [&] {
				const int last = Engine::mixer()->numChannels() - 1;
				return Engine::mixer()->channelSendModel(integer(a, "from", 0, last), integer(a, "to", 0, last));
			});
		}
	}
	if (a.contains("global"))
	{
		bind("global", [&] {
			const auto& clips = Engine::getSong()->globalAutomationTrack()->getClips();
			return clips.at(integer(a, "global", 0, static_cast<int>(clips.size()) - 1));
		});
	}
	if ((op == "transport.play" && a.value("playMode").toString() == "pattern")
		|| (op == "project.import" && a.value("container").toString() == "pattern"))
	{
		bind("pattern", [&] {
			const auto store = Engine::patternStore();
			const int index = a.contains("pattern") ? integer(a, "pattern", 0, store->numOfPatterns() - 1) : store->currentPattern();
			return PatternTrack::findPatternTrack(index);
		});
	}
	if ((op.startsWith("instrument.") && op != "instrument.load") || op.startsWith("host.")
		|| op.startsWith("graph.") || op.startsWith("expression.") || op.startsWith("plugin.program.")
		|| op.startsWith("plugin.preset.") || op == "clip.fromSlices")
	{
		bind("plugin", [&] { return editablePlugin(a); });
	}
	return result;
}

QJsonArray StudioCapabilities::schema()
{
	return {
		specification("studio.capabilities", "Discover native operation schemas", false, {}, {}),
		specification("studio.resources.inspect", "Measure LMMS process CPU time and resident memory; unavailable counters are null, child plugins and external inference are excluded", false, {}, {}),
		specification("asset.roots", "Discover current native sample/preset/project/plugin-bank directory aliases and metadata; local refers to the current project/checkpoint directory. Does not create paths or verify content", false, {"names"}, {}),
		specification("asset.search", "Search native filename tokens and suffixes under one explicit root; paged metadata candidates only. Keep root/query/filters/budget and searchGeneration fixed while paging. scanComplete=false requires narrower directories or another budget; returned paths still need checked native loading", false, {"root", "query", "extensions", "kind", "recursive", "followSymlinks", "hidden", "maximumEntries", "offset", "limit", "names", "searchGeneration"}, {"root"}),
		specification("project.inspect", "Inspect native project sections with count/byte-limited pages. resources inventories declared instrument/effect files, sample sources, rack LFO waves and unavailable effects across Song/PatternStore/Mixer; resourceGeneration rejects changed observations, names=false previews text. Filesystem presence does not prove decoding, portability or sound. Other sections include summary, tracks, clips, notes, channels, routes, controllers, globalAutomations and warnings. Warnings use diagnosticGeneration and messageOffset/messageLimit for UTF-16 chunks. No arguments retains the full legacy snapshot", false, {"section", "container", "track", "clip", "offset", "limit", "names", "messageOffset", "messageLimit", "diagnosticGeneration", "resourceGeneration"}, {}),
		specification("transport.inspect", "Observe native transport positions, recording state and actual backend kind", false, {}, {}),
		specification("audio.backend.list", "Discover compiled native audio/MIDI backends without probing hardware; both support exact process-local runtime selection", false, {}, {}),
		specification("audio.backend.select", "Select exactly one native audio backend using loaded settings; stopped/disarmed transport required; restores previous device on failure; process-local, MIDI unchanged", false, {"name"}, {"name"}),
		specification("audio.backend.inspect", "Discover native master input sources or output destinations and their connected zero-based channels; paged current graph, separate from project mixer routes and startup settings; no device-receipt proof", false, {"direction", "offset", "limit"}, {"direction"}),
		specification("audio.backend.route", "Connect/disconnect one exact external audio endpoint on a zero-based native master channel; stopped/disarmed transport required; pauses and joins native DSP outside the model lock, invalidates queued input for input changes and verifies mutation/rollback; process-local", false, {"direction", "channel", "port", "enabled"}, {"direction", "channel", "port", "enabled"}),
		specification("audio.bus.inspect", "Inspect a native instrument/sample track's stereo output ports, pending publication/retirement and external destinations; after track gain/pan/effects, before mixer/master; current graph only, no device-receipt proof", false, {"container", "track", "offset", "limit"}, {"track"}),
		specification("audio.bus.configure", "Enable/disable and synchronize native track audio ports while stopped/disarmed; port work joins callbacks outside model edits; native failures can leave a request pending, so inspect before routing; process-local", false, {"container", "track", "enabled"}, {"track", "enabled"}),
		specification("audio.bus.route", "Connect/disconnect one published native track output channel (0 left, 1 right) to an exact external audio destination; stopped/disarmed transport required; verified edge change and rollback; process-local", false, {"container", "track", "channel", "port", "enabled"}, {"track", "channel", "port", "enabled"}),
		specification("audio.processing.inspect", "Observe the native DSP failure latch and retry eligibility; backend health is separate", false, {}, {}),
		specification("audio.processing.reset", "Permit native DSP retry after project repair; requires stopped/disarmed transport and no export or unfinished take", false, {}, {}),
		specification("audio.device.restore", "Retry retained native audio-device restoration or failed export transport cleanup; rejects active render execution", false, {}, {}),
		specification("midi.inspect", "Inspect native MIDI models, delivery and subscriptions; select an instrument track or a parameter with a direct MIDI controller connection", false, {"container", "track", "parameter"}, {}),
		specification("midi.backend.inspect", "Discover the current backend's client-wide native MIDI graph; input lists external sources, output lists destinations; paged snapshots do not prove device receipt", false, {"direction", "offset", "limit"}, {"direction"}),
		specification("midi.backend.select", "Select exactly one native MIDI backend using loaded settings; stopped/disarmed transport required; preserves port models and subscriptions and requires input resets; abandonFailedOutput explicitly permits discarding unconfirmed output only for an eligible terminal backend, reported in outputAbandonment; a commit can retain audio restoration work; process-local, no graph replay", false, {"name", "abandonFailedOutput"}, {"name"}),
		specification("midi.backend.route", "Connect/disconnect one exact external MIDI endpoint for the whole client; requires stopped/disarmed transport, drains output cleanup, verifies the graph, and leaves input delivery requiring reset after input changes; process-local", false, {"direction", "port", "enabled"}, {"direction", "port", "enabled"}),
		specification("midi.backend.reconnect", "Retain an already connected exact client-wide MIDI endpoint for automatic recovery while stopped/disarmed, or forget its process-local preference without changing the graph, even offline; not project-saved", false, {"direction", "port", "enabled"}, {"direction", "port", "enabled"}),
		specification("midi.record", "Append bounded native MIDI input notes and optional per-note pitch; requires stopped/disarmed transport, active MIDI input and a running audio clock", true, {"container", "track", "clip", "durationMs", "timeoutMs", "maximumNotes", "quantization", "playMode", "position", "recordPitch", "maximumPitchPoints"}, {"track", "clip", "durationMs"}),
		specification("midi.input.reset", "Reset failed MIDI delivery for a track or directly connected parameter; requires stopped/disarmed transport and active backend when observed", false, {"container", "track", "parameter"}, {}),
		specification("midi.configure", "Configure a track's native MIDI I/O; a parameter selector instead configures its direct MIDI controller's readable, inputChannel (0..16) and inputController (-1..127) settings", true, {"container", "track", "parameter", "readable", "writable", "inputChannel", "outputChannel", "inputController", "outputController", "fixedInputVelocity", "fixedOutputVelocity", "fixedOutputNote", "outputProgram", "baseVelocity", "outputSource"}, {}),
		specification("midi.subscribe", "Set named MIDI subscription intent for a track or directly connected parameter; parameters support input only; enabling requires an available non-raw port", true, {"container", "track", "parameter", "direction", "port", "enabled"}, {"direction", "port", "enabled"}),
		specification("transport.play", "Start native song, pattern or MIDI clip playback on the running backend; dummy produces no sound output", false, {"playMode", "pattern", "container", "track", "clip", "loop"}, {}),
		specification("transport.pause", "Set native pause state explicitly; requires existing playback and no recording", false, {"paused"}, {"paused"}),
		specification("transport.stop", "Stop native playback using its timeline stop behaviour; rejects recording", false, {}, {}),
		specification("transport.seek", "Seek a native timeline in ticks; active playback requires matching playMode", false, {"playMode", "position"}, {"position"}),
		specification("project.new", "Create a native project; summary=true returns a bounded overview", true, {"empty", "summary"}, {}),
		specification("project.load", "Load an existing LMMS project; summary=true avoids returning its complete contents", true, {"path", "summary"}, {"path"}),
		specification("project.save", "Save using LMMS project serialization", false, {"path", "withResources"}, {"path"}),
		specification("project.import", "Import through native LMMS filters into song tracks or a selected pattern column; summary=true returns a bounded overview", true, {"path", "container", "pattern", "summary"}, {"path"}),
		specification("project.exportMidi", "Export native editable notes and pattern placements as MIDI; static tempo, pitch and gain, approximate step lengths, no rendered audio or automation", false, {"path"}, {"path"}),
		specification("song.set", "Edit global tempo, gain and pitch", true, {"tempo", "masterVolume", "masterPitch"}, {}),
		specification("timeline.set", "Set native loop markers", true, {"begin", "end", "enabled"}, {"begin", "end", "enabled"}),
		specification("track.add", "Add a song or pattern-store track", true, {"container", "type", "name"}, {"type"}),
		specification("track.edit", "Rename, mute, solo or move a track to final zero-based row to within its container; native solo isolation and mute restoration work without views and may override requested mute; pattern content and selection follow moves; rediscover indices afterwards", true, {"container", "track", "name", "muted", "solo", "to"}, {"track"}),
		specification("track.remove", "Remove an idle native track; pattern owners also remove their native column. Native note/sample/automation recording must be completed or disarmed. Restore solo masks before teardown; rediscover indices afterwards", true, {"container", "track"}, {"track"}),
		specification("track.clone", "Clone a native track and return its current registered row after native completion notifications; rediscover indices afterwards", true, {"container", "track"}, {"track"}),
		specification("clip.add", "Create an arrangement clip", true, {"container", "track", "position", "name"}, {"track", "position"}),
		specification("clip.edit", "Edit native clip geometry/name/stored mute through checked virtual setters with one enabled complete clip checkpoint. Manual length/offset disable automatic sizing; enable autoResize separately to refresh native length/offset. PatternStore owns cell positions; native recording must be idle. Returns position/length/offset/autoResize/changed; explicit automatic refresh is an applied edit even if visible values match", true, {"container", "track", "clip", "position", "length", "offset", "autoResize", "name", "muted"}, {"track", "clip"}),
		specification("clip.split", "Split at a relative tick position using native non-destructive offsets", true, {"container", "track", "clip", "position"}, {"track", "clip", "position"}),
		specification("clip.remove", "Remove an arrangement clip", true, {"container", "track", "clip"}, {"track", "clip"}),
		specification("clip.clone", "Copy native clip state to a compatible track; arrangement copies require position, pattern cells require targetClip and replace:true", true, {"container", "track", "clip", "position", "targetContainer", "targetTrack", "targetClip", "replace"}, {"track", "clip"}),
		specification("clip.selection.copy", "Copy 1..4096 distinct native source clips together, preserving resident audio, notes and closed linked-control groups across selected clips. Each item has source container/track/clip and optional targetContainer/targetTrack; arrangement destinations require position, pattern destinations require targetClip and replace:true. Receipts follow input order; copying does not remap automation targets", true, {"items"}, {"items"}),
		specification("sample.load", "Decode an audio asset into a native sample clip; empty path clears its source and offset. preserveSettings defaults true for unresolved clips, otherwise false; preserving retains placement/offset/length, gain, reversal and known source bounds, refusing shorter replacements", true, {"container", "track", "clip", "path", "preserveSettings"}, {"track", "clip", "path"}),
		specification("sample.edit", "Edit source frame window, signed linear gain or reversal; bounds use final playback order", true, {"container", "track", "clip", "startFrame", "endFrame", "gain", "reversed"}, {"track", "clip"}),
		specification("note.selection.copy", "Copy 1..4096 distinct source note indices into an existing MIDI clip, preserving native note/curve content and closed inline control groups. position is relative to the destination clip and places the earliest selected note; relative timing stays intact. Receipts follow request order; source notes are retained", true, {"container", "track", "clip", "notes", "targetContainer", "targetTrack", "targetClip", "position"}, {"track", "clip", "notes", "targetTrack", "targetClip", "position"}),
		specification("note.selection.shift", "Shift 1..4096 distinct current MIDI note indices in place by semitones and/or ticks, retaining native note identities and detuning graphs. Keys clamp individually to native bounds; a negative time shift stops the earliest selected note at zero. Positive endpoint overflow refuses the whole edit. Receipts follow request order; rediscover indices after sorting", true, {"container", "track", "clip", "notes", "semitones", "ticks"}, {"track", "clip", "notes"}),
		specification("note.selection.quantize", "Quantize 1..4096 distinct current MIDI note indices in place using native rounding and a tick grid. mode is both (default), positions or lengths. Zero-length notes are skipped; a rounded zero length becomes one grid unit. Note/control identities and links remain. Endpoint overflow refuses the whole edit; receipts follow request order with current indices, positions and lengths", true, {"container", "track", "clip", "notes", "grid", "mode"}, {"track", "clip", "notes", "grid"}),
		specification("note.selection.reverse", "Reverse positions of 1..4096 distinct current MIDI note indices in place using native earliest start/latest end geometry and each note length. Keys, lengths, note/control identities and stored inline curve points remain. Native bounds refuse the whole edit before writing; receipts follow request order with current sorted indices, positions and lengths", true, {"container", "track", "clip", "notes"}, {"track", "clip", "notes"}),
		specification("note.selection.length", "Adjust 1..4096 distinct current MIDI note lengths in place. mode maximum/minimum requires signed native length; trim/fill forbids length and uses all clip note starts as references. Trim only shortens to the next later start; fill only extends to the first start at/after current end or the selected phrase final bar. Keys, positions, types, control identities and curve points remain. Endpoint overflow refuses the whole edit; receipts follow request order", true, {"container", "track", "clip", "notes", "mode", "length"}, {"track", "clip", "notes", "mode"}),
		specification("note.selection.glue", "Glue 1..4096 distinct selected MIDI note indices of the same key when overlapping/touching, using native length geometry. Each group's earliest note survives with its own settings/controls; equal-start ties keep native clip order. Consumed notes and their own control state retire normally, without merging their curves/controllers. Sources map to survivors in request-order receipts; rediscover indices afterward", true, {"container", "track", "clip", "notes"}, {"track", "clip", "notes"}),
		specification("note.selection.split", "Split 1..4096 distinct current MIDI note indices at a clip-relative tick position, only strictly inside positive-length notes. Original left notes/controls retain identity; fresh right notes copy native content and complete closed inline groups among crossing notes. Right curve points retain native relative timing without resampling. Noncrossing notes stay unchanged. Receipts follow request order with current left/right indices; rediscover afterward", true, {"container", "track", "clip", "notes", "position"}, {"track", "clip", "notes", "position"}),
		specification("note.selection.knife", "Cut 1..4096 distinct selected MIDI notes along a native knife line fromPosition/fromKey to toPosition/toKey. Uses native note-centre intercept/truncation, exclusive minimum and inclusive maximum endpoint key; equal endpoint keys do nothing. Endpoints accept native signed integers, including off-clip coordinates. keepLongest defaults false; true retains the longer half in the original note, left on ties. Both-halves mode copies closed crossing control groups; originals retain controls. Receipts follow request order with cut/kept and current indices", true, {"container", "track", "clip", "notes", "fromPosition", "fromKey", "toPosition", "toKey", "keepLongest"}, {"track", "clip", "notes", "fromPosition", "fromKey", "toPosition", "toKey"}),
		specification("note.add", "Add an explicitly timed native MIDI note", true, {"container", "track", "clip", "position", "length", "key", "volume", "panning"}, {"track", "clip", "position", "length", "key"}),
		specification("note.batch.add", "Construct 1..4096 native regular notes from explicit scalar input objects in one prepared insertion and one enabled complete clip checkpoint. Every item requires position/positive length/key; optional volume/panning use native defaults and bounded integer conversion. Identical inputs create distinct unselected notes; old note/control content remains. Returns added and input-order input/current note/scalar receipts; rediscover indices afterward", true, {"container", "track", "clip", "items"}, {"track", "clip", "items"}),
		specification("note.edit", "Apply supplied absolute fields to one native note in place with one enabled complete clip checkpoint. All arguments and final endpoints are admitted before writes. Omitted fields/type/selection/control content remain. Returns changed, sourceNote, current note index and native scalar values; rediscover indices after position/key changes", true, {"container", "track", "clip", "note", "position", "length", "key", "volume", "panning"}, {"track", "clip", "note"}),
		specification("note.selection.edit", "Apply the same supplied absolute position/length/key/volume/panning to 1..4096 distinct native notes in one prepared in-place edit. Omitted fields/type/selection/control content remain; complete native ordering and one enabled clip checkpoint are retained. Empty field sets are native no-ops. Returns changed and request-order original sourceNote/current note/scalar receipts; rediscover indices afterward", true, {"container", "track", "clip", "notes", "position", "length", "key", "volume", "panning"}, {"track", "clip", "notes"}),
		specification("note.remove", "Remove a MIDI note", true, {"container", "track", "clip", "note"}, {"track", "clip", "note"}),
		specification("note.selection.remove", "Remove 1..4096 distinct native notes in one prepared edit with one enabled complete clip checkpoint. Remaining notes retain identities/control content; deleted controls detach normally. Step count remains unchanged. Returns removed and original sourceNote receipts in request order; rediscover current indices afterward", true, {"container", "track", "clip", "notes"}, {"track", "clip", "notes"}),
		specification("note.clear", "Remove all notes currently in one native MIDI clip in one prepared edit, retaining the step count. Returns removed; an empty clip is a native no-op. Enabled native history requires complete closed clip coverage", true, {"container", "track", "clip"}, {"track", "clip"}),
		specification("note.detuning.inspect", "Inspect native per-note pitch automation: full, summary or points section; points continues with fromPosition/nextPosition; optional position evaluates without creating a curve", false, {"container", "track", "clip", "note", "position", "section", "fromPosition", "limit"}, {"track", "clip", "note"}),
		specification("note.detuning.point", "Write native detuning in semitones at a tick relative to note start; supports discontinuities and locked tangents", true, {"container", "track", "clip", "note", "position", "value", "outValue", "progression", "tension", "inTangent", "outTangent", "lockedTangents"}, {"track", "clip", "note", "position", "value"}),
		specification("note.detuning.edit", "Configure interpolation of an existing per-note detuning change: 0 discrete, 1 linear, 2 cubic Hermite; tension 0..1", true, {"container", "track", "clip", "note", "progression", "tension", "summary"}, {"track", "clip", "note"}),
		specification("note.detuning.removePoint", "Remove an exact native detuning point using prepared curve editing; finish recording or dragging first", true, {"container", "track", "clip", "note", "position", "summary"}, {"track", "clip", "note", "position"}),
		specification("note.detuning.clear", "Clear prepared native per-note pitch automation, retain geometry and return to zero detuning; finish recording or dragging first", true, {"container", "track", "clip", "note", "summary"}, {"track", "clip", "note"}),
		specification("step.set", "Enable or disable a native beat cell on the current row (at most 4096 steps). Enable retains the first existing Step note or inserts a native default Step; disable clears all Step duplicates at the cell, retaining regular/off-grid notes. Uses one complete enabled native undo checkpoint and prepared publication. Returns step/enabled/changed/removed and current note/position when enabled; rediscover note indices afterward", true, {"container", "track", "clip", "step", "enabled"}, {"track", "clip", "step", "enabled"}),
		specification("step.resize", "Resize one current MIDI row to 1..4096 steps in whole native bars. Shrinking clears every Step note on removed grid cells, including duplicates; regular notes, off-grid steps and steps outside the original prefix remain. Count and prepared roster publish together with one enabled native undo checkpoint. Growth adds empty cells. Returns fromCount/count/removed/changed; rediscover note indices after shrinking", true, {"container", "track", "clip", "count"}, {"track", "clip", "count"}),
		specification("step.clone", "Double the current native step count up to 4096 and copy the first Step note at each original grid position into the new half. Complete note/curve/control content and closed copied groups are retained independently; regular notes and original notes remain. Existing destination step collisions refuse before editing. Empty rows still double. Receipts follow source-step order with original source and current copy indices; new copies are unselected", true, {"container", "track", "clip"}, {"track", "clip"}),
		specification("instrument.load", "Replace a native instrument or hosted subplugin through native teardown/construction, refuse overlapping replacement or selected-track recording, and return the surviving current track index after callbacks", true, {"container", "track", "instrument", "attributes"}, {"track", "instrument"}),
		specification("instrument.inspect", "Discover native sound-design controls, graphs, resources and flags; parameters default to 128 per page, maximum 1024; follow parameterPage.nextOffset with offset while hasMore", false, {"container", "track", "choices", "offset", "limit"}, {"track"}),
		specification("graph.list", "Discover native waveform and effect curves with keys, lengths, ranges, units, step and supported actions; select instrument by track/container, or an effect by effect plus track/container or channel", false, {"container", "track", "channel", "effect"}, {}),
		specification("expression.list", "Discover native formula keys, current source, symbols and semantics; select an instrument by track/container or an effect by effect plus track/container or channel", false, {"container", "track", "channel", "effect"}, {}),
		specification("plugin.program.list", "Inspect indexed native plugin programs without selecting them; names can be unavailable; select an instrument or an effect, then page by offset and limit", false, {"container", "track", "channel", "effect", "offset", "limit"}, {}),
		specification("plugin.program.select", "Select a zero-based native plugin program, preserving LMMS automation targets; this changes the plugin sound state and is separate from MIDI bank/program selection", true, {"container", "track", "channel", "effect", "program"}, {"program"}),
		specification("plugin.preset.inspect", "Validate a native FXP/FXB file against the selected instrument or effect and inspect its format and digest without applying it", false, {"container", "track", "channel", "effect", "path"}, {"path"}),
		specification("plugin.preset.load", "Load a native FXP/FXB program or bank into the selected instrument or effect; content determines the format; LMMS parameter identities and automation remain attached", true, {"container", "track", "channel", "effect", "path"}, {"path"}),
		specification("plugin.preset.save", "Atomically export the current native program (format fxp) or bank (format fxb); capture restores the active program and is checkpointed because bank enumeration may select programs", true, {"container", "track", "channel", "effect", "path", "format"}, {"path", "format"}),
		specification("expression.validate", "Compile and preview a proposed native formula without publishing it; returns valid/error and optional preview samples; validation is limited to the plugin's documented preview context", false, {"container", "track", "channel", "effect", "expression", "source", "preview"}, {"expression", "source"}),
		specification("expression.set", "Validate and publish a native formula with plugin-specific DSP and serialization consequences; discover empty-source, waveform and output semantics first", true, {"container", "track", "channel", "effect", "expression", "source", "preview"}, {"expression", "source"}),
		specification("graph.inspect", "Read a native graph's metadata and samples; rediscover keys after changing the instrument or effect", false, {"container", "track", "channel", "effect", "graph"}, {"graph"}),
		specification("graph.set", "Replace exactly the visible native graph samples using finite values in its discovered range; native step snapping applies and any hidden waveform tail is retained", true, {"container", "track", "channel", "effect", "graph", "values"}, {"graph", "values"}),
		specification("graph.transform", "Apply a discovered native graph action; shift requires integer degrees; transformations preserve native algorithm and amplitude semantics", true, {"container", "track", "channel", "effect", "graph", "action", "degrees"}, {"graph", "action"}),
		specification("graph.load", "Decode a native audio file into an importable waveform graph using LMMS resampling; samples are embedded in plugin state for save, undo and recovery", true, {"container", "track", "channel", "effect", "graph", "path"}, {"graph", "path"}),
		specification("host.inspect", "Inspect plugins inside the current instrument's native host and management availability", false, {"container", "track"}, {"track"}),
		specification("host.plugin.add", "Add a plugin to the existing native host; format availability depends on the installed host; uniqueId is a decimal string", true, {"container", "track", "format", "path", "name", "label", "binary", "uniqueId"}, {"track", "format"}),
		specification("host.plugin.remove", "Remove a plugin from the native host; rediscover indexes afterwards", true, {"container", "track", "hostedPlugin"}, {"track", "hostedPlugin"}),
		specification("host.plugin.rename", "Rename a plugin inside the native host", true, {"container", "track", "hostedPlugin", "name"}, {"track", "hostedPlugin", "name"}),
		specification("host.plugin.swap", "Swap two plugin positions inside the native host; rediscover indexes afterwards", true, {"container", "track", "hostedPlugin", "targetPlugin"}, {"track", "hostedPlugin", "targetPlugin"}),
		specification("host.plugin.active", "Activate or deactivate a plugin inside the native host", true, {"container", "track", "hostedPlugin", "active"}, {"track", "hostedPlugin", "active"}),
		specification("host.parameter.list", "Inspect paginated controls inside a hosted plugin; native indices require rediscovery after layout changes or restore", false, {"container", "track", "hostedPlugin", "offset", "limit"}, {"track", "hostedPlugin"}),
		specification("host.parameter.inspect", "Inspect a hosted control and paginated numeric scale points, including fractional choices", false, {"container", "track", "hostedPlugin", "hostedParameter", "choiceOffset", "choiceLimit"}, {"track", "hostedPlugin", "hostedParameter"}),
		specification("host.parameter.set", "Write a writable hosted control within native range/type limits and report native readback; no acceptance status is supplied by the host", true, {"container", "track", "hostedPlugin", "hostedParameter", "value"}, {"track", "hostedPlugin", "hostedParameter", "value"}),
		specification("host.catalog.start", "Start native Carla discovery; scan state and results belong to this instrument instance; defaults maximumPlugins 4096 and timeoutMs 60000", false, {"container", "track", "format", "binary", "path", "discoveryTool", "maximumPlugins", "timeoutMs"}, {"track", "format"}),
		specification("host.catalog.poll", "Advance native discovery once and page retained candidates; a finished scan does not prove plugin compatibility", false, {"container", "track", "scan", "offset", "limit"}, {"track", "scan"}),
		specification("host.catalog.stop", "Stop a native discovery scan and retain its collected metadata until the next scan or instrument destruction", false, {"container", "track", "scan"}, {"track", "scan"}),
		specification("host.patchbay.inspect", "Refresh Carla Patchbay's internal audio/MIDI/CV graph; treated as a project edit because native refresh can remove illegal connections", true, {"container", "track"}, {"track"}),
		specification("host.patchbay.connect", "Connect compatible output and input endpoints in Carla Patchbay and confirm the route in a fresh graph", true, {"container", "track", "sourceGroup", "sourcePort", "destinationGroup", "destinationPort"}, {"track", "sourceGroup", "sourcePort", "destinationGroup", "destinationPort"}),
		specification("host.patchbay.disconnect", "Disconnect a Carla Patchbay route by its current endpoints; native connection IDs are not stable across refreshes", true, {"container", "track", "sourceGroup", "sourcePort", "destinationGroup", "destinationPort"}, {"track", "sourceGroup", "sourcePort", "destinationGroup", "destinationPort"}),
		specification("instrument.slices.inspect", "Inspect native slicer source frames, normalized boundaries, tempo state and addressable MIDI keys", false, {"container", "track"}, {"track"}),
		specification("instrument.slices.analyze", "Run the instrument's native onset detector; optional estimateTempo uses its native duration heuristic", true, {"container", "track", "estimateTempo"}, {"track"}),
		specification("instrument.slices.edit", "Replace native slice boundaries with strictly increasing normalized points including endpoints 0 and 1", true, {"container", "track", "points"}, {"track", "points"}),
		specification("instrument.slices.midi", "Preview the instrument's native slice MIDI timing without modifying the project", false, {"container", "track"}, {"track"}),
		specification("clip.fromSlices", "Insert native slicer MIDI into a target clip; target track/container default to the slicer source; replace defaults false", true, {"container", "track", "clip", "targetContainer", "targetTrack", "position", "replace"}, {"track", "clip"}),
		specification("instrument.program.list", "Inspect programs from the loaded native sound bank; supports MIDI bank/name filtering and pagination", false, {"container", "track", "bank", "query", "offset", "limit"}, {"track"}),
		specification("instrument.program.select", "Select an existing native bank/program pair with checked failure preservation", true, {"container", "track", "bank", "program"}, {"track", "bank", "program"}),
		specification("instrument.tuning.inspect", "Inspect native tuning and nominal frequencies for one MIDI key or all 128 keys", false, {"container", "track", "key"}, {"track"}),
		specification("tuning.list", "Inspect the project's native scale/keymap library; details includes intervals and mapping entries", false, {"details"}, {}),
		specification("tuning.scale.inspect", "Inspect a native scale slot, implicit unison and all interval forms", false, {"slot"}, {"slot"}),
		specification("tuning.scale.set", "Replace a native scale using Scala pitch lines excluding implicit 1/1; blank intervals make a constant reference scale", true, {"slot", "name", "intervals"}, {"slot", "name", "intervals"}),
		specification("tuning.scale.load", "Load a checked Scala scale file into a native project slot", true, {"slot", "path"}, {"slot", "path"}),
		specification("tuning.scale.save", "Atomically export the native scale slot as Scala .scl", false, {"slot", "path"}, {"slot", "path"}),
		specification("tuning.keymap.inspect", "Inspect a native keymap, mapped negative degrees, unmapped entries and formal octave", false, {"slot"}, {"slot"}),
		specification("tuning.keymap.set", "Replace a native keymap using integer/x lines; omitted MIDI/reference/origin fields retain slot settings", true, {"slot", "name", "mapping", "firstKey", "lastKey", "middleKey", "baseKey", "baseFrequency", "octaveDegree", "linearFromMiddle"}, {"slot", "name", "mapping"}),
		specification("tuning.keymap.load", "Load a checked Scala .kbm including its formal octave and linear middle origin", true, {"slot", "path"}, {"slot", "path"}),
		specification("tuning.keymap.save", "Atomically export a native keyboard map as Scala .kbm", false, {"slot", "path"}, {"slot", "path"}),
		specification("instrument.shaping.waveform.set", "Decode or clear a native volume/cutoff/resonance envelope-LFO user wave, preserving controls and existing selection unless select=true; missing references remain editable and require checked repair", true, {"container", "track", "target", "path", "select"}, {"track", "target", "path"}),
		specification("instrument.resource.set", "Load or clear a checked native instrument resource; empty path clears; select activates its native selector when present", true, {"container", "track", "resource", "path", "select"}, {"track", "resource", "path"}),
		specification("preset.load", "Restore native instrument preset settings and linked controls while preserving destination clips, mixer routing and mute/solo state; refuse overlapping restoration or selected-track note recording; return the surviving current track index", true, {"container", "track", "path"}, {"track", "path"}),
		specification("preset.save", "Serialize a native instrument preset with source identity/value checks, atomically commit after final validation and return committed, actual path and current track index; preserve live state and automatic MIDI assignment", false, {"container", "track", "path"}, {"track", "path"}),
		specification("plugin.list", "Discover installed native plugin descriptors and subplugin keys", false, {"subplugins"}, {}),
		specification("parameter.list", "Page native controls including chain bypass, effects and sends; filters apply before paging; default 128, maximum 1024, byte limits may return fewer; follow nextOffset while hasMore", false, {"container", "track", "channel", "effect", "from", "to", "controller", "choices", "offset", "limit"}, {}),
		specification("parameter.inspect", "Inspect a current native handle, type, model path and tempo/controller state; choice and linked-peer pages default to 128, maximum 1024. Follow choicePage.nextOffset with choiceOffset and linkedPage.nextOffset with linkedOffset while hasMore. Linked peer IDs exclude this model; complete projects, closed track/clip copies and native clipboard/drag selections require every member to serialize; inline curves and per-object journals remain incomplete", false, {"parameter", "choices", "choiceOffset", "choiceLimit", "linkedOffset", "linkedLimit"}, {"parameter"}),
		specification("parameter.set", "Set a native parameter within its actual bounds", true, {"parameter", "value"}, {"parameter", "value"}),
		specification("parameter.link", "Join two current native control groups; parameter inherits peer's stored raw value fitted to its native range, and existing native controller precedence remains. All members must be discoverable writable supported inputs. Feedback rejects linking; complete project checkpoints must serialize every member", true, {"parameter", "peer"}, {"parameter", "peer"}),
		specification("parameter.unlink", "Detach the selected writable native control from its ring, retaining stored values and controller bindings; remaining members stay linked. Native per-object journal topology remains incomplete", true, {"parameter"}, {"parameter"}),
		specification("parameter.sync", "Set native tempo sync: 0 off, 1 double whole, 2 whole, 3 half, 4 quarter, 5 eighth, 6 sixteenth, 7 thirty-second, 8 custom", true, {"parameter", "mode", "customNumerator", "customDenominator"}, {"parameter", "mode"}),
		specification("controller.list", "Inspect the complete native rack with editable parameter handles; use project.inspect controller pages for bounded inventories; refuses retired or changing roster identities", false, {}, {}),
		specification("controller.waveform.set", "Decode or clear a native LFO waveform without a dialog; empty path clears, optional select activates user waveform", true, {"controller", "path", "select"}, {"controller", "path"}),
		specification("controller.add", "Create a native LFO rack controller and return its surviving current index; summary=true avoids full rack inspection", true, {"type", "name", "summary"}, {"type"}),
		specification("controller.edit", "Rename a native rack controller and return its actual name and current rack index after callbacks", true, {"controller", "name"}, {"controller", "name"}),
		specification("controller.remove", "Detach and retire a native rack controller and its bindings before removal observers; the retiring target cannot be re-added; remove peak controllers through their owning effects", true, {"controller"}, {"controller"}),
		specification("controller.connect", "Check and connect/rewire a shared rack controller with feedback validation; preserve native connection identity and control-use flag; return current rack index and changed, with no notification for an identical finalized binding", true, {"controller", "parameter"}, {"controller", "parameter"}),
		specification("controller.midi.connect", "Prepare and replace a parameter-owned native MIDI CC binding, activate it and retire prior resources before final notification; returns changed; inputController 0..127, inputChannel 0 omni or 1..16, readable defaults true; no rack index", true, {"parameter", "inputController", "inputChannel", "readable"}, {"parameter", "inputController"}),
		specification("controller.midi.learn", "Wait for the first native MIDI CC while preserving linked parameter state; prepare its native input binding, retire detector and prior resources before final notification, return changed; optional channel/named input, timeout 1..600000 ms (default 30000); requires stopped/disarmed transport and observed active queued MIDI input", true, {"parameter", "port", "inputChannel", "timeoutMs"}, {"parameter"}),
		specification("controller.disconnect", "Check and detach a parameter before retiring its controller connection; retain stored manual/group state and return changed; unbound is a no-op", true, {"parameter"}, {"parameter"}),
		specification("automation.connect", "Prepare a native automation binding and initial point together; finish recording or dragging first", true, {"container", "track", "clip", "parameter", "summary"}, {"track", "clip", "parameter"}),
		specification("automation.record", "Record native parameter/controller automation into a staged clip take until duration or song clip end; stopped/disarmed transport and a running audio clock required", true, {"container", "track", "clip", "durationMs", "timeoutMs", "maximumPoints", "quantization", "position"}, {"track", "clip", "durationMs"}),
		specification("automation.disconnect", "Check and disconnect a native automation target while retaining curve values and geometry; finish recording or dragging first", true, {"container", "track", "clip", "parameter", "summary"}, {"track", "clip", "parameter"}),
		specification("automation.inspect", "Inspect native automation: full, summary, points or targets section; points pages by fromPosition/nextPosition, targets by offset/nextOffset; optional position evaluates", false, {"container", "track", "clip", "position", "section", "fromPosition", "offset", "limit", "names"}, {"track", "clip"}),
		specification("automation.edit", "Prepare native curve settings together: 0 discrete, 1 linear, 2 cubic Hermite; tension 0..1. Finish recording or dragging first", true, {"container", "track", "clip", "progression", "tension"}, {"track", "clip"}),
		specification("automation.point", "Prepare and replace a native point/settings at an exact source tick, including outValue discontinuity and optional locked tangents. Recording/dragging must be idle; returns changed and final node data", true, {"container", "track", "clip", "position", "value", "outValue", "progression", "tension", "inTangent", "outTangent", "lockedTangents"}, {"track", "clip", "position", "value"}),
		specification("automation.removePoint", "Remove a native automation point at an exact source tick using prepared editing and native automatic sizing; finish recording or dragging first", true, {"container", "track", "clip", "position"}, {"track", "clip", "position"}),
		specification("automation.clear", "Clear prepared native curve points and retain geometry and parameter connections; finish recording or dragging first", true, {"container", "track", "clip"}, {"track", "clip"}),
		specification("automation.global.inspect", "Inspect existing hidden global automation by exactly one parameter or global index; full, summary, points or targets sections; reading never creates a curve", false, {"parameter", "position", "global", "section", "fromPosition", "offset", "limit", "names"}, {}),
		specification("automation.global.point", "Prepare a native hidden global automation point/settings at a song tick; returns changed and final node data", true, {"parameter", "position", "value", "outValue", "progression", "tension", "inTangent", "outTangent", "lockedTangents"}, {"parameter", "position", "value"}),
		specification("automation.global.edit", "Prepare interpolation settings of native hidden global automation together", true, {"parameter", "progression", "tension", "summary"}, {"parameter"}),
		specification("automation.global.removePoint", "Remove a prepared native hidden global automation point with native automatic sizing; finish recording or dragging first", true, {"parameter", "position", "summary"}, {"parameter", "position"}),
		specification("automation.global.clear", "Clear prepared native hidden global automation points and retain geometry and model binding; finish recording or dragging first", true, {"parameter", "summary"}, {"parameter"}),
		specification("mixer.add", "Create a native mixer channel", true, {"name"}, {}),
		specification("mixer.edit", "Edit a native mixer channel or move it to final index to; master stays at zero; rediscover channel indices after a move", true, {"channel", "name", "volume", "muted", "solo", "to"}, {"channel"}),
		specification("mixer.remove", "Remove a native non-master mixer channel, its effects and routes; assigned tracks return to master and later channel indices shift left; rediscover indices and parameter handles", true, {"channel"}, {"channel"}),
		specification("mixer.reset", "Reset a native mixer channel's effects, level, mute, solo, name and color; remove incoming and outgoing routes and restore a non-master channel's unity send to master. Track assignments and surviving model bindings remain", true, {"channel"}, {"channel"}),
		specification("mixer.route", "Create a native send with cycle validation", true, {"from", "to", "amount"}, {"from", "to", "amount"}),
		specification("mixer.unroute", "Remove a native send", true, {"from", "to"}, {"from", "to"}),
		specification("effect.inspect", "Page ordered native effects and chain bypass; optional effect selects one index; effects default to 16, maximum 128; each effect's parameters default to 128, maximum 1024; follow nextOffset and nested parameterPage.nextOffset; details and choices default true", false, {"container", "track", "channel", "effect", "details", "choices", "offset", "limit", "parameterOffset", "parameterLimit"}, {}),
		specification("effect.add", "Append a native effect to the current track or mixer chain and return its surviving current index after callbacks", true, {"container", "track", "channel", "plugin", "attributes"}, {"plugin"}),
		specification("effect.remove", "Detach and retire the selected native effect with weak cleanup; refuse changed ownership or chain selectors", true, {"container", "track", "channel", "effect"}, {"effect"}),
		specification("effect.move", "Move a surviving native effect to final index to or one position up/down, validate its retained index after callbacks; supply exactly one of to or direction", true, {"container", "track", "channel", "effect", "direction", "to"}, {"effect"}),
		specification("effect.clear", "Clear the current native track or mixer effect chain, validate empty disabled state after callbacks and report removed count", true, {"container", "track", "channel"}, {}),
		specification("audio.render", "Render the native mix or one selected instrument/sample stem with native ranges, effects, routing and codec settings; returns stem selection, renderRange and sha256 for evaluation", false, {"path", "container", "track", "sampleRate", "bitDepth", "bitRate", "stereoMode", "compressionLevel", "exportLoop", "betweenMarkers", "loopCount", "timeoutMs"}, {"path"}),
		specification("audio.record", "Capture bounded native input into a sample clip; optional playback uses the native song; requires a running capture backend and stopped transport", true, {"container", "track", "clip", "durationMs", "maximumBytes", "timeoutMs", "playback", "position"}, {"track", "clip", "durationMs"}),
		specification("audio.analyze", "Measure an accepted source copy and report its sourceSha256; optional sourceSha256 rejects a changed source. This does not perform perceptual listening", false, {"path", "maximumSeconds", "sourceSha256"}, {"path"})
	};
}

bool StudioCapabilities::validateArguments(const QString& operation, const QJsonObject& arguments, QString& error,
	const std::function<bool()>& cancelled)
{
	try
	{
		FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
		if (!FileResourceIO::check(error)) { return false; }
		for (const auto& item : schema())
		{
			const auto specification = item.toObject(); if (specification.value("name").toString() != operation) { continue; }
			std::size_t visited = 0; validateSchemaValue(specification.value("inputSchema").toObject(), arguments, "args", visited);
			if (!FileResourceIO::check(error)) { return false; } error.clear(); return true;
		}
		error = "Unknown native studio operation: " + operation;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native capability argument validation failed with a nonstandard exception"; }
	return false;
}

bool StudioCapabilities::mutatesProject(const QString& operation)
{
	for (const auto item : schema())
	{
		const auto spec = item.toObject();
		if (spec.value("name").toString() == operation) { return spec.value("mutatesProject").toBool(); }
	}
	return false;
}

QJsonObject StudioCapabilities::execute(const QString& operation, const QJsonObject& arguments,
	const std::function<bool()>& cancelled)
{
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
		{
			fail("Studio capabilities must execute on the application thread");
		}
		if (!Engine::getSong() || !Engine::audioEngine()) { fail("Native LMMS engine is not initialized"); }
		if (Engine::audioEngine()->isChangingMidiBackend()) { fail("Native MIDI backend selection is in progress"); }
		QString validationError; if (!validateArguments(operation, arguments, validationError, cancelled)) { fail(validationError); }
		if (operation == "asset.roots" || operation == "asset.search")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			return {{"ok", true}, {"result", operation == "asset.roots" ? StudioAssets::roots(arguments) : StudioAssets::search(arguments)}};
		}
		if (operation == "preset.save" || operation == "project.save")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			// Export uses native serialization and the atomic file commit as its
			// publication boundary; it need not dirty the editable project.
			return {{"ok", true}, {"result", dispatch(operation, arguments)}};
		}
		if (operation == "mixer.add" || operation == "mixer.edit" || operation == "mixer.remove" || operation == "mixer.reset" || operation == "mixer.route" || operation == "mixer.unroute")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer()); const auto generation = mixer->replacementGeneration(); const auto project = owner->replacementGeneration();
			MixerChannel* selected = nullptr; MixerChannel* receiver = nullptr; QPointer<EffectChain> selectedAnchor, receiverAnchor;
			if (operation == "mixer.edit" || operation == "mixer.remove" || operation == "mixer.reset") { selected = channel(arguments); selectedAnchor = &selected->m_fxChain; }
			if (operation == "mixer.route" || operation == "mixer.unroute")
			{
				selected = mixer->mixerChannel(integer(arguments, "from", 1, mixer->numChannels() - 1)); receiver = mixer->mixerChannel(integer(arguments, "to", 0, mixer->numChannels() - 1));
				selectedAnchor = &selected->m_fxChain; receiverAnchor = &receiver->m_fxChain;
			}
			checkInspectionWork(); currentMixer(owner, mixer, generation, project); QJsonObject admitted(arguments);
			if (selected) { admitted.insert(receiver ? "from" : "channel", currentMixerChannelIndex(owner, mixer, generation, project, selected, selectedAnchor)); }
			if (receiver) { admitted.insert("to", currentMixerChannelIndex(owner, mixer, generation, project, receiver, receiverAnchor)); }
			auto result = dispatch(operation, admitted); currentMixer(owner, mixer, generation, project);
			struct ChannelState
			{
				MixerChannel* identity; QPointer<EffectChain> anchor; QString name; std::optional<QColor> color;
				bool beforeSolo; float volume, muted, solo; std::vector<QPointer<Effect>> effects;
			};
			struct RouteState { QPointer<MixerRoute> route; MixerChannel* from; MixerChannel* to; float amount; };
			std::vector<ChannelState> channels;
			for (int index = 0; index < mixer->numChannels(); ++index)
			{
				auto channel = mixer->mixerChannel(index);
				ChannelState state{channel, &channel->m_fxChain, channel->m_name, channel->color(), channel->m_muteBeforeSolo,
					channel->m_volumeModel.storedValue(), channel->m_muteModel.storedValue(), channel->m_soloModel.storedValue(), {}};
				for (auto effect : channel->m_fxChain.effects()) { state.effects.emplace_back(effect); }
				channels.push_back(std::move(state));
			}
			std::vector<RouteState> routes;
			for (auto route : mixer->m_mixerRoutes) { routes.push_back({route, route->sender(), route->receiver(), route->amount()->storedValue()}); }
			const auto solo = mixer->soloChannel(); owner->setModified(); currentMixer(owner, mixer, generation, project);
			if (mixer->numChannels() != channels.size() || mixer->m_mixerRoutes.size() != routes.size() || mixer->soloChannel() != solo) { fail("Native mixer topology changed during project notification"); }
			for (std::size_t index = 0; index < channels.size(); ++index)
			{
				const auto& state = channels[index];
				if (!state.anchor || mixer->mixerChannel(index) != state.identity) { fail("Native mixer channel changed during project notification"); }
				const auto channel = state.identity;
				if (channel->m_name != state.name || channel->color() != state.color || channel->m_muteBeforeSolo != state.beforeSolo
					|| channel->m_volumeModel.storedValue() != state.volume || channel->m_muteModel.storedValue() != state.muted || channel->m_soloModel.storedValue() != state.solo
					|| channel->m_fxChain.effects().size() != state.effects.size()) { fail("Native mixer channel state changed during project notification"); }
				for (std::size_t row = 0; row < state.effects.size(); ++row) { if (!state.effects[row] || channel->m_fxChain.effects()[row] != state.effects[row].data()) { fail("Native mixer effect changed during project notification"); } }
			}
			for (std::size_t index = 0; index < routes.size(); ++index)
			{
				const auto& state = routes[index];
				if (!state.route || mixer->m_mixerRoutes[index] != state.route.data() || state.route->sender() != state.from || state.route->receiver() != state.to || state.route->amount()->storedValue() != state.amount)
				{ fail("Native mixer route changed during project notification"); }
			}
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "instrument.shaping.waveform.set")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer()); const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString filename = owner->projectFileName();
			const QPointer<InstrumentTrack> native(instrumentTrack(arguments)); const QPointer<TrackContainer> container(native->trackContainer()); const QPointer<InstrumentSoundShaping> shaping(native->soundShaping());
			const QString target = string(arguments, "target"), path = resourcePath(arguments); const bool select = arguments.contains("select") && boolean(arguments, "select");
			const QPointer<EnvelopeAndLfoParameters> parameters(target == "volume" ? &shaping->getVolumeParameters() : target == "cutoff" ? &shaping->getCutoffParameters() : &shaping->getResonanceParameters());
			const auto ready = [&] { currentMixer(owner, mixer, routing, project); if (owner->projectFileName() != filename || !native || native->isRetiring() || native->isReplacingInstrument() || !container || native->trackContainer() != container.data()
				|| std::find(container->tracks().begin(), container->tracks().end(), native.data()) == container->tracks().end() || !shaping || native->soundShaping() != shaping.data() || !parameters || parameters->parent() != shaping.data()) { fail("Native instrument shaping context changed during waveform work"); } };
			ready(); checkInspectionWork(); ready(); QString error; if (!parameters->setLfoUserWave(path, error, select)) { fail(error); } ready();
			const auto buffer = parameters->getLfoUserWave(); const QString source = parameters->lfoUserWaveFile(); const bool pending = parameters->lfoUserWaveUnresolved(); const auto wave = parameters->getLfoWaveModel().storedValue();
			owner->setModified(); ready(); if (parameters->getLfoUserWave() != buffer || parameters->lfoUserWaveFile() != source || parameters->lfoUserWaveUnresolved() != pending || parameters->getLfoWaveModel().storedValue() != wave) { fail("Native instrument shaping waveform changed during project notification"); }
			const auto found = std::find(container->tracks().begin(), container->tracks().end(), native.data());
			return {{"ok", true}, {"result", QJsonObject{{"container", container.data() == Engine::getSong() ? "song" : "pattern"}, {"track", static_cast<int>(std::distance(container->tracks().begin(), found))}, {"target", target}, {"path", source}, {"unresolved", pending}, {"frames", static_cast<double>(buffer->size())}, {"sampleRate", static_cast<int>(buffer->sampleRate())}, {"waveParameter", static_cast<int>(parameters->getLfoWaveModel().id())}, {"wave", wave}}}};
		}
		if (operation == "controller.waveform.set")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<LfoController> native(dynamic_cast<LfoController*>(controller(arguments)));
			const QPointer<Controller> target(native.data()); const QPointer<Mixer> mixer(Engine::mixer());
			const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString filename = owner->projectFileName();
			const auto ready = [&] { currentMixer(owner, mixer, routing, project); if (owner->projectFileName() != filename) { fail("Native LFO project path changed during editing"); } currentControllerIndex(owner, target); };
			if (!native) { fail("Waveform resources require a native LFO controller"); }
			checkInspectionWork(); ready(); auto result = dispatch(operation, arguments); ready();
			const auto sample = native->userWaveBuffer(); const QString source = native->userWaveFile(); const bool pending = native->userWaveUnresolved(); const auto wave = native->waveModel().storedValue();
			owner->setModified(); const auto index = currentControllerIndex(owner, target); ready();
			if (native->userWaveBuffer() != sample || native->userWaveFile() != source || native->userWaveUnresolved() != pending || native->waveModel().storedValue() != wave) { fail("Native LFO waveform changed during project notification"); }
			result.insert("userWave", inspectLfoWave(native.data())); result.insert("controller", index); return {{"ok", true}, {"result", result}};
		}
		if (operation == "project.new")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer());
			const auto alive = [&] { if (!owner || Engine::getSong() != owner.data() || !mixer || Engine::mixer() != mixer.data()) { fail("Native new project owner retired"); } };
			checkInspectionWork(); alive(); dispatch(operation, arguments); alive();
			const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString path = owner->projectFileName();
			NativeProjectSaveSnapshot source(owner.data());
			const auto ready = [&] { alive(); if (owner->replacementGeneration() != project || mixer->replacementGeneration() != routing || owner->projectFileName() != path || !owner->isModified()) { fail("Native new project publication state changed"); } source.validate(); alive(); };
			owner->setModified(); ready();
			const auto result = arguments.value("summary").toBool() ? inspectSummary() : inspect(); ready();
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "project.import")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer()); const QPointer<TrackContainer> target(container(arguments));
			const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString path = owner->projectFileName();
			const bool inPattern = target.data() == Engine::patternStore(); const int count = inPattern ? Engine::patternStore()->numOfPatterns() : 0;
			const int pattern = inPattern ? (arguments.contains("pattern") ? integer(arguments, "pattern", 0, std::max(1, count) - 1) : count ? Engine::patternStore()->currentPattern() : 0) : -1;
			const QPointer<PatternTrack> column(inPattern && count ? PatternTrack::findPatternTrack(pattern) : nullptr);
			const auto context = [&] {
				if (!owner || Engine::getSong() != owner.data() || !mixer || Engine::mixer() != mixer.data() || !target
					|| target.data() != (inPattern ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(owner.data()))
					|| owner->replacementGeneration() != project || mixer->replacementGeneration() != routing || owner->projectFileName() != path)
				{ fail("Native import project or destination changed during execution"); }
				if (inPattern && count && (!column || PatternTrack::findPatternTrack(pattern) != column.data())) { fail("Native import selected pattern column changed"); }
			};
			checkInspectionWork(); context();
			if (inPattern && !count && Engine::patternStore()->numOfPatterns() != 0) { fail("Native empty import destination changed before execution"); }
			auto pinned = arguments; if (inPattern) { pinned.insert("pattern", pattern); }
			dispatch(operation, pinned); context(); NativeProjectSaveSnapshot completed(owner.data());
			const auto ready = [&] { context(); completed.validate(); if (!owner->isModified()) { fail("Native import publication dirty state changed"); } };
			owner->setModified(); ready(); const auto result = arguments.value("summary").toBool() ? inspectSummary() : inspect(); ready();
			QJsonObject response{{"ok", true}, {"result", result}};
			if (owner->hasErrors()) { response.insert("nativeWarnings", owner->errorSummary()); } ready(); return response;
		}
		if (operation == "project.load")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong());
			const auto ready = [&] { if (!owner || Engine::getSong() != owner.data()) { fail("Native project load owner retired"); } };
			checkInspectionWork(); ready(); const auto result = dispatch(operation, arguments); ready();
			owner->setModified(); ready();
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "sample.load")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer()); const QPointer<SampleClip> target(sampleClip(arguments)); const QPointer<Track> track(target->getTrack());
			const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString projectPath(owner->projectFileName());
			const auto ready = [&] { if (!owner || Engine::getSong() != owner.data() || !mixer || Engine::mixer() != mixer.data() || owner->replacementGeneration() != project || mixer->replacementGeneration() != routing || owner->projectFileName() != projectPath || !target || !track || target->getTrack() != track.data() || clip(arguments) != target.data()) { fail("Native sample load context retired or changed"); } };
			checkInspectionWork(); ready(); dispatch(operation, arguments); ready();
			const auto expected = inspectSample(target.data()); const auto buffer = target->sample().buffer(); const auto position = target->startPosition(), duration = target->length(), offset = target->startTimeOffset(); const QString name(target->name()); const auto automatic = target->getAutoResize(); const auto color = target->color();
			NativeTrackSaveSnapshot source(track.data(), true);
			const auto published = [&] { ready(); source.validate(); if (!owner->isModified() || target->sample().buffer() != buffer || inspectSample(target.data()) != expected || target->startPosition() != position || target->length() != duration || target->startTimeOffset() != offset || target->name() != name || target->getAutoResize() != automatic || target->color() != color) { fail("Native sample load changed during project notification"); } };
			owner->setModified(); published(); const auto result = inspectSample(target.data()); published(); return {{"ok", true}, {"result", result}};
		}
		if (operation == "instrument.resource.set")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events; const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<Mixer> mixer(Engine::mixer()); const QPointer<TrackContainer> tracks(container(arguments)); const QPointer<InstrumentTrack> target(instrumentTrack(arguments)); const QPointer<Instrument> native(target->instrument());
			const auto project = owner->replacementGeneration(), routing = mixer->replacementGeneration(); const QString projectPath = owner->projectFileName();
			const auto ready = [&] { currentInstrumentTrackIndex(owner, tracks, target, arguments); if (!mixer || Engine::mixer() != mixer.data() || owner->replacementGeneration() != project || mixer->replacementGeneration() != routing || owner->projectFileName() != projectPath || !native || target->instrument() != native.data()) { fail("Native resource context retired or changed"); } };
			checkInspectionWork(); ready(); auto result = dispatch(operation, arguments); ready();
			const auto resources = native->fileResources(); ready(); std::vector<QPointer<AutomatableModel>> selectors; for (const auto& resource : resources) { selectors.emplace_back(resource.selector); }
			NativeTrackSaveSnapshot source(target.data(), false);
			const auto published = [&] {
				ready(); source.validate(); if (!owner->isModified()) { fail("Native resource dirty state changed during publication"); } const auto current = native->fileResources(); ready(); if (current.size() != resources.size()) { fail("Native resource inventory changed during publication"); }
				for (std::size_t index = 0; index < resources.size(); ++index)
				{
					const auto& before = resources[index]; const auto& after = current[index];
					if (before.key != after.key || before.name != after.name || before.path != after.path || before.fileTypes != after.fileTypes || before.selector != selectors[index].data()
						|| after.selector != selectors[index].data() || before.selectionValue != after.selectionValue || before.requiredForRestore != after.requiredForRestore || before.identity != after.identity || before.unresolved != after.unresolved)
					{ fail("Native resource state changed during project notification"); }
				}
			};
			owner->setModified(); published(); result.insert("resources", inspectResources(native.data())); published();
			const int row = currentInstrumentTrackIndex(owner, tracks, target, arguments); result.insert("track", row); return {{"ok", true}, {"result", result}};
		}
		if (operation == "instrument.load" || operation == "preset.load")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<TrackContainer> tracks(container(arguments)); const QPointer<InstrumentTrack> target(instrumentTrack(arguments));
			checkInspectionWork(); currentInstrumentTrackIndex(owner, tracks, target, arguments);
			auto result = dispatch(operation, arguments); currentInstrumentTrackIndex(owner, tracks, target, arguments);
			const QPointer<Instrument> native(target->instrument()); Instrument* const selected = native.data();
			if (operation == "instrument.load" && !native) { fail("Native loaded instrument retired before project notification"); }
			owner->setModified(); const int index = currentInstrumentTrackIndex(owner, tracks, target, arguments);
			if (native.data() != selected || target->instrument() != selected) { fail("Native loaded instrument changed during project notification"); }
			result.insert("track", index); return {{"ok", true}, {"result", result}};
		}
		if (operation == "effect.add" || operation == "effect.remove" || operation == "effect.move" || operation == "effect.clear")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const QPointer<Song> owner(Engine::getSong()); const QPointer<EffectChain> effects(chain(arguments));
			checkInspectionWork(); currentEffectChain(owner, effects, arguments);
			const auto result = dispatch(operation, arguments); currentEffectChain(owner, effects, arguments);
			std::vector<QPointer<Effect>> roster; for (auto effect : effects->effects()) { roster.emplace_back(effect); }
			const bool enabled = effects->enabledModel()->value(); owner->setModified(); currentEffectChain(owner, effects, arguments);
			if (effects->effects().size() != roster.size() || effects->enabledModel()->value() != enabled)
			{ fail("Native effect chain changed during project notification"); }
			for (std::size_t index = 0; index < roster.size(); ++index)
			{ if (!roster[index] || effects->effects()[index] != roster[index].data()) { fail("Native effect identity changed during project notification"); } }
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "controller.add" || operation == "controller.edit" || operation == "controller.remove" || operation == "controller.connect" || operation == "controller.midi.connect" || operation == "controller.disconnect" || operation == "automation.connect" || operation == "automation.disconnect" || operation == "automation.removePoint" || operation == "automation.clear" || operation == "automation.global.removePoint" || operation == "automation.global.clear" || operation == "note.detuning.removePoint" || operation == "note.detuning.clear" || operation == "automation.point" || operation == "automation.edit" || operation == "automation.global.point" || operation == "automation.global.edit" || operation == "note.detuning.point" || operation == "note.detuning.edit" || operation == "track.remove" || operation == "track.clone" || operation == "track.edit" || operation == "clip.selection.copy" || operation == "note.selection.copy" || operation == "note.selection.shift" || operation == "note.selection.quantize" || operation == "note.selection.reverse" || operation == "note.selection.length" || operation == "note.selection.glue" || operation == "note.selection.split" || operation == "note.selection.knife" || operation == "note.selection.remove" || operation == "note.remove" || operation == "note.clear" || operation == "note.selection.edit" || operation == "note.edit" || operation == "note.add" || operation == "note.batch.add" || operation == "step.clone" || operation == "step.resize" || operation == "step.set" || operation == "parameter.link" || operation == "parameter.unlink" || operation == "parameter.set")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto changes = Engine::audioEngine()->requestChangesGuard();
			const auto result = dispatch(operation, arguments); Engine::getSong()->setModified();
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "studio.resources.inspect") { return {{"ok", true}, {"result", StudioResources::sample()}}; }
		if (operation == "audio.bus.inspect" || operation == "audio.bus.configure" || operation == "audio.bus.route")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			AudioBusHandle* bus = nullptr;
			{ const auto guard = Engine::audioEngine()->requestChangesGuard(); bus = trackAudioBus(arguments); }
			checkInspectionWork(); const auto device = Engine::audioEngine()->audioDev();
			if (!device) { fail("Native audio backend is unavailable"); }
			if (operation == "audio.bus.inspect") { return {{"ok", true}, {"result", inspectTrackAudioBus(bus, arguments)}}; }
			if (operation == "audio.bus.configure")
			{
				device->configureBusOutput(bus, boolean(arguments, "enabled"), [] { checkInspectionWork(); return false; });
				return {{"ok", true}, {"result", inspectTrackAudioBus(bus, arguments)}};
			}
			const auto channel = integer(arguments, "channel", 0, 1);
			const auto route = device->setBusRoute(bus, static_cast<unsigned int>(channel), string(arguments, "port"), boolean(arguments, "enabled"),
				[] { checkInspectionWork(); return false; });
			return {{"ok", true}, {"result", QJsonObject{{"scope", "track"}, {"direction", "output"},
				{"track", arguments.value("track")}, {"container", arguments.value("container").toString("song")}, {"channel", channel},
				{"localPort", route.localPort}, {"port", route.remotePort}, {"enabled", route.connected}, {"changed", route.changed},
				{"processingInterrupted", route.processingInterrupted}, {"connectionVerified", true}, {"deviceReceiptVerified", false},
				{"persistence", "process-local track graph; native client recreation and receipt replay do not restore connections"}}}};
		}
		if (operation == "audio.backend.inspect" || operation == "audio.backend.route")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto direction = string(arguments, "direction");
			if (direction != "input" && direction != "output") { fail("direction must be input or output"); }
			const bool input = direction == "input";
			if (operation == "audio.backend.inspect") { return {{"ok", true}, {"result", inspectAudioBackendGraph(input, arguments)}}; }
			const auto device = Engine::audioEngine()->audioDev(); if (!device) { fail("Native audio backend is unavailable"); }
			const auto channel = integer(arguments, "channel", 0, std::numeric_limits<int>::max()); checkInspectionWork();
			// The native device joins callback work; never hold the model guard here.
			const auto route = device->setBackendRoute(input, static_cast<unsigned int>(channel), string(arguments, "port"), boolean(arguments, "enabled"),
				[] { checkInspectionWork(); return false; });
			return {{"ok", true}, {"result", QJsonObject{{"scope", "master"}, {"direction", direction}, {"channel", channel},
				{"localPort", route.localPort}, {"port", route.remotePort}, {"enabled", route.connected}, {"changed", route.changed},
				{"processingInterrupted", route.processingInterrupted}, {"inputBuffersInvalidated", input && route.changed},
				{"connectionVerified", true}, {"deviceReceiptVerified", false},
				{"persistence", "process-local graph change; loaded startup settings and project state unchanged; receipt replay does not reapply connections"}}}};
		}
		if (operation == "midi.backend.inspect" || operation == "midi.backend.route" || operation == "midi.backend.reconnect")
		{
			FileResourceIO::Scope work(cancelled, false); FileResourceIO::EventSuspension events;
			const auto direction = string(arguments, "direction");
			if (direction != "input" && direction != "output") { fail("direction must be input or output"); }
			const bool input = direction == "input";
			if (operation == "midi.backend.inspect") { return {{"ok", true}, {"result", inspectMidiBackendGraph(input, arguments)}}; }
			const auto client = Engine::audioEngine()->midiClient(); if (!client) { fail("Native MIDI backend is unavailable"); }
			checkInspectionWork();
			if (operation == "midi.backend.reconnect")
			{
				const auto preference = client->setBackendReconnect(input, string(arguments, "port"), boolean(arguments, "enabled"));
				return {{"ok", true}, {"result", QJsonObject{{"scope", "client"}, {"direction", direction}, {"port", preference.remotePort},
					{"reconnect", preference.enabled}, {"changed", preference.changed}, {"graphChanged", false}, {"deviceReceiptVerified", false},
					{"persistence", "process-local preference; receipt replay does not recreate it; use midi.backend.inspect for current connection state"}}}};
			}
			{
				const auto guard = Engine::audioEngine()->requestChangesGuard(); requirePlaybackTransport();
			}
			if (!Engine::audioEngine()->canChangeBackendGraph())
			{ fail("Native MIDI graph changes require stopped transport and no active device transition or render"); }
			// Reader joining and output-drain waits must not hold the model guard.
			const auto route = client->setBackendRoute(input, string(arguments, "port"), boolean(arguments, "enabled"),
				[] { checkInspectionWork(); return false; });
			return {{"ok", true}, {"result", QJsonObject{{"scope", "client"}, {"direction", direction},
				{"localPort", route.localPort}, {"port", route.remotePort}, {"enabled", route.connected}, {"changed", route.changed},
				{"reconnect", route.reconnect}, {"reconnectChanged", route.reconnectChanged},
				{"connectionVerified", true}, {"deviceReceiptVerified", false}, {"inputDeliveryResetRequired", input && route.changed},
				{"persistence", "process-local graph state; receipt replay does not reapply connections"}}}};
		}
		if (operation == "controller.midi.learn")
		{
			QPointer<AutomatableModel> target;
			{ const auto guard = Engine::audioEngine()->requestChangesGuard(); target = parameter(arguments); }
			return {{"ok", true}, {"result", StudioMidiLearn::learn(target.data(), arguments, cancelled)}};
		}
		if (operation == "automation.record")
		{
			QPointer<AutomationClip> target;
			{ const auto guard = Engine::audioEngine()->requestChangesGuard(); target = dynamic_cast<AutomationClip*>(clip(arguments)); }
			return {{"ok", true}, {"result", StudioAutomationRecording::capture(target.data(), arguments, cancelled)}};
		}
		if (operation == "midi.record")
		{
			QPointer<MidiClip> target;
			{ const auto guard = Engine::audioEngine()->requestChangesGuard(); target = dynamic_cast<MidiClip*>(clip(arguments)); }
			return {{"ok", true}, {"result", StudioMidiRecording::capture(target.data(), arguments, cancelled)}};
		}
		if (operation == "audio.record")
		{
			QPointer<SampleClip> target;
			{ const auto guard = Engine::audioEngine()->requestChangesGuard(); target = dynamic_cast<SampleClip*>(clip(arguments)); }
			const auto result = StudioRecording::capture(target.data(), arguments, cancelled);
			return {{"ok", true}, {"result", result}};
		}
		if (operation == "audio.render") { return {{"ok", true}, {"result", StudioAudio::render(arguments, cancelled)}}; }
		if (operation == "audio.analyze") { return {{"ok", true}, {"result", StudioAudio::analyze(arguments, cancelled)}}; }
		if (operation == "midi.backend.select")
		{
			FileResourceIO::Scope work(cancelled, false);
			const bool abandon = arguments.contains("abandonFailedOutput") && boolean(arguments, "abandonFailedOutput");
			const auto selected = Engine::audioEngine()->selectMidiBackend(arguments.value("name").toString(), abandon);
			FileResourceIO::Suspension committed;
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			const auto& loss = selected.outputAbandonment;
			const QJsonObject abandoned{{"abandoned", loss.abandoned}, {"trackedNotes", QString::number(static_cast<qulonglong>(loss.trackedNotes))},
				{"pedalHolds", QString::number(static_cast<qulonglong>(loss.pedalHolds))},
				{"pendingEvents", static_cast<int>(loss.backend.pendingEvents)}, {"cleanupPendingChannels", static_cast<int>(loss.backend.cleanupPendingChannels)},
				{"nativeErrorCode", loss.backend.nativeErrorCode}, {"lastNativeErrorCode", loss.backend.lastNativeErrorCode},
				{"deviceCleanupVerified", false}, {"scope", "retired client software queues and local ledgers; hardware may retain notes or buffered events; no replay or physical cleanup acknowledgment"}};
			return {{"ok", true}, {"result", QJsonObject{{"committed", true}, {"ports", static_cast<int>(selected.ports)},
				{"previousBackend", selected.previousBackend},
				{"inputResetsRequired", static_cast<int>(selected.inputResetsRequired)},
				{"retirementExceptions", static_cast<int>(selected.retirementExceptions)},
				{"notificationFailures", static_cast<int>(selected.notificationFailures)},
				{"outputAbandonment", abandoned},
				{"audioRestored", selected.audioRestored}, {"recoveryRequired", selected.audioRestored ? "" : "audio.device.restore"},
				{"current", inspectTransport()}}}};
		}
		if (operation == "audio.backend.select")
		{
			// Device stop/join must happen outside the generic model guard.
			FileResourceIO::Scope work(cancelled, true);
			Engine::audioEngine()->selectAudioBackend(arguments.value("name").toString());
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			return {{"ok", true}, {"result", inspectTransport()}};
		}
		if (operation == "audio.device.restore")
		{
			if (RemotePlugin::hasActiveRenderContext() && !Engine::audioEngine()->isRestoringAudioDevice())
			{
				fail("Native export finalization must finish before external device restoration");
			}
			// Joining an audio thread under the generic model guard can deadlock
			// its render callback. The engine serializes this owner-thread action.
			if (!Engine::audioEngine()->restoreAudioDevice())
			{
				fail(QString("Native audio device restoration failed: ")
					+ AudioEngine::deviceRestoreFailureName(Engine::audioEngine()->deviceRestoreFailure()));
			}
			return {{"ok", true}, {"result", inspectAudioDeviceRestore()}};
		}
		// Ordinary capabilities need the supplied cancellation source throughout
		// native execution, not just in validateArguments' temporary scope.
		FileResourceIO::Scope work(cancelled, false);
		auto guard = Engine::audioEngine()->requestChangesGuard();
		const QPointer<Song> owner(Engine::getSong()); const auto project = owner->replacementGeneration();
		const auto ready = [&] {
			if (!owner || Engine::getSong() != owner.data() || owner->replacementGeneration() != project)
			{ fail("Native capability project retired or changed during execution"); }
		};
		const auto boundary = [&] {
			// A boundary probe must not dispatch queued work between a native
			// result and its publication. Nested native work keeps its own policy.
			FileResourceIO::EventSuspension events; ready(); checkInspectionWork(); ready();
		};
		boundary(); const auto result = dispatch(operation, arguments); boundary();
		if (mutatesProject(operation)) { owner->setModified(); ready(); }
		boundary();
		return {{"ok", true}, {"result", result}};
	}
	catch (const std::exception& error)
	{
		QString warnings;
		if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()
			&& Engine::getSong() && Engine::getSong()->hasErrors()) { warnings = Engine::getSong()->errorSummary(); }
		return {{"ok", false}, {"error", QString::fromUtf8(error.what())},
			{"nativeWarnings", warnings}};
	}
	catch (...)
	{
		QString warnings;
		if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()
			&& Engine::getSong() && Engine::getSong()->hasErrors()) { warnings = Engine::getSong()->errorSummary(); }
		return {{"ok", false}, {"error", "Native studio capability failed with a nonstandard exception"}, {"nativeWarnings", warnings}};
	}
}

QJsonObject StudioCapabilities::inspectSummary()
{
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard(); checkInspectionWork();
	const auto song = Engine::getSong(); const auto& timeline = song->getTimeline(Song::PlayMode::Song);
	const auto tempo = tempoReadback(song);
	const auto heads = [](TrackContainer* owner) {
		QJsonArray tracks; const InspectionPage page{0, 16};
		for (int index = 0; index < static_cast<int>(owner->tracks().size()) && index < page.limit; ++index)
		{ checkInspectionWork(); tracks.append(inspectTrackInventory(owner->tracks().at(index), index, false)); }
		auto result = page.metadata(static_cast<int>(owner->tracks().size()), tracks.size()); result.insert("tracks", tracks); return result;
	};
	auto songTracks = heads(song), patternTracks = heads(Engine::patternStore());
	QJsonObject result{{"inspection", "summary"}, {"tempo", tempo.value("state").toString() == "valid" ? tempo.value("effectiveValue") : QJsonValue(QJsonValue::Null)},
		{"tempoReadback", tempo}, {"masterVolume", song->masterVolume()},
		{"masterPitch", song->masterPitch()}, {"ticksPerBar", song->ticksPerBar()}, {"lengthBars", song->length()},
		{"tracks", songTracks.take("tracks")}, {"patternTracks", patternTracks.take("tracks")},
		{"trackPages", QJsonObject{{"song", songTracks}, {"pattern", patternTracks}}},
		{"channelCount", Engine::mixer()->numChannels()}, {"routeCount", static_cast<int>(Engine::mixer()->m_mixerRoutes.size())},
		{"controllerCount", static_cast<int>(song->controllers().size())}, {"globalAutomationCount", song->globalAutomationTrack()->numOfClips()},
		{"soloChannel", Engine::mixer()->soloChannel()}, {"nativeWarningsPresent", song->hasErrors()},
		{"nativeWarningCount", static_cast<int>(song->nativeErrors().size())},
		{"diagnosticGeneration", QString::number(static_cast<qulonglong>(song->errorGeneration()))},
		{"transport", QJsonObject{{"playing", song->isPlaying()}, {"paused", song->isPaused()}, {"recording", song->isRecording()},
			{"playMode", playModeName(song->playMode())}, {"songPosition", static_cast<int>(song->getPlayPos(Song::PlayMode::Song))}}},
		{"loop", QJsonObject{{"begin", static_cast<int>(timeline.loopBegin())}, {"end", static_cast<int>(timeline.loopEnd())}, {"enabled", timeline.loopEnabled()}}},
		{"detailSections", QJsonArray{"tracks", "clips", "notes", "channels", "routes", "controllers", "globalAutomations", "warnings", "resources"}}, {"handleScope", "current project revision; rediscover indices after structural edits or restoration"}};
	checkInspectionWork();
	if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 64 * 1024) { fail("Native project summary exceeds its 64 KiB budget"); }
	return result;
}

QJsonObject StudioCapabilities::inspect()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	auto song = Engine::getSong();
	const auto tempo = tempoReadback(song);
	QJsonArray channels;
	for (int i = 0; i < Engine::mixer()->numChannels(); ++i)
	{
		auto ch = Engine::mixer()->mixerChannel(i);
		channels.append(QJsonObject{{"index", i}, {"name", ch->m_name},
			{"volume", ch->m_volumeModel.value()}, {"muted", ch->m_muteModel.value()},
			{"solo", ch->m_soloModel.value()},
			{"muteBeforeSolo", Engine::mixer()->soloChannel() >= 0 ? ch->m_muteBeforeSolo : ch->m_muteModel.value()},
			{"effects", effectNames(&ch->m_fxChain)}, {"effectsEnabled", ch->m_fxChain.enabledModel()->value()}});
	}
	QJsonArray routes;
	for (auto route : Engine::mixer()->m_mixerRoutes)
	{
		routes.append(QJsonObject{{"from", static_cast<int>(route->senderIndex())},
			{"to", static_cast<int>(route->receiverIndex())}, {"amount", route->amount()->value()}, {"parameter", static_cast<int>(route->amount()->id())}});
	}
	QJsonArray global;
	for (const auto clip : song->globalAutomationTrack()->getClips())
	{
		if (const auto curve = dynamic_cast<AutomationClip*>(clip)) { global.append(inspectCurve(curve)); }
	}
	const auto& timeline = song->getTimeline(Song::PlayMode::Song);
	return {{"tempo", tempo.value("state").toString() == "valid" ? tempo.value("effectiveValue") : QJsonValue(QJsonValue::Null)},
		{"tempoReadback", tempo}, {"masterVolume", song->masterVolume()},
		{"masterPitch", song->masterPitch()}, {"ticksPerBar", song->ticksPerBar()},
		{"lengthBars", song->length()}, {"tracks", inspectTracks(song)},
		{"patternTracks", inspectTracks(Engine::patternStore())}, {"controllers", inspectControllers()}, {"channels", channels}, {"routes", routes},
		{"soloChannel", Engine::mixer()->soloChannel()},
		{"globalAutomations", global}, {"tuning", inspectTuningLibrary()},
		{"transport", inspectTransport()},
		{"loop", QJsonObject{{"begin", static_cast<int>(timeline.loopBegin())},
			{"end", static_cast<int>(timeline.loopEnd())}, {"enabled", timeline.loopEnabled()}}},
		{"nativeWarnings", song->hasErrors() ? song->errorSummary() : QString()}};
}

} // namespace lmms
