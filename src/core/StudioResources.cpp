/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioResources.h"

#include <algorithm>
#include <cmath>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <sys/resource.h>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif
#endif

namespace lmms
{
namespace
{
constexpr double Maximum = 9007199254740991.;
bool number(const QJsonValue& value)
{
	return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= Maximum;
}
bool optionalNumber(const QJsonValue& value) { return value.isNull() || number(value); }
bool count(const QJsonValue& value)
{
	return number(value) && value.toDouble() <= 2147483646 && value.toDouble() == value.toInt(-1);
}
QJsonValue measurement(double value) { return std::isfinite(value) && value >= 0 && value <= Maximum ? QJsonValue(value) : QJsonValue(QJsonValue::Null); }
bool validInterval(const QJsonObject& value)
{
	return value.size() == 3 && optionalNumber(value.value("wallMs")) && optionalNumber(value.value("cpuMs"))
		&& optionalNumber(value.value("cpuPercentOfOneCore"))
		&& (value.value("cpuMs").isNull() || number(value.value("wallMs")))
		&& (value.value("cpuPercentOfOneCore").isNull() || (number(value.value("cpuMs")) && value.value("wallMs").toDouble() > 0));
}
}

QJsonObject StudioResources::sample()
{
	static const auto instance = QUuid::createUuid().toString(QUuid::WithoutBraces);
	static const auto timer = [] { QElapsedTimer result; result.start(); return result; }();
	QJsonObject result{{"format", 1}, {"scope", "lmms-process"}, {"process", QString::number(QCoreApplication::applicationPid())}, {"instance", instance},
		{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"monotonicMs", timer.nsecsElapsed() / 1000000.},
		{"cpuUserMs", QJsonValue::Null}, {"cpuSystemMs", QJsonValue::Null},
		{"residentBytes", QJsonValue::Null}, {"peakResidentBytes", QJsonValue::Null}};
	QJsonObject sources{{"cpu", "unavailable"}, {"resident", "unavailable"}, {"peakResident", "unavailable"}}; QJsonArray errors;
#ifdef Q_OS_WIN
	FILETIME created{}, exited{}, kernel{}, user{};
	if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
	{
		const auto ms = [](const FILETIME& time) { return (static_cast<double>(time.dwHighDateTime) * 4294967296. + time.dwLowDateTime) / 10000.; };
		result.insert("cpuUserMs", measurement(ms(user))); result.insert("cpuSystemMs", measurement(ms(kernel))); sources.insert("cpu", "GetProcessTimes");
	}
	else { errors.append("GetProcessTimes failed: " + QString::number(GetLastError())); }
	// Resolve the Windows 7+ kernel API without a PSAPI import-library dependency.
	using MemoryInfo = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
	const auto module = GetModuleHandleW(L"kernel32.dll");
	const auto memoryInfo = module ? reinterpret_cast<MemoryInfo>(GetProcAddress(module, "K32GetProcessMemoryInfo")) : nullptr;
	PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof(memory);
	if (memoryInfo && memoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
	{
		result.insert("residentBytes", measurement(static_cast<double>(memory.WorkingSetSize)));
		result.insert("peakResidentBytes", measurement(static_cast<double>(memory.PeakWorkingSetSize)));
		sources.insert("resident", "K32GetProcessMemoryInfo.WorkingSetSize"); sources.insert("peakResident", "K32GetProcessMemoryInfo.PeakWorkingSetSize");
	}
	else { errors.append(memoryInfo ? "K32GetProcessMemoryInfo failed: " + QString::number(GetLastError()) : "K32GetProcessMemoryInfo unavailable"); }
#else
	struct rusage usage{};
	if (::getrusage(RUSAGE_SELF, &usage) == 0)
	{
		const auto ms = [](const timeval& time) { return static_cast<double>(time.tv_sec) * 1000. + time.tv_usec / 1000.; };
		result.insert("cpuUserMs", measurement(ms(usage.ru_utime))); result.insert("cpuSystemMs", measurement(ms(usage.ru_stime)));
		sources.insert("cpu", "getrusage(RUSAGE_SELF)");
#ifdef Q_OS_LINUX
		result.insert("peakResidentBytes", measurement(static_cast<double>(usage.ru_maxrss) * 1024.)); sources.insert("peakResident", "getrusage.ru_maxrss KiB");
#endif
	}
	else { errors.append("getrusage failed: " + QString::number(errno)); }
#ifdef Q_OS_LINUX
	QFile statm("/proc/self/statm"); bool parsed = false;
	if (statm.open(QIODevice::ReadOnly))
	{
		const auto fields = statm.read(256).simplified().split(' '); const auto pageSize = ::sysconf(_SC_PAGESIZE);
		if (fields.size() >= 2 && pageSize > 0)
		{
			const auto pages = fields.at(1).toULongLong(&parsed);
			if (parsed)
			{
				result.insert("residentBytes", measurement(static_cast<double>(pages) * pageSize));
				sources.insert("resident", "/proc/self/statm resident pages (estimate)");
			}
		}
	}
	if (!parsed) { errors.append("Linux resident-memory estimate unavailable"); }
#elif defined(Q_OS_MACOS)
	mach_task_basic_info_data_t memory{}; mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
	const auto status = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&memory), &size);
	if (status == KERN_SUCCESS && size == MACH_TASK_BASIC_INFO_COUNT)
	{
		result.insert("residentBytes", measurement(static_cast<double>(memory.resident_size)));
		result.insert("peakResidentBytes", measurement(static_cast<double>(memory.resident_size_max)));
		sources.insert("resident", "MACH_TASK_BASIC_INFO.resident_size"); sources.insert("peakResident", "MACH_TASK_BASIC_INFO.resident_size_max");
	}
	else { errors.append("MACH_TASK_BASIC_INFO failed: " + QString::number(status)); }
#else
	errors.append("Resident-memory measurements are unavailable on this platform");
#endif
#endif
	result.insert("sources", sources); result.insert("errors", errors); return result;
}

bool StudioResources::validSample(const QJsonObject& value)
{
	static const QRegularExpression pid("^[1-9][0-9]{0,19}$"), uuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
	if (value.size() != 12 || value.value("format") != QJsonValue(1) || value.value("scope") != QJsonValue("lmms-process")
		|| !pid.match(value.value("process").toString()).hasMatch()
		|| !uuid.match(value.value("instance").toString()).hasMatch() || !value.value("at").isString()
		|| !QDateTime::fromString(value.value("at").toString(), Qt::ISODateWithMs).isValid() || !number(value.value("monotonicMs"))
		|| !value.value("sources").isObject() || !value.value("errors").isArray()) { return false; }
	for (const auto field : {"cpuUserMs", "cpuSystemMs", "residentBytes", "peakResidentBytes"}) { if (!optionalNumber(value.value(field))) { return false; } }
	const auto sources = value.value("sources").toObject(); if (sources.size() != 3) { return false; }
	for (const auto field : {"cpu", "resident", "peakResident"})
	{
		if (!sources.value(field).isString() || sources.value(field).toString().isEmpty() || sources.value(field).toString().size() > 128) { return false; }
	}
	const auto errors = value.value("errors").toArray(); if (errors.size() > 8) { return false; }
	for (const auto& error : errors) { if (!error.isString() || error.toString().size() > 1024) { return false; } }
	return true;
}

QJsonObject StudioResources::interval(const QJsonObject& before, const QJsonObject& after)
{
	QJsonObject result{{"wallMs", QJsonValue::Null}, {"cpuMs", QJsonValue::Null}, {"cpuPercentOfOneCore", QJsonValue::Null}};
	if (!validSample(before) || !validSample(after) || before.value("process") != after.value("process")
		|| before.value("instance") != after.value("instance") || after.value("monotonicMs").toDouble() < before.value("monotonicMs").toDouble()) { return result; }
	const auto wall = after.value("monotonicMs").toDouble() - before.value("monotonicMs").toDouble(); result.insert("wallMs", wall);
	double cpu = 0;
	for (const auto field : {"cpuUserMs", "cpuSystemMs"})
	{
		if (!number(before.value(field)) || !number(after.value(field)) || after.value(field).toDouble() < before.value(field).toDouble()) { return result; }
		cpu += after.value(field).toDouble() - before.value(field).toDouble();
	}
	const auto measuredCpu = measurement(cpu); result.insert("cpuMs", measuredCpu);
	if (wall > 0 && measuredCpu.isDouble()) { result.insert("cpuPercentOfOneCore", measurement(100. * cpu / wall)); }
	return result;
}

bool StudioResources::validUsage(const QJsonObject& value)
{
	return value.size() == 8 && value.value("format") == QJsonValue(1) && count(value.value("transitions"))
		&& count(value.value("cpuMeasuredTransitions")) && value.value("cpuMeasuredTransitions").toInt() <= value.value("transitions").toInt()
		&& number(value.value("wallMs")) && number(value.value("cpuMs")) && optionalNumber(value.value("maximumObservedResidentBytes"))
		&& value.value("lastSample").isObject() && validSample(value.value("lastSample").toObject())
		&& value.value("lastInterval").isObject() && validInterval(value.value("lastInterval").toObject());
}

QJsonObject StudioResources::accumulate(const QJsonObject& usage, const QJsonObject& before, const QJsonObject& after)
{
	// Telemetry failure must not invalidate a musical edit. Invalid prior durable
	// usage is rejected at session open; a failed fresh sample retains prior usage.
	if ((!usage.isEmpty() && !validUsage(usage)) || !validSample(before) || !validSample(after)) { return usage; }
	const auto measured = interval(before, after);
	QJsonValue maximum = usage.isEmpty() ? QJsonValue(QJsonValue::Null) : usage.value("maximumObservedResidentBytes");
	for (const auto& sample : {before, after})
	{
		const auto resident = sample.value("residentBytes");
		if (number(resident) && (maximum.isNull() || resident.toDouble() > maximum.toDouble())) { maximum = resident; }
	}
	return {{"format", 1}, {"transitions", std::min(2147483646, usage.value("transitions").toInt() + 1)},
		{"cpuMeasuredTransitions", std::min(2147483646, usage.value("cpuMeasuredTransitions").toInt() + (number(measured.value("cpuMs")) ? 1 : 0))},
		{"wallMs", std::min(Maximum, usage.value("wallMs").toDouble() + measured.value("wallMs").toDouble())},
		{"cpuMs", std::min(Maximum, usage.value("cpuMs").toDouble() + measured.value("cpuMs").toDouble())},
		{"maximumObservedResidentBytes", maximum}, {"lastSample", after}, {"lastInterval", measured}};
}
QString StudioResources::budgetError(const QJsonObject& job, const QJsonObject& before, const QJsonObject& current)
{
	const auto cpuLimit = job.value("maximumCpuTimeMs").toInt(), residentLimit = job.value("maximumResidentMiB").toInt();
	if (!cpuLimit && !residentLimit) { return {}; }
	if (!validSample(current)) { return "Resource budget cannot be checked: native process measurements are unavailable"; }
	if (cpuLimit)
	{
		const auto usage = job.value("resourceUsage").toObject();
		const auto priorTransitions = job.value("rounds").toInt() + job.value("operations").toInt();
		if ((!usage.isEmpty() && (!validUsage(usage) || usage.value("cpuMeasuredTransitions") != usage.value("transitions")
			|| usage.value("transitions").toInt() < priorTransitions)) || (usage.isEmpty() && priorTransitions))
		{
			return "CPU budget cannot be checked: earlier job execution has incomplete CPU accounting";
		}
		const auto delta = interval(before, current).value("cpuMs");
		if (!number(delta)) { return "CPU budget cannot be checked: native CPU counters are unavailable or reset"; }
		if (usage.value("cpuMs").toDouble() + delta.toDouble() >= cpuLimit) { return "Conversation execution reached its measured CPU-time budget"; }
	}
	if (residentLimit)
	{
		const auto resident = current.value("residentBytes");
		if (!number(resident)) { return "Resident-memory budget cannot be checked: native resident-memory measurement is unavailable"; }
		if (resident.toDouble() >= static_cast<double>(residentLimit) * 1024. * 1024.)
		{
			return "Conversation execution reached its process resident-memory budget";
		}
	}
	return {};
}
} // namespace lmms
