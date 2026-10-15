/*
 * VstPlugin.cpp - implementation of VstPlugin class
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "VstPlugin.h"
#include "NativeValueNotification.h"
#include <exception>
#include "VstParameterModels.h"
#include "VstParameterWire.h"
#include "VstParameterTransfer.h"
#include "VstPresetStorage.h"
#include "VstPeFile.h"
#include "FileResourceIO.h"

#include "communication.h"

#include <QDebug>
#include <QPointer>
#include <QDir>
#include <QDomElement>
#include <QFileInfo>
#include <QLocale>
#include <QTemporaryDir>
#include <QFile>
#include <QTimerEvent>
#include <QThread>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(LMMS_BUILD_LINUX) && (QT_VERSION < QT_VERSION_CHECK(6,0,0))
#	include <QX11Info>
#	include <X11EmbedContainer.h>
#endif

#include <QWindow>


#ifdef LMMS_BUILD_WIN32
#	include <windows.h>
#	include <QLayout>
#endif

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "FileDialog.h"
#include "FontHelper.h"
#include "GuiApplication.h"
#include "LocaleHelper.h"
#include "MainWindow.h"
#include "PathUtil.h"
#include "SimpleTextFloat.h"
#include "Song.h"

#ifdef LMMS_BUILD_LINUX
#	include <X11/Xlib.h>
#endif

namespace lmms
{

enum class ExecutableType
{
	Unknown, Win32, Win64, Linux64,
};

VstPlugin::VstPlugin( const QString & _plugin ) :
	m_plugin( PathUtil::toAbsolute(_plugin) ),
	m_pluginWindowID( 0 ),
	m_embedMethod( (gui::getGUI() != nullptr)
			? ConfigManager::inst()->vstEmbedMethod()
			: "headless" ),
	m_version( 0 ),
	m_currentProgram()
{
	setSplittedChannels( true );

	auto pluginType = ExecutableType::Unknown;
#ifdef LMMS_BUILD_LINUX
	QFileInfo fi(m_plugin);
	if (fi.suffix() == "so")
	{
		pluginType = ExecutableType::Linux64;
	}
	else
#endif
	{
		try {
			const auto machine = readVstPeMachine(m_plugin);
			switch (machine)
			{
			case 0x8664:
				pluginType = ExecutableType::Win64;
				break;
			case 0x014c:
				pluginType = ExecutableType::Win32;
				break;
			default:
				qWarning() << "Unknown PE machine type"
					<< QString::number(machine, 16);
				break;
			}
		} catch (std::runtime_error& e) {
			qCritical() << "Error while determining PE file's machine type: " << e.what();
		}
	}

	switch(pluginType)
	{
	case ExecutableType::Win64:
		tryLoad( REMOTE_VST_PLUGIN_FILEPATH_64 ); // Default: RemoteVstPlugin64
		break;
	case ExecutableType::Win32:
		tryLoad( REMOTE_VST_PLUGIN_FILEPATH_32 ); // Default: 32/RemoteVstPlugin32
		break;
#ifdef LMMS_BUILD_LINUX
	case ExecutableType::Linux64:
		tryLoad( NATIVE_LINUX_REMOTE_VST_PLUGIN_FILEPATH_64 ); // Default: NativeLinuxRemoteVstPlugin32
		break;
#endif
	default:
		m_failed = true;
		return;
	}

	setTempo( Engine::getSong()->getTempo() );

	connect(Engine::getSong(), &Song::tempoChanged, this, [this](bpm_t tempo) {
		try { setTempo(tempo); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("VST tempo update failed outside a native value notification frame"); } }
	}, Qt::DirectConnection);
	const QPointer<AudioEngine> engine(Engine::audioEngine());
	connect(engine.data(), &AudioEngine::sampleRateChanged, this, [this, engine] {
		try { updateSampleRate(); }
		catch (...)
		{
			if (engine && !engine->reportDeviceNotificationFailure(std::current_exception()))
			{ qWarning("VST rate update failed outside a native device notification frame"); }
		}
	});

	// update once per second
	m_idleTimer.start( 1000 );
	connect( &m_idleTimer, SIGNAL( timeout() ),
				this, SLOT( idleUpdate() ) );
}




VstPlugin::~VstPlugin()
{
	delete m_pluginWidget;
}




void VstPlugin::tryLoad( const QString &remoteVstPluginExecutable )
{
	init( remoteVstPluginExecutable, false, {m_embedMethod} );

	waitForHostInfoGotten();
	if( failed() )
	{
		return;
	}

	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });

	VstHostLanguage hlang = VstHostLanguage::English;
	switch( QLocale::system().language() )
	{
		case QLocale::French: hlang = VstHostLanguage::French; break;
		case QLocale::German: hlang = VstHostLanguage::German; break;
		case QLocale::Italian: hlang = VstHostLanguage::Italian; break;
		case QLocale::Japanese: hlang = VstHostLanguage::Japanese; break;
		case QLocale::Korean: hlang = VstHostLanguage::Korean; break;
		case QLocale::Spanish: hlang = VstHostLanguage::Spanish; break;
		default: break;
	}
	sendMessage( message( IdVstSetLanguage ).addInt( static_cast<int>(hlang) ) );
	sendMessage( message( IdVstLoadPlugin ).addString( QSTR_TO_STDSTR( m_plugin ) ) );

	waitForInitDone();

}




void VstPlugin::loadSettings( const QDomElement & _this )
{
	bool validCount = false;
	const int num_params = _this.attribute("numparams", "0").toInt(&validCount);
	if (!_this.hasAttribute("chunk") && _this.hasAttribute("numparams")
		&& (!validCount || num_params < 0 || num_params != m_parameterCount))
	{ throw std::runtime_error("Saved VST parameter count differs from the loaded plugin"); }
	const bool hasNativePreset = _this.hasAttribute("nativepreset");
	if (hasNativePreset && _this.hasAttribute("chunk")) { throw std::runtime_error("Saved VST state has conflicting native encodings"); }
	QByteArray nativePreset;
	if (hasNativePreset)
	{
		nativePreset = decodeVstPresetState(_this.attribute("nativepreset"));
	}
	// Validate persisted bytes before selecting a program or calling the plugin.
	const auto chunk = _this.hasAttribute("chunk") ? decodeVstChunkState(_this.attribute("chunk")) : QByteArray{};
	if (!hasNativePreset && _this.hasAttribute("program"))
	{
		// Zero-program plugins historically serialized program=0.
		if (programPage(0, 1).total > 0)
		{
			bool valid = false;
			const int index = _this.attribute("program").toInt(&valid);
			if (!valid) { throw std::runtime_error("Invalid saved VST program index"); }
			setProgram(index);
		}
	}

	// if it exists try to load settings chunk
	if( _this.hasAttribute( "chunk" ) )
	{
		loadChunk(chunk);
	}
	else if (hasNativePreset) { loadNativePresetState(nativePreset); }
	else if( num_params > 0 )
	{
		// no chunk, restore individual parameters
		QMap<QString, QString> dump;
		for( int i = 0; i < num_params; ++i )
		{
			const QString key = "param" +
						QString::number( i );
			dump[key] = _this.attribute( key );
		}
		setParameterDump( dump );
	}
}




void VstPlugin::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if ( m_embedMethod != "none" )
	{
		if( pluginWidget() != nullptr )
		{
			_this.setAttribute( "guivisible", pluginWidget()->isVisible() );
		}
	}
	else
	{
		int visible = isUIVisible();
		if ( visible != -1 )
		{
			_this.setAttribute( "guivisible", visible );
		}
	}

	// try to save all settings in a chunk
	QByteArray chunk = saveChunk();
	if( !chunk.isEmpty() )
	{
		_this.removeAttribute("nativepreset");
		_this.setAttribute( "chunk", QString( chunk.toBase64() ) );
	}
	else
	{
		// Preserve the whole native bank for recovery, while retaining the legacy
		// current-program fields for older LMMS versions. Zero-program effects use FXP.
		_this.removeAttribute("chunk");
		_this.setAttribute("nativepreset", QString::fromLatin1(saveNativePresetState().toBase64()));
		// Older readers still restore the selected program's individual parameters.
		const QMap<QString, QString> & dump = parameterDump();
		_this.setAttribute( "numparams", dump.size() );
		for( QMap<QString, QString>::const_iterator it = dump.begin();
							it != dump.end(); ++it )
		{
			_this.setAttribute( it.key(), it.value() );
		}
	}

	_this.setAttribute( "program", currentProgram() );
}

void VstPlugin::toggleUI()
{
	if ( m_embedMethod == "none" )
	{
		RemotePlugin::toggleUI();
	}
	else if (pluginWidget())
	{
		toggleEditorVisibility();
	}
}




void VstPlugin::setTempo( bpm_t _bpm )
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage( message( IdVstSetTempo ).addInt( _bpm ) );
}




void VstPlugin::updateSampleRate()
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage( message( IdSampleRateInformation ).
			addInt( Engine::audioEngine()->outputSampleRate() ) );
	waitForMessage( IdInformationUpdated, true );
}




int VstPlugin::currentProgram()
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage( message( IdVstCurrentProgram ) );
	if (waitForMessage(IdVstCurrentProgram, true).id != IdVstCurrentProgram || failed())
	{ throw std::runtime_error("VST worker did not report its current program"); }

	return m_currentProgram;
}

Plugin::NativeProgramPage VstPlugin::programPage(int offset, int limit)
{
	if (offset < 0 || limit < 1 || limit > 1024) { throw std::runtime_error("Invalid VST program page bounds"); }
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage(message(IdVstProgramPage).addInt(offset).addInt(limit));
	const auto reply = waitForMessage(IdVstProgramPage, true);
	if (reply.id != IdVstProgramPage || failed()) { throw std::runtime_error("VST worker did not return a program page"); }
	if (reply.getQString(0) != "1") { throw std::runtime_error(("VST program query failed: " + reply.getQString(1)).toStdString()); }
	const auto integer = [&](int field) {
		bool valid = false; const int value = reply.getQString(field).toInt(&valid);
		if (!valid) { throw std::runtime_error("Malformed VST program reply"); } return value;
	};
	Plugin::NativeProgramPage page;
	page.total = integer(1); page.current = integer(2); page.offset = integer(3);
	const int length = integer(4);
	if (page.total < 0 || page.offset != offset || length < 0 || length > limit
		|| length != (offset >= page.total ? 0 : std::min(limit, page.total - offset))
		|| (page.total == 0 ? page.current != -1 : page.current < 0 || page.current >= page.total))
	{ throw std::runtime_error("Inconsistent VST program page"); }
	for (int relative = 0; relative < length; ++relative)
	{
		const int field = 5 + relative * 3;
		const int index = integer(field), available = integer(field + 2);
		if (index != offset + relative || (available != 0 && available != 1)) { throw std::runtime_error("Malformed VST program entry"); }
		page.programs.push_back({index, reply.getQString(field + 1), available == 1});
	}
	m_currentProgram = page.current;
	return page;
}



const QMap<QString, QString> & VstPlugin::parameterDump()
{
	if (QThread::currentThread() != thread()) { throw std::runtime_error("VST parameter queries require the plugin owner thread"); }
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	if (failed() || m_parameterCount < 0) { throw std::runtime_error("VST worker is unavailable for parameter queries"); }
	QMap<QString, QString> staged;
	int offset = 0;
	do
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		sendMessage(message(IdVstParameterPage).addInt(offset).addInt(VstParameterPageSize));
		// Blocking waits retain the request deadline without dispatching reentrant Qt timers.
		const auto reply = waitForMessage(IdVstParameterPage);
		if (reply.id != IdVstParameterPage || failed()) { throw std::runtime_error("VST worker did not return its parameters"); }
		const auto entries = readVstParameterPage(m_parameterCount, offset, reply.argumentCount(),
			[&reply](int field) { return reply.getString(field); });
		for (const auto& entry : entries)
		{
			const auto index = QString::number(entry.index);
			staged.insert("param" + index, index + ":" + QString::fromStdString(entry.name)
				+ ":" + QString::fromStdString(vstParameterText(entry.value)));
		}
		offset += static_cast<int>(entries.size());
	} while (offset < m_parameterCount);
	QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
	m_parameterDump = std::move(staged);
	return m_parameterDump;
}




void VstPlugin::setParameterDump( const QMap<QString, QString> & _pdump )
{
	if (QThread::currentThread() != thread()) { throw std::runtime_error("VST parameter restore requires the plugin owner thread"); }
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	const auto parameters = VstParameterModels::decode(_pdump, true);
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	if (m_parameterCount < 0 || parameters.size() != static_cast<std::size_t>(m_parameterCount))
	{ throw std::runtime_error("Saved VST parameter count differs from the loaded plugin"); }
	if (failed()) { throw std::runtime_error("VST worker is unavailable for parameter restore"); }
	const auto exchange = [this](const message& request) {
		sendMessage(request);
		const auto reply = waitForMessage(IdVstParameterRestore);
		if (reply.id != IdVstParameterRestore || failed())
		{ throw std::runtime_error("VST worker did not acknowledge parameter restore"); }
		if (reply.argumentCount() == 2 && reply.getString(0) == "0")
		{ throw std::runtime_error("VST parameter restore failed: " + reply.getString(1)); }
		if (reply.argumentCount() != 1 || reply.getString(0) != "1")
		{ throw std::runtime_error("Malformed VST parameter restore acknowledgment"); }
	};
	try
	{
		exchange(message(IdVstParameterRestore).addString("begin").addInt(m_parameterCount));
		for (int offset = 0; offset < m_parameterCount; )
		{
			QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
			const auto range = vstParameterRange(m_parameterCount, offset, VstParameterPageSize);
			message page(IdVstParameterRestore);
			page.addString("append").addInt(offset).addInt(range.count);
			for (int relative = 0; relative < range.count; ++relative)
			{ page.addString(vstParameterText(parameters[offset + relative].value)); }
			exchange(page);
			offset += range.count;
		}
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		exchange(message(IdVstParameterRestore).addString("commit"));
	}
	catch (...)
	{
		// Discard any staged values while preserving the original refusal/cancellation.
		if (!failed()) { try { exchange(message(IdVstParameterRestore).addString("abort")); } catch (...) {} }
		throw;
	}
}

QWidget *VstPlugin::pluginWidget()
{
	return m_pluginWidget;
}




bool VstPlugin::processMessage( const message & _m )
{
	switch( _m.id )
	{
	case IdVstPluginWindowID:
		m_pluginWindowID = _m.getInt();
		if (m_embedMethod == "none" && !gui::GuiApplication::isWayland()
			&& ConfigManager::inst()->value("ui", "vstalwaysontop").toInt())
		{
#ifdef LMMS_BUILD_WIN32
			// We're changing the owner, not the parent,
			// so this is legal despite MSDN's warning
			SetWindowLongPtr( (HWND)(intptr_t) m_pluginWindowID,
					GWLP_HWNDPARENT,
					(LONG_PTR) gui::getGUI()->mainWindow()->winId() );
#endif

#if defined(LMMS_BUILD_LINUX) && (QT_VERSION < QT_VERSION_CHECK(6,0,0))
			XSetTransientForHint( QX11Info::display(),
					m_pluginWindowID,
					gui::getGUI()->mainWindow()->winId() );
#endif
		}
		break;

	case IdVstPluginEditorGeometry:
		m_pluginGeometry = QSize( _m.getInt( 0 ),
								  _m.getInt( 1 ) );
			break;

		case IdVstPluginName:
			m_name = _m.getQString();
			break;

		case IdVstPluginVersion:
			m_version = _m.getInt();
			break;

		case IdVstPluginVendorString:
			m_vendorString = _m.getQString();
			break;

		case IdVstPluginProductString:
			m_productString = _m.getQString();
			break;

		case IdVstCurrentProgram:
			m_currentProgram = _m.getInt();
			break;

		case IdVstCurrentProgramName:
			m_currentProgramName = _m.getQString();
			break;

		case IdVstProgramNames:
			m_allProgramNames = _m.getQString().split('|');
			break;
		case IdVstProgramPage:
		case IdVstSelectProgram:
		case IdVstParameterPage:
		case IdVstParameterTextPage:
		case IdVstParameterRestore:
			// The waiting caller validates the complete structured reply.
			break;

		case IdVstLoadAllParameterLabels:
		case IdVstLoadAllParameterDisplays:
		{
			if (_m.argumentCount() == 2 && _m.getString(0) == "-1")
			{ throw std::runtime_error("VST parameter text query failed: " + _m.getString(1)); }
			if (_m.argumentCount() != 1) { throw std::runtime_error("Malformed VST parameter text reply"); }
			const auto fields = decodeVstParameterTextList(_m.getString(), m_parameterCount);
			std::vector<QString> staged;
			staged.reserve(fields.size());
			for (const auto& field : fields) { staged.push_back(QString::fromStdString(field)); }
			auto& destination = _m.id == IdVstLoadAllParameterLabels ? m_allParameterLabels : m_allParameterDisplays;
			destination = std::move(staged);
			break;
		}

		case IdVstUpdateParameterLabel:
		case IdVstUpdateParameterDisplay:
		{
			if (_m.argumentCount() != 2) { throw std::runtime_error("Malformed VST parameter text update"); }
			if (_m.getString(0) == "-1")
			{ throw std::runtime_error("VST parameter text query failed: " + _m.getString(1)); }
			const int index = vstParameterInteger(_m.getString(0));
			auto& destination = _m.id == IdVstUpdateParameterLabel ? m_allParameterLabels : m_allParameterDisplays;
			if (index < 0 || index >= m_parameterCount || destination.size() != static_cast<std::size_t>(m_parameterCount)
				|| _m.getString(1).size() > 8)
			{ throw std::runtime_error("VST parameter text update is outside the loaded layout"); }
			destination[index] = _m.getQString(1);
			break;
		}

		case IdVstPluginUniqueID:
			// TODO: display graphically in case of failure
			printf("unique ID: %s\n", _m.getString().c_str() );
			break;

		case IdVstParameterCount:
		{
			bool valid = false;
			const int count = _m.getQString().toInt(&valid);
			if (!valid || count < 0 || _m.argumentCount() != 1)
			{ throw std::runtime_error("VST worker reported an invalid parameter count"); }
			m_parameterCount = count;
			break;
		}
		case IdVstParameterDump:
		{
			bool valid = false;
			const int num_params = _m.getQString().toInt(&valid);
			if (valid && num_params == -1 && _m.argumentCount() == 2)
			{ throw std::runtime_error(("VST parameter query failed: " + _m.getQString(1)).toStdString()); }
			if (!valid || num_params != m_parameterCount || num_params < 0
				|| num_params > (std::numeric_limits<int>::max() - 1) / 3
				|| _m.argumentCount() != 1 + std::size_t{3} * num_params)
			{ throw std::runtime_error("VST worker parameter count changed or is malformed"); }
			QMap<QString, QString> staged;
			int p = 0;
			for( int i = 0; i < num_params; ++i )
			{
				const auto indexText = _m.getQString(++p);
				const int index = indexText.toInt(&valid);
				if (!valid || index != i) { throw std::runtime_error("VST worker parameter index is malformed"); }
				const auto name = _m.getQString(++p);
				const auto value = _m.getQString(++p);
				staged["param" + QString::number(index)] = indexText + ":" + name + ":" + value;
			}
			VstParameterModels::decode(staged);
			m_parameterDump = std::move(staged);
			break;
		}
		default:
			return RemotePlugin::processMessage( _m );
	}
	return true;

}


QWidget *VstPlugin::editor()
{
	return m_pluginWidget;
}


void VstPlugin::presetFileRequest(const QString& path, const char* operation)
{
	if (path.isEmpty() || path.contains(QChar(0))) { throw std::runtime_error("VST preset requires a valid file path"); }
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	if (failed()) { throw std::runtime_error("VST worker is unavailable"); }
	sendMessage(message(IdVstPresetFile).addString(QSTR_TO_STDSTR(QDir::toNativeSeparators(path))).addString(operation));
	const auto reply = waitForMessage(IdVstPresetFile, true);
	if (reply.id != IdVstPresetFile || failed()) { throw std::runtime_error("VST worker did not acknowledge the preset operation"); }
	if (reply.getQString(0) != "1") { throw std::runtime_error(("VST preset operation failed: " + reply.getQString(1)).toStdString()); }
}

Plugin::NativePresetInfo VstPlugin::readPresetFile(const QString& path, bool apply)
{
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	return readVstPresetStorage(path, apply, [this](const QString& file, const char* mode) { presetFileRequest(file, mode); });
}

Plugin::NativePresetInfo VstPlugin::inspectPresetFile(const QString& path) { return readPresetFile(path, false); }
Plugin::NativePresetInfo VstPlugin::loadPresetFile(const QString& path) { return readPresetFile(path, true); }

Plugin::NativePresetInfo VstPlugin::savePresetFile(const QString& path, bool bank)
{
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	return saveVstPresetStorage(path, bank, [this](const QString& file, const char* mode) { presetFileRequest(file, mode); });
}

QString VstPlugin::presetOpenPath()
{
	gui::FileDialog dialog(nullptr, tr("Open Preset"), "", tr("VST Plugin Preset (*.fxp *.fxb)"));
	dialog.setFileMode(gui::FileDialog::ExistingFile);
	return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty() ? dialog.selectedFiles().first() : QString{};
}

void VstPlugin::setProgram( int index )
{
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	const auto page = programPage(0, 1);
	if (index < 0 || index >= page.total) { throw std::out_of_range("VST program index is outside the loaded plugin"); }
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage(message(IdVstSelectProgram).addInt(index));
	const auto reply = waitForMessage(IdVstSelectProgram, true);
	if (reply.id != IdVstSelectProgram || failed()) { throw std::runtime_error("VST worker did not acknowledge program selection"); }
	if (reply.getQString(0) != "1") { throw std::runtime_error(("VST program selection failed: " + reply.getQString(1)).toStdString()); }
	bool valid = false; const int actual = reply.getQString(1).toInt(&valid);
	if (!valid || actual != index) { throw std::runtime_error("VST worker selected a different program"); }
	m_currentProgram = actual;
}




void VstPlugin::rotateProgram( int offset )
{
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	const auto page = programPage(0, 1);
	if (page.total == 0) { return; }
	setProgram(static_cast<int>(std::clamp<qint64>(static_cast<qint64>(page.current) + offset, 0, page.total - 1)));
}




void VstPlugin::loadProgramNames()
{
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	QStringList names;
	int offset = 0, total = -1;
	do
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		const auto page = programPage(offset, 1024);
		if (total >= 0 && total != page.total) { throw std::runtime_error("VST program count changed during enumeration"); }
		total = page.total;
		for (const auto& program : page.programs)
		{ names.append(program.nameAvailable ? program.name : tr("Program %1 (name unavailable)").arg(program.index + 1)); }
		offset += static_cast<int>(page.programs.size());
	} while (offset < total);
	m_allProgramNames = std::move(names);
}




void VstPlugin::loadParameterLabels()
{
	requestParameterText(IdVstLoadAllParameterLabels);
}




void VstPlugin::loadParameterDisplays()
{
	requestParameterText(IdVstLoadAllParameterDisplays);
}




void VstPlugin::updateParameterLabel(int index)
{
	requestParameterText(IdVstUpdateParameterLabel, index);
}




void VstPlugin::updateParameterDisplay(int index)
{
	requestParameterText(IdVstUpdateParameterDisplay, index);
}

void VstPlugin::requestParameterText(int messageId, int index)
{
	if (QThread::currentThread() != thread()) { throw std::runtime_error("VST parameter text queries require the plugin owner thread"); }
	FileResourceIO::EventSuspension events;
	const auto audioGuard = Engine::audioEngine()->requestChangesGuard();
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	if (failed() || m_parameterCount < 0) { throw std::runtime_error("VST worker is unavailable for parameter text queries"); }
	const bool single = messageId == IdVstUpdateParameterLabel || messageId == IdVstUpdateParameterDisplay;
	if (!single)
	{
		std::vector<QString> staged;
		staged.reserve(m_parameterCount);
		const char* kind = messageId == IdVstLoadAllParameterLabels ? "labels" : "displays";
		int offset = 0;
		do
		{
			QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
			sendMessage(message(IdVstParameterTextPage).addInt(offset).addInt(VstParameterPageSize).addString(kind));
			const auto reply = waitForMessage(IdVstParameterTextPage);
			if (reply.id != IdVstParameterTextPage || failed()) { throw std::runtime_error("VST worker did not return parameter text"); }
			const auto entries = readVstParameterTextPage(m_parameterCount, offset, reply.argumentCount(),
				[&reply](int field) { return reply.getString(field); });
			for (const auto& entry : entries) { staged.push_back(QString::fromStdString(entry)); }
			offset += static_cast<int>(entries.size());
		} while (offset < m_parameterCount);
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		auto& destination = messageId == IdVstLoadAllParameterLabels ? m_allParameterLabels : m_allParameterDisplays;
		destination = std::move(staged);
		return;
	}
	message request(messageId);
	if (single)
	{
		if (index < 0 || index >= m_parameterCount) { throw std::out_of_range("VST parameter text index is outside the loaded layout"); }
		const auto& cached = messageId == IdVstUpdateParameterLabel ? m_allParameterLabels : m_allParameterDisplays;
		if (cached.size() != static_cast<std::size_t>(m_parameterCount))
		{ throw std::runtime_error("Load VST parameter text before updating individual entries"); }
		request.addInt(index);
	}
	sendMessage(request);
	const auto reply = waitForMessage(messageId, true);
	if (failed() || reply.id != messageId) { throw std::runtime_error("VST worker did not return parameter text"); }
}




QString VstPlugin::presetSavePath()
{
	QString name = currentProgramName().section(": ", 1);
	if (name.isEmpty()) { name = "default"; }
	name.replace('"', '\'');
	gui::FileDialog dialog(nullptr, tr("Save Preset"), name + ".fxp", tr("VST Plugin Preset (*.fxp *.fxb)"));
	if (!p_name.isEmpty()) { dialog.setDirectory(QFileInfo(p_name).absolutePath()); }
	dialog.setAcceptMode(gui::FileDialog::AcceptSave);
	dialog.setFileMode(gui::FileDialog::AnyFile);
	if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) { return {}; }
	auto path = dialog.selectedFiles().first();
	if (path.isEmpty()) { return {}; }
	const auto extension = QFileInfo(path).suffix().toLower();
	if (extension != "fxp" && extension != "fxb") { path += ".fxp"; }
	p_name = path;
	return path;
}

void VstPlugin::setParam( int i, float f )
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	if (failed() || i < 0 || i >= m_parameterCount || !std::isfinite(f) || f < 0.f || f > 1.f)
	{ throw std::runtime_error("Cannot write an invalid VST parameter or a failed worker"); }
	sendMessage(message(IdVstSetParameter).addInt(i).addString(vstParameterText(f)));
}



void VstPlugin::idleUpdate()
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage( message( IdVstIdleUpdate ) );
}

void VstPlugin::showUI()
{
	if ( m_embedMethod == "none" )
	{
		RemotePlugin::showUI();
	}
	else if ( m_embedMethod != "headless" )
	{
		if (! editor()) {
			qWarning() << "VstPlugin::showUI called before VstPlugin::createUI";
		}
		toggleEditorVisibility( true );
	}
}

void VstPlugin::hideUI()
{
	if ( m_embedMethod == "none" )
	{
		RemotePlugin::hideUI();
	}
	else if ( pluginWidget() != nullptr )
	{
		toggleEditorVisibility( false );
	}
}

// X11Embed only
void VstPlugin::handleClientEmbed()
{
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage( IdShowUI );
}



void VstPlugin::loadChunk(const QByteArray& chunk)
{
	FileResourceIO::EventSuspension events;
	if (chunk.isEmpty() || chunk.size() > static_cast<qint64>(VstPresetMaxBytes)) { throw std::runtime_error("Invalid saved native VST chunk length"); }
	QTemporaryDir directory;
	if (!directory.isValid()) { throw std::runtime_error("Cannot create native VST chunk storage"); }
	const auto path = directory.filePath("chunk.bin"); QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(chunk) != chunk.size() || !file.flush())
	{ throw std::runtime_error("Cannot stage the saved native VST chunk"); }
	file.close();
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage(message(IdLoadSettingsFromFile).addString(QSTR_TO_STDSTR(QDir::toNativeSeparators(path))).addInt(chunk.size()));
	const auto reply = waitForMessage(IdLoadSettingsFromFile, true);
	if (failed() || reply.id != IdLoadSettingsFromFile || reply.getQString(0) != "1")
	{ throw std::runtime_error(("Native VST chunk load failed: " + reply.getQString(1)).toStdString()); }
}

QByteArray VstPlugin::saveChunk()
{
	FileResourceIO::EventSuspension events;
	QTemporaryDir directory;
	if (!directory.isValid()) { throw std::runtime_error("Cannot create native VST chunk storage"); }
	const auto path = directory.filePath("chunk.bin");
	lock();
	const auto unlockGuard = qScopeGuard([this] { unlock(); });
	sendMessage(message(IdSaveSettingsToFile).addString(QSTR_TO_STDSTR(QDir::toNativeSeparators(path))));
	const auto reply = waitForMessage(IdSaveSettingsToFile, true);
	if (failed() || reply.id != IdSaveSettingsToFile || reply.getQString(0) != "1")
	{ throw std::runtime_error(("Native VST chunk save failed: " + reply.getQString(1)).toStdString()); }
	if (reply.getQString(1) == "0") { return {}; }
	if (reply.getQString(1) != "1") { throw std::runtime_error("Malformed native VST chunk save response"); }
	const auto chunk = vst_preset_storage_detail::readPresetBytes(path);
	if (chunk.isEmpty()) { throw std::runtime_error("Native VST chunk save returned an empty file"); }
	return chunk;
}

QByteArray VstPlugin::saveNativePresetState()
{
	QTemporaryDir directory;
	if (!directory.isValid()) { throw std::runtime_error("Cannot create native VST preset storage"); }
	const auto path = directory.filePath("preset.bin");
	presetFileRequest(path, "save-state");
	const auto bytes = vst_preset_storage_detail::readPresetBytes(path);
	vst_preset_storage_detail::presetInfo(bytes);
	return bytes;
}

void VstPlugin::loadNativePresetState(const QByteArray& bytes)
{
	QTemporaryDir directory;
	if (!directory.isValid()) { throw std::runtime_error("Cannot create native VST preset storage"); }
	const auto path = directory.filePath("preset.bin"); QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() || !file.flush())
	{ throw std::runtime_error("Cannot stage saved native VST preset state"); }
	file.close();
	presetFileRequest(path, "load");
}

void VstPlugin::toggleEditorVisibility( int visible )
{
	QWidget* w = editor();
	if ( ! w ) {
		return;
	}

	if ( visible < 0 ) {
		visible = ! w->isVisible();
	}
	w->setVisible( visible );
}

void VstPlugin::createUI( QWidget * parent )
{
	if (!gui::getGUI() || m_embedMethod == "headless") { return; }
	if ( m_pluginWidget ) {
		qWarning() << "VstPlugin::createUI called twice";
		m_pluginWidget->setParent( parent );
		return;
	}

	if( m_pluginWindowID == 0 )
	{
		return;
	}

	QWidget* container = nullptr;

	if (m_embedMethod == "qt" )
	{
		QWindow* vw = QWindow::fromWinId(m_pluginWindowID);
		container = QWidget::createWindowContainer(vw, parent );
		container->installEventFilter(this);
	} else

#ifdef LMMS_BUILD_WIN32
	if (m_embedMethod == "win32" )
	{
		QWidget * helper = new QWidget;
		QHBoxLayout * l = new QHBoxLayout( helper );
		QWidget * target = new QWidget( helper );
		l->setSpacing( 0 );
		l->setContentsMargins(0, 0, 0, 0);
		l->addWidget( target );

		// we've to call that for making sure, Qt created the windows
		helper->winId();
		HWND targetHandle = (HWND)target->winId();
		HWND pluginHandle = (HWND)(intptr_t)m_pluginWindowID;

		DWORD style = GetWindowLong(pluginHandle, GWL_STYLE);
		style = style & ~(WS_POPUP);
		style = style | WS_CHILD;
		SetWindowLong(pluginHandle, GWL_STYLE, style);
		SetParent(pluginHandle, targetHandle);

		DWORD threadId = GetWindowThreadProcessId(pluginHandle, nullptr);
		DWORD currentThreadId = GetCurrentThreadId();
		AttachThreadInput(currentThreadId, threadId, true);

		container = helper;
		RemotePlugin::showUI();

	} else
#endif

#if defined(LMMS_BUILD_LINUX) && (QT_VERSION < QT_VERSION_CHECK(6,0,0))
	if (m_embedMethod == "xembed" )
	{
		if (parent)
		{
			parent->setAttribute(Qt::WA_NativeWindow);
		}
		auto embedContainer = new QX11EmbedContainer(parent);
		connect(embedContainer, SIGNAL(clientIsEmbedded()), this, SLOT(handleClientEmbed()));
		embedContainer->embedClient( m_pluginWindowID );
		container = embedContainer;
	} else
#endif
	{
		qCritical() << "Unknown embed method" << m_embedMethod;
		return;
	}

	container->setFixedSize( m_pluginGeometry );
	container->setWindowTitle( name() );

	m_pluginWidget = container;
}

bool VstPlugin::eventFilter(QObject *obj, QEvent *event)
{
	if (embedMethod() == "qt" && obj == m_pluginWidget)
	{
		if (event->type() == QEvent::Show) {
			RemotePlugin::showUI();
		}
		qDebug() << obj << event;
	}
	return false;
}

QString VstPlugin::embedMethod() const
{
	return m_embedMethod;
}

namespace gui {

VstPluginKnob::VstPluginKnob(VstPlugin* plugin, int paramIndex, const QString& name, QWidget* parent)
	: gui::Knob{gui::KnobType::Bright26, name.left(15), SMALL_FONT_SIZE, parent, name}
	, m_plugin{plugin}
	, m_paramIndex{paramIndex}
{
	assert(m_plugin != nullptr);
	setDescription(name + ":");
}

void VstPluginKnob::timerEvent(QTimerEvent* event)
{
	if (event->timerId() != m_rateLimitTimerId) { return; }

	if (textFloat().source() != static_cast<FloatModelEditorBase*>(this))
	{
		// This knob is no longer controlling the text float,
		// so we don't need the timer to continue running.
		killTimer(m_rateLimitTimerId);
		m_rateLimitTimerId = 0;
		return;
	}

	// Enough time has passed, so time for an update
	m_updateNow = true;
}

auto VstPluginKnob::currentValueToText() -> QString
{
	constexpr auto updatesPerSecond = 15;

	if (m_rateLimitTimerId == 0)
	{
		// Use a timer to control the rate of text float updates.
		// Otherwise the CPU will spike when the parameter emits a bunch of dataChanged()
		// events in a short period of time - i.e. when the parameter's value changes rapidly.
		m_rateLimitTimerId = startTimer(std::chrono::milliseconds{1000} / updatesPerSecond);
	}

	return getParameterText();
}

auto VstPluginKnob::currentValueToTextUpdate() -> std::optional<QString>
{
	if (!m_updateNow) { return std::nullopt; }
	m_updateNow = false;

	return getParameterText();
}

auto VstPluginKnob::getParameterText() const -> QString
{
	try
	{
		m_plugin->updateParameterLabel(m_paramIndex);
		m_plugin->updateParameterDisplay(m_paramIndex);
		const auto& paramLabels = m_plugin->allParameterLabels();
		const auto& paramDisplays = m_plugin->allParameterDisplays();
		return paramDisplays.at(m_paramIndex) + ' ' + paramLabels.at(m_paramIndex);
	}
	catch (const std::exception& error)
	{
		// Metadata failure must not throw out of a Qt paint/event callback.
		return tr("VST parameter unavailable: %1").arg(QString::fromUtf8(error.what()));
	}
}

} // namespace gui
} // namespace lmms
