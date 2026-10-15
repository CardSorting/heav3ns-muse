#include "../core/NativeInstrumentTestAccess.h"
/* Native Carla host contract tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QApplication>
#include <QCompleter>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QScopeGuard>
#include <functional>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../../plugins/CarlaBase/Carla.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiEvent.h"
#include "MidiClient.h"
#include "MidiEventToByteSeq.h"
#include "MidiClip.h"
#include "NotePlayHandle.h"
#include "Note.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../core/StudioParameterTestUtils.h"
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
#include <CarlaUtils.h>
#endif

using namespace lmms;

namespace lmms
{
class MidiPortTestAccess
{
public:
	static void useClient(MidiPort* port, MidiClient* client)
	{
		port->m_midiClient->removePort(port); port->m_midiClient = client; client->addPort(port);
	}
};
}

#ifdef LMMS_CARLA_NATIVE_HOST_API
// The fixture owns the storage returned through Carla's metadata ABI.
_CarlaPluginInfo::_CarlaPluginInfo() noexcept
	: type(static_cast<PluginType>(0)), category(static_cast<PluginCategory>(0)), hints(0),
	optionsAvailable(0), optionsEnabled(0), filename(nullptr), name(nullptr), label(nullptr),
	maker(nullptr), copyright(nullptr), iconName(nullptr), uniqueId(0) {}
_CarlaPluginInfo::~_CarlaPluginInfo() noexcept = default;
_CarlaParameterInfo::_CarlaParameterInfo() noexcept
	: name(nullptr), symbol(nullptr), unit(nullptr), comment(nullptr), groupName(nullptr), scalePointCount(0) {}
_CarlaParameterInfo::~_CarlaParameterInfo() noexcept = default;
_CarlaScalePointInfo::_CarlaScalePointInfo() noexcept : value(0), label(nullptr) {}
_CarlaScalePointInfo::~_CarlaScalePointInfo() noexcept = default;
#endif

#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
_CarlaPluginDiscoveryMetadata::_CarlaPluginDiscoveryMetadata() noexcept
	: name(nullptr), maker(nullptr), category(static_cast<PluginCategory>(0)), hints(0) {}
_CarlaPluginDiscoveryIO::_CarlaPluginDiscoveryIO() noexcept
	: audioIns(0), audioOuts(0), cvIns(0), cvOuts(0), midiIns(0), midiOuts(0), parameterIns(0), parameterOuts(0) {}
_CarlaPluginDiscoveryInfo::_CarlaPluginDiscoveryInfo() noexcept
	: btype(static_cast<BinaryType>(0)), ptype(static_cast<PluginType>(0)), filename(nullptr), label(nullptr), uniqueId(0) {}
#endif

namespace
{
struct HostedRecord
{
	int format;
	QString name, label, path;
	qint64 uniqueId;
	bool active;
	std::vector<float> parameters{.25f, 2.f, -1.f, .5f, .5f, .5f, .5f, .5f};
};
struct NativeState
{
	const NativeHostDescriptor* host = nullptr;
	std::vector<float> values{42.f, .25f};
	std::vector<bool> offlineChanges;
	std::vector<NativeMidiEvent> midi;
	std::vector<NativeMidiEvent> emittedMidi;
	std::vector<bool> midiOutputAccepted;
	bool emittingMidi = false, midiFromWorker = false, nullMidiOutput = false;
	bool outputDuringProcess = false;
	int processFailure = 0;
	int failure = 0;
	int parameterWrites = 0;
	int uiWrites = 0;
	int activations = 0;
	int cleanups = 0;
	int nullInfo = -1;
	int saveFailure = 0;
	int loadFailure = 0;
	int stateLoads = 0;
	int parameterPolicy = 0;
	QString stateName = QString::fromUtf8("Hosted \xC3\xA9" "cho");
	bool processingWasOffline = false;
	bool offlineDispatchedBeforeProcess = true;
	bool cleanupHadResources = false;
	bool management = false;
	bool attachFails = false;
	bool releasedBeforeEngine = false;
	int hostCreates = 0, hostFrees = 0, hostMutations = 0, hostFailure = 0;
	int hostedParameterWrites = 0, hostedParameterFailure = 0, hostedWriteBehavior = 0;
	bool flattenedParameters = false, reloadDuringHostEdit = false;
	uint32_t nativeParameterSlots = 6;
	int discoveryStarts = 0, discoveryStops = 0;
	bool patchbayInstrument = false, patchbayConnected = false, patchbayExternalRequested = false;
	int patchbayRefreshes = 0, patchbayEdits = 0, patchbayFailure = 0;
	int patchbayAttaches = 0;
	bool patchbayInstalledWhileInactive = true;
	uint32_t patchbayConnection = 0, patchbayDisconnected = 0;
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	CARLA_BACKEND_NAMESPACE::EngineCallbackFunc graphCallback = nullptr;
	void* graphContext = nullptr;
#endif
	QByteArray missingSymbol;
	std::vector<HostedRecord> hosted;
};
NativeState native;

std::pair<HostedRecord*, uint32_t> flattenedParameter(uint32_t index)
{
	for (auto& plugin : native.hosted)
	{
		if (index < plugin.parameters.size()) { return {&plugin, index}; }
		index -= static_cast<uint32_t>(plugin.parameters.size());
	}
	return {nullptr, 0};
}

NativePluginHandle instantiate(const NativeHostDescriptor* host)
{
	native.host = host;
	return native.failure == 2 ? nullptr : &native;
}
void cleanup(NativePluginHandle)
{
	++native.cleanups;
	native.cleanupHadResources = native.host->resourceDir != nullptr;
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	if (native.graphCallback)
	{
		// Engine cleanup can deliver final events before its runners are joined.
		// The adapter's context must still exist, with collection disabled.
		native.graphCallback(native.graphContext, CARLA_BACKEND_NAMESPACE::ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED, 2, 0, 0, 0, 0, nullptr);
		native.graphCallback = nullptr; native.graphContext = nullptr;
	}
#endif
	// Cleanup must not allow a synchronous notification to reenter a dying handle.
	native.host->dispatcher(native.host->handle, NATIVE_HOST_OPCODE_RELOAD_PARAMETERS, 0, 0, nullptr, 0);
	if (native.patchbayInstrument) { native.hosted.clear(); native.patchbayConnected = false; }
	native.host = nullptr;
}
uint32_t parameterCount(NativePluginHandle)
{
	if (native.failure == 3) { throw std::runtime_error("native parameter metadata failed"); }
	if (native.flattenedParameters) { return native.nativeParameterSlots; }
	return static_cast<uint32_t>(native.values.size());
}
const NativeParameter* parameterInfo(NativePluginHandle, uint32_t index)
{
	if (native.flattenedParameters)
	{
		static NativeParameter parameter{}; static QByteArray name;
		const auto target = flattenedParameter(index);
		name = target.first ? target.first->name.toUtf8() + ":" + QByteArray::number(target.second) : QByteArray("Unused");
		parameter.name = name.constData();
		parameter.hints = static_cast<NativeParameterHints>(target.first ? NATIVE_PARAMETER_IS_ENABLED | NATIVE_PARAMETER_IS_AUTOMATABLE : 0);
		parameter.ranges.min = 0; parameter.ranges.max = 1; parameter.ranges.step = .001f;
		return &parameter;
	}
	if (index >= native.values.size() || int(index) == native.nullInfo) { return nullptr; }
	static std::array<NativeParameter, 3> parameters = []
	{
		std::array<NativeParameter, 3> result{};
		for (auto& parameter : result)
		{
			parameter.hints = static_cast<NativeParameterHints>(NATIVE_PARAMETER_IS_ENABLED | NATIVE_PARAMETER_IS_AUTOMATABLE);
			parameter.ranges.min = 0; parameter.ranges.max = 100; parameter.ranges.step = .001f;
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
			parameter.groupName = "Native controls";
#endif
		}
		result[0].name = "Level"; result[1].name = "Balance"; result[2].name = "Tone";
		return result;
	}();
	if (index == 0)
	{
		unsigned int hints = NATIVE_PARAMETER_IS_ENABLED | NATIVE_PARAMETER_IS_AUTOMATABLE;
		if (native.parameterPolicy == 1) { hints &= ~NATIVE_PARAMETER_IS_ENABLED; }
		if (native.parameterPolicy == 2) { hints |= NATIVE_PARAMETER_IS_OUTPUT; }
		if (native.parameterPolicy == 3) { hints &= ~NATIVE_PARAMETER_IS_AUTOMATABLE; }
		if (native.parameterPolicy == 4) { hints |= NATIVE_PARAMETER_IS_INTEGER | NATIVE_PARAMETER_USES_SCALEPOINTS; }
		if (native.parameterPolicy == 5) { hints |= NATIVE_PARAMETER_IS_BOOLEAN; }
		static const NativeParameterScalePoint labels[]{{"Off", 0.f}, {"Warm", 1.f}, {"Bright", 2.f}, {"Fractional", .5f}};
		parameters[0].hints = static_cast<NativeParameterHints>(hints);
		parameters[0].ranges.max = native.parameterPolicy == 5 ? 1 : 100;
		parameters[0].scalePointCount = 4; parameters[0].scalePoints = labels;
	}
	return &parameters.at(index);
}
float parameterValue(NativePluginHandle, uint32_t index)
{
	if (native.flattenedParameters)
	{
		const auto target = flattenedParameter(index); return target.first ? target.first->parameters.at(target.second) : 0.f;
	}
	return native.values.at(index);
}
void setParameter(NativePluginHandle, uint32_t index, float value)
{
	++native.parameterWrites;
	if (native.flattenedParameters)
	{
		const auto target = flattenedParameter(index);
		if (target.first) { target.first->parameters.at(target.second) = value; }
		return;
	}
	native.values.at(index) = value;
}
void uiParameter(NativePluginHandle, uint32_t, float) { ++native.uiWrites; }
char* getState(NativePluginHandle)
{
	if (native.saveFailure == 1) { return nullptr; }
	if (native.saveFailure == 5) { throw std::runtime_error("native save exception"); }
	if (native.saveFailure == 6) { throw 29; }
	QDomDocument doc;
	auto root = doc.createElement("CARLA-PROJECT"); doc.appendChild(root);
	root.setAttribute("Name", native.stateName);
	if (native.patchbayInstrument) { root.setAttribute("PatchbayConnected", native.patchbayConnected ? 1 : 0); }
	for (const auto value : native.values)
	{
		auto parameter = doc.createElement("Parameter"); parameter.setAttribute("value", value); root.appendChild(parameter);
	}
	for (const auto& plugin : native.hosted)
	{
		auto element = doc.createElement("HostedPlugin");
		element.setAttribute("format", plugin.format); element.setAttribute("name", plugin.name);
		element.setAttribute("label", plugin.label); element.setAttribute("path", plugin.path);
		element.setAttribute("uniqueId", QString::number(plugin.uniqueId)); element.setAttribute("active", plugin.active ? 1 : 0);
		for (const auto value : plugin.parameters)
		{
			auto parameter = doc.createElement("HostedParameter"); parameter.setAttribute("value", value); element.appendChild(parameter);
		}
		root.appendChild(element);
	}
	const auto bytes = native.saveFailure == 2 ? QByteArray{}
		: native.saveFailure == 3 ? QByteArray("<CARLA-PROJECT>")
		: native.saveFailure == 4 ? QByteArray("<unrelated/>") : doc.toByteArray();
	auto* result = static_cast<char*>(std::malloc(bytes.size() + 1));
	if (result) { std::memcpy(result, bytes.constData(), bytes.size() + 1); }
	return result;
}
void setState(NativePluginHandle, const char* data)
{
	++native.stateLoads;
	if (native.loadFailure)
	{
		native.values = {0.f}; // Failure after native state has already changed.
		if (native.loadFailure == 1) { throw std::runtime_error("native load exception"); }
		throw 31;
	}
	QDomDocument doc;
	if (!doc.setContent(QByteArray(data))) { return; }
	native.stateName = doc.documentElement().attribute("Name");
	if (native.patchbayInstrument) { native.patchbayConnected = doc.documentElement().attribute("PatchbayConnected").toInt() != 0; }
	native.values.clear();
	for (auto element = doc.documentElement().firstChildElement("Parameter"); !element.isNull();
		element = element.nextSiblingElement("Parameter"))
	{
		native.values.push_back(element.attribute("value").toFloat());
	}
	native.hosted.clear();
	for (auto element = doc.documentElement().firstChildElement("HostedPlugin"); !element.isNull();
		element = element.nextSiblingElement("HostedPlugin"))
	{
		native.hosted.push_back({element.attribute("format").toInt(), element.attribute("name"), element.attribute("label"),
			element.attribute("path"), element.attribute("uniqueId").toLongLong(), element.attribute("active").toInt() != 0});
		auto& parameters = native.hosted.back().parameters; parameters.clear();
		for (auto parameter = element.firstChildElement("HostedParameter"); !parameter.isNull(); parameter = parameter.nextSiblingElement("HostedParameter"))
		{
			parameters.push_back(parameter.attribute("value").toFloat());
		}
	}
	// A real native state restore can announce a changed parameter layout before returning.
	native.host->dispatcher(native.host->handle, NATIVE_HOST_OPCODE_RELOAD_PARAMETERS, 0, 0, nullptr, 0);
}
void activate(NativePluginHandle) { ++native.activations; }
void deactivate(NativePluginHandle) { --native.activations; }
intptr_t dispatch(NativePluginHandle, NativePluginDispatcherOpcode opcode, int32_t, intptr_t value, void*, float)
{
	if (opcode == NATIVE_PLUGIN_OPCODE_OFFLINE_CHANGED) { native.offlineChanges.push_back(value != 0); }
	return 0;
}
#if CARLA_VERSION_HEX >= CARLA_VERSION_HEX_3
void process(NativePluginHandle, const float**, float** output, uint32_t frames, const NativeMidiEvent* midi, uint32_t count)
#else
void process(NativePluginHandle, float**, float** output, uint32_t frames, const NativeMidiEvent* midi, uint32_t count)
#endif
{
	native.midi.assign(midi, midi + count);
	native.emittingMidi = true; const auto clearProcessing = qScopeGuard([] { native.emittingMidi = false; });
	native.midiOutputAccepted.clear();
	const auto emitMidi = []
	{
		for (auto event : native.emittedMidi)
		{
			native.midiOutputAccepted.push_back(native.host->write_midi_event(native.host->handle, &event));
			std::memset(&event, 0, sizeof(event)); // The engine reuses callback storage.
		}
		if (native.nullMidiOutput) { native.midiOutputAccepted.push_back(native.host->write_midi_event(native.host->handle, nullptr)); }
	};
	if (native.midiFromWorker) { std::thread worker(emitMidi); worker.join(); } else { emitMidi(); }
	if (native.processFailure == 1) { throw std::runtime_error("native processing failed after emitting MIDI"); }
	if (native.processFailure == 2) { throw 47; }
	native.processingWasOffline = native.host->is_offline(native.host->handle);
	if (!native.offlineChanges.empty())
	{
		native.offlineDispatchedBeforeProcess &= native.offlineChanges.back() == native.processingWasOffline;
	}
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		output[0][frame] = native.values.at(0) / 100.f;
		output[1][frame] = -output[0][frame];
	}
}
const NativePluginDescriptor descriptor = []
{
	NativePluginDescriptor result{.midiIns = 1, .midiOuts = 1};
	result.instantiate = instantiate; result.cleanup = cleanup;
	result.get_parameter_count = parameterCount; result.get_parameter_info = parameterInfo;
	result.get_parameter_value = parameterValue; result.set_parameter_value = setParameter;
	result.ui_set_parameter_value = uiParameter;
	result.activate = activate; result.deactivate = deactivate; result.process = process;
	result.get_state = getState; result.set_state = setState; result.dispatcher = dispatch;
	return result;
}();
const NativePluginDescriptor withoutSave = [] { auto result = descriptor; result.get_state = nullptr; return result; }();
const NativePluginDescriptor withoutLoad = [] { auto result = descriptor; result.set_state = nullptr; return result; }();
const Plugin::Descriptor lmmsDescriptor = {
	"carlahosttest", "Carla host test", "Native host fixture", "LMMS", 1,
	Plugin::Type::Instrument, nullptr, nullptr, nullptr
};

#ifdef LMMS_CARLA_NATIVE_HOST_API
CarlaHostHandle attachHost(const NativePluginDescriptor* desc, NativePluginHandle handle)
{
	++native.hostCreates;
	if (native.attachFails || desc != &descriptor || handle != &native) { return nullptr; }
	return reinterpret_cast<CarlaHostHandle>(&native);
}
void freeHost(CarlaHostHandle)
{
	++native.hostFrees; native.releasedBeforeEngine = native.host && native.activations == 1;
}
uint32_t hostCount(CarlaHostHandle) { return static_cast<uint32_t>(native.hosted.size()); }
void notifyHostLayout()
{
	if (native.reloadDuringHostEdit)
	{
		native.host->dispatcher(native.host->handle, NATIVE_HOST_OPCODE_RELOAD_PARAMETERS, 0, 0, nullptr, 0);
	}
}
const CarlaPluginInfo* hostInfo(CarlaHostHandle, uint index)
{
	if (native.hostFailure == 4) { return nullptr; }
	const auto& plugin = native.hosted.at(index);
	static CarlaPluginInfo info;
	static QByteArray name, label, path;
	name = plugin.name.toUtf8(); label = plugin.label.toUtf8(); path = plugin.path.toUtf8();
	info.type = static_cast<PluginType>(plugin.format); info.name = name.constData(); info.label = label.constData();
	info.filename = path.constData(); info.maker = "native host fixture"; info.uniqueId = plugin.uniqueId;
	return &info;
}
float hostInternalValue(CarlaHostHandle, uint index, int32_t)
{
	return native.hosted.at(index).active ? 1.f : 0.f;
}
bool hostAdd(CarlaHostHandle, BinaryType, PluginType type, const char* path, const char* name,
	const char* label, int64_t uniqueId, const void*, uint)
{
	++native.hostMutations;
	if (native.hostFailure == 1) { return false; }
	native.hosted.push_back({int(type), QString::fromUtf8(name), QString::fromUtf8(label),
		QString::fromUtf8(path), uniqueId, true});
	notifyHostLayout();
	return native.hostFailure != 2;
}
bool hostRemove(CarlaHostHandle, uint index)
{
	++native.hostMutations;
	if (native.hostFailure == 1) { return false; }
	native.hosted.erase(native.hosted.begin() + index); notifyHostLayout(); return true;
}
bool hostRename(CarlaHostHandle, uint index, const char* name)
{
	++native.hostMutations;
	if (native.hostFailure == 1) { return false; }
	native.hosted.at(index).name = QString::fromUtf8(name); return true;
}
bool hostSwap(CarlaHostHandle, uint first, uint second)
{
	++native.hostMutations;
	if (native.hostFailure == 1) { return false; }
	std::swap(native.hosted.at(first), native.hosted.at(second)); notifyHostLayout(); return true;
}
void hostActive(CarlaHostHandle, uint index, bool active)
{
	++native.hostMutations;
	if (native.hostFailure != 3) { native.hosted.at(index).active = active; }
}
const char* hostLastError(CarlaHostHandle) { return "host fixture rejection"; }
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
void graphCallback(CarlaHostHandle, CARLA_BACKEND_NAMESPACE::EngineCallbackFunc callback, void* context)
{
	if (callback)
	{
		++native.patchbayAttaches;
		native.patchbayInstalledWhileInactive &= native.activations == 0 && native.hosted.empty();
	}
	native.graphCallback = callback; native.graphContext = context;
}
bool graphRefresh(CarlaHostHandle, bool external)
{
	using namespace CARLA_BACKEND_NAMESPACE;
	++native.patchbayRefreshes; native.patchbayExternalRequested |= external;
	const auto sink = std::numeric_limits<uint32_t>::max();
	native.graphCallback(native.graphContext, ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, 2, PATCHBAY_ICON_PLUGIN, 0, 0, 0, "Hosted synth");
	native.graphCallback(native.graphContext, ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, sink, PATCHBAY_ICON_HARDWARE, -1, 0, 0, "LMMS output");
	native.graphCallback(native.graphContext, ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, 2, 4, PATCHBAY_PORT_TYPE_AUDIO, 0, 0, "audio out");
	native.graphCallback(native.graphContext, ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, sink, 1, PATCHBAY_PORT_TYPE_AUDIO | PATCHBAY_PORT_IS_INPUT, 0, 0, "audio in");
	if (native.patchbayConnected)
	{
		native.patchbayConnection = static_cast<uint32_t>(100 * native.patchbayRefreshes);
		native.graphCallback(native.graphContext, ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, native.patchbayConnection, 0, 0, 0, 0, "2:4:4294967295:1");
	}
	// CarlaEngineNative delivers its NativeHostDescriptor notifications through
	// an independent path after the base engine callback. Exercise both paths.
	native.host->ui_parameter_changed(native.host->handle, 0, native.values[0]);
	return true;
}
bool graphConnect(CarlaHostHandle, bool external, uint groupA, uint portA, uint groupB, uint portB)
{
	++native.patchbayEdits; native.patchbayExternalRequested |= external;
	if (groupA != 2 || portA != 4 || groupB != std::numeric_limits<uint32_t>::max() || portB != 1) { return false; }
	native.patchbayConnected = true;
	return native.patchbayFailure == 0;
}
bool graphDisconnect(CarlaHostHandle, bool external, uint id)
{
	++native.patchbayEdits; native.patchbayExternalRequested |= external; native.patchbayDisconnected = id;
	if (id != native.patchbayConnection) { return false; }
	native.patchbayConnected = false; return true;
}
#endif
uint32_t hostedParameterCount(CarlaHostHandle, uint plugin)
{
	return native.hostedParameterFailure == 10 ? std::numeric_limits<uint32_t>::max()
		: static_cast<uint32_t>(native.hosted.at(plugin).parameters.size());
}
QByteArray hostedParameterName;
const CarlaParameterInfo* hostedParameterInfo(CarlaHostHandle, uint, uint32_t index)
{
	if (native.hostedParameterFailure == 1) { return nullptr; }
	static CarlaParameterInfo info;
	hostedParameterName = "Nested " + QByteArray::number(index);
	info.name = hostedParameterName.constData(); info.symbol = "nested_control"; info.unit = "unit";
	info.comment = "Native control documentation"; info.groupName = "filter:Filter";
	info.scalePointCount = native.hostedParameterFailure == 11 ? std::numeric_limits<uint32_t>::max() : index == 0 ? 3 : 0;
	return &info;
}
const ParameterData* hostedParameterData(CarlaHostHandle, uint, uint32_t index)
{
	if (native.hostedParameterFailure == 2) { return nullptr; }
	// Invalidate metadata strings from the preceding native call.
	hostedParameterName.fill('X');
	static ParameterData data; data = {};
	data.type = index == 3 ? CARLA_BACKEND_NAMESPACE::PARAMETER_OUTPUT : CARLA_BACKEND_NAMESPACE::PARAMETER_INPUT;
	data.index = static_cast<int32_t>(index); data.rindex = 100 + static_cast<int32_t>(index);
	data.hints = CARLA_BACKEND_NAMESPACE::PARAMETER_IS_ENABLED | CARLA_BACKEND_NAMESPACE::PARAMETER_IS_AUTOMATABLE;
	if (index == 0) { data.hints |= CARLA_BACKEND_NAMESPACE::PARAMETER_USES_SCALEPOINTS | CARLA_BACKEND_NAMESPACE::PARAMETER_USES_CUSTOM_TEXT | CARLA_BACKEND_NAMESPACE::PARAMETER_IS_LOGARITHMIC; }
	if (index == 1) { data.hints |= CARLA_BACKEND_NAMESPACE::PARAMETER_IS_INTEGER; }
	if (index == 2) { data.hints |= CARLA_BACKEND_NAMESPACE::PARAMETER_IS_BOOLEAN; }
	if (index == 4) { data.hints &= ~CARLA_BACKEND_NAMESPACE::PARAMETER_IS_ENABLED; }
	if (index == 5) { data.hints |= CARLA_BACKEND_NAMESPACE::PARAMETER_IS_READ_ONLY; }
	if (index == 6) { data.hints |= CARLA_BACKEND_NAMESPACE::PARAMETER_IS_NOT_SAVED; }
	if (index == 7) { data.hints &= ~CARLA_BACKEND_NAMESPACE::PARAMETER_IS_AUTOMATABLE; }
	return &data;
}
const ParameterRanges* hostedParameterRanges(CarlaHostHandle, uint, uint32_t index)
{
	if (native.hostedParameterFailure == 3) { return nullptr; }
	static ParameterRanges ranges; ranges = {};
	ranges.def = index == 2 ? -1.f : 0.f; ranges.min = index == 2 ? -1.f : 0.f; ranges.max = index == 1 ? 10.f : 1.f;
	ranges.step = .01f; ranges.stepSmall = .001f; ranges.stepLarge = .1f;
	if (native.hostedParameterFailure == 5) { ranges.min = ranges.max + 1; }
	if (native.hostedParameterFailure == 9) { ranges.step = std::numeric_limits<float>::quiet_NaN(); }
	return &ranges;
}
const CarlaScalePointInfo* hostedScalePoint(CarlaHostHandle, uint, uint32_t, uint32_t index)
{
	if (native.hostedParameterFailure == 7) { return nullptr; }
	static CarlaScalePointInfo point; static QByteArray label;
	point.value = native.hostedParameterFailure == 6 ? std::numeric_limits<float>::quiet_NaN() : .25f * (index + 1);
	label = "Choice " + QByteArray::number(index); point.label = label.constData(); return &point;
}
float hostedParameterValue(CarlaHostHandle, uint plugin, uint32_t index)
{
	return native.hostedParameterFailure == 4 ? std::numeric_limits<float>::quiet_NaN() : native.hosted.at(plugin).parameters.at(index);
}
const char* hostedParameterText(CarlaHostHandle, uint, uint32_t) { return "Native displayed value"; }
void hostedParameterWrite(CarlaHostHandle, uint plugin, uint32_t index, float value)
{
	++native.hostedParameterWrites;
	if (native.hostedWriteBehavior == 1) { return; }
	native.hosted.at(plugin).parameters.at(index) = native.hostedWriteBehavior == 2 ? std::floor(value * 4) / 4 : value;
	if (native.hostedWriteBehavior == 3) { throw std::runtime_error("hosted parameter failed after mutation"); }
	if (native.hostedParameterFailure == 8) { native.hosted.at(plugin).parameters.at(index) = std::numeric_limits<float>::infinity(); }
}
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
CarlaPluginDiscoveryHandle startDiscovery(const char*, BinaryType binary, PluginType format, const char*,
	CarlaPluginDiscoveryCallback callback, CarlaPluginCheckCacheCallback, void* context)
{
	++native.discoveryStarts;
	CarlaPluginDiscoveryInfo info;
	info.btype = binary; info.ptype = format; info.filename = ""; info.label = "native-test-synth";
	info.metadata.name = "Discovered synth"; info.metadata.maker = "Native fixture"; info.uniqueId = 42;
	info.metadata.hints = CARLA_BACKEND_NAMESPACE::PLUGIN_IS_SYNTH; info.io.audioOuts = 2; info.io.midiIns = 1;
	callback(context, &info, nullptr);
	return &native;
}
bool pollDiscovery(CarlaPluginDiscoveryHandle) { return false; }
void stopDiscovery(CarlaPluginDiscoveryHandle) { ++native.discoveryStops; }
#endif
CarlaPluginHost::Resolver hostResolver()
{
	return [](const char* symbol) -> QFunctionPointer
	{
		if (native.missingSymbol == symbol) { return nullptr; }
#define SYMBOL(name, function) \
		static_assert(std::is_same_v<decltype(&function), decltype(&carla_##name)>, "Native host fixture ABI must match Carla"); \
		if (std::strcmp(symbol, "carla_" #name) == 0) { return reinterpret_cast<QFunctionPointer>(&function); }
		SYMBOL(create_native_plugin_host_handle, attachHost)
		SYMBOL(host_handle_free, freeHost)
		SYMBOL(get_current_plugin_count, hostCount)
		SYMBOL(get_plugin_info, hostInfo)
		SYMBOL(get_internal_parameter_value, hostInternalValue)
		SYMBOL(add_plugin, hostAdd)
		SYMBOL(remove_plugin, hostRemove)
		SYMBOL(rename_plugin, hostRename)
		SYMBOL(switch_plugins, hostSwap)
		SYMBOL(set_active, hostActive)
		SYMBOL(get_last_error, hostLastError)
		SYMBOL(get_parameter_count, hostedParameterCount)
		SYMBOL(get_parameter_info, hostedParameterInfo)
		SYMBOL(get_parameter_data, hostedParameterData)
		SYMBOL(get_parameter_ranges, hostedParameterRanges)
		SYMBOL(get_parameter_scalepoint_info, hostedScalePoint)
		SYMBOL(get_current_parameter_value, hostedParameterValue)
		SYMBOL(get_parameter_text, hostedParameterText)
		SYMBOL(set_parameter_value, hostedParameterWrite)
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
		SYMBOL(set_engine_callback, graphCallback)
		SYMBOL(patchbay_refresh, graphRefresh)
		SYMBOL(patchbay_connect, graphConnect)
		SYMBOL(patchbay_disconnect, graphDisconnect)
#endif
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
		SYMBOL(plugin_discovery_start, startDiscovery)
		SYMBOL(plugin_discovery_idle, pollDiscovery)
		SYMBOL(plugin_discovery_stop, stopDiscovery)
#endif
#undef SYMBOL
		return nullptr;
	};
}
#endif

class NativeCarlaTrack : public InstrumentTrack
{
public:
	NativeCarlaTrack() : InstrumentTrack(Engine::getSong()) {}
	int constructionFailure = 0;
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override
	{
		if (constructionFailure == 1) { throw std::runtime_error("instrument factory exception"); }
		if (constructionFailure == 2) { throw 37; }
		CarlaPluginHost::Resolver resolver;
#ifdef LMMS_CARLA_NATIVE_HOST_API
		if (native.management) { resolver = hostResolver(); }
#endif
		return new CarlaInstrument(this, &lmmsDescriptor, native.patchbayInstrument, resolver);
	}
};

QByteArray contents(const QString& path)
{
	QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool trackUnlocked(InstrumentTrack* track)
{
	bool acquired = false;
	std::thread probe([&] { acquired = track->tryLock(); if (acquired) { track->unlock(); } });
	probe.join(); return acquired;
}

struct InstrumentSession
{
	std::unique_ptr<InstrumentTrack> track = std::make_unique<InstrumentTrack>(Engine::getSong());
	std::unique_ptr<CarlaInstrument> instrument;
	explicit InstrumentSession(bool patchbay = false)
		: instrument(std::make_unique<CarlaInstrument>(track.get(), &lmmsDescriptor, patchbay)) {}
};

class NativeMidiSink final : public MidiClient
{
public:
	std::vector<MidiEvent> events;
	std::vector<TimePos> times;
	std::function<void()> onEvent;
	bool isRaw() const override { return true; }
	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort*) override
	{
		native.outputDuringProcess |= native.emittingMidi;
		events.push_back(event); times.push_back(time);
		if (onEvent) { onEvent(); }
	}
};

NativeMidiEvent outputMessage(uint32_t time, std::initializer_list<uint8_t> bytes)
{
	NativeMidiEvent event{}; event.time = time; event.size = static_cast<uint8_t>(bytes.size());
	std::copy(bytes.begin(), bytes.end(), event.data); return event;
}

QList<CarlaParamFloatModel*> parameters(CarlaInstrument* instrument)
{
	QList<CarlaParamFloatModel*> result;
	for (auto* child : instrument->children())
	{
		if (auto* parameter = dynamic_cast<CarlaParamFloatModel*>(child)) { result.append(parameter); }
	}
	return result;
}
}

// These entry points replace only the external Carla library in this test.
// The LMMS Carla.cpp host, native instrument/model code, and audio engine are real.
extern "C" {
const char* carla_get_library_filename() { return nullptr; }
const char* carla_get_library_folder() { return "."; }
const NativePluginDescriptor* carla_get_native_rack_plugin()
{
	return native.failure == 1 ? nullptr : native.failure == 4 ? &withoutSave : native.failure == 5 ? &withoutLoad : &descriptor;
}
const NativePluginDescriptor* carla_get_native_patchbay_plugin() { return carla_get_native_rack_plugin(); }
}

class CarlaHostTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid());
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stopExport(); Engine::getSong()->clearProject();
		Engine::getSong()->clearErrors();
		native = NativeState{};
	}
	void unavailableHostManagementDoesNotChangeTheNativeInstrument()
	{
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto response = StudioCapabilities::execute("host.inspect", {{"track", 0}});
		QVERIFY(response.value("ok").toBool());
		QVERIFY(!response.value("result").toObject().value("available").toBool());
		QVERIFY(!response.value("result").toObject().value("reason").toString().isEmpty());
		QVERIFY(!StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "internal"}, {"label", "test"}}).value("ok").toBool());
		QCOMPARE(native.values[0], 42.f); QCOMPARE(native.activations, 1); QCOMPARE(native.hostMutations, 0);
	}
#ifdef LMMS_CARLA_NATIVE_HOST_API
	void missingHostSymbolsAndFailedAttachmentPreserveTheNativeEngine_data()
	{
		QTest::addColumn<QByteArray>("missing");
		for (const char* symbol : {"carla_create_native_plugin_host_handle", "carla_host_handle_free", "carla_get_current_plugin_count",
			"carla_get_plugin_info", "carla_get_internal_parameter_value", "carla_add_plugin", "carla_remove_plugin",
			"carla_rename_plugin", "carla_switch_plugins", "carla_set_active", "carla_get_last_error"})
		{
			QTest::newRow(symbol) << QByteArray(symbol);
		}
		QTest::newRow("failed-attachment") << QByteArray{};
	}
	void missingHostSymbolsAndFailedAttachmentPreserveTheNativeEngine()
	{
		QFETCH(QByteArray, missing); native.management = true; native.missingSymbol = missing; native.attachFails = missing.isEmpty();
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto response = StudioCapabilities::execute("host.inspect", {{"track", 0}});
		QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("result").toObject().value("available").toBool());
		QCOMPARE(native.hostCreates, missing.isEmpty() ? 1 : 0); QCOMPARE(native.hostMutations, 0);
		QCOMPARE(native.activations, 1); QCOMPARE(native.cleanups, 0);
		track.reset(); QCOMPARE(native.hostFrees, 0); QCOMPARE(native.cleanups, 1);
	}
	void nativeHostOperationsUseTheOwnedEngineAndSurviveStateRestore()
	{
		native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		QVERIFY(StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject().value("available").toBool());
		QCOMPARE(native.hostCreates, 1); QCOMPARE(native.activations, 1);
		const auto add = StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "internal"},
			{"label", "synth"}, {"name", "First"}, {"uniqueId", "9223372036854775807"}});
		QVERIFY(add.value("ok").toBool()); QCOMPARE(add.value("result").toObject().value("added").toInt(-1), 0);
		QCOMPARE(native.hosted[0].uniqueId, std::numeric_limits<qint64>::max());
		QVERIFY(StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "lv2"},
			{"label", "urn:lmms:test"}, {"name", "Second"}}).value("ok").toBool());
		auto entries = StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject().value("plugins").toArray();
		QCOMPARE(entries.size(), 2); QCOMPARE(entries[0].toObject().value("name").toString(), QString("First"));
		QCOMPARE(entries[1].toObject().value("name").toString(), QString("Second"));
		QCOMPARE(entries[0].toObject().value("uniqueId").toString(), QString("9223372036854775807"));
		QVERIFY(StudioCapabilities::execute("host.plugin.rename", {{"track", 0}, {"hostedPlugin", 0}, {"name", "Lead"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("host.plugin.active", {{"track", 0}, {"hostedPlugin", 0}, {"active", false}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("host.plugin.swap", {{"track", 0}, {"hostedPlugin", 0}, {"targetPlugin", 1}}).value("ok").toBool());
		QCOMPARE(native.hosted[1].name, QString("Lead")); QVERIFY(!native.hosted[1].active);
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto saved = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty());
		QVERIFY(StudioCapabilities::execute("host.plugin.remove", {{"track", 0}, {"hostedPlugin", 0}}).value("ok").toBool());
		QCOMPARE(native.hosted.size(), std::size_t{1}); QCOMPARE(native.hosted[0].name, QString("Lead"));
		track->restoreState(saved);
		QVERIFY(native.releasedBeforeEngine); QCOMPARE(native.hostFrees, 1);
		entries = StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject().value("plugins").toArray();
		QCOMPARE(entries.size(), 2); QCOMPARE(entries[1].toObject().value("name").toString(), QString("Lead"));
		QVERIFY(!entries[1].toObject().value("active").toBool()); QCOMPARE(native.hostCreates, 2);
		QCOMPARE(native.activations, 1); QCOMPARE(native.uiWrites, 0);
		track.reset(); QCOMPARE(native.hostFrees, 2); QCOMPARE(native.cleanups, 2); QVERIFY(native.releasedBeforeEngine);
	}
	void invalidHostRequestsDoNotReachTheNativeApi_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("arguments");
		QTest::newRow("unknown-format") << QString("host.plugin.add") << QJsonObject{{"format", "unknown"}};
		QTest::newRow("missing-label") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}};
		QTest::newRow("missing-binary") << QString("host.plugin.add") << QJsonObject{{"format", "vst2"}};
		QTest::newRow("unknown-binary") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"binary", "unknown"}};
		QTest::newRow("null-name") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"name", QString(QChar::Null)}};
		QTest::newRow("numeric-id") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"uniqueId", 123}};
		QTest::newRow("overflow-id") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"uniqueId", "9223372036854775808"}};
		QTest::newRow("nondecimal-id") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"uniqueId", "1e3"}};
		QTest::newRow("newline-id") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}, {"uniqueId", "123\n"}};
		QTest::newRow("negative-index") << QString("host.plugin.remove") << QJsonObject{{"hostedPlugin", -1}};
		QTest::newRow("absent-index") << QString("host.plugin.remove") << QJsonObject{{"hostedPlugin", 1}};
		QTest::newRow("empty-name") << QString("host.plugin.rename") << QJsonObject{{"hostedPlugin", 0}, {"name", " "}};
		QTest::newRow("absent-swap-target") << QString("host.plugin.swap") << QJsonObject{{"hostedPlugin", 0}, {"targetPlugin", 1}};
		QTest::newRow("nonboolean-active") << QString("host.plugin.active") << QJsonObject{{"hostedPlugin", 0}, {"active", 1}};
	}
	void invalidHostRequestsDoNotReachTheNativeApi()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, arguments); native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		arguments.insert("track", 0);
		QVERIFY(!StudioCapabilities::execute(operation, arguments).value("ok").toBool());
		QCOMPARE(native.hostMutations, 0); QCOMPARE(native.hosted.size(), std::size_t{1});
		QCOMPARE(native.hosted[0].name, QString("Existing")); QVERIFY(native.hosted[0].active);
	}
	void nativeHostFailuresRemainVisibleAndNativeSnapshotsRecoverPartialChanges_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("arguments"); QTest::addColumn<int>("failure");
		QTest::newRow("failed-add") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}} << 1;
		QTest::newRow("partial-add") << QString("host.plugin.add") << QJsonObject{{"format", "internal"}, {"label", "test"}} << 2;
		QTest::newRow("failed-remove") << QString("host.plugin.remove") << QJsonObject{{"hostedPlugin", 0}} << 1;
		QTest::newRow("failed-rename") << QString("host.plugin.rename") << QJsonObject{{"hostedPlugin", 0}, {"name", "New"}} << 1;
		QTest::newRow("failed-swap") << QString("host.plugin.swap") << QJsonObject{{"hostedPlugin", 0}, {"targetPlugin", 1}} << 1;
		QTest::newRow("unconfirmed-activation") << QString("host.plugin.active") << QJsonObject{{"hostedPlugin", 0}, {"active", false}} << 3;
		QTest::newRow("unavailable-info") << QString("host.inspect") << QJsonObject{} << 4;
	}
	void nativeHostFailuresRemainVisibleAndNativeSnapshotsRecoverPartialChanges()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, arguments); QFETCH(int, failure); native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "One", "one", {}, 0, true},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_LV2), "Two", "urn:test", {}, 0, true}};
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto saved = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty());
		native.hostFailure = failure; arguments.insert("track", 0);
		const auto response = StudioCapabilities::execute(operation, arguments);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("host fixture rejection"));
		if (failure == 2) { QCOMPARE(native.hosted.size(), std::size_t{3}); }
		native.hostFailure = 0; track->restoreState(saved);
		const auto recovered = StudioCapabilities::execute("host.inspect", {{"track", 0}});
		QVERIFY(recovered.value("ok").toBool()); QCOMPARE(native.hosted.size(), std::size_t{2});
		QCOMPARE(native.hosted[0].name, QString("One")); QCOMPARE(native.hosted[1].name, QString("Two"));
		QVERIFY(native.hosted[0].active); QVERIFY(trackUnlocked(track.get()));
	}
#endif
#ifdef LMMS_CARLA_NATIVE_HOST_API
	void hostedSwapPreservesAutomationIdentityAndNativeSaveBindings()
	{
		native.management = true; native.flattenedParameters = true; native.reloadDuringHostEdit = true;
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "A", "a", {}, 0, true, {.1f, .2f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "B", "b", {}, 0, true, {.3f, .4f, .5f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "C", "c", {}, 0, true, {.6f}}};
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto models = parameters(static_cast<CarlaInstrument*>(track->instrument())); QCOMPARE(models.size(), 6);
		auto* a = models[0]; auto* b = models[2]; const auto aId = a->id(); const auto bId = b->id();
		auto* automation = static_cast<AutomationTrack*>(Track::create(Track::Type::Automation, Engine::getSong()));
		auto* curve = static_cast<AutomationClip*>(automation->createClip(0));
		QVERIFY(curve->addObject(a)); curve->putValue(0, .8f, false);
		const auto swapped = StudioCapabilities::execute("host.plugin.swap", {{"track", 0}, {"hostedPlugin", 0}, {"targetPlugin", 1}});
		QVERIFY2(swapped.value("ok").toBool(), qPrintable(swapped.value("error").toString()));
		QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(a));
		QCOMPARE(a->id(), aId); QCOMPARE(b->id(), bId); QCOMPARE(native.parameterWrites, 0);
		QCOMPARE(a->displayName(), QString("A:0")); QCOMPARE(b->displayName(), QString("B:0"));
		a->setValue(.8f, true); b->setValue(.7f, true);
		QCOMPARE(native.hosted[1].parameters[0], .8f); QCOMPARE(native.hosted[0].parameters[0], .7f);
		QCOMPARE(native.hosted[0].parameters[2], .5f); QCOMPARE(native.parameterWrites, 2);
		DataFile saved(DataFile::Type::SongProject);
		const auto trackState = track->saveState(saved, saved.content());
		auto curveState = saved.createElement("curve"); saved.content().appendChild(curveState); curve->saveSettings(saved, curveState);
		QVERIFY(DataFile::serializationError(saved).isEmpty());
		track->restoreState(trackState); curve->loadSettings(curveState); AutomationClip::resolveAllIDs();
		QCOMPARE(curve->objects().size(), std::size_t{1});
		auto* restored = curve->objects().front().data(); QVERIFY(restored);
		QCOMPARE(restored->displayName(), QString("A:0")); restored->setValue(.9f, true);
		QCOMPARE(native.hosted[1].parameters[0], .9f); QCOMPARE(native.hosted[0].parameters[0], .7f);
		QCOMPARE(native.uiWrites, 0); QVERIFY(trackUnlocked(track.get()));
	}
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	void unavailablePatchbayPreservesOtherHostedCapabilities_data()
	{
		QTest::addColumn<QByteArray>("missing");
		for (const char* name : {"carla_set_engine_callback", "carla_patchbay_refresh", "carla_patchbay_connect", "carla_patchbay_disconnect", "rack", "preloaded-engine"})
		{
			QTest::newRow(name) << QByteArray(name);
		}
	}
	void unavailablePatchbayPreservesOtherHostedCapabilities()
	{
		QFETCH(QByteArray, missing); native.management = true; native.patchbayInstrument = missing != "rack"; native.missingSymbol = missing;
		if (missing == "preloaded-engine") { native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "existing", {}, 0, true}); }
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto inspection = StudioCapabilities::execute("host.inspect", {{"track", 0}}); QVERIFY(inspection.value("ok").toBool());
		const auto host = inspection.value("result").toObject(); QVERIFY(host.value("available").toBool());
		QVERIFY(host.value("parameterAccess").toObject().value("available").toBool());
		QVERIFY(!host.value("patchbay").toObject().value("available").toBool());
		QVERIFY(!host.value("patchbay").toObject().value("reason").toString().isEmpty());
		QVERIFY(!StudioCapabilities::execute("host.patchbay.inspect", {{"track", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "internal"}, {"label", "native-synth"}}).value("ok").toBool());
		QCOMPARE(native.patchbayRefreshes, 0); QCOMPARE(native.patchbayEdits, 0); QVERIFY(!native.graphCallback);
		QCOMPARE(native.hosted.size(), missing == "preloaded-engine" ? std::size_t{2} : std::size_t{1}); QVERIFY(trackUnlocked(track.get()));
	}
	void nativePatchbayRoutesPersistThroughStudioAndRecoverFromPartialEdits()
	{
		native.management = true; native.patchbayInstrument = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		QVERIFY(StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "internal"}, {"label", "native-synth"}}).value("ok").toBool());
		const auto* nativeHost = native.host; native.values[0] = 63.25f;
		for (const auto* operation : {"host.patchbay.inspect", "host.patchbay.connect", "host.patchbay.disconnect"})
		{
			QVERIFY(StudioCapabilities::mutatesProject(operation));
		}
		const auto available = StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject();
		QVERIFY(available.value("patchbay").toObject().value("available").toBool());
		const auto snapshot = StudioCapabilities::execute("host.patchbay.inspect", {{"track", 0}}); QVERIFY(snapshot.value("ok").toBool());
		const auto graph = snapshot.value("result").toObject(); QCOMPARE(graph.value("clients").toArray().size(), 2);
		QVERIFY(graph.value("connections").toArray().isEmpty()); QVERIFY(!graph.value("external").toBool());
		QCOMPARE(graph.value("clients").toArray()[1].toObject().value("group").toDouble(), 4294967295.0);
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
		QCOMPARE(parameters(static_cast<CarlaInstrument*>(track->instrument()))[0]->value(), 63.25f);
#endif
		QCOMPARE(native.host, nativeHost); QCOMPARE(native.parameterWrites, 0); QVERIFY(native.graphCallback);
		QCOMPARE(native.patchbayAttaches, 1); QVERIFY(native.patchbayInstalledWhileInactive);
		const QJsonObject endpoints{{"track", 0}, {"sourceGroup", 2}, {"sourcePort", 4}, {"destinationGroup", 4294967295.0}, {"destinationPort", 1}};
		const auto connected = StudioCapabilities::execute("host.patchbay.connect", endpoints); QVERIFY(connected.value("ok").toBool());
		QVERIFY(native.patchbayConnected); const auto connectionId = native.patchbayConnection;
		QCOMPARE(connected.value("result").toObject().value("connections").toArray().size(), 1);
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto saved = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty());
		QVERIFY(StudioCapabilities::execute("host.patchbay.disconnect", endpoints).value("ok").toBool());
		QVERIFY(!native.patchbayConnected); QVERIFY(native.patchbayDisconnected != connectionId);
		QCOMPARE(native.patchbayAttaches, 1); // Multiple refreshes never replace the native callback slot.
		track->restoreState(saved); QVERIFY(native.patchbayConnected); QVERIFY(native.releasedBeforeEngine);
		QVERIFY(StudioCapabilities::execute("host.patchbay.inspect", {{"track", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("host.patchbay.disconnect", endpoints).value("ok").toBool());
		QDomDocument beforeFailure; auto beforeRoot = beforeFailure.createElement("saved"); beforeFailure.appendChild(beforeRoot);
		const auto before = track->saveState(beforeFailure, beforeRoot); QVERIFY(DataFile::serializationError(beforeFailure).isEmpty());
		native.patchbayFailure = 1;
		const auto failed = StudioCapabilities::execute("host.patchbay.connect", endpoints); QVERIFY(!failed.value("ok").toBool());
		QVERIFY(native.patchbayConnected); QVERIFY(native.graphCallback); QVERIFY(trackUnlocked(track.get()));
		native.patchbayFailure = 0; track->restoreState(before); QVERIFY(!native.patchbayConnected);
		QVERIFY(StudioCapabilities::execute("host.patchbay.inspect", {{"track", 0}}).value("ok").toBool());
		QVERIFY(!native.patchbayExternalRequested); QCOMPARE(native.uiWrites, 0);
		QVERIFY(native.patchbayInstalledWhileInactive); QCOMPARE(native.patchbayAttaches, 3);
		track.reset(); QVERIFY(!native.graphCallback); QVERIFY(native.releasedBeforeEngine);
	}
	void invalidPatchbayArgumentsNeverRefreshOrEditTheGraph_data()
	{
		QTest::addColumn<QJsonObject>("changes");
		QTest::newRow("negative") << QJsonObject{{"sourceGroup", -1}};
		QTest::newRow("fractional") << QJsonObject{{"sourcePort", .5}};
		QTest::newRow("overflow") << QJsonObject{{"destinationGroup", 4294967296.0}};
		QTest::newRow("string") << QJsonObject{{"destinationPort", "1"}};
		QTest::newRow("null") << QJsonObject{{"sourceGroup", QJsonValue::Null}};
		QTest::newRow("missing") << QJsonObject{{"missingField", "sourcePort"}};
		QTest::newRow("connection-id") << QJsonObject{{"connectionId", 100}};
		QTest::newRow("external-graph") << QJsonObject{{"external", true}};
	}
	void invalidPatchbayArgumentsNeverRefreshOrEditTheGraph()
	{
		QFETCH(QJsonObject, changes); native.management = true; native.patchbayInstrument = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		QJsonObject arguments{{"track", 0}, {"sourceGroup", 2}, {"sourcePort", 4}, {"destinationGroup", 4294967295.0}, {"destinationPort", 1}};
		for (auto it = changes.begin(); it != changes.end(); ++it)
		{
			if (it.key() == "missingField") { arguments.remove(it.value().toString()); }
			else { arguments.insert(it.key(), it.value()); }
		}
		for (const auto* operation : {"host.patchbay.connect", "host.patchbay.disconnect"})
		{
			QVERIFY(!StudioCapabilities::execute(operation, arguments).value("ok").toBool());
		}
		QCOMPARE(native.patchbayRefreshes, 0); QCOMPARE(native.patchbayEdits, 0); QVERIFY(native.graphCallback); QVERIFY(trackUnlocked(track.get()));
	}
#endif
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	void nativeDiscoveryCandidatesLoadThroughStudioAndReleaseWithTheInstrument()
	{
		native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto toolPath = configuration.filePath("fixture-discovery-tool");
		QFile tool(toolPath); QVERIFY(tool.open(QIODevice::WriteOnly)); QVERIFY(tool.write("fixture") > 0); tool.close();
		const auto host = StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject();
		QVERIFY(host.value("discovery").toObject().value("available").toBool());
		const auto started = StudioCapabilities::execute("host.catalog.start", {{"track", 0}, {"format", "internal"}, {"discoveryTool", toolPath}});
		QVERIFY(started.value("ok").toBool()); const auto page = started.value("result").toObject();
		QCOMPARE(page.value("state").toString(), QString("running")); QCOMPARE(page.value("total").toInt(), 1);
		auto load = page.value("plugins").toArray()[0].toObject().value("load").toObject();
		QCOMPARE(load.value("uniqueId").toString(), QString("42")); load.insert("track", 0);
		QVERIFY(StudioCapabilities::execute("host.plugin.add", load).value("ok").toBool());
		QCOMPARE(native.hosted.size(), std::size_t{1}); QCOMPARE(native.hosted[0].name, QString("Discovered synth"));
		QCOMPARE(native.hosted[0].label, QString("native-test-synth")); QCOMPARE(native.hosted[0].uniqueId, qint64{42});
		const auto scan = page.value("scan").toString();
		const auto done = StudioCapabilities::execute("host.catalog.poll", {{"track", 0}, {"scan", scan}});
		QVERIFY(done.value("ok").toBool()); QCOMPARE(done.value("result").toObject().value("state").toString(), QString("finished"));
		QCOMPARE(native.discoveryStops, 1);
		QVERIFY(StudioCapabilities::execute("host.catalog.start", {{"track", 0}, {"format", "internal"}, {"discoveryTool", toolPath}}).value("ok").toBool());
		track.reset(); QCOMPARE(native.discoveryStarts, 2); QCOMPARE(native.discoveryStops, 2); QCOMPARE(native.cleanups, 1);
		QVERIFY(native.releasedBeforeEngine);
	}
#endif
	void hostedRemovalPreservesFollowingAutomationAndDetachesRemovedTargets()
	{
		native.management = true; native.flattenedParameters = true; native.reloadDuringHostEdit = true;
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "A", "a", {}, 0, true, {.1f, .2f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "B", "b", {}, 0, true, {.3f, .4f, .5f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "C", "c", {}, 0, true, {.6f}}};
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto models = parameters(static_cast<CarlaInstrument*>(track->instrument()));
		QPointer<CarlaParamFloatModel> removed = models[0]; auto* following = models[2]; auto* last = models[5];
		auto* automation = static_cast<AutomationTrack*>(Track::create(Track::Type::Automation, Engine::getSong()));
		auto* removedCurve = static_cast<AutomationClip*>(automation->createClip(0));
		auto* retainedCurve = static_cast<AutomationClip*>(automation->createClip(0));
		QVERIFY(removedCurve->addObject(removed.data())); QVERIFY(retainedCurve->addObject(following));
		QVERIFY(StudioCapabilities::execute("host.plugin.remove", {{"track", 0}, {"hostedPlugin", 0}}).value("ok").toBool());
		QVERIFY(removed.isNull()); QVERIFY(removedCurve->objects().empty());
		QCOMPARE(retainedCurve->objects().front().data(), static_cast<AutomatableModel*>(following));
		QCOMPARE(following->displayName(), QString("B:0")); QCOMPARE(last->displayName(), QString("C:0"));
		QCOMPARE(native.parameterWrites, 0);
		following->setValue(.75f, true); last->setValue(.85f, true);
		QCOMPARE(native.hosted[0].parameters[0], .75f); QCOMPARE(native.hosted[1].parameters[0], .85f);
		QCOMPARE(native.hosted[0].parameters[2], .5f);
	}
	void failedHostedLayoutEditsKeepModelBindingsUsable()
	{
		native.management = true; native.flattenedParameters = true; native.reloadDuringHostEdit = true;
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "A", "a", {}, 0, true, {.1f, .2f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "B", "b", {}, 0, true, {.3f, .4f}}};
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0]; const auto id = model->id();
		auto* automation = static_cast<AutomationTrack*>(Track::create(Track::Type::Automation, Engine::getSong()));
		auto* curve = static_cast<AutomationClip*>(automation->createClip(0)); QVERIFY(curve->addObject(model));
		native.hostFailure = 1;
		QVERIFY(!StudioCapabilities::execute("host.plugin.swap", {{"track", 0}, {"hostedPlugin", 0}, {"targetPlugin", 1}}).value("ok").toBool());
		QCOMPARE(model->id(), id); QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(model));
		model->setValue(.7f, true); QCOMPARE(native.hosted[0].parameters[0], .7f);
		native.hostFailure = 0;
		QVERIFY(StudioCapabilities::execute("host.plugin.swap", {{"track", 0}, {"hostedPlugin", 0}, {"targetPlugin", 1}}).value("ok").toBool());
		model->setValue(.8f, true); QCOMPARE(native.hosted[1].parameters[0], .8f); QCOMPARE(native.hosted[0].parameters[0], .3f);
	}
	void uncertainHostedLayoutsRejectBeforeLosingAutomation_data()
	{
		QTest::addColumn<bool>("missingCount"); QTest::newRow("outside-window") << false; QTest::newRow("missing-count") << true;
	}
	void uncertainHostedLayoutsRejectBeforeLosingAutomation()
	{
		QFETCH(bool, missingCount);
		native.management = true; native.flattenedParameters = true; native.nativeParameterSlots = 3;
		if (missingCount) { native.missingSymbol = "carla_get_parameter_count"; }
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "A", "a", {}, 0, true, {.1f, .2f}},
			{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "B", "b", {}, 0, true, {.3f, .4f, .5f}}};
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0];
		auto* automation = static_cast<AutomationTrack*>(Track::create(Track::Type::Automation, Engine::getSong()));
		auto* curve = static_cast<AutomationClip*>(automation->createClip(0)); QVERIFY(curve->addObject(model));
		const auto response = StudioCapabilities::execute("host.plugin.swap", {{"track", 0}, {"hostedPlugin", 0}, {"targetPlugin", 1}});
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains(missingCount ? "parameter layout" : "automation window"));
		QCOMPARE(native.hostMutations, 0); QCOMPARE(native.parameterWrites, 0); QCOMPARE(native.hosted[0].name, QString("A"));
		QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(model));
		model->setValue(.9f, true); QCOMPARE(native.hosted[0].parameters[0], .9f);
	}
	void appendingHostedPluginsPreservesExistingControllerBindings()
	{
		native.management = true; native.flattenedParameters = true; native.reloadDuringHostEdit = true;
		native.hosted = {{int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "A", "a", {}, 0, true, {.1f, .2f}}};
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0]; const auto id = model->id();
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", static_cast<int>(id)}}).value("ok").toBool());
		auto* connection = model->controllerConnection(); const auto writes = native.parameterWrites;
		QVERIFY(StudioCapabilities::execute("host.plugin.add", {{"track", 0}, {"format", "internal"}, {"label", "b"}, {"name", "B"}}).value("ok").toBool());
		QCOMPARE(model->id(), id); QCOMPARE(model->controllerConnection(), connection);
		QCOMPARE(model->displayName(), QString("A:0")); QCOMPARE(native.parameterWrites, writes);
		QCOMPARE(native.hosted[0].name, QString("A")); QCOMPARE(native.hosted.size(), std::size_t{2});
	}
	void missingHostedParameterSymbolsKeepManagementAvailable_data()
	{
		QTest::addColumn<QByteArray>("missing");
		for (const char* symbol : {"carla_get_parameter_count", "carla_get_parameter_info", "carla_get_parameter_data",
			"carla_get_parameter_ranges", "carla_get_parameter_scalepoint_info", "carla_get_current_parameter_value",
			"carla_get_parameter_text", "carla_set_parameter_value"})
		{
			QTest::newRow(symbol) << QByteArray(symbol);
		}
	}
	void missingHostedParameterSymbolsKeepManagementAvailable()
	{
		QFETCH(QByteArray, missing); native.management = true; native.missingSymbol = missing;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		const auto info = StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject();
		QVERIFY(info.value("available").toBool());
		const auto access = info.value("parameterAccess").toObject(); QVERIFY(!access.value("available").toBool());
		QVERIFY(access.value("reason").toString().contains(QString::fromUtf8(missing)));
		QVERIFY(!StudioCapabilities::execute("host.parameter.list", {{"track", 0}, {"hostedPlugin", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("host.plugin.rename", {{"track", 0}, {"hostedPlugin", 0}, {"name", "Renamed"}}).value("ok").toBool());
		QCOMPARE(native.hostCreates, 1); QCOMPARE(native.hostedParameterWrites, 0); QCOMPARE(native.hosted[0].name, QString("Renamed"));
	}
	void hostedParameterDiscoveryPagesNativeMetadataAndFractionalChoices()
	{
		native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		QVERIFY(StudioCapabilities::execute("host.inspect", {{"track", 0}}).value("result").toObject().value("parameterAccess").toObject().value("available").toBool());
		const auto first = StudioCapabilities::execute("host.parameter.list", {{"track", 0}, {"hostedPlugin", 0}, {"limit", 3}});
		QVERIFY(first.value("ok").toBool()); const auto page = first.value("result").toObject();
		QCOMPARE(page.value("total").toInt(), 8); QVERIFY(page.value("hasMore").toBool());
		const auto controls = page.value("parameters").toArray(); QCOMPARE(controls.size(), 3);
		QCOMPARE(controls[0].toObject().value("name").toString(), QString("Nested 0"));
		QCOMPARE(controls[1].toObject().value("name").toString(), QString("Nested 1"));
		QCOMPARE(controls[1].toObject().value("kind").toString(), QString("integer"));
		QCOMPARE(controls[2].toObject().value("kind").toString(), QString("boolean"));
		QCOMPARE(controls[2].toObject().value("minimum").toDouble(), -1.0);
		QCOMPARE(controls[0].toObject().value("nativeIndex").toInt(), 100);
		QVERIFY(controls[0].toObject().value("logarithmic").toBool());
		QCOMPARE(controls[0].toObject().value("text").toString(), QString("Native displayed value"));
		QCOMPARE(controls[0].toObject().value("group").toString(), QString("filter:Filter"));
		QCOMPARE(controls[0].toObject().value("scalePointCount").toInt(), 3);
		const auto tail = StudioCapabilities::execute("host.parameter.list", {{"track", 0}, {"hostedPlugin", 0}, {"offset", 3}}).value("result").toObject();
		QVERIFY(!tail.value("hasMore").toBool()); const auto remainder = tail.value("parameters").toArray(); QCOMPARE(remainder.size(), 5);
		for (int i = 0; i < 3; ++i) { QVERIFY(!remainder[i].toObject().value("writable").toBool()); }
		QVERIFY(!remainder[0].toObject().value("saved").toBool()); QVERIFY(!remainder[1].toObject().value("saved").toBool());
		QVERIFY(!remainder[3].toObject().value("saved").toBool());
		QVERIFY(remainder[4].toObject().value("writable").toBool()); QVERIFY(!remainder[4].toObject().value("automatable").toBool());
		const auto choices = StudioCapabilities::execute("host.parameter.inspect", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 0}, {"choiceLimit", 2}}).value("result").toObject();
		QVERIFY(choices.value("hasMoreChoices").toBool()); const auto points = choices.value("choices").toArray(); QCOMPARE(points.size(), 2);
		QCOMPARE(points[0].toObject().value("value").toDouble(), .25); QCOMPARE(points[1].toObject().value("value").toDouble(), .5);
		QCOMPARE(points[0].toObject().value("label").toString(), QString("Choice 0"));
		QCOMPARE(points[1].toObject().value("label").toString(), QString("Choice 1"));
		const auto last = StudioCapabilities::execute("host.parameter.inspect", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 0}, {"choiceOffset", 2}}).value("result").toObject();
		QVERIFY(!last.value("hasMoreChoices").toBool()); QCOMPARE(last.value("choices").toArray().size(), 1);
		const auto past = StudioCapabilities::execute("host.parameter.list", {{"track", 0}, {"hostedPlugin", 0}, {"offset", std::numeric_limits<int>::max()}}).value("result").toObject();
		QVERIFY(past.value("parameters").toArray().isEmpty()); QVERIFY(!past.value("hasMore").toBool());
		QCOMPARE(native.hostedParameterWrites, 0); QCOMPARE(native.parameterWrites, 0); QCOMPARE(native.values.size(), std::size_t{2});
	}
	void invalidHostedParameterRequestsNeverWriteNativeState_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("arguments");
		for (int index : {3, 4, 5})
		{
			QTest::newRow(qPrintable(QString("nonwritable-%1").arg(index))) << QString("host.parameter.set") << QJsonObject{{"hostedParameter", index}, {"value", .75}};
		}
		QTest::newRow("fractional-integer") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", 1}, {"value", 2.5}};
		QTest::newRow("boolean-interior") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", 2}, {"value", 0}};
		QTest::newRow("outside-range") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", 0}, {"value", 2}};
		QTest::newRow("wrong-value-type") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", 0}, {"value", ".5"}};
		QTest::newRow("absent-parameter") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", 8}, {"value", .5}};
		QTest::newRow("negative-parameter") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", -1}, {"value", .5}};
		QTest::newRow("fractional-parameter") << QString("host.parameter.set") << QJsonObject{{"hostedParameter", .5}, {"value", .5}};
		QTest::newRow("absent-plugin") << QString("host.parameter.list") << QJsonObject{{"hostedPlugin", 1}};
		QTest::newRow("zero-limit") << QString("host.parameter.list") << QJsonObject{{"limit", 0}};
		QTest::newRow("large-limit") << QString("host.parameter.list") << QJsonObject{{"limit", 1025}};
		QTest::newRow("negative-offset") << QString("host.parameter.list") << QJsonObject{{"offset", -1}};
		QTest::newRow("zero-choice-limit") << QString("host.parameter.inspect") << QJsonObject{{"hostedParameter", 0}, {"choiceLimit", 0}};
		QTest::newRow("large-choice-limit") << QString("host.parameter.inspect") << QJsonObject{{"hostedParameter", 0}, {"choiceLimit", 1025}};
		QTest::newRow("negative-choice-offset") << QString("host.parameter.inspect") << QJsonObject{{"hostedParameter", 0}, {"choiceOffset", -1}};
	}
	void invalidHostedParameterRequestsNeverWriteNativeState()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, arguments); native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		const auto original = native.hosted[0].parameters;
		arguments.insert("track", 0); if (!arguments.contains("hostedPlugin")) { arguments.insert("hostedPlugin", 0); }
		QVERIFY(!StudioCapabilities::execute(operation, arguments).value("ok").toBool());
		QCOMPARE(native.hostedParameterWrites, 0); QVERIFY(native.hosted[0].parameters == original);
	}
	void malformedHostedParameterMetadataAndReadbackFailExplicitly_data()
	{
		QTest::addColumn<int>("failure");
		for (int failure = 1; failure <= 11; ++failure) { QTest::newRow(qPrintable(QString::number(failure))) << failure; }
	}
	void malformedHostedParameterMetadataAndReadbackFailExplicitly()
	{
		QFETCH(int, failure); native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		native.hostedParameterFailure = failure;
		QJsonObject arguments{{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 0}};
		if (failure == 8) { arguments.insert("value", .75); }
		const auto response = StudioCapabilities::execute(failure == 8 ? "host.parameter.set" : "host.parameter.inspect", arguments);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.value("error").toString().isEmpty());
		QCOMPARE(native.hostedParameterWrites, failure == 8 ? 1 : 0);
	}
	void hostedParameterWritesReportReadbackAndRestoreThroughNativeState_data()
	{
		QTest::addColumn<int>("behavior");
		QTest::newRow("exact-write") << 0; QTest::newRow("ignored-write") << 1;
		QTest::newRow("quantized-write") << 2; QTest::newRow("partial-exception") << 3;
	}
	void hostedParameterWritesReportReadbackAndRestoreThroughNativeState()
	{
		QFETCH(int, behavior); native.management = true;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		native.hosted.push_back({int(CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL), "Existing", "test", {}, 0, true});
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto before = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty());
		native.hostedWriteBehavior = behavior;
		const auto response = StudioCapabilities::execute("host.parameter.set", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 0}, {"value", .7}});
		QCOMPARE(response.value("ok").toBool(), behavior != 3); QCOMPARE(native.hostedParameterWrites, 1);
		if (behavior != 3)
		{
			const auto result = response.value("result").toObject();
			QCOMPARE(result.value("requestedValue").toDouble(), .7);
			QCOMPARE(result.value("nativeRequestValue").toDouble(), double(.7f));
			QCOMPARE(result.value("value").toDouble(), double(native.hosted[0].parameters[0]));
			QCOMPARE(result.value("matchesNativeRequest").toBool(), behavior == 0);
			QVERIFY(result.value("confirmation").toString().contains("no acceptance status"));
		}
		else { QVERIFY(response.value("error").toString().contains("after mutation")); }
		QCOMPARE(native.hosted[0].parameters[0], behavior == 1 ? .25f : behavior == 2 ? .5f : .7f);
		native.hostedWriteBehavior = 0;
		if (behavior == 0)
		{
			QVERIFY(StudioCapabilities::execute("host.parameter.set", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 1}, {"value", 3}}).value("ok").toBool());
			QVERIFY(StudioCapabilities::execute("host.parameter.set", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 2}, {"value", 1}}).value("ok").toBool());
			QDomDocument edited; auto parent = edited.createElement("edited"); edited.appendChild(parent);
			const auto saved = track->saveState(edited, parent); QVERIFY(DataFile::serializationError(edited).isEmpty());
			native.hosted[0].parameters[0] = .1f; track->restoreState(saved);
			QCOMPARE(native.hosted[0].parameters[0], .7f); QCOMPARE(native.hosted[0].parameters[1], 3.f); QCOMPARE(native.hosted[0].parameters[2], 1.f);
		}
		track->restoreState(before);
		const auto recovered = StudioCapabilities::execute("host.parameter.inspect", {{"track", 0}, {"hostedPlugin", 0}, {"hostedParameter", 0}});
		QVERIFY(recovered.value("ok").toBool()); QCOMPARE(recovered.value("result").toObject().value("value").toDouble(), .25);
		QCOMPARE(native.hosted[0].parameters[1], 2.f); QCOMPARE(native.hosted[0].parameters[2], -1.f);
		QCOMPARE(native.values[0], 42.f); QCOMPARE(native.uiWrites, 0); QVERIFY(trackUnlocked(track.get()));
	}
#endif
	void headlessCallbacksNeedNoWidgetsOrNestedEventDispatch_data()
	{
		QTest::addColumn<bool>("patchbay");
		QTest::newRow("rack") << false; QTest::newRow("patchbay") << true;
	}
	void headlessCallbacksNeedNoWidgetsOrNestedEventDispatch()
	{
		QFETCH(bool, patchbay);
		InstrumentSession session(patchbay);
		QVERIFY(qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr);
		QVERIFY(native.host); QCOMPARE(native.activations, 1);
		QVERIFY(session.instrument->findChildren<QCompleter*>().isEmpty());
		QVERIFY(session.instrument->instantiateView(nullptr) == nullptr);
		QVERIFY(native.host->ui_midi_program_changed == nullptr);
		for (bool directory : {false, true})
		{
			QVERIFY(native.host->ui_open_file(native.host->handle, directory, "Open", "*") == nullptr);
			QVERIFY(native.host->ui_save_file(native.host->handle, directory, "Save", "*") == nullptr);
		}
		bool delivered = false; QObject receiver;
		QTimer::singleShot(0, &receiver, [&delivered] { delivered = true; });
		native.host->dispatcher(native.host->handle, NATIVE_HOST_OPCODE_HOST_IDLE, 0, 0, nullptr, 0);
		QVERIFY(!delivered);
		QCoreApplication::processEvents(); QVERIFY(delivered);
		session.instrument.reset();
		QCOMPARE(native.activations, 0); QCOMPARE(native.cleanups, 1); QVERIFY(native.cleanupHadResources);
	}
	void missingNativeEngineFailsWithoutAUsableInstrument_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("missing-library") << 1; QTest::newRow("failed-instance") << 2;
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
		QTest::newRow("failed-parameter-discovery") << 3;
#endif
	}
	void missingNativeEngineFailsWithoutAUsableInstrument()
	{
		QFETCH(int, failure); native.failure = failure;
		QVERIFY_EXCEPTION_THROWN(InstrumentSession{}, std::runtime_error);
		QCOMPARE(native.activations, 0); QCOMPARE(native.cleanups, failure == 3 ? 1 : 0);
		native.failure = 0;
		InstrumentSession retry; QVERIFY(retry.instrument); QCOMPARE(native.activations, 1);
	}
	void nativeAudioMidiAndOfflineTransitionsSurviveHeadlessProcessing()
	{
		InstrumentSession session;
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod());
		QVERIFY(session.instrument->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 69, 100), TimePos{}, 7));
		session.instrument->play(output.data());
		QCOMPARE(native.midi.size(), std::size_t{1}); QCOMPARE(native.midi[0].time, uint32_t{7});
		QCOMPARE(native.midi[0].size, uint8_t{3}); QCOMPARE(native.midi[0].data[1], uint8_t{69});
		QVERIFY(!native.processingWasOffline); QVERIFY(native.offlineChanges.empty());
		QCOMPARE(output.front()[0], .42f); QCOMPARE(output.back()[1], -.42f);
		Engine::getSong()->startExport();
		QVERIFY(session.instrument->handleIsOffline());
		session.instrument->play(output.data()); session.instrument->play(output.data());
		QVERIFY(native.processingWasOffline); QVERIFY(native.midi.empty());
		QCOMPARE(native.offlineChanges.size(), std::size_t{1}); QVERIFY(native.offlineChanges[0]);
		Engine::getSong()->stopExport(); session.instrument->play(output.data());
		QVERIFY(!native.processingWasOffline); QCOMPARE(native.offlineChanges.size(), std::size_t{2});
		QVERIFY(!native.offlineChanges[1]); QVERIFY(native.offlineDispatchedBeforeProcess);
	}
	void generatedMidiUsesNativePortWithoutDryDuplicationOrInstrumentFeedback_data()
	{
		QTest::addColumn<bool>("patchbay"); QTest::newRow("rack") << false; QTest::newRow("patchbay") << true;
	}
	void generatedMidiUsesNativePortWithoutDryDuplicationOrInstrumentFeedback()
	{
		QFETCH(bool, patchbay); native.patchbayInstrument = patchbay;
		NativeMidiSink sink; auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		MidiPortTestAccess::useClient(track->midiPort(), &sink);
		QVERIFY(test::executeWithAllParameters("instrument.inspect", {{"track", 0}}).value("result").toObject().value("midiOutputSupported").toBool());
		QVERIFY(StudioCapabilities::execute("midi.configure", {{"track", 0}, {"writable", true}, {"outputChannel", 0}, {"outputSource", "instrument"}}).value("ok").toBool());
		QCOMPARE(StudioCapabilities::execute("midi.inspect", {{"track", 0}}).value("result").toObject().value("outputSource").toString(), QString("instrument"));
		native.emittedMidi = {outputMessage(10, {0x93, 72, 101}), outputMessage(0, {0xb3, 74, 9}),
			outputMessage(10, {0x83, 72, 40}), outputMessage(1, {0xcf, 127}), outputMessage(2, {0xef, 0, 64})};
		track->processOutEvent(MidiEvent(MidiNoteOn, 3, 60, 99), {}, 2); QVERIFY(sink.events.empty());
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod()); track->instrument()->play(buffer.data());
		QCOMPARE(native.midi.size(), std::size_t{1}); QCOMPARE(native.midi[0].time, uint32_t{2});
		QCOMPARE(sink.events.size(), std::size_t{5}); QVERIFY(!native.outputDuringProcess);
		QVERIFY(std::all_of(native.midiOutputAccepted.begin(), native.midiOutputAccepted.end(), [](bool value) { return value; }));
		QCOMPARE(sink.events[0].type(), MidiControlChange); QCOMPARE(sink.events[0].controllerNumber(), uint8_t{74});
		QCOMPARE(sink.events[1].type(), MidiProgramChange); QCOMPARE(sink.events[1].channel(), int8_t{15}); QCOMPARE(sink.events[1].program(), uint8_t{127});
		QCOMPARE(sink.events[2].pitchBend(), int16_t{8192});
		QCOMPARE(sink.events[3].type(), MidiNoteOn); QCOMPARE(sink.events[3].key(), int16_t{72}); QCOMPARE(sink.events[3].velocity(), uint8_t{101});
		QCOMPARE(sink.events[4].type(), MidiNoteOff); QCOMPARE(sink.events[4].velocity(), uint8_t{40});
		QCOMPARE(sink.times[3], TimePos::fromFrames(10, Engine::framesPerTick()));
		QVERIFY(sink.events[3].source() == MidiEvent::Source::Internal);
		native.emittedMidi.clear(); track->instrument()->play(buffer.data()); QVERIFY(native.midi.empty()); QCOMPARE(sink.events.size(), std::size_t{5});
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto saved = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty()); track->restoreState(saved);
		QVERIFY(track->midiPort()->outputSource() == MidiPort::OutputSource::Instrument);
		native.emittedMidi = {outputMessage(0, {0x90, 55, 90})}; track->instrument()->play(buffer.data());
		QCOMPARE(sink.events.back().key(), int16_t{55});
		track->silenceAllNotes(); QCOMPARE(sink.events.back().type(), MidiNoteOff); QCOMPARE(sink.events.back().key(), int16_t{55});
		const auto count = sink.events.size(); Engine::getSong()->startExport(); track->instrument()->play(buffer.data()); Engine::getSong()->stopExport();
		QCOMPARE(sink.events.size(), count);
	}
	void rejectedNativeMidiDiscardsTheBlockAndReleasesEarlierNotes_data()
	{
		QTest::addColumn<int>("scenario");
		const char* names[] = {"null", "empty", "short", "truncated", "oversized", "running-status", "system-message",
			"invalid-data", "wrong-port", "period-boundary", "overflow-time", "queue-overflow"};
		for (int i = 0; i < 12; ++i) { QTest::newRow(names[i]) << i; }
	}
	void rejectedNativeMidiDiscardsTheBlockAndReleasesEarlierNotes()
	{
		QFETCH(int, scenario); NativeMidiSink sink; auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		MidiPortTestAccess::useClient(track->midiPort(), &sink); auto* port = track->midiPort();
		port->setWritable(true); port->setOutputChannel(0); port->setOutputSource(MidiPort::OutputSource::Instrument);
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod());
		native.emittedMidi = {outputMessage(0, {0x90, 55, 90})}; track->instrument()->play(buffer.data()); sink.events.clear();
		auto invalid = outputMessage(0, {0x90, 80, 100});
		if (scenario == 0) { native.nullMidiOutput = true; }
		if (scenario >= 1 && scenario <= 4) { invalid.size = scenario == 1 ? 0 : scenario == 2 ? 1 : scenario == 3 ? 2 : 4; }
		if (scenario == 5) { invalid.data[0] = 60; }
		if (scenario == 6) { invalid.data[0] = 0xf0; }
		if (scenario == 7) { invalid.data[1] = 128; }
		if (scenario == 8) { invalid.port = 1; }
		if (scenario == 9) { invalid.time = Engine::audioEngine()->framesPerPeriod(); }
		if (scenario == 10) { invalid.time = std::numeric_limits<uint32_t>::max(); }
		native.emittedMidi = {outputMessage(0, {0x90, 70, 100}), invalid};
		if (scenario == 11) { native.emittedMidi.assign(CarlaInstrument::kMaxMidiEvents + 1, invalid); }
		QVERIFY(track->instrument()->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 65, 100), {}, 0));
		QVERIFY_EXCEPTION_THROWN(track->instrument()->play(buffer.data()), std::runtime_error);
		QCOMPARE(sink.events.size(), std::size_t{1}); QCOMPARE(sink.events[0].type(), MidiNoteOff); QCOMPARE(sink.events[0].key(), int16_t{55});
		QVERIFY(!native.midiOutputAccepted.back()); QVERIFY(!native.outputDuringProcess);
		native.emittedMidi.clear(); native.nullMidiOutput = false; track->instrument()->play(buffer.data()); QVERIFY(native.midi.empty());
		QCOMPARE(sink.events.size(), std::size_t{1});
	}
	void nativeProcessingFailureCannotPublishMidiOrReplayConsumedInput_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("standard") << 1; QTest::newRow("unknown") << 2;
	}
	void nativeProcessingFailureCannotPublishMidiOrReplayConsumedInput()
	{
		QFETCH(int, failure); NativeMidiSink sink; auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		MidiPortTestAccess::useClient(track->midiPort(), &sink); track->midiPort()->setWritable(true);
		track->midiPort()->setOutputSource(MidiPort::OutputSource::Instrument);
		native.emittedMidi = {outputMessage(0, {0x90, 70, 100})}; native.processFailure = failure;
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod());
		QVERIFY(track->instrument()->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, 0));
		bool failed = false; try { track->instrument()->play(buffer.data()); } catch (...) { failed = true; }
		QVERIFY(failed); QVERIFY(sink.events.empty()); native.processFailure = 0; native.emittedMidi.clear();
		track->instrument()->play(buffer.data()); QVERIFY(native.midi.empty()); QVERIFY(sink.events.empty());
	}
	void outputCallbacksOutsideTheirProcessingThreadAreRejected()
	{
		NativeMidiSink sink; auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		MidiPortTestAccess::useClient(track->midiPort(), &sink); track->midiPort()->setWritable(true);
		track->midiPort()->setOutputSource(MidiPort::OutputSource::Instrument);
		const auto event = outputMessage(0, {0x90, 70, 100});
		QVERIFY(!native.host->write_midi_event(native.host->handle, &event));
		native.emittedMidi = {event}; native.midiFromWorker = true;
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod()); track->instrument()->play(buffer.data());
		QVERIFY(!native.midiOutputAccepted[0]); QVERIFY(sink.events.empty());
		native.midiFromWorker = false; track->instrument()->play(buffer.data()); QCOMPARE(sink.events.size(), std::size_t{1});
		QVERIFY(!native.host->write_midi_event(native.host->handle, &event));
	}
	void nativeOutputReentryOccursAfterTheInstrumentMutexIsReleased()
	{
		NativeMidiSink sink; auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		MidiPortTestAccess::useClient(track->midiPort(), &sink); track->midiPort()->setWritable(true);
		track->midiPort()->setOutputSource(MidiPort::OutputSource::Instrument);
		bool entered = false, accepted = false;
		sink.onEvent = [&]
		{
			if (entered) { return; } entered = true;
			accepted = track->instrument()->handleMidiEvent(MidiEvent(MidiControlChange, 0, 74, 80), {}, 0);
		};
		native.emittedMidi = {outputMessage(0, {0x90, 70, 100})};
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod()); track->instrument()->play(buffer.data());
		QVERIFY(entered); QVERIFY(accepted); QVERIFY(!native.outputDuringProcess);
		native.emittedMidi.clear(); track->instrument()->play(buffer.data());
		QCOMPARE(native.midi.size(), std::size_t{1}); QCOMPARE(native.midi[0].data[0], uint8_t{0xb0});
		sink.onEvent = {};
	}
	void nativeMidiInputRejectsUnencodableOutOfPeriodAndExcessEvents_data()
	{
		QTest::addColumn<int>("scenario");
		QTest::newRow("invalid-data") << 0; QTest::newRow("unsupported") << 1; QTest::newRow("out-of-period") << 2; QTest::newRow("capacity") << 3;
	}
	void nativeMidiInputRejectsUnencodableOutOfPeriodAndExcessEvents()
	{
		QFETCH(int, scenario); InstrumentSession session;
		if (scenario == 0) { QVERIFY(!session.instrument->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 128, 100), {}, 0)); }
		if (scenario == 1) { QVERIFY(!session.instrument->handleMidiEvent(MidiEvent(MidiSync), {}, 0)); }
		if (scenario == 2) { QVERIFY(!session.instrument->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, std::numeric_limits<f_cnt_t>::max())); }
		const auto count = scenario == 3 ? CarlaInstrument::kMaxMidiEvents : 1;
		for (uint32_t i = 0; i < count; ++i) { QVERIFY(session.instrument->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, 0)); }
		if (scenario == 3) { QVERIFY(!session.instrument->handleMidiEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, 0)); }
		std::vector<SampleFrame> buffer(Engine::audioEngine()->framesPerPeriod()); session.instrument->play(buffer.data());
		QCOMPARE(native.midi.size(), static_cast<std::size_t>(count)); session.instrument->play(buffer.data()); QVERIFY(native.midi.empty());
	}
	void failedNativeSerializationPreservesExistingProjectFiles_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("null-state") << 1; QTest::newRow("empty-state") << 2;
		QTest::newRow("malformed-xml") << 3; QTest::newRow("foreign-xml") << 4;
		QTest::newRow("standard-exception") << 5; QTest::newRow("unknown-exception") << 6;
		QTest::newRow("missing-save-callback") << 7;
	}
	void failedNativeSerializationPreservesExistingProjectFiles()
	{
		QFETCH(int, failure);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		const auto path = directory.filePath("existing.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(path));
		const auto original = contents(path); QVERIFY(original.contains("CARLA-PROJECT"));
		if (failure == 7) { native.failure = 4; track->loadInstrument("carlahosttest"); }
		else { native.saveFailure = failure; }
		QVERIFY(!Engine::getSong()->saveProjectFile(path)); QCOMPARE(contents(path), original);
		DataFile preset(DataFile::Type::InstrumentTrackSettings);
		track->savePreset(preset, preset.content());
		QVERIFY(!preset.serializationError().isEmpty());
		QVERIFY(!preset.writeFile(directory.filePath("failed.xpf")));
		QVERIFY(!QFileInfo::exists(directory.filePath("failed.xpf")));
		QJsonArray assets;
		QVERIFY(!Engine::getSong()->saveProjectFile(directory.filePath("checkpoint.mmp"), false, true, &assets));
		QVERIFY(!QFileInfo::exists(directory.filePath("checkpoint.mmp")));
		native.failure = 0; native.saveFailure = 0;
		if (failure == 7) { track->loadInstrument("carlahosttest"); }
		QVERIFY(Engine::getSong()->saveProjectFile(path)); QVERIFY(trackUnlocked(track.get()));
	}
	void invalidStateDoesNotReachDestructiveNativeRestoration_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("missing-state") << 0; QTest::newRow("foreign-state") << 1;
		QTest::newRow("ambiguous-states") << 2; QTest::newRow("missing-load-callback") << 3;
	}
	void invalidStateDoesNotReachDestructiveNativeRestoration()
	{
		QFETCH(int, failure); if (failure == 3) { native.failure = 5; }
		InstrumentSession session; const auto before = native.values;
		QDomDocument doc; auto root = doc.createElement("carlahosttest"); doc.appendChild(root);
		if (failure == 1) { root.appendChild(doc.createElement("unrelated")); }
		if (failure >= 2) { root.appendChild(doc.createElement("CARLA-PROJECT")); }
		if (failure == 2) { root.appendChild(doc.createElement("CARLA-PRESET")); }
		session.instrument->loadSettings(root);
		QVERIFY(Engine::getSong()->hasErrors()); QCOMPARE(native.stateLoads, 0); QVERIFY(native.values == before);
	}
	void stateRestorationFindsNativeStateAmongModelMetadata_data()
	{
		QTest::addColumn<QString>("tag");
		QTest::newRow("project") << QString("CARLA-PROJECT");
		QTest::newRow("preset-case-insensitive") << QString("carla-preset");
	}
	void stateRestorationFindsNativeStateAmongModelMetadata()
	{
		QFETCH(QString, tag); InstrumentSession session;
		const auto name = native.stateName;
		QDomDocument saved; auto root = saved.createElement("carlahosttest"); saved.appendChild(root);
		session.instrument->saveSettings(saved, root);
		QVERIFY(DataFile::serializationError(saved).isEmpty());
		root.firstChildElement("CARLA-PROJECT").setTagName(tag);
		auto metadata = saved.createElement("extension-metadata");
		root.insertBefore(metadata, root.firstChild());
		native.values[0] = 2; native.stateName = "changed";
		session.instrument->loadSettings(root);
		QVERIFY(!Engine::getSong()->hasErrors()); QCOMPARE(native.stateLoads, 1);
		QCOMPARE(native.values[0], 42.f); QCOMPARE(native.stateName, name);
	}
	void interruptedNativeRestoreCannotBeCheckpointedUntilRecovery_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("standard-exception") << 1; QTest::newRow("unknown-exception") << 2;
	}
	void interruptedNativeRestoreCannotBeCheckpointedUntilRecovery()
	{
		QFETCH(int, failure); InstrumentSession session;
		QDomDocument saved; auto root = saved.createElement("carlahosttest"); saved.appendChild(root);
		session.instrument->saveSettings(saved, root);
		native.loadFailure = failure; session.instrument->loadSettings(root);
		QVERIFY(Engine::getSong()->hasErrors()); QCOMPARE(native.values[0], 0.f);
		QDomDocument failed; auto partial = failed.createElement("carlahosttest"); failed.appendChild(partial);
		session.instrument->saveSettings(failed, partial);
		QVERIFY(!DataFile::serializationError(failed).isEmpty());
		native.loadFailure = 0; Engine::getSong()->clearErrors(); session.instrument->loadSettings(root);
		QVERIFY(!Engine::getSong()->hasErrors()); QCOMPARE(native.values[0], 42.f);
		QDomDocument recovered; auto restored = recovered.createElement("carlahosttest"); recovered.appendChild(restored);
		session.instrument->saveSettings(recovered, restored);
		QVERIFY(DataFile::serializationError(recovered).isEmpty());
	}
	void throwingInstrumentReplacementReleasesTrackAndSupportsNativeRestore_data()
	{
		QTest::addColumn<int>("path"); QTest::addColumn<int>("failure");
		for (int path = 0; path != 3; ++path)
		{
			for (int failure = 1; failure != 3; ++failure)
			{
				QTest::newRow(qPrintable(QString("path-%1-exception-%2").arg(path).arg(failure))) << path << failure;
			}
		}
	}
	void throwingInstrumentReplacementReleasesTrackAndSupportsNativeRestore()
	{
		QFETCH(int, path); QFETCH(int, failure);
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* clip = static_cast<MidiClip*>(track->createClip(TimePos{}));
		clip->addNote(Note(TimePos{48}, TimePos{}, 60));
		QDomDocument doc; auto root = doc.createElement("saved"); doc.appendChild(root);
		const auto saved = track->saveState(doc, root); QVERIFY(DataFile::serializationError(doc).isEmpty());
		auto legacy = saved.cloneNode(true).toElement();
		auto settings = legacy.firstChildElement("instrumenttrack");
		auto wrapper = settings.firstChildElement("instrument");
		settings.appendChild(wrapper.firstChildElement().cloneNode(true)); settings.removeChild(wrapper);
		track->constructionFailure = failure;
		bool caught = false;
		try
		{
			if (path == 0) { track->loadInstrument("carlahosttest"); }
			else { track->restoreState(path == 1 ? saved : legacy); }
		}
		catch (...) { caught = true; }
		QVERIFY(caught); QVERIFY(trackUnlocked(track.get())); QVERIFY(track->instrument() == nullptr);
		QCOMPARE(native.activations, 0); QCOMPARE(native.cleanups, 1);
		track->constructionFailure = 0; track->restoreState(saved);
		QVERIFY(track->instrument()); QVERIFY(trackUnlocked(track.get()));
		QCOMPARE(native.activations, 1); QCOMPARE(native.values[0], 42.f);
		QCOMPARE(track->getClips().size(), std::size_t{1});
		QCOMPARE(static_cast<MidiClip*>(track->getClips()[0])->notes().size(), std::size_t{1});
	}
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
	void studioDiscoveryAndWritesRespectNativeParameterCapabilities_data()
	{
		QTest::addColumn<int>("policy"); QTest::addColumn<QString>("kind");
		QTest::newRow("continuous") << 0 << QString("number");
		QTest::newRow("disabled") << 1 << QString("number");
		QTest::newRow("output") << 2 << QString("number");
		QTest::newRow("manual-only") << 3 << QString("number");
		QTest::newRow("integer-scale-points") << 4 << QString("integer");
		QTest::newRow("boolean") << 5 << QString("boolean");
	}
	void studioDiscoveryAndWritesRespectNativeParameterCapabilities()
	{
		QFETCH(int, policy); QFETCH(QString, kind); native.parameterPolicy = policy;
		if (policy == 5) { native.values[0] = 0; }
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0];
		const int id = model->id();
		const bool writable = policy != 1 && policy != 2, automatable = writable && policy != 3;
		const auto response = StudioCapabilities::execute("parameter.inspect", {{"parameter", id}});
		QVERIFY(response.value("ok").toBool()); const auto info = response.value("result").toObject();
		QCOMPARE(info.value("writable").toBool(), writable); QCOMPARE(info.value("automatable").toBool(), automatable);
		QCOMPARE(info.value("kind").toString(), kind);
		QCOMPARE(info.value("location").toObject().value("track").toInt(-1), 0);
		if (policy == 4)
		{
			QCOMPARE(info.value("choices").toArray().size(), 3);
			QCOMPARE(info.value("choices").toArray()[2].toObject().value("name").toString(), QString("Bright"));
		}
		for (const auto operation : {"parameter.list", "instrument.inspect"})
		{
			const auto listed = StudioCapabilities::execute(operation, {{"track", 0}});
			QVERIFY(listed.value("ok").toBool()); bool found = false;
			for (const auto& value : listed.value("result").toObject().value("parameters").toArray())
			{
				if (value.toObject().value("id").toInt() == id) { QCOMPARE(value.toObject(), info); found = true; }
			}
			QVERIFY(found);
		}
		const float before = native.values[0];
		if (policy >= 4)
		{
			QVERIFY(!StudioCapabilities::execute("parameter.set", {{"parameter", id}, {"value", .5}}).value("ok").toBool());
			QCOMPARE(native.values[0], before); QCOMPARE(native.parameterWrites, 0);
		}
		const float target = policy == 5 ? 1.f : policy == 4 ? 2.f : 2.25f;
		const auto changed = StudioCapabilities::execute("parameter.set", {{"parameter", id}, {"value", target}});
		QCOMPARE(changed.value("ok").toBool(), writable);
		QCOMPARE(native.values[0], writable ? target : before); QCOMPARE(model->value(), native.values[0]);
		QCOMPARE(native.parameterWrites, writable ? 1 : 0); QCOMPARE(native.uiWrites, 0);
		if (!writable)
		{
			// Project metadata cannot turn a native output into an input control.
			QDomDocument saved; auto root = saved.createElement("carlahosttest"); saved.appendChild(root);
			NativeInstrumentTestAccess::save(track->instrument(), saved, root);
			auto metadata = saved.createElement("PARAM_KNOB_0"); metadata.setAttribute("output", 0);
			metadata.setAttribute("value", before); metadata.setAttribute("id", id); root.appendChild(metadata);
			NativeInstrumentTestAccess::load(track->instrument(), root);
			QVERIFY(!model->isWritable()); QCOMPARE(native.parameterWrites, 0);
		}
	}
	void studioAutomationBindingsRespectNativeParameterHints_data()
	{
		QTest::addColumn<int>("policy");
		QTest::newRow("automatable") << 0; QTest::newRow("disabled") << 1;
		QTest::newRow("output") << 2; QTest::newRow("manual-only") << 3;
	}
	void studioAutomationBindingsRespectNativeParameterHints()
	{
		QFETCH(int, policy); native.parameterPolicy = policy;
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0];
		const int id = model->id(); const bool accepted = policy == 0;
		const auto globalBefore = Engine::getSong()->globalAutomationTrack()->numOfClips();
		const auto point = StudioCapabilities::execute("automation.global.point", {{"parameter", id}, {"position", 0}, {"value", 10}});
		QCOMPARE(point.value("ok").toBool(), accepted);
		QCOMPARE(Engine::getSong()->globalAutomationTrack()->numOfClips(), globalBefore + (accepted ? 1 : 0));
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "automation"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 1}, {"position", 0}}).value("ok").toBool());
		const auto connected = StudioCapabilities::execute("automation.connect", {{"track", 1}, {"clip", 0}, {"parameter", id}});
		QCOMPARE(connected.value("ok").toBool(), accepted);
		auto* curve = static_cast<AutomationClip*>(Engine::getSong()->tracks()[1]->getClip(0));
		QCOMPARE(curve->objects().empty(), !accepted);
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		const auto controller = StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", id}});
		QCOMPARE(controller.value("ok").toBool(), accepted); QCOMPARE(model->controllerConnection() != nullptr, accepted);
		if (accepted)
		{
			QVERIFY(StudioCapabilities::execute("controller.disconnect", {{"parameter", id}}).value("ok").toBool());
			QVERIFY(StudioCapabilities::execute("automation.disconnect", {{"track", 1}, {"clip", 0}, {"parameter", id}}).value("ok").toBool());
			QVERIFY(StudioCapabilities::execute("automation.global.clear", {{"parameter", id}}).value("ok").toBool());
		}
		QCOMPARE(native.uiWrites, 0);
	}
	void nativeStateRestoresParameterValuesAndIdentityWithoutFeedback()
	{
		InstrumentSession session;
		auto models = parameters(session.instrument.get()); QCOMPARE(models.size(), 2);
		QCOMPARE(models[0]->value(), 42.f); QCOMPARE(native.parameterWrites, 0);
		models[0]->setValue(67.f); QCOMPARE(native.values[0], 67.f);
		QCOMPARE(native.parameterWrites, 1); QCOMPARE(native.uiWrites, 0);
		QDomDocument doc; auto root = doc.createElement("carlahosttest"); doc.appendChild(root);
		session.instrument->saveSettings(doc, root);
		QVERIFY(!root.firstChildElement("CARLA-PROJECT").isNull());
		models[0]->setValue(9.f); const auto writes = native.parameterWrites;
		session.instrument->loadSettings(root);
		QCOMPARE(parameters(session.instrument.get())[0], models[0]);
		QCOMPARE(models[0]->value(), 67.f); QCOMPARE(native.values[0], 67.f);
		QCOMPARE(native.parameterWrites, writes); QCOMPARE(native.uiWrites, 0);
		// Reconstruct the instrument from the same native saved state.
		session.instrument.reset(); native.values = {0.f, 0.f};
		session.instrument = std::make_unique<CarlaInstrument>(session.track.get(), &lmmsDescriptor, false);
		session.instrument->loadSettings(root);
		QCOMPARE(parameters(session.instrument.get())[0]->value(), 67.f); QCOMPARE(native.values[0], 67.f);
	}
	void headlessNativeChangesReachInspectionAndSnapshotsWithoutOverwritingControlledValues()
	{
		auto track = std::make_unique<NativeCarlaTrack>(); track->loadInstrument("carlahosttest");
		auto* model = parameters(static_cast<CarlaInstrument*>(track->instrument()))[0]; const int id = model->id();
		native.values[0] = 53.f;
		const auto observed = StudioCapabilities::execute("parameter.inspect", {{"parameter", id}});
		QVERIFY(observed.value("ok").toBool());
		QCOMPARE(observed.value("result").toObject().value("value").toDouble(), 53.);
		QCOMPARE(native.parameterWrites, 0);
		native.values[0] = 63.f;
		QDomDocument saved; auto root = saved.createElement("carlahosttest"); saved.appendChild(root);
		NativeInstrumentTestAccess::save(track->instrument(), saved, root);
		QVERIFY(DataFile::serializationError(saved).isEmpty()); QCOMPARE(model->storedValue(), 63.f);
		native.values[0] = 1.f; NativeInstrumentTestAccess::load(track->instrument(), root);
		QCOMPARE(native.values[0], 63.f); QCOMPARE(model->storedValue(), 63.f);
		QVERIFY(StudioCapabilities::execute("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("controller.connect", {{"controller", 0}, {"parameter", id}}).value("ok").toBool());
		native.values[0] = 19.f; const auto writes = native.parameterWrites;
		QVERIFY(StudioCapabilities::execute("parameter.inspect", {{"parameter", id}}).value("ok").toBool());
		QCOMPARE(model->storedValue(), 63.f); QCOMPARE(native.parameterWrites, writes);
		QDomDocument controlled; auto controls = controlled.createElement("carlahosttest"); controlled.appendChild(controls);
		NativeInstrumentTestAccess::save(track->instrument(), controlled, controls);
		QVERIFY(DataFile::serializationError(controlled).isEmpty()); QCOMPARE(model->storedValue(), 63.f);
		QCOMPARE(native.parameterWrites, writes);
	}
	void reloadPreservesSurvivingModelsAndHandlesChangedParameterCounts()
	{
		InstrumentSession session; auto models = parameters(session.instrument.get());
		QPointer<CarlaParamFloatModel> first = models[0], removed = models[1];
		native.values = {55.f, .5f, 88.f};
		session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_RELOAD_PARAMETERS, 0, 0, nullptr, 0);
		models = parameters(session.instrument.get()); QCOMPARE(models.size(), 3);
		QCOMPARE(models[0], first.data()); QCOMPARE(models[2]->value(), 88.f);
		native.values.resize(1);
		session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_RELOAD_ALL, 0, 0, nullptr, 0);
		QCOMPARE(parameters(session.instrument.get()).size(), 1); QVERIFY(removed.isNull());
		QCOMPARE(parameters(session.instrument.get())[0], first.data()); QCOMPARE(first->value(), 55.f);
		QCOMPARE(native.parameterWrites, 0);
	}
	void legacyNativeStateAndExplicitKnobOverridesRemainCoherent_data()
	{
		QTest::addColumn<bool>("knobOverride");
		QTest::newRow("native-state-only") << false;
		QTest::newRow("saved-knob-override") << true;
	}
	void legacyNativeStateAndExplicitKnobOverridesRemainCoherent()
	{
		QFETCH(bool, knobOverride);
		InstrumentSession session;
		QDomDocument doc;
		QVERIFY(doc.setContent(QString("<carlahosttest><CARLA-PROJECT><Parameter value='73'/></CARLA-PROJECT></carlahosttest>")));
		auto root = doc.documentElement();
		if (knobOverride) { root.setAttribute("PARAM_KNOB_0", 81); }
		session.instrument->loadSettings(root);
		const float expected = knobOverride ? 81.f : 73.f;
		QCOMPARE(parameters(session.instrument.get()).size(), 1);
		QCOMPARE(parameters(session.instrument.get())[0]->value(), expected);
		QCOMPARE(native.values[0], expected);
		QCOMPARE(native.parameterWrites, knobOverride ? 1 : 0); QCOMPARE(native.uiWrites, 0);
	}
	void invalidAndWorkerCallbacksCannotIndexStaleModelsOrWriteBack()
	{
		InstrumentSession session; auto models = parameters(session.instrument.get());
		session.instrument->handleUiParameterChanged(std::numeric_limits<uint32_t>::max(), 3.f);
		session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER, -1, 0, nullptr, 0);
		std::thread worker([&]
		{
			session.instrument->handleUiParameterChanged(0, 71.f);
			session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER, 999, 0, nullptr, 0);
		}); worker.join();
		QCOMPARE(models[0]->value(), 42.f);
		QCoreApplication::processEvents(); QCOMPARE(models[0]->value(), 71.f);
		QCOMPARE(native.parameterWrites, 0); QCOMPARE(native.uiWrites, 0);
		native.values[0] = 64.f;
		std::thread reload([&]
		{
			session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_UPDATE_PARAMETER, 0, 0, nullptr, 0);
		}); reload.join();
		QCOMPARE(models[0]->value(), 71.f);
		QCoreApplication::processEvents(); QCOMPARE(models[0]->value(), 64.f);
		native.nullInfo = 0;
		session.instrument->handleDispatcher(NATIVE_HOST_OPCODE_RELOAD_PARAMETERS, 0, 0, nullptr, 0);
		QVERIFY(!models[0]->enabled()); models[0]->setValue(12.f); QCOMPARE(native.parameterWrites, 0);
		// QObject ownership cancels queued callbacks when a native instrument is replaced.
		std::thread late([&] { session.instrument->handleUiParameterChanged(0, 99.f); }); late.join();
		session.instrument.reset(); QCoreApplication::processEvents(); QCOMPARE(native.cleanups, 1);
	}
#endif
};

QTEST_GUILESS_MAIN(CarlaHostTest)
#include "CarlaHostTest.moc"
