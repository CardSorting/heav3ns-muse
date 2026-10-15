/* Native Carla plugin discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include "CarlaPluginDiscovery.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
#include <CarlaUtils.h>
#endif

namespace lmms
{
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
using namespace CARLA_BACKEND_NAMESPACE;
#endif
namespace
{
[[noreturn]] void discoveryError(const QString& message) { throw std::runtime_error(message.toStdString()); }
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
const std::pair<const char*, PluginType> discoveryTypes[] = {
	{"internal", PLUGIN_INTERNAL}, {"ladspa", PLUGIN_LADSPA}, {"dssi", PLUGIN_DSSI},
	{"lv2", PLUGIN_LV2}, {"vst2", PLUGIN_VST2}, {"vst3", PLUGIN_VST3}, {"au", PLUGIN_AU},
	{"dls", PLUGIN_DLS}, {"gig", PLUGIN_GIG}, {"sf2", PLUGIN_SF2}, {"sfz", PLUGIN_SFZ},
	{"jsfx", PLUGIN_JSFX}, {"clap", PLUGIN_CLAP}
};
QString formatName(PluginType type)
{
	for (const auto& entry : discoveryTypes) { if (entry.second == type) { return entry.first; } }
	discoveryError("Native discovery returned an unknown plugin format");
}
QString binaryName(BinaryType binary)
{
	switch (binary)
	{
	case BINARY_POSIX32: return "posix32";
	case BINARY_POSIX64: return "posix64";
	case BINARY_WIN32: return "win32";
	case BINARY_WIN64: return "win64";
	default: discoveryError("Native discovery returned an unknown binary type");
	}
}
BinaryType discoveryBinary(const QString& name)
{
	if (name == "native") { return BINARY_NATIVE; }
	for (const auto binary : {BINARY_POSIX32, BINARY_POSIX64, BINARY_WIN32, BINARY_WIN64})
	{
		if (name == binaryName(binary)) { return binary; }
	}
	discoveryError("Unknown discovery binary type");
}
#endif
}

struct CarlaPluginDiscovery::Impl
{
	QString unavailable = "This Carla build does not expose native plugin discovery";
	QString libraryPath, searchPath;
	QString id, state = "idle", error;
	QElapsedTimer timer;
	qint64 finishedMs = 0;
	int maximumPlugins = 4096, timeoutMs = 60000;
	bool callbackFailed = false, resultLimit = false;
	std::vector<PluginHost::DiscoveredPlugin> plugins;
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	CarlaPluginDiscoveryHandle handle = nullptr;
	decltype(&carla_plugin_discovery_start) start = nullptr;
	decltype(&carla_plugin_discovery_idle) idle = nullptr;
	decltype(&carla_plugin_discovery_stop) stop = nullptr;
	~Impl() { try { release(); } catch (...) {} }
	void release()
	{
		const auto previous = handle; handle = nullptr;
		if (previous) { stop(previous); }
	}
	void finish(const QString& status, const QString& reason = {})
	{
		state = status; error = reason;
		try { release(); }
		catch (...) { state = "failed"; error = "Native discovery cleanup threw an exception"; }
		finishedMs = timer.isValid() ? timer.elapsed() : 0;
	}
	static void discovered(void* context, const CarlaPluginDiscoveryInfo* info, const char* hash) noexcept
	{
		auto& self = *static_cast<Impl*>(context);
		if (!info || self.callbackFailed || self.resultLimit) { return; }
		if (self.plugins.size() >= static_cast<std::size_t>(self.maximumPlugins)) { self.resultLimit = true; return; }
		try
		{
			PluginHost::DiscoveredPlugin result;
			result.plugin.format = formatName(info->ptype); result.plugin.binary = binaryName(info->btype);
			result.plugin.path = QString::fromUtf8(info->filename); result.plugin.label = QString::fromUtf8(info->label);
			result.plugin.name = QString::fromUtf8(info->metadata.name);
			result.plugin.uniqueId = std::bit_cast<qint64>(static_cast<quint64>(info->uniqueId));
			// File-based cached formats return the filename in label. LV2 returns
			// only a bundle basename in filename; its URI is the actual load key.
			if (info->ptype == PLUGIN_SFZ)
			{
				if (result.plugin.path.isEmpty()) { result.plugin.path = result.plugin.label; }
			}
			if (info->ptype == PLUGIN_JSFX && result.plugin.path.isEmpty())
			{
				result.plugin.path = QDir(self.searchPath).filePath(result.plugin.label);
			}
			if (info->ptype == PLUGIN_LV2) { result.plugin.path.clear(); }
			result.maker = QString::fromUtf8(info->metadata.maker); result.hash = QString::fromUtf8(hash);
			result.category = static_cast<int>(info->metadata.category);
			result.instrument = info->metadata.hints & PLUGIN_IS_SYNTH;
			result.audioInputs = info->io.audioIns; result.audioOutputs = info->io.audioOuts;
			result.midiInputs = info->io.midiIns; result.midiOutputs = info->io.midiOuts;
			result.cvInputs = info->io.cvIns; result.cvOutputs = info->io.cvOuts;
			result.parameterInputs = info->io.parameterIns; result.parameterOutputs = info->io.parameterOuts;
			self.plugins.push_back(std::move(result));
		}
		catch (...) { self.callbackFailed = true; } // Never unwind through Carla's callback ABI.
	}
	void checkResult()
	{
		if (callbackFailed) { finish("failed", "Native discovery returned invalid data or the callback could not retain it"); }
		else if (resultLimit) { finish("limited", "Discovery reached maximumPlugins; retained results are partial"); }
	}
#endif
	void requireId(const QString& scan) const
	{
		if (id.isEmpty() || scan != id) { discoveryError("Discovery scan does not exist in this instrument instance"); }
	}
	PluginHost::DiscoveryPage page(int offset, int limit) const
	{
		if (offset < 0 || limit < 1 || limit > 1024) { discoveryError("Discovery pagination requires offset >= 0 and limit 1..1024"); }
		PluginHost::DiscoveryPage result;
		result.scan = id; result.state = state; result.error = error; result.offset = offset;
		result.elapsedMs = state == "running" && timer.isValid() ? timer.elapsed() : finishedMs;
		result.total = static_cast<int>(plugins.size());
		const int begin = std::min(offset, result.total), count = std::min(limit, result.total - begin);
		result.plugins.assign(plugins.begin() + begin, plugins.begin() + begin + count);
		result.hasMore = begin + count < result.total;
		return result;
	}
};

CarlaPluginDiscovery::CarlaPluginDiscovery(const QString& libraryPath, const Resolver& resolver)
	: m_impl(std::make_unique<Impl>())
{
	m_impl->libraryPath = libraryPath;
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	try
	{
		if (!resolver) { m_impl->unavailable = "No native Carla discovery symbol resolver is available"; return; }
#define DISCOVERY_FUNCTION(name, field) \
		m_impl->field = reinterpret_cast<decltype(m_impl->field)>(resolver("carla_plugin_discovery_" #name)); \
		if (!m_impl->field) { m_impl->unavailable = "Carla library lacks carla_plugin_discovery_" #name; return; }
		DISCOVERY_FUNCTION(start, start)
		DISCOVERY_FUNCTION(idle, idle)
		DISCOVERY_FUNCTION(stop, stop)
#undef DISCOVERY_FUNCTION
		m_impl->unavailable.clear();
	}
	catch (...) { m_impl->unavailable = "Carla discovery symbol resolution failed"; }
#else
	Q_UNUSED(resolver)
#endif
}
CarlaPluginDiscovery::~CarlaPluginDiscovery() = default;
bool CarlaPluginDiscovery::available() const { return m_impl->unavailable.isEmpty(); }
QString CarlaPluginDiscovery::unavailableReason() const { return m_impl->unavailable; }

PluginHost::DiscoveryPage CarlaPluginDiscovery::start(const PluginHost::DiscoveryQuery& query)
{
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	if (!available()) { discoveryError(unavailableReason()); }
	if (m_impl->handle || m_impl->state == "starting") { discoveryError("Stop or finish the current discovery scan first"); }
	if (query.maximumPlugins < 1 || query.maximumPlugins > 65536 || query.timeoutMs < 1 || query.timeoutMs > 600000)
	{
		discoveryError("Discovery requires maximumPlugins 1..65536 and timeoutMs 1..600000");
	}
	for (const auto& value : {query.path, query.discoveryTool})
	{
		if (value.contains(QChar::Null)) { discoveryError("Discovery paths cannot contain a null character"); }
	}
	PluginType type = PLUGIN_NONE;
	for (const auto& entry : discoveryTypes) { if (query.format == entry.first) { type = entry.second; break; } }
	if (type == PLUGIN_NONE) { discoveryError("Unknown discovery format"); }
	const auto binary = discoveryBinary(query.binary);
	const bool cached = type == PLUGIN_INTERNAL || type == PLUGIN_LV2 || type == PLUGIN_SFZ || type == PLUGIN_JSFX;
	if ((type == PLUGIN_INTERNAL || type == PLUGIN_LV2 || type == PLUGIN_SFZ || type == PLUGIN_JSFX
		|| type == PLUGIN_DLS || type == PLUGIN_GIG || type == PLUGIN_SF2) && binary != BINARY_NATIVE)
	{
		discoveryError("This discovery format requires the native binary type");
	}
	if (!query.path.isEmpty() && !QFileInfo(query.path).isDir()) { discoveryError("Discovery path must be an existing directory"); }
	if (query.path.contains(QDir::listSeparator())) { discoveryError("Discovery accepts one search directory per scan"); }
	if (query.path.isEmpty() && type != PLUGIN_INTERNAL && type != PLUGIN_LV2)
	{
		discoveryError("This discovery format requires a search directory");
	}
	auto tool = query.discoveryTool;
	if (tool.isEmpty())
	{
		const auto directory = QFileInfo(m_impl->libraryPath).absoluteDir();
		const auto suffix = binary == BINARY_WIN32 || binary == BINARY_WIN64 ? ".exe" : "";
		QStringList names;
		if (binary == BINARY_NATIVE) { names.append("carla-discovery-native"); }
		names.append(QString("carla-discovery-") + binaryName(binary));
		for (const auto& name : names)
		{
			const auto candidate = directory.filePath(name + suffix);
			if (QFileInfo(candidate).isFile()) { tool = candidate; break; }
		}
		// The native API requires a nonempty tool string even for cached formats,
		// which it enumerates in-process without launching that tool.
		if (tool.isEmpty() && cached) { tool = directory.filePath("carla-discovery-native" + QString(suffix)); }
	}
	if (tool.isEmpty() || ((!cached || !query.discoveryTool.isEmpty()) && !QFileInfo(tool).isFile()))
	{
		discoveryError("Supply the path of an installed Carla discovery tool");
	}
	const auto path = QDir::toNativeSeparators(query.path).toUtf8(), executable = QFileInfo(tool).absoluteFilePath().toUtf8();
	m_impl->plugins.clear(); m_impl->error.clear(); m_impl->callbackFailed = false; m_impl->resultLimit = false;
	m_impl->maximumPlugins = query.maximumPlugins; m_impl->timeoutMs = query.timeoutMs;
	m_impl->searchPath = query.path;
	m_impl->id = QUuid::createUuid().toString(QUuid::WithoutBraces); m_impl->state = "starting"; m_impl->timer.start();
	try
	{
		m_impl->handle = m_impl->start(executable.constData(), binary, type, path.isEmpty() ? nullptr : path.constData(),
			Impl::discovered, nullptr, m_impl.get());
		if (!m_impl->handle) { m_impl->finish("noHandle", "Carla returned no scan handle; no candidates or a native startup failure"); }
		else { m_impl->state = "running"; m_impl->checkResult(); }
	}
	catch (...) { m_impl->finish("failed", "Native Carla discovery startup threw an exception"); }
	return m_impl->page(0, 128);
#else
	Q_UNUSED(query) discoveryError(unavailableReason());
#endif
}

PluginHost::DiscoveryPage CarlaPluginDiscovery::poll(const QString& scan, int offset, int limit)
{
	m_impl->requireId(scan);
	if (offset < 0 || limit < 1 || limit > 1024) { discoveryError("Discovery pagination requires offset >= 0 and limit 1..1024"); }
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	if (m_impl->handle)
	{
		try
		{
			if (m_impl->timer.elapsed() >= m_impl->timeoutMs) { m_impl->finish("timedOut", "Discovery exceeded its polling deadline"); }
			else
			{
				const bool running = m_impl->idle(m_impl->handle); m_impl->checkResult();
				if (m_impl->handle && m_impl->timer.elapsed() >= m_impl->timeoutMs)
				{
					m_impl->finish("timedOut", "Discovery exceeded its polling deadline");
				}
				else if (m_impl->handle && !running) { m_impl->finish("finished"); }
			}
		}
		catch (...) { m_impl->finish("failed", "Native Carla discovery polling threw an exception"); }
	}
#endif
	return m_impl->page(offset, limit);
}
PluginHost::DiscoveryPage CarlaPluginDiscovery::stop(const QString& scan)
{
	m_impl->requireId(scan);
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	if (m_impl->handle) { m_impl->finish("stopped"); }
#endif
	return m_impl->page(0, 128);
}
} // namespace lmms
