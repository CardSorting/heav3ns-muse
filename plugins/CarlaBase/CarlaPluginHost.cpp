/* Existing Carla engine management. SPDX-License-Identifier: GPL-2.0-or-later */
#include "CarlaPluginHost.h"
#include "CarlaPluginDiscovery.h"
#include "CarlaPluginPatchbay.h"
#include <QLibrary>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#ifdef LMMS_CARLA_NATIVE_HOST_API
#include <CarlaNativePlugin.h>
#endif

namespace lmms
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
using namespace CARLA_BACKEND_NAMESPACE;
#endif
namespace
{
[[noreturn]] void hostError(const QString& message) { throw std::runtime_error(message.toStdString()); }
#ifdef LMMS_CARLA_NATIVE_HOST_API
void checkedText(const QString& value, const char* field)
{
	if (value.contains(QChar::Null)) { hostError(QString("Hosted plugin %1 contains a null character").arg(field)); }
}
const std::pair<const char*, PluginType> pluginTypes[] = {
	{"internal", PLUGIN_INTERNAL}, {"ladspa", PLUGIN_LADSPA}, {"dssi", PLUGIN_DSSI},
	{"lv2", PLUGIN_LV2}, {"vst2", PLUGIN_VST2}, {"vst3", PLUGIN_VST3},
	{"au", PLUGIN_AU}, {"dls", PLUGIN_DLS}, {"gig", PLUGIN_GIG}, {"sf2", PLUGIN_SF2},
	{"sfz", PLUGIN_SFZ}, {"jsfx", PLUGIN_JSFX}, {"clap", PLUGIN_CLAP}
};
QString pluginTypeName(PluginType type)
{
	for (const auto& item : pluginTypes) { if (item.second == type) { return item.first; } }
	return QString("native-type-%1").arg(static_cast<int>(type));
}
BinaryType binaryType(const QString& name)
{
	if (name == "native")
	{
#ifdef Q_OS_WIN
		return sizeof(void*) == 8 ? BINARY_WIN64 : BINARY_WIN32;
#else
		return sizeof(void*) == 8 ? BINARY_POSIX64 : BINARY_POSIX32;
#endif
	}
	if (name == "posix32") { return BINARY_POSIX32; }
	if (name == "posix64") { return BINARY_POSIX64; }
	if (name == "win32") { return BINARY_WIN32; }
	if (name == "win64") { return BINARY_WIN64; }
	hostError("Unknown hosted plugin binary type");
}
#endif
}

struct CarlaPluginHost::Impl
{
	QString failure = "This Carla build does not expose the native host management API";
	QString parameterFailure = "This Carla build does not expose native hosted parameter access";
	QString patchbayFailure = "Native patchbay routing is unavailable";
	std::unique_ptr<QLibrary> library;
	std::unique_ptr<CarlaPluginDiscovery> discovery;
	std::unique_ptr<CarlaPluginPatchbay> patchbay;
	LayoutEdit layoutEdit;
#ifdef LMMS_CARLA_NATIVE_HOST_API
	const NativePluginDescriptor* descriptor = nullptr;
	NativePluginHandle instrument = nullptr;
	CarlaHostHandle handle = nullptr;
#define HOST_FUNCTION(name) decltype(&carla_##name) name = nullptr
	HOST_FUNCTION(create_native_plugin_host_handle);
	HOST_FUNCTION(host_handle_free);
	HOST_FUNCTION(get_current_plugin_count);
	HOST_FUNCTION(get_plugin_info);
	HOST_FUNCTION(get_internal_parameter_value);
	HOST_FUNCTION(add_plugin);
	HOST_FUNCTION(remove_plugin);
	HOST_FUNCTION(rename_plugin);
	HOST_FUNCTION(switch_plugins);
	HOST_FUNCTION(set_active);
	HOST_FUNCTION(get_last_error);
	HOST_FUNCTION(get_parameter_count);
	HOST_FUNCTION(get_parameter_info);
	HOST_FUNCTION(get_parameter_data);
	HOST_FUNCTION(get_parameter_ranges);
	HOST_FUNCTION(get_parameter_scalepoint_info);
	HOST_FUNCTION(get_current_parameter_value);
	HOST_FUNCTION(get_parameter_text);
	HOST_FUNCTION(set_parameter_value);
#undef HOST_FUNCTION
	~Impl() { patchbay.reset(); discovery.reset(); if (handle) { host_handle_free(handle); } }
	void require() const { if (!handle || !failure.isEmpty()) { hostError(failure); } }
	void requireParameters() const { require(); if (!parameterFailure.isEmpty()) { hostError(parameterFailure); } }
	LayoutChange layout(int first = -1, int second = -1) const
	{
		if (!descriptor || !descriptor->get_parameter_count) { return {}; }
		const auto slots = descriptor->get_parameter_count(instrument);
		if (slots > static_cast<uint32_t>(std::numeric_limits<int>::max())) { hostError("Native parameter window is invalid"); }
		LayoutChange result{true, std::vector<int>(slots, -1)};
		if (first < 0)
		{
			for (uint32_t i = 0; i < slots; ++i) { result.oldToNew[i] = static_cast<int>(i); }
			return result; // Appending preserves every existing native slot.
		}
		if (!get_parameter_count) { return {}; }
		const auto plugins = count();
		std::vector<int> counts(plugins), starts(plugins), newStarts(plugins);
		int total = 0;
		for (int i = 0; i < plugins; ++i)
		{
			const auto parameters = get_parameter_count(handle, i);
			if (parameters > static_cast<uint32_t>(std::numeric_limits<int>::max() - total)) { hostError("Hosted parameter layout exceeds its native index range"); }
			starts[i] = total; counts[i] = static_cast<int>(parameters); total += counts[i];
		}
		int next = 0;
		for (int i = 0; i < plugins; ++i)
		{
			if (second < 0 && i == first) { newStarts[i] = -1; continue; }
			const int previous = second < 0 ? i : i == first ? second : i == second ? first : i;
			newStarts[previous] = next; next += counts[previous];
		}
		for (int i = 0; i < plugins; ++i)
		{
			const auto available = std::min(counts[i], std::max(0, static_cast<int>(slots) - starts[i]));
			for (int parameter = 0; parameter < available; ++parameter)
			{
				result.oldToNew[starts[i] + parameter] = newStarts[i] < 0 ? -1 : newStarts[i] + parameter;
			}
		}
		return result;
	}
	void editLayout(const LayoutChange& change, const std::function<void()>& edit) const
	{
		if (layoutEdit) { layoutEdit(change, edit); } else { edit(); }
	}
	int count() const
	{
		require(); const auto count = get_current_plugin_count(handle);
		if (count > static_cast<uint32_t>(std::numeric_limits<int>::max())) { hostError("Native host plugin count is invalid"); }
		return static_cast<int>(count);
	}
	void index(int value) const
	{
		if (value < 0 || value >= count()) { hostError("Hosted plugin index does not exist; rediscover after structural edits"); }
	}
	bool active(int index) const
	{
		const auto value = get_internal_parameter_value(handle, index, PARAMETER_ACTIVE);
		if (!std::isfinite(value) || value < 0 || value > 1) { hostError("Native host returned an invalid activation state"); }
		return value > .5f;
	}
	[[noreturn]] void failed(const QString& operation) const
	{
		const auto error = get_last_error(handle);
		hostError(operation + ": " + (error && *error ? QString::fromUtf8(error) : QString("native Carla rejected the operation")));
	}
#endif
};

CarlaPluginHost::CarlaPluginHost(const NativePluginDescriptor* descriptor, NativePluginHandle instrument,
	const QString& libraryPath, const Resolver& supplied, const LayoutEdit& layoutEdit, bool patchbay) : m_impl(std::make_unique<Impl>())
{
	m_impl->layoutEdit = layoutEdit;
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->descriptor = descriptor; m_impl->instrument = instrument;
	try
	{
		Resolver resolve = supplied;
		if (!resolve)
		{
			if (libraryPath.isEmpty()) { m_impl->failure = "Carla did not provide its native library path"; return; }
			m_impl->library = std::make_unique<QLibrary>(libraryPath);
			if (!m_impl->library->load()) { m_impl->failure = m_impl->library->errorString(); return; }
			resolve = [this](const char* name) { return m_impl->library->resolve(name); };
		}
#define RESOLVE(name) \
		m_impl->name = reinterpret_cast<decltype(m_impl->name)>(resolve("carla_" #name)); \
		if (!m_impl->name) { m_impl->failure = "Carla library lacks carla_" #name; return; }
		RESOLVE(create_native_plugin_host_handle)
		RESOLVE(host_handle_free)
		RESOLVE(get_current_plugin_count)
		RESOLVE(get_plugin_info)
		RESOLVE(get_internal_parameter_value)
		RESOLVE(add_plugin)
		RESOLVE(remove_plugin)
		RESOLVE(rename_plugin)
		RESOLVE(switch_plugins)
		RESOLVE(set_active)
		RESOLVE(get_last_error)
#undef RESOLVE
		// Parameter access is optional: missing symbols must not disable management.
		const auto resolveParameters = [&]
		{
#define RESOLVE_PARAMETER(name) \
			m_impl->name = reinterpret_cast<decltype(m_impl->name)>(resolve("carla_" #name)); \
			if (!m_impl->name) { m_impl->parameterFailure = "Carla library lacks carla_" #name; return; }
			RESOLVE_PARAMETER(get_parameter_count)
			RESOLVE_PARAMETER(get_parameter_info)
			RESOLVE_PARAMETER(get_parameter_data)
			RESOLVE_PARAMETER(get_parameter_ranges)
			RESOLVE_PARAMETER(get_parameter_scalepoint_info)
			RESOLVE_PARAMETER(get_current_parameter_value)
			RESOLVE_PARAMETER(get_parameter_text)
			RESOLVE_PARAMETER(set_parameter_value)
#undef RESOLVE_PARAMETER
			m_impl->parameterFailure.clear();
		};
		resolveParameters();
		if (!descriptor || !instrument) { m_impl->failure = "The Carla native instrument is unavailable"; return; }
		m_impl->handle = m_impl->create_native_plugin_host_handle(descriptor, instrument);
		if (!m_impl->handle) { m_impl->failure = "Carla could not attach management to the existing native instrument"; return; }
		m_impl->failure.clear();
		m_impl->discovery = std::make_unique<CarlaPluginDiscovery>(libraryPath, resolve);
		if (patchbay && m_impl->get_current_plugin_count(m_impl->handle) != 0)
		{
			m_impl->patchbayFailure = "Native patchbay callback initialization requires an empty engine";
		}
		else { m_impl->patchbay = std::make_unique<CarlaPluginPatchbay>(m_impl->handle, patchbay, resolve); }
	}
	catch (const std::exception& error) { m_impl->failure = "Carla native host attachment failed: " + QString::fromUtf8(error.what()); }
	catch (...) { m_impl->failure = "Carla native host attachment failed with a plugin exception"; }
#else
	Q_UNUSED(descriptor) Q_UNUSED(instrument) Q_UNUSED(libraryPath) Q_UNUSED(supplied) Q_UNUSED(patchbay)
#endif
}

CarlaPluginHost::~CarlaPluginHost() = default;
void CarlaPluginHost::prepareForEngineCleanup()
{
	if (m_impl->patchbay) { m_impl->patchbay->prepareForEngineCleanup(); }
	m_impl->discovery.reset();
#ifdef LMMS_CARLA_NATIVE_HOST_API
	if (m_impl->handle)
	{
		const auto handle = std::exchange(m_impl->handle, nullptr);
		m_impl->host_handle_free(handle);
	}
#endif
	m_impl->failure = "The native Carla engine is closing";
}
bool CarlaPluginHost::available() const
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	return m_impl->handle && m_impl->failure.isEmpty();
#else
	return false;
#endif
}
QString CarlaPluginHost::unavailableReason() const { return m_impl->failure; }
QStringList CarlaPluginHost::formats() const
{
	QStringList result;
#ifdef LMMS_CARLA_NATIVE_HOST_API
	if (available()) { for (const auto& type : pluginTypes) { result.append(type.first); } }
#endif
	return result;
}
std::vector<PluginHost::Entry> CarlaPluginHost::plugins() const
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	const auto count = m_impl->count(); std::vector<Entry> result; result.reserve(count);
	for (int index = 0; index < count; ++index)
	{
		const auto* info = m_impl->get_plugin_info(m_impl->handle, index);
		if (!info) { m_impl->failed("Could not inspect hosted plugin"); }
		// Carla owns and may reuse this memory on the next API call.
		Entry entry{index, pluginTypeName(info->type), QString::fromUtf8(info->name),
			QString::fromUtf8(info->label), QString::fromUtf8(info->filename), QString::fromUtf8(info->maker),
			QString::number(info->uniqueId), false};
		entry.active = m_impl->active(index);
		result.push_back(std::move(entry));
	}
	return result;
#else
	hostError(m_impl->failure);
#endif
}
int CarlaPluginHost::add(const Specification& plugin)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->require();
	checkedText(plugin.path, "path"); checkedText(plugin.label, "label"); checkedText(plugin.name, "name");
	PluginType type = PLUGIN_NONE;
	for (const auto& item : pluginTypes) { if (plugin.format == item.first) { type = item.second; break; } }
	if (type == PLUGIN_NONE) { hostError("Unknown or unsupported hosted plugin format"); }
	const auto binary = binaryType(plugin.binary);
	if (type != PLUGIN_INTERNAL && type != PLUGIN_LV2 && type != PLUGIN_AU)
	{
		if (plugin.path.isEmpty() || !QFileInfo::exists(plugin.path)) { hostError("Hosted plugin path does not exist"); }
	}
	if ((type == PLUGIN_INTERNAL || type == PLUGIN_LV2) && plugin.label.isEmpty())
	{
		hostError("This hosted plugin format requires a native label or URI");
	}
	const auto path = plugin.path.toUtf8(), name = plugin.name.toUtf8(), label = plugin.label.toUtf8();
	const auto before = m_impl->count();
	if (before == std::numeric_limits<int>::max()) { hostError("Native host plugin count cannot grow further"); }
	m_impl->editLayout(m_impl->layout(), [&]
	{
		if (!m_impl->add_plugin(m_impl->handle, binary, type, path.isEmpty() ? nullptr : path.constData(),
			name.isEmpty() ? nullptr : name.constData(), label.isEmpty() ? nullptr : label.constData(), plugin.uniqueId, nullptr, 0))
		{
			m_impl->failed("Could not add hosted plugin");
		}
		if (m_impl->count() != before + 1) { hostError("Native host did not confirm the added plugin"); }
	});
	return before;
#else
	Q_UNUSED(plugin) hostError(m_impl->failure);
#endif
}
void CarlaPluginHost::remove(int index)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->index(index); const auto before = m_impl->count();
	m_impl->editLayout(m_impl->layout(index), [&]
	{
		if (!m_impl->remove_plugin(m_impl->handle, index)) { m_impl->failed("Could not remove hosted plugin"); }
		if (m_impl->count() != before - 1) { hostError("Native host did not confirm plugin removal"); }
	});
#else
	Q_UNUSED(index) hostError(m_impl->failure);
#endif
}
void CarlaPluginHost::rename(int index, const QString& name)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->index(index); checkedText(name, "name");
	if (name.trimmed().isEmpty()) { hostError("Hosted plugin name must not be empty"); }
	if (!m_impl->rename_plugin(m_impl->handle, index, name.toUtf8().constData())) { m_impl->failed("Could not rename hosted plugin"); }
#else
	Q_UNUSED(index) Q_UNUSED(name) hostError(m_impl->failure);
#endif
}
void CarlaPluginHost::move(int first, int second)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->index(first); m_impl->index(second);
	if (first != second)
	{
		m_impl->editLayout(m_impl->layout(first, second), [&]
		{
			if (!m_impl->switch_plugins(m_impl->handle, first, second)) { m_impl->failed("Could not switch hosted plugins"); }
		});
	}
#else
	Q_UNUSED(first) Q_UNUSED(second) hostError(m_impl->failure);
#endif
}
void CarlaPluginHost::setActive(int index, bool active)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->index(index); m_impl->set_active(m_impl->handle, index, active);
	if (m_impl->active(index) != active)
	{
		m_impl->failed("Native host did not confirm plugin activation");
	}
#else
	Q_UNUSED(index) Q_UNUSED(active) hostError(m_impl->failure);
#endif
}

bool CarlaPluginHost::parametersAvailable() const
{
	return available() && m_impl->parameterFailure.isEmpty();
}
QString CarlaPluginHost::parametersUnavailableReason() const
{
	return available() ? m_impl->parameterFailure : m_impl->failure;
}
int CarlaPluginHost::parameterCount(int plugin) const
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	m_impl->requireParameters(); m_impl->index(plugin);
	const auto count = m_impl->get_parameter_count(m_impl->handle, plugin);
	if (count > static_cast<uint32_t>(std::numeric_limits<int>::max())) { hostError("Native host parameter count is invalid"); }
	return static_cast<int>(count);
#else
	Q_UNUSED(plugin) hostError(parametersUnavailableReason());
#endif
}
PluginHost::Parameter CarlaPluginHost::parameter(int plugin, int index) const
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	const auto count = parameterCount(plugin);
	if (index < 0 || index >= count) { hostError("Hosted parameter index does not exist; rediscover after native layout changes"); }
	Parameter result; result.index = index;
	const auto* info = m_impl->get_parameter_info(m_impl->handle, plugin, index);
	if (!info) { m_impl->failed("Could not inspect hosted parameter"); }
	// Each result can be backed by reused native storage. Copy before another call.
	result.name = QString::fromUtf8(info->name); result.symbol = QString::fromUtf8(info->symbol);
	result.unit = QString::fromUtf8(info->unit); result.comment = QString::fromUtf8(info->comment);
	result.group = QString::fromUtf8(info->groupName);
	if (info->scalePointCount > static_cast<uint32_t>(std::numeric_limits<int>::max())) { hostError("Native host scale-point count is invalid"); }
	result.scalePointCount = static_cast<int>(info->scalePointCount);
	const auto* data = m_impl->get_parameter_data(m_impl->handle, plugin, index);
	if (!data) { m_impl->failed("Could not inspect hosted parameter data"); }
	result.nativeIndex = data->rindex;
	result.kind = data->hints & PARAMETER_IS_BOOLEAN ? Parameter::Kind::Boolean
		: data->hints & PARAMETER_IS_INTEGER ? Parameter::Kind::Integer : Parameter::Kind::Number;
	result.enabled = data->hints & PARAMETER_IS_ENABLED;
	result.output = data->type == PARAMETER_OUTPUT;
	result.writable = result.enabled && data->type == PARAMETER_INPUT && !(data->hints & PARAMETER_IS_READ_ONLY);
	result.automatable = result.writable && (data->hints & PARAMETER_IS_AUTOMATABLE);
	result.logarithmic = data->hints & PARAMETER_IS_LOGARITHMIC;
	result.saved = result.enabled && !(data->hints & PARAMETER_IS_NOT_SAVED) && data->type == PARAMETER_INPUT;
	const bool customText = data->hints & PARAMETER_USES_CUSTOM_TEXT;
	const auto* ranges = m_impl->get_parameter_ranges(m_impl->handle, plugin, index);
	if (!ranges) { m_impl->failed("Could not inspect hosted parameter ranges"); }
	result.defaultValue = ranges->def; result.minimum = ranges->min; result.maximum = ranges->max;
	result.step = ranges->step; result.smallStep = ranges->stepSmall; result.largeStep = ranges->stepLarge;
	for (const auto value : {result.defaultValue, result.minimum, result.maximum, result.step, result.smallStep, result.largeStep})
	{
		if (!std::isfinite(value)) { hostError("Native host returned non-finite parameter ranges"); }
	}
	if (result.minimum > result.maximum || result.step < 0 || result.smallStep < 0 || result.largeStep < 0)
	{
		hostError("Native host returned invalid parameter ranges");
	}
	result.value = m_impl->get_current_parameter_value(m_impl->handle, plugin, index);
	if (!std::isfinite(result.value)) { hostError("Native host returned a non-finite parameter value"); }
	if (customText) { result.text = QString::fromUtf8(m_impl->get_parameter_text(m_impl->handle, plugin, index)); }
	return result;
#else
	Q_UNUSED(plugin) Q_UNUSED(index) hostError(parametersUnavailableReason());
#endif
}
std::vector<PluginHost::Choice> CarlaPluginHost::parameterChoices(int plugin, int index, int offset, int limit) const
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	if (offset < 0 || limit < 1 || limit > 1024) { hostError("Hosted parameter choices require offset >= 0 and limit 1..1024"); }
	const auto metadata = parameter(plugin, index);
	const int start = std::min(offset, metadata.scalePointCount);
	const int count = std::min(limit, metadata.scalePointCount - start);
	std::vector<Choice> result; result.reserve(count);
	for (int i = 0; i < count; ++i)
	{
		const auto* point = m_impl->get_parameter_scalepoint_info(m_impl->handle, plugin, index, start + i);
		if (!point) { m_impl->failed("Could not inspect hosted parameter scale point"); }
		if (!std::isfinite(point->value)) { hostError("Native host returned a non-finite scale point"); }
		result.push_back({point->value, QString::fromUtf8(point->label)});
	}
	return result;
#else
	Q_UNUSED(plugin) Q_UNUSED(index) Q_UNUSED(offset) Q_UNUSED(limit) hostError(parametersUnavailableReason());
#endif
}
PluginHost::Parameter CarlaPluginHost::setParameter(int plugin, int index, double value)
{
#ifdef LMMS_CARLA_NATIVE_HOST_API
	const auto metadata = parameter(plugin, index);
	if (!metadata.writable) { hostError("Hosted parameter is disabled, output-only or read-only"); }
	if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum)
	{
		hostError("Hosted parameter value is outside its native range");
	}
	if ((metadata.kind == Parameter::Kind::Integer && std::floor(value) != value)
		|| (metadata.kind == Parameter::Kind::Boolean && value != metadata.minimum && value != metadata.maximum))
	{
		hostError("Hosted parameter value does not match its native discrete type");
	}
	m_impl->set_parameter_value(m_impl->handle, plugin, index, static_cast<float>(value));
	return parameter(plugin, index);
#else
	Q_UNUSED(plugin) Q_UNUSED(index) Q_UNUSED(value) hostError(parametersUnavailableReason());
#endif
}

bool CarlaPluginHost::discoveryAvailable() const
{
	return available() && m_impl->discovery && m_impl->discovery->available();
}
QString CarlaPluginHost::discoveryUnavailableReason() const
{
	return !available() ? m_impl->failure : m_impl->discovery ? m_impl->discovery->unavailableReason() : "Native discovery is unavailable";
}
PluginHost::DiscoveryPage CarlaPluginHost::startDiscovery(const DiscoveryQuery& query)
{
	if (!discoveryAvailable()) { hostError(discoveryUnavailableReason()); }
	return m_impl->discovery->start(query);
}
PluginHost::DiscoveryPage CarlaPluginHost::pollDiscovery(const QString& scan, int offset, int limit)
{
	if (!discoveryAvailable()) { hostError(discoveryUnavailableReason()); }
	return m_impl->discovery->poll(scan, offset, limit);
}
PluginHost::DiscoveryPage CarlaPluginHost::stopDiscovery(const QString& scan)
{
	if (!discoveryAvailable()) { hostError(discoveryUnavailableReason()); }
	return m_impl->discovery->stop(scan);
}

bool CarlaPluginHost::patchbayAvailable() const
{
	return available() && m_impl->patchbay && m_impl->patchbay->available();
}
QString CarlaPluginHost::patchbayUnavailableReason() const
{
	return !available() ? m_impl->failure : m_impl->patchbay ? m_impl->patchbay->unavailableReason() : m_impl->patchbayFailure;
}
PluginHost::PatchbayGraph CarlaPluginHost::patchbay()
{
	if (!patchbayAvailable()) { hostError(patchbayUnavailableReason()); }
	return m_impl->patchbay->inspect();
}
PluginHost::PatchbayGraph CarlaPluginHost::connectPatchbay(const PatchbayRoute& route)
{
	if (!patchbayAvailable()) { hostError(patchbayUnavailableReason()); }
	return m_impl->patchbay->connect(route);
}
PluginHost::PatchbayGraph CarlaPluginHost::disconnectPatchbay(const PatchbayRoute& route)
{
	if (!patchbayAvailable()) { hostError(patchbayUnavailableReason()); }
	return m_impl->patchbay->disconnect(route);
}

} // namespace lmms
