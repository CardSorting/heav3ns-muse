/*
 * carla.cpp - Carla for LMMS
 *
 * Copyright (C) 2014-2018 Filipe Coelho <falktx@falktx.com>
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

#include "Carla.h"

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "DataFile.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentPlayHandle.h"
#include "InstrumentTrack.h"
#include "Knob.h"
#include "MidiEventToByteSeq.h"
#include "MainWindow.h"
#include "FontHelper.h"
#include "Song.h"

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMdiArea>
#include <QPushButton>
#include <QScrollArea>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QStringListModel>
#include <QTimerEvent>
#include <QThread>
#include <QVBoxLayout>

#include <cstring>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

#include "embed.h"

namespace lmms
{

// this doesn't seem to be defined anywhere
static const double ticksPerBeat = 48.0;

/*
 * Current TODO items:
 *  - get plugin instance name (to use in external window title)
 *
 * All other items are to be done in Carla itself.
 */

// -----------------------------------------------------------------------

#define handlePtr ((CarlaInstrument*)handle)

static uint32_t host_get_buffer_size(NativeHostHandle handle)
{
    return handlePtr->handleGetBufferSize();
}

static double host_get_sample_rate(NativeHostHandle handle)
{
    return handlePtr->handleGetSampleRate();
}

static bool host_is_offline(NativeHostHandle handle)
{
    return handlePtr->handleIsOffline();
}

static const NativeTimeInfo* host_get_time_info(NativeHostHandle handle)
{
    return handlePtr->handleGetTimeInfo();
}

namespace
{
struct MidiOutputBlock
{
    CarlaInstrument* instrument;
    uint32_t frames, count = 0;
    bool enabled, failed = false;
    std::array<NativeMidiEvent, CarlaInstrument::kMaxMidiEvents> events{};
};
thread_local MidiOutputBlock* currentMidiOutput = nullptr;
}

static bool host_write_midi_event(NativeHostHandle handle, const NativeMidiEvent* event) noexcept
{
    auto* block = currentMidiOutput;
    if (!block || block->instrument != handle) { return false; }
    if (!event) { block->failed |= block->enabled; return false; }
    try
    {
        if (event->port != 0 || event->time >= block->frames
            || !readFromByteSeq(event->data, event->size))
        {
            block->failed |= block->enabled; return false;
        }
        if (!block->enabled) { return true; } // Unselected/offline output has no external destination.
        if (block->failed || block->count == block->events.size()) { block->failed = true; return false; }
        block->events[block->count++] = *event;
        return true; // Accepted by the host buffer, not acknowledgment from a device.
    }
    catch (...) { block->failed |= block->enabled; return false; }
}

static void host_ui_parameter_changed(NativeHostHandle handle, uint32_t index, float value)
{
    handlePtr->handleUiParameterChanged(index, value);
}

static void host_ui_custom_data_changed(NativeHostHandle handle, const char* key, const char* value)
{
    // unused
}

static void host_ui_closed(NativeHostHandle handle)
{
    handlePtr->handleUiClosed();
}

static intptr_t host_dispatcher(NativeHostHandle handle, NativeHostDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt)
{
    return handlePtr->handleDispatcher(opcode, index, value, ptr, opt);
}

#undef handlePtr

// -----------------------------------------------------------------------

static const char* host_ui_open_file(NativeHostHandle, bool isDir, const char* title, const char* filter)
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!getGUI() || !app || QThread::currentThread() != app->thread()) { return nullptr; }
    static QByteArray retStr;
    const QFileDialog::Options options(isDir ? QFileDialog::ShowDirsOnly : 0x0);

    retStr = QFileDialog::getOpenFileName(QApplication::activeWindow(), title, "", filter, nullptr, options).toUtf8();

    return retStr.isEmpty() ? nullptr : retStr.constData();
}

static const char* host_ui_save_file(NativeHostHandle, bool isDir, const char* title, const char* filter)
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!getGUI() || !app || QThread::currentThread() != app->thread()) { return nullptr; }
    static QByteArray retStr;
    const QFileDialog::Options options(isDir ? QFileDialog::ShowDirsOnly : 0x0);

    retStr = QFileDialog::getSaveFileName(QApplication::activeWindow(), title, "", filter, nullptr, options).toUtf8();

    return retStr.isEmpty() ? nullptr : retStr.constData();
}

// -----------------------------------------------------------------------


CarlaInstrument::CarlaInstrument(InstrumentTrack* const instrumentTrack, const Descriptor* const descriptor, const bool isPatchbay,
    const CarlaPluginHost::Resolver& hostResolver)
    : Instrument(instrumentTrack, descriptor, nullptr, Flag::IsSingleStreamed | Flag::IsMidiBased | Flag::IsNotBendable),
      kIsPatchbay(isPatchbay),
      fHandle(nullptr),
      fDescriptor(isPatchbay ? carla_get_native_patchbay_plugin() : carla_get_native_rack_plugin()),
      fMidiEventCount(0),
      m_paramModels(),
      m_hostResolver(hostResolver)
{
    if (!fDescriptor || !fDescriptor->instantiate || !fDescriptor->process || !fDescriptor->cleanup)
    {
        throw std::runtime_error("Carla native engine is unavailable or incomplete");
    }
    fHost.handle      = this;
    fHost.uiName      = nullptr;
    fHost.uiParentId  = 0;

    // carla/resources contains PyQt scripts required for launch
    QDir path(carla_get_library_folder());
#if defined(CARLA_OS_LINUX)
    path.cdUp();
    path.cdUp();
    QString resourcesPath = path.absolutePath() + "/share/carla/resources";
#else
    // parse prefix from dll filename
    QString resourcesPath = path.absolutePath() + "/resources";
#endif
    fHost.resourceDir            = strdup(resourcesPath.toUtf8().constData());
    bool activated = false;
    auto failedConstruction = qScopeGuard([&]
    {
        if (m_pluginHost) { m_pluginHost->prepareForEngineCleanup(); }
        const auto handle = fHandle;
        fHandle = nullptr;
        if (handle)
        {
            if (activated && fDescriptor->deactivate) { fDescriptor->deactivate(handle); }
            fDescriptor->cleanup(handle);
        }
        m_pluginHost.reset();
        std::free(const_cast<char*>(fHost.resourceDir));
        fHost.resourceDir = nullptr;
    });
    fHost.get_buffer_size        = host_get_buffer_size;
    fHost.get_sample_rate        = host_get_sample_rate;
    fHost.is_offline             = host_is_offline;
    fHost.get_time_info          = host_get_time_info;
    fHost.write_midi_event       = host_write_midi_event;
    fHost.ui_parameter_changed   = host_ui_parameter_changed;
    fHost.ui_custom_data_changed = host_ui_custom_data_changed;
    fHost.ui_closed              = host_ui_closed;
    fHost.ui_open_file           = host_ui_open_file;
    fHost.ui_save_file           = host_ui_save_file;
    fHost.dispatcher             = host_dispatcher;

    std::memset(&fTimeInfo, 0, sizeof(NativeTimeInfo));
    fTimeInfo.bbt.valid = true; // always valid

    fHandle = fDescriptor->instantiate(&fHost);
    if (!fHandle)
    {
        throw std::runtime_error("Carla native engine could not create an instrument");
    }

    m_lastOffline = handleIsOffline();

    // Carla's base callback slot is not synchronized with its runner threads.
    // Install graph observation before activation or loading hosted plugins.
    if (kIsPatchbay) { pluginHost(); }

    if (fHandle != nullptr && fDescriptor->activate != nullptr)
    {
        fDescriptor->activate(fHandle);
        activated = true;
    }

#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
    // The parameter name model is usable without any graphical presentation.
    m_completerModel = new QStringListModel(this);
    refreshParams();
#endif

    // Register processing only after the instrument's native models are ready.
    auto iph = new InstrumentPlayHandle(this, instrumentTrack);
    Engine::audioEngine()->addPlayHandle(iph);

    connect(Engine::audioEngine(), SIGNAL(sampleRateChanged()), this, SLOT(sampleRateChanged()));
    failedConstruction.dismiss();
}

CarlaInstrument::~CarlaInstrument()
{
    Engine::audioEngine()->removePlayHandlesOfTypes(instrumentTrack(), PlayHandle::Type::NotePlayHandle | PlayHandle::Type::InstrumentPlayHandle);
    if (m_pluginHost) { m_pluginHost->prepareForEngineCleanup(); }

    const auto handle = fHandle;
    fHandle = nullptr;
    if (handle)
    {
        if (fDescriptor->deactivate) { fDescriptor->deactivate(handle); }
        fDescriptor->cleanup(handle);
    }
    m_pluginHost.reset(); // Keep callback storage/library alive through native cleanup.

    if (fHost.resourceDir != nullptr)
    {
        std::free((char*)fHost.resourceDir);
        fHost.resourceDir = nullptr;
    }

    if (fHost.uiName != nullptr)
    {
        std::free((char*)fHost.uiName);
        fHost.uiName = nullptr;
    }

#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
    clearParamModels();
#endif
}

// -------------------------------------------------------------------

uint32_t CarlaInstrument::handleGetBufferSize() const
{
    return Engine::audioEngine()->framesPerPeriod();
}

double CarlaInstrument::handleGetSampleRate() const
{
    return Engine::audioEngine()->outputSampleRate();
}

bool CarlaInstrument::handleIsOffline() const
{
    return Engine::getSong() && Engine::getSong()->isExporting();
}

const NativeTimeInfo* CarlaInstrument::handleGetTimeInfo() const
{
    return &fTimeInfo;
}

void CarlaInstrument::handleUiParameterChanged(const uint32_t index, const float value)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, [this, index, value] { handleUiParameterChanged(index, value); },
            Qt::QueuedConnection);
        return;
    }
    QScopedValueRollback<bool> updating(m_updatingParams, true);
	if (m_paramModels.size() > index)
	{
		m_paramModels[index]->setValue(value);
	}
}

void CarlaInstrument::handleUiClosed()
{
    emit uiClosed();
}

intptr_t CarlaInstrument::handleDispatcher(const NativeHostDispatcherOpcode opcode, const int32_t index,
	const intptr_t value, void* const ptr, const float opt)
{
    intptr_t ret = 0;

    // source/includes/CarlaNative.h
    // NATIVE_HOST_OPCODE_NULL					= 0, nothing
    // NATIVE_HOST_OPCODE_UPDATE_PARAMETER		= 1, uses index, -1 for all
    // NATIVE_HOST_OPCODE_UPDATE_MIDI_PROGRAM	= 2, uses index, -1 for all; may use value for channel
    // NATIVE_HOST_OPCODE_RELOAD_PARAMETERS		= 3, nothing
    // NATIVE_HOST_OPCODE_RELOAD_MIDI_PROGRAMS	= 4, nothing
    // NATIVE_HOST_OPCODE_RELOAD_ALL			= 5, nothing
    // NATIVE_HOST_OPCODE_UI_UNAVAILABLE		= 6, nothing
    // NATIVE_HOST_OPCODE_HOST_IDLE				= 7, nothing
    // NATIVE_HOST_OPCODE_INTERNAL_PLUGIN		= 8, nothing
    // NATIVE_HOST_OPCODE_QUEUE_INLINE_DISPLAY	= 9, nothing
    // NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER	= 10 uses index, value as bool

    switch (opcode)
    {
    case NATIVE_HOST_OPCODE_UI_UNAVAILABLE:
        handleUiClosed();
        break;
    case NATIVE_HOST_OPCODE_HOST_IDLE:
        // A plugin callback must not pump arbitrary studio commands while an
        // instrument is being constructed, restored, or destroyed.
        if (getGUI() && QCoreApplication::instance()
            && QThread::currentThread() == QCoreApplication::instance()->thread())
        {
            QCoreApplication::processEvents();
        }
        break;
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
    case NATIVE_HOST_OPCODE_UI_TOUCH_PARAMETER:
        // param index, value as bool
        // true = mousePress
        // false = mouseRelease
        if (!value) {
            updateParamModel(index);
        }
        break;
    case NATIVE_HOST_OPCODE_RELOAD_ALL:
        refreshParams();
        break;
    case NATIVE_HOST_OPCODE_UPDATE_PARAMETER:
        if (index == -1) { refreshParams(); }
        else if (index >= 0) { updateParamModel(static_cast<uint32_t>(index)); }
        break;
    case NATIVE_HOST_OPCODE_RELOAD_PARAMETERS:
        refreshParams();
        break;
    case NATIVE_HOST_OPCODE_UPDATE_MIDI_PROGRAM:
    case NATIVE_HOST_OPCODE_RELOAD_MIDI_PROGRAMS:
    case NATIVE_HOST_OPCODE_INTERNAL_PLUGIN:
    case NATIVE_HOST_OPCODE_QUEUE_INLINE_DISPLAY:
        break;
#endif
    default:
        break;
    }

    return ret;
}

// -------------------------------------------------------------------

QString CarlaInstrument::nodeName() const
{
    return  descriptor()->name;
}

static bool isCarlaState(const QDomElement& element)
{
    return element.tagName().compare("CARLA-PROJECT", Qt::CaseInsensitive) == 0
        || element.tagName().compare("CARLA-PRESET", Qt::CaseInsensitive) == 0;
}

void CarlaInstrument::saveSettings(QDomDocument& doc, QDomElement& parent)
{
    try
    {
        if (!m_stateError.isEmpty()) { throw std::runtime_error(m_stateError.toStdString()); }
        if (!fHandle || !fDescriptor->get_state)
        {
            throw std::runtime_error("Native Carla state saving is unavailable");
        }
        const std::unique_ptr<char, decltype(&std::free)> state(fDescriptor->get_state(fHandle), std::free);
        QDomDocument carlaDoc;
        if (!state || !carlaDoc.setContent(QByteArray(state.get())) || !isCarlaState(carlaDoc.documentElement()))
        {
            throw std::runtime_error("Native Carla returned missing or invalid project state");
        }
        parent.appendChild(doc.importNode(carlaDoc.documentElement(), true));

#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
        // Native changes may arrive while the Carla editor is closed. Snapshot
        // their model values too, without overwriting controlled manual values.
        refreshParams();
        for (uint32_t index = 0; index < m_paramModels.size(); ++index)
        {
            QString idStr = CARLA_SETTING_PREFIX + QString::number(index);
            m_paramModels[index]->saveSettings(doc, parent, idStr);
        }
#endif
    }
    catch (const std::exception& error)
    {
        DataFile::recordSerializationError(doc, tr("Could not preserve Carla state: %1").arg(QString::fromUtf8(error.what())));
    }
    catch (...)
    {
        DataFile::recordSerializationError(doc, tr("Could not preserve Carla state: native plugin exception"));
    }
}

void CarlaInstrument::refreshParameterModels()
{
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
    refreshParams();
#endif
}

PluginHost* CarlaInstrument::pluginHost()
{
    if (!m_pluginHost)
    {
        const auto library = m_hostResolver ? QString{} : QString::fromUtf8(carla_get_library_filename());
        m_pluginHost = std::make_unique<CarlaPluginHost>(fDescriptor, fHandle, library, m_hostResolver,
            [this](const CarlaPluginHost::LayoutChange& change, const std::function<void()>& edit)
            { editHostedLayout(change, edit); }, kIsPatchbay);
    }
    return m_pluginHost.get();
}

void CarlaInstrument::editHostedLayout(const CarlaPluginHost::LayoutChange& change, const std::function<void()>& edit)
{
#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
	if (m_updatingParams) { throw std::runtime_error("Cannot edit the Carla layout during parameter restoration"); }
	refreshParams();
	for (std::size_t i = 0; i < m_paramModels.size(); ++i)
	{
		if (!m_paramModels[i]->isAutomatedOrControlled() && !AutomationClip::isConnected(m_paramModels[i])) { continue; }
		if (!change.known)
		{
			throw std::runtime_error("Carla cannot establish the parameter layout needed to preserve existing automation");
		}
		if (i < change.oldToNew.size() && change.oldToNew[i] >= static_cast<int>(m_paramModels.size()))
		{
			throw std::runtime_error("This edit would move an automated Carla parameter outside the native automation window");
		}
	}
	{
		// Carla can synchronously announce reloads while its plugin order changes.
		// Keep identities intact until the native edit has reported success.
		QScopedValueRollback<bool> updating(m_updatingParams, true);
		edit();
		const auto count = fDescriptor->get_parameter_count ? fDescriptor->get_parameter_count(fHandle) : 0;
		if (count > static_cast<uint32_t>(std::numeric_limits<int>::max())) { throw std::runtime_error("Carla returned an invalid automation window"); }
		std::vector<CarlaParamFloatModel*> remapped(count, nullptr);
		for (std::size_t i = 0; i < m_paramModels.size(); ++i)
		{
			const int target = change.known && i < change.oldToNew.size() ? change.oldToNew[i] : -1;
			if (target >= 0 && static_cast<std::size_t>(target) < remapped.size())
			{
				if (remapped[target]) { throw std::runtime_error("Carla returned an ambiguous automation layout"); }
				remapped[target] = m_paramModels[i];
			}
			else if (target >= 0 && (m_paramModels[i]->isAutomatedOrControlled() || AutomationClip::isConnected(m_paramModels[i])))
			{
				throw std::runtime_error("Carla changed its automation window during a structural edit");
			}
		}
		for (auto* model : m_paramModels)
		{
			if (std::find(remapped.begin(), remapped.end(), model) == remapped.end()) { delete model; }
		}
		m_paramModels = std::move(remapped);
	}
	refreshParams();
#else
	Q_UNUSED(change)
	edit();
#endif
}

void CarlaInstrument::refreshParams(bool init)
{
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this, [this, init] { refreshParams(init); }, Qt::QueuedConnection);
		return;
	}
	if (!fHandle || !m_completerModel || m_updatingParams) { return; }
	QScopedValueRollback<bool> updating(m_updatingParams, true);
	m_paramGroupCount = 0;
	if (fDescriptor->get_parameter_count != nullptr &&
		fDescriptor->get_parameter_info  != nullptr &&
		fDescriptor->get_parameter_value != nullptr &&
		fDescriptor->set_parameter_value != nullptr)
	{
		QList<QString> completerData;
		QList<QString> groups; // used to count no. groups.

		uint32_t paramCount = fDescriptor->get_parameter_count(fHandle);
		while (m_paramModels.size() > paramCount)
		{
			delete m_paramModels.back();
			m_paramModels.pop_back();
		}
		m_paramModels.resize(paramCount, nullptr);
		for (uint32_t index = 0; index < paramCount; ++index)
		{
			if (m_paramModels[index]) { continue; }
			auto* model = new CarlaParamFloatModel(this);
			m_paramModels[index] = model;
			connect(model, &CarlaParamFloatModel::dataChanged,
				this, [this, model]
				{
					const auto found = std::find(m_paramModels.begin(), m_paramModels.end(), model);
					if (found != m_paramModels.end()) { paramModelChanged(static_cast<uint32_t>(found - m_paramModels.begin())); }
				}, Qt::DirectConnection);
		}
		for (uint32_t i=0; i < paramCount; ++i)
		{
			const NativeParameter* paramInfo(fDescriptor->get_parameter_info(fHandle, i));
			if (!paramInfo)
			{
				m_paramModels[i]->setEnabled(false);
				m_paramModels[i]->setOutput(true);
				continue;
			}

			m_paramModels[i]->setOutput((paramInfo->hints & NATIVE_PARAMETER_IS_OUTPUT));
			m_paramModels[i]->setEnabled((paramInfo->hints & NATIVE_PARAMETER_IS_ENABLED));
			m_paramModels[i]->setNativeHints(paramInfo->hints);
			QMap<int, QString> labels;
			if ((paramInfo->hints & NATIVE_PARAMETER_USES_SCALEPOINTS) && paramInfo->scalePoints)
			{
				for (uint32_t point = 0; point < paramInfo->scalePointCount; ++point)
				{
					const auto& choice = paramInfo->scalePoints[point];
					const double value = choice.value;
					if (choice.label && std::isfinite(value) && std::floor(value) == value
						&& value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max())
					{
						labels.insert(static_cast<int>(value), QString::fromUtf8(choice.label));
					}
				}
			}
			m_paramModels[i]->setValueLabels(labels);

			// Get parameter name
			QString name = "_NO_NAME_";
			if (paramInfo->name != nullptr)
			{
				name = paramInfo->name;
			}

			if (paramInfo->groupName != nullptr)
			{
				m_paramModels[i]->setGroupName(paramInfo->groupName);

				if (m_paramModels[i]->enabled() && !groups.contains(paramInfo->groupName))
				{
					groups.push_back(paramInfo->groupName);
					m_paramGroupCount++;
				}
				m_paramModels[i]->setGroupId(groups.indexOf(paramInfo->groupName));
			}
			else
			{
				m_paramModels[i]->setGroupName({});
				m_paramModels[i]->setGroupId(0);
			}

			completerData.push_back(name);

			m_paramModels[i]->setDisplayName(name);
			m_paramModels[i]->setRange(paramInfo->ranges.min,
										paramInfo->ranges.max,
				m_paramModels[i]->valueType() == AutomatableModel::ValueType::Number ? paramInfo->ranges.step : 1.f);
			const auto nativeValue = fDescriptor->get_parameter_value(fHandle, i);
			if (init || m_paramModels[i]->isOutput() || !m_paramModels[i]->isAutomatedOrControlled())
			{
				m_paramModels[i]->setValue(nativeValue, true);
			}

			// Load settings into model.
			if (init)
			{
				// Older Carla states have no LMMS knob settings. Their default
				// must come from the restored native state rather than zero.
				m_paramModels[i]->setInitValue(nativeValue);
				QString idStr = CARLA_SETTING_PREFIX + QString::number(i);
				m_paramModels[i]->loadSettings(m_settingsElem, idStr);
				if (!m_paramModels[i]->isWritable()) { m_paramModels[i]->setValue(nativeValue, true); }
				if (m_paramModels[i]->enabled() && !m_paramModels[i]->isOutput()
					&& m_paramModels[i]->value() != nativeValue)
				{
					fDescriptor->set_parameter_value(fHandle, i, m_paramModels[i]->value());
				}
			}
		}
		// Set completer data
		m_completerModel->setStringList(completerData);
	}
	emit paramsUpdated();
}

void CarlaInstrument::clearParamModels()
{
	//Delete the models, this also disconnects all connections (automation and controller connections)
	for (uint32_t index = 0; index < m_paramModels.size(); ++index)
	{
		delete m_paramModels[index];
	}

	//Clear the list
	m_paramModels.clear();

	m_paramGroupCount = 0;
}

void CarlaInstrument::paramModelChanged(uint32_t index)
{ // Update Carla param (LMMS -> Carla)
	if (m_updatingParams || !fHandle || index >= m_paramModels.size()) { return; }
	if (m_paramModels[index]->enabled() && !m_paramModels[index]->isOutput())
	{
		if (fDescriptor->set_parameter_value != nullptr)
		{
			fDescriptor->set_parameter_value(fHandle, index, m_paramModels[index]->value());

		}

		// TODO? Shouldn't Carla be doing this?
		if (getGUI() && fDescriptor->ui_set_parameter_value != nullptr)
		{
			fDescriptor->ui_set_parameter_value(fHandle, index, m_paramModels[index]->value());
		}
	}
}

void CarlaInstrument::updateParamModel(uint32_t index)
{ // Called on param changed (Carla -> LMMS)
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this, [this, index] { updateParamModel(index); }, Qt::QueuedConnection);
		return;
	}
	QScopedValueRollback<bool> updating(m_updatingParams, true);
	if (fHandle && index < m_paramModels.size() && fDescriptor->get_parameter_value != nullptr)
	{
		m_paramModels[index]->setValue(
			fDescriptor->get_parameter_value(fHandle, index)
		);
	}
}

void CarlaInstrument::loadSettings(const QDomElement& elem)
{
    if (fHandle == nullptr || fDescriptor->set_state == nullptr)
    {
        collectErrorForUI(tr("Native Carla state restoration is unavailable"));
        return;
    }

    QDomElement state;
    for (auto child = elem.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
    {
        if (!isCarlaState(child)) { continue; }
        if (!state.isNull())
        {
            collectErrorForUI(tr("Carla settings contain more than one native project state"));
            return;
        }
        state = child;
    }
    if (state.isNull())
    {
        collectErrorForUI(tr("Carla settings have no native project or preset state"));
        return;
    }

    QDomDocument carlaDoc;
    carlaDoc.appendChild(carlaDoc.importNode(state, true));

    try
    {
        // set_state may synchronously announce parameter changes. Restore the
        // native state completely before exposing the resulting model values.
        {
            QScopedValueRollback<bool> updating(m_updatingParams, true);
            fDescriptor->set_state(fHandle, carlaDoc.toString(0).toUtf8().constData());
        }

#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
        // Store to load parameter knobs settings when added.
        m_settingsElem = elem;
        refreshParams(true);
#endif
        m_stateError.clear();
    }
    catch (const std::exception& error)
    {
        m_stateError = tr("Native Carla state restoration failed: %1").arg(QString::fromUtf8(error.what()));
        collectErrorForUI(m_stateError);
    }
    catch (...)
    {
        m_stateError = tr("Native Carla state restoration failed with a plugin exception");
        collectErrorForUI(m_stateError);
    }
}

void CarlaInstrument::play(SampleFrame* workingBuffer)
{
    const uint bufsize = Engine::audioEngine()->framesPerPeriod();

	zeroSampleFrames(workingBuffer, bufsize);

    if (fHandle == nullptr)
    {
        return;
    }

    // set time info
    Song * const s = Engine::getSong();
    fTimeInfo.playing  = s->isPlaying();
    fTimeInfo.frame    = s->getPlayPos(s->playMode()).frames(Engine::framesPerTick());
    fTimeInfo.usecs    = s->getMilliseconds()*1000;
    fTimeInfo.bbt.bar  = s->getBars() + 1;
    fTimeInfo.bbt.beat = s->getBeat() + 1;
    fTimeInfo.bbt.tick = s->getBeatTicks();
    fTimeInfo.bbt.barStartTick   = ticksPerBeat*s->getTimeSigModel().getNumerator()*s->getBars();
    fTimeInfo.bbt.beatsPerBar    = s->getTimeSigModel().getNumerator();
    fTimeInfo.bbt.beatType       = s->getTimeSigModel().getDenominator();
    fTimeInfo.bbt.ticksPerBeat   = ticksPerBeat;
    fTimeInfo.bbt.beatsPerMinute = s->getTempo();

#ifndef _MSC_VER
    float buf1[bufsize];
    float buf2[bufsize];
#else
    float *buf1 = static_cast<float *>(_alloca(bufsize * sizeof(float)));
    float *buf2 = static_cast<float *>(_alloca(bufsize * sizeof(float)));
#endif

    float* rBuf[] = { buf1, buf2 };
    std::memset(buf1, 0, sizeof(float)*bufsize);
    std::memset(buf2, 0, sizeof(float)*bufsize);

    auto* outputPort = instrumentTrack()->midiPort();
    MidiOutputBlock midiOutput{this, bufsize, 0,
        supportsMidiOutput() && !handleIsOffline() && outputPort->acceptsOutput(MidiPort::OutputSource::Instrument)};
    try
    {
        const QMutexLocker ml(&fMutex);
        const auto clearInput = qScopeGuard([this] { fMidiEventCount = 0; });
        const bool offline = handleIsOffline();
        if (offline != m_lastOffline)
        {
            m_lastOffline = offline;
            if (fDescriptor->dispatcher)
            {
                fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_OFFLINE_CHANGED, 0, offline, nullptr, 0.0f);
            }
        }
        const QScopedValueRollback capture(currentMidiOutput, &midiOutput);
// TODO FIXME this is just here so it compiles.
// https://github.com/falkTX/Carla/blob/8bceb9ed173a10b29038f8abb4383710c0e497c1/source/includes/CarlaNative.h
//     FIXME for v3.0, use const for the input buffer
#if CARLA_VERSION_HEX >= CARLA_VERSION_HEX_3
        fDescriptor->process(fHandle, (const float**)rBuf, rBuf, bufsize, fMidiEvents, fMidiEventCount);
#else
        fDescriptor->process(fHandle, rBuf, rBuf, bufsize, fMidiEvents, fMidiEventCount);
#endif
    }
    catch (...)
    {
        if (midiOutput.enabled) { try { outputPort->releaseOutput(); } catch (...) {} }
        throw;
    }
    if (midiOutput.failed)
    {
        outputPort->releaseOutput();
        throw std::runtime_error("Carla MIDI output was invalid or exceeded the 512-event block limit");
    }
    // Stable in-place insertion sort avoids allocating on the audio thread.
    for (uint32_t i = 1; i < midiOutput.count; ++i)
    {
        const auto event = midiOutput.events[i]; auto destination = i;
        while (destination > 0 && midiOutput.events[destination - 1].time > event.time)
        {
            midiOutput.events[destination] = midiOutput.events[destination - 1]; --destination;
        }
        midiOutput.events[destination] = event;
    }
    try
    {
        for (uint32_t i = 0; i < midiOutput.count; ++i)
        {
            const auto& event = midiOutput.events[i];
            const auto decoded = readFromByteSeq(event.data, event.size);
            outputPort->processOutEvent(*decoded, TimePos::fromFrames(event.time, Engine::framesPerTick()), MidiPort::OutputSource::Instrument);
        }
    }
    catch (...)
    {
        try { outputPort->releaseOutput(); } catch (...) {}
        throw;
    }

    for (uint i=0; i < bufsize; ++i)
    {
        workingBuffer[i][0] = buf1[i];
        workingBuffer[i][1] = buf2[i];
    }
}

bool CarlaInstrument::handleMidiEvent(const MidiEvent& event, const TimePos&, f_cnt_t offset)
{
    const QMutexLocker ml(&fMutex);

    if (!fHandle || offset >= Engine::audioEngine()->framesPerPeriod() || fMidiEventCount >= kMaxMidiEvents)
        return false;

    NativeMidiEvent& nEvent(fMidiEvents[fMidiEventCount++]);
    std::memset(&nEvent, 0, sizeof(NativeMidiEvent));

    nEvent.port    = 0;
    nEvent.time    = offset;
    std::size_t written = writeToByteSeq(event, nEvent.data, sizeof(NativeMidiEvent::data));
    if (written) { nEvent.size = written; return true; }
    --fMidiEventCount; return false;
}

gui::PluginView* CarlaInstrument::instantiateView(QWidget* parent)
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!getGUI() || !app || QThread::currentThread() != app->thread()) { return nullptr; }
// Disable plugin focus per https://bugreports.qt.io/browse/QTBUG-30181
#ifndef CARLA_OS_MAC
    if (QWidget* const window = parent ? parent->window() : nullptr)
        // TODO: Remove cast; Only needed for Qt4
        fHost.uiParentId = (uintptr_t)window->winId();
    else
#endif
        fHost.uiParentId = 0;

    std::free((char*)fHost.uiName);

    // TODO - get plugin instance name
    //fHost.uiName = strdup(parent->windowTitle().toUtf8().constData());
    fHost.uiName = strdup(kIsPatchbay ? "CarlaPatchbay-LMMS" : "CarlaRack-LMMS");

    return new gui::CarlaInstrumentView(this, parent);
}

void CarlaInstrument::sampleRateChanged()
{
    if (fHandle && fDescriptor->dispatcher)
    {
        fDescriptor->dispatcher(fHandle, NATIVE_PLUGIN_OPCODE_SAMPLE_RATE_CHANGED, 0, 0, nullptr, handleGetSampleRate());
    }
}

// -------------------------------------------------------------------

namespace gui
{

CarlaInstrumentView::CarlaInstrumentView(CarlaInstrument* const instrument, QWidget* const parent)
    : InstrumentViewFixedSize(instrument, parent),
      fHandle(instrument->fHandle),
      fDescriptor(instrument->fDescriptor),
      fTimerId(fHandle != nullptr && fDescriptor->ui_idle != nullptr ? startTimer(30) : 0),
      m_carlaInstrument(instrument),
      m_parent(parent),
      m_paramsSubWindow(nullptr),
      m_paramsView(nullptr)
{
    setAutoFillBackground(true);

    QPalette pal;
    pal.setBrush(backgroundRole(), instrument->kIsPatchbay ? PLUGIN_NAME::getIconPixmap("artwork-patchbay") : PLUGIN_NAME::getIconPixmap("artwork-rack"));
    setPalette(pal);

	auto l = new QHBoxLayout(this);
	l->setContentsMargins( 20, 180, 10, 10 );
    l->setSpacing(3);
    l->setAlignment(Qt::AlignTop);

    // Show GUI button
    m_toggleUIButton = new QPushButton( tr( "Show GUI" ), this );
    m_toggleUIButton->setCheckable( true );
    m_toggleUIButton->setChecked( false );
    m_toggleUIButton->setIcon( embed::getIconPixmap( "zoom" ) );
    m_toggleUIButton->setFont(adjustedToPixelSize(m_toggleUIButton->font(), SMALL_FONT_SIZE));
    connect( m_toggleUIButton, SIGNAL( clicked(bool) ), this, SLOT( toggleUI( bool ) ) );

    m_toggleUIButton->setToolTip(
            tr("Click here to show or hide the graphical user interface (GUI) of Carla."));

    // Open params sub window button
    m_toggleParamsWindowButton = new QPushButton(tr("Params"), this);
    m_toggleParamsWindowButton->setIcon(embed::getIconPixmap("controller"));
    m_toggleParamsWindowButton->setCheckable(true);
    m_toggleParamsWindowButton->setFont(adjustedToPixelSize(m_toggleParamsWindowButton->font(), SMALL_FONT_SIZE));
#if CARLA_VERSION_HEX < CARLA_MIN_PARAM_VERSION
    m_toggleParamsWindowButton->setEnabled(false);
    m_toggleParamsWindowButton->setToolTip(tr("Available from Carla version 2.1 and up."));
#else
    connect(m_toggleParamsWindowButton, SIGNAL(clicked(bool)), this, SLOT(toggleParamsWindow()));
#endif

    // Add widgets to layout
    l->addWidget( m_toggleUIButton );
    l->addWidget(m_toggleParamsWindowButton);

    // Connect signals
    connect(m_toggleUIButton, SIGNAL(clicked(bool)), this, SLOT(toggleUI(bool)));
    connect(instrument, SIGNAL(uiClosed()), this, SLOT(uiClosed()));
}

CarlaInstrumentView::~CarlaInstrumentView()
{
    if (m_toggleUIButton->isChecked())
    {
        toggleUI(false);
    }

#if CARLA_VERSION_HEX >= CARLA_MIN_PARAM_VERSION
    if (m_paramsView)
    {
        delete m_paramsView;
        m_paramsView = nullptr;
    }
#endif
}

void CarlaInstrumentView::toggleUI(bool visible)
{
    if (fHandle != nullptr && fDescriptor->ui_show != nullptr) {
// TODO: remove when fixed upstream
// change working path to location of carla.dll to avoid conflict with lmms
#if defined(CARLA_OS_WIN32) || defined(CARLA_OS_WIN64)
        if (visible) {
            QString backupDir = QDir::currentPath();
            QDir::setCurrent(carla_get_library_folder());
            fDescriptor->ui_show(fHandle, true);
            QDir::setCurrent(backupDir);
            return;
        }
#endif
        fDescriptor->ui_show(fHandle, visible);
    }
}

void CarlaInstrumentView::uiClosed()
{
    m_toggleUIButton->setChecked(false);
}

void CarlaInstrumentView::modelChanged()
{
}

void CarlaInstrumentView::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == fTimerId)
        fDescriptor->ui_idle(fHandle);

    InstrumentView::timerEvent(event);
}

void CarlaInstrumentView::toggleParamsWindow()
{
	if (!m_paramsSubWindow)
	{
		m_paramsView = new CarlaParamsView(this, m_parent);
		connect(m_paramsSubWindow, SIGNAL(uiClosed()), this, SLOT(paramsUiClosed()));
	}
	else
	{
		if (m_paramsSubWindow->isVisible())
		{
			m_paramsSubWindow->hide();
		}
		else
		{
			m_paramsSubWindow->show();
		}
	}
}

void CarlaInstrumentView::paramsUiClosed()
{
	m_toggleParamsWindowButton->setChecked(false);
}

// -------------------------------------------------------------------

CarlaParamsView::CarlaParamsView(CarlaInstrumentView* const instrumentView, QWidget* const parent)
	: InstrumentView(instrumentView->m_carlaInstrument, parent),
	m_carlaInstrument(instrumentView->m_carlaInstrument),
	m_carlaInstrumentView(instrumentView),
	m_maxColumns(6),
	m_curColumn(0),
	m_curRow(0),
	m_curOutColumn(0),
	m_curOutRow(0)
{
	auto centralWidget = new QWidget(this);
	auto verticalLayout = new QVBoxLayout(centralWidget);

	// -- Toolbar
	m_toolBarLayout = new QHBoxLayout();

	// Toolbar widgets
	QSizePolicy sizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	sizePolicy.setHorizontalStretch(0);
	sizePolicy.setVerticalStretch(0);

	// Params filter line edit
	m_paramsFilterLineEdit = new QLineEdit(this);
	m_paramsFilterLineEdit->setPlaceholderText(tr("Search.."));
	auto* completer = new QCompleter(m_carlaInstrument->m_completerModel, m_paramsFilterLineEdit);
	completer->setCaseSensitivity(Qt::CaseInsensitive);
	completer->setCompletionMode(QCompleter::PopupCompletion);
	m_paramsFilterLineEdit->setCompleter(completer);

	// Clear filter line edit button
	m_clearFilterButton = new QPushButton(tr(""), this);
	m_clearFilterButton->setIcon(embed::getIconPixmap("edit_erase"));
	m_clearFilterButton->setToolTip(tr("Clear filter text"));
	sizePolicy.setHeightForWidth(m_clearFilterButton->sizePolicy().hasHeightForWidth());
	m_clearFilterButton->setSizePolicy(sizePolicy);

	// Show automated only button
	m_automatedOnlyButton = new QPushButton(tr(""), this);
	m_automatedOnlyButton->setIcon(embed::getIconPixmap("automation"));
	m_automatedOnlyButton->setToolTip(
				tr("Only show knobs with a connection."));
	m_automatedOnlyButton->setCheckable(true);
	sizePolicy.setHeightForWidth(m_automatedOnlyButton->sizePolicy().hasHeightForWidth());
	m_automatedOnlyButton->setSizePolicy(sizePolicy);

	// Group name combobox
	m_groupFilterCombo = new QComboBox(this);
	m_groupFilterModel = new QStringListModel(this);
	m_groupFilterCombo->setModel(m_groupFilterModel);

	// Add stuff to toolbar
	m_toolBarLayout->addWidget(m_paramsFilterLineEdit);
	m_toolBarLayout->addWidget(m_clearFilterButton);
	m_toolBarLayout->addWidget(m_automatedOnlyButton);
	m_toolBarLayout->addWidget(m_groupFilterCombo);

	// -- Input params
	auto inputFrame = new QFrame(this);
	auto inputLayout = new QVBoxLayout(inputFrame);
	auto inputLabel = new QLabel("Input parameters", inputFrame);

	m_inputScrollArea = new QScrollArea(inputFrame);
	m_inputScrollAreaWidgetContent = new QWidget();
	m_inputScrollAreaLayout = new QGridLayout(m_inputScrollAreaWidgetContent);

	m_inputScrollAreaWidgetContent->setLayout(m_inputScrollAreaLayout);
	m_inputScrollAreaWidgetContent->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);

	m_inputScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	m_inputScrollArea->setWidget(m_inputScrollAreaWidgetContent);
	m_inputScrollArea->setWidgetResizable(true);
	m_inputScrollArea->setFrameShadow(QFrame::Plain);
	m_inputScrollArea->setFrameShape(QFrame::NoFrame);

	m_inputScrollAreaLayout->setContentsMargins(3, 3, 3, 3);
	m_inputScrollAreaLayout->setVerticalSpacing(12);
	m_inputScrollAreaLayout->setHorizontalSpacing(6);
	m_inputScrollAreaLayout->setColumnStretch(m_maxColumns, 1);

	inputLayout->addWidget(inputLabel);
	inputLayout->addWidget(m_inputScrollArea);

	// -- Output params
	auto outputFrame = new QFrame(this);
	auto outputLayout = new QVBoxLayout(outputFrame);
	auto outputLabel = new QLabel("Output parameters", outputFrame);

	m_outputScrollArea = new QScrollArea(outputFrame);
	m_outputScrollAreaWidgetContent = new QWidget();
	m_outputScrollAreaLayout = new QGridLayout(m_outputScrollAreaWidgetContent);

	m_outputScrollAreaWidgetContent->setLayout(m_outputScrollAreaLayout);
	m_outputScrollAreaWidgetContent->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);

	m_outputScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	m_outputScrollArea->setWidget(m_outputScrollAreaWidgetContent);
	m_outputScrollArea->setWidgetResizable(true);
	m_outputScrollArea->setFrameShadow(QFrame::Plain);
	m_outputScrollArea->setFrameShape(QFrame::NoFrame);

	m_outputScrollAreaLayout->setContentsMargins(3, 28, 3, 3);
	m_outputScrollAreaLayout->setVerticalSpacing(12);
	m_outputScrollAreaLayout->setHorizontalSpacing(6);
	m_outputScrollAreaLayout->setColumnStretch(m_maxColumns, 1);

	outputLayout->addWidget(outputLabel);
	outputLayout->addWidget(m_outputScrollArea);

	// -- QSplitter
	auto splitter = new QSplitter(Qt::Vertical, this);

	// -- Add layout and widgets.
	verticalLayout->addLayout(m_toolBarLayout);
	splitter->addWidget(inputFrame);
	splitter->addWidget(outputFrame);
	verticalLayout->addWidget(splitter);

	// -- Sub window
	auto win = new CarlaParamsSubWindow(getGUI()->mainWindow()->workspace()->viewport(),
		Qt::SubWindow | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint);
	m_carlaInstrumentView->m_paramsSubWindow = getGUI()->mainWindow()->workspace()->addSubWindow(win);
	m_carlaInstrumentView->m_paramsSubWindow->setSizePolicy(
		QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
	m_carlaInstrumentView->m_paramsSubWindow->setMinimumHeight(200);
	m_carlaInstrumentView->m_paramsSubWindow->setMinimumWidth(200);
	m_carlaInstrumentView->m_paramsSubWindow->resize(600, 400);
	m_carlaInstrumentView->m_paramsSubWindow->setWidget(centralWidget);
	centralWidget->setWindowTitle(m_carlaInstrument->instrumentTrack()->name() + tr(" - Parameters"));

	// -- Connect signals
	connect(m_carlaInstrumentView->m_paramsSubWindow, SIGNAL(resized()), this, SLOT(windowResized()));
	connect(m_paramsFilterLineEdit, SIGNAL(textChanged(const QString)), this, SLOT(filterKnobs()));
	connect(m_clearFilterButton, SIGNAL(clicked(bool)), this, SLOT(clearFilterText()));
	connect(m_automatedOnlyButton, SIGNAL(toggled(bool)), this, SLOT(filterKnobs()));
	connect(m_groupFilterCombo, SIGNAL(currentTextChanged(const QString)), this, SLOT(filterKnobs()));
	connect(m_carlaInstrument, SIGNAL(paramsUpdated()), this, SLOT(refreshKnobs()));

	m_carlaInstrumentView->m_paramsSubWindow->show(); // Show the subwindow

	// Add knobs if there are any already.
	// Call this after show() so the m_inputScrollArea->width() is set properly.
	refreshKnobs(); // Will trigger filterKnobs() due m_groupFilterCombo->setCurrentIndex(0)
}

CarlaParamsView::~CarlaParamsView()
{
	// Close and delete m_paramsSubWindow
	if (m_carlaInstrumentView->m_paramsSubWindow)
	{
		m_carlaInstrumentView->m_paramsSubWindow->setAttribute(Qt::WA_DeleteOnClose);
		m_carlaInstrumentView->m_paramsSubWindow->close();

		delete m_carlaInstrumentView->m_paramsSubWindow;
		m_carlaInstrumentView->m_paramsSubWindow = nullptr;
	}

	m_carlaInstrumentView->m_paramsView = nullptr;

	// Clear models
	if (!m_carlaInstrument->m_paramModels.empty())
	{
		m_carlaInstrument->clearParamModels();
	}
}

void CarlaParamsView::clearFilterText()
{
	m_paramsFilterLineEdit->setText("");
}

void CarlaParamsView::filterKnobs()
{
	clearKnobs(); // Remove all knobs from the layout.

	if (!m_carlaInstrument->m_paramGroupCount)
	{
		return;
	}

	// Calc how many knobs will fit horizontal in the params window.
	uint16_t maxKnobWidth = m_maxKnobWidthPerGroup[m_groupFilterCombo->currentIndex()];
	maxKnobWidth += m_inputScrollAreaLayout->spacing();
	if (!maxKnobWidth)
	{
		// Prevent possible division by zero.
		return;
	}
	m_maxColumns = m_inputScrollArea->width() / maxKnobWidth;

	QString text = m_paramsFilterLineEdit->text();
	for (uint32_t i = 0; i < m_knobs.size(); ++i)
	{
		// Don't show disabled (unused) knobs.
		if (!m_carlaInstrument->m_paramModels[i]->enabled())
		{
			continue;
		}

		// Filter on automation only
		if (m_automatedOnlyButton->isChecked())
		{
			if (! m_carlaInstrument->m_paramModels[i]->isAutomatedOrControlled())
			{
				continue;
			}
		}

		// Filter on group name
		if (m_groupFilterCombo->currentText() != m_carlaInstrument->m_paramModels[i]->groupName())
		{
			continue;
		}

		// Filter on text
		if (text != "")
		{
			if (m_knobs[i]->objectName().contains(text, Qt::CaseInsensitive))
			{	
				addKnob(i);
			}
		}
		else
		{
			addKnob(i);
		}
	}

	// Add spacer so all knobs go to top
	auto verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
	m_inputScrollAreaLayout->addItem(verticalSpacer, m_curRow+1, 0, 1, 1);
}

void CarlaParamsView::refreshKnobs()
{
	// Make sure all the knobs are deleted.
	for (uint32_t i = 0; i < m_knobs.size(); ++i)
	{
		delete m_knobs[i]; // Delete knob widgets itself.
	}
	m_knobs.clear(); // Clear the pointer list.

	// Reset position data.
	m_curColumn = 0;
	m_curRow = 0;

	m_curOutColumn = 0;
	m_curOutRow = 0;

	// Clear max knob width per group
	m_maxKnobWidthPerGroup.clear();
	m_maxKnobWidthPerGroup.reserve(m_carlaInstrument->m_paramGroupCount);
	for (uint8_t i = 0; i < m_carlaInstrument->m_paramGroupCount; i++)
	{
		m_maxKnobWidthPerGroup[i] = 0;
	}

	if (m_carlaInstrument->m_paramModels.empty()) { return; }

	// Make room in QList m_knobs
	m_knobs.reserve(m_carlaInstrument->m_paramModels.size());

	QStringList groupNameList;
	groupNameList.reserve(m_carlaInstrument->m_paramGroupCount);

	for (const auto currentParamModel : m_carlaInstrument->m_paramModels)
	{
		bool enabled = currentParamModel->enabled();
		const QString name = currentParamModel->displayName();

		auto currentKnob = new Knob(KnobType::Dark28, name, m_inputScrollAreaWidgetContent, Knob::LabelRendering::LegacyFixedFontSize);
		currentKnob->setHintText(name, "");
		currentKnob->setObjectName(name); // this is being used for filtering the knobs.

		// Set the newly created model to the knob.
		currentKnob->setModel(currentParamModel);
		currentKnob->setEnabled(enabled);

		m_knobs.push_back(currentKnob);

		if (enabled)
		{
			// Collect group names
			if (!groupNameList.contains(currentParamModel->groupName()))
			{
				groupNameList.append(currentParamModel->groupName());
			}

			// Store biggest knob width per group (so we can calc how many knobs we can fit horizontally)
			auto & maxGroupWidth = m_maxKnobWidthPerGroup[currentParamModel->groupId()];
			maxGroupWidth = std::max(maxGroupWidth, static_cast<uint16_t>(currentKnob->width()));
		}
	}

	// Set new list with group names to the model
	if (!groupNameList.count())
	{
		groupNameList.append("No params");
	}
	m_groupFilterModel->setStringList(groupNameList);
	m_groupFilterCombo->setCurrentIndex(0);
}


void CarlaParamsView::windowResized()
{
	filterKnobs();
}


void CarlaParamsView::addKnob(uint32_t index)
{
	bool output = m_carlaInstrument->m_paramModels[index]->isOutput();
	if (output)
	{
		m_outputScrollAreaLayout->addWidget(
			m_knobs[index], m_curOutRow, m_curOutColumn, Qt::AlignHCenter | Qt::AlignTop);
		m_knobs[index]->setEnabled(false); // We should not be able to adjust output.
		m_knobs[index]->show();
		if (m_curOutColumn < m_maxColumns - 1)
		{
			m_curOutColumn++;
		}
		else
		{
			m_curOutColumn = 0;
			m_curOutRow++;
		}
	}
	else
	{
		// Add the new knob to layout
		m_inputScrollAreaLayout->addWidget(m_knobs[index], m_curRow, m_curColumn, Qt::AlignHCenter | Qt::AlignTop);
		m_inputScrollAreaLayout->setColumnStretch(m_curColumn, 1);

		// Chances that we did close() on the widget is big, so show it.
		m_knobs[index]->show();

		// Keep track of current column and row index.
		if (m_curColumn < m_maxColumns - 1)
		{
			m_curColumn++;
		}
		else
		{
			m_curColumn = 0;
			m_curRow++;
		}
	}
}
void CarlaParamsView::clearKnobs()
{
	// Remove knobs from layout.
	for (uint16_t i = 0; i < m_knobs.size(); ++i)
	{
		m_knobs[i]->close();
	}

	// Remove spacers
	for (int16_t i=m_inputScrollAreaLayout->count() - 1; i > 0; i--)
	{
		auto item = m_inputScrollAreaLayout->takeAt(i);
		if (item->widget()) {continue;}
		delete item;
	}
	for (int16_t i=m_outputScrollAreaLayout->count() - 1; i > 0; i--)
	{
		auto item = m_outputScrollAreaLayout->takeAt(i);
		if (item->widget()) {continue;}
		delete item;
	}

	// Reset position data.
	m_curColumn = 0;
	m_curRow = 0;

	m_curOutColumn = 0;
	m_curOutRow = 0;
}


} // namespace gui

} // namespace lmms
