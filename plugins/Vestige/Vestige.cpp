/*
 * Vestige.cpp - instrument-plugin for hosting VST-instruments
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

#include "Vestige.h"

#include <QDomElement>
#include <QFileInfo>
#include <QDropEvent>
#include <QGridLayout>
#include <QMdiArea>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QtGlobal>
#include <memory>
#include <algorithm>

#include "AudioEngine.h"
#include "Clipboard.h"
#include "ConfigManager.h"
#include "embed.h"
#include "Engine.h"
#include "FileDialog.h"
#include "FontHelper.h"
#include "GuiApplication.h"
#include "InstrumentPlayHandle.h"
#include "InstrumentTrack.h"
#include "FileResourceIO.h"
#include <QMutexLocker>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <stdexcept>
#include "MainWindow.h"
#include "PathUtil.h"
#include "PixmapButton.h"
#include "Song.h"
#include "StringPairDrag.h"
#include "SubWindow.h"
#include "TextFloat.h"
#include "VstPlugin.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor Q_DECL_EXPORT  vestige_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"VeSTige",
	QT_TRANSLATE_NOOP( "PluginBrowser",
			"VST-host for using VST(i)-plugins within LMMS" ),
	"Tobias Doerffel <tobydox/at/users.sf.net>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
#if defined(LMMS_BUILD_WIN32) || defined(LMMS_BUILD_CYGWIN)
	"dll",
#elif defined(LMMS_BUILD_LINUX)
#	if defined(LMMS_HAVE_VST_32) || defined(LMMS_HAVE_VST_64)
		"dll,so",
#	else
		"so",
#	endif
#endif
	nullptr,
} ;

}

namespace gui
{


class vstSubWin : public SubWindow
{
public:
	vstSubWin( QWidget * _parent ) :
		SubWindow( _parent )
	{
		setAttribute(Qt::WA_DeleteOnClose, false);
		setWindowFlag(Qt::WindowMaximizeButtonHint, false);
		setDetachable(false);
	}

	~vstSubWin() override = default;

	void closeEvent( QCloseEvent * e ) override
	{
		// ignore close-events - for some reason otherwise the VST GUI
		// remains hidden when re-opening
		hide();
		e->ignore();
	}
};


} // namespace gui


class VstInstrumentPlugin : public VstPlugin
{
public:
	using VstPlugin::VstPlugin;

	void createUI( QWidget *parent ) override
	{
		Q_UNUSED(parent);
		if (!gui::getGUI() || !hasEditor()) {
			return;
		}
		if ( embedMethod() != "none" ) {
			m_pluginSubWindow.reset(new gui::vstSubWin( gui::getGUI()->mainWindow()->workspace() ));
			VstPlugin::createUI( m_pluginSubWindow.get() );
			m_pluginSubWindow->setWidget(pluginWidget());
		} else {
			VstPlugin::createUI( nullptr );
		}
	}

	/// Overwrite editor() to return the sub window instead of the embed widget
	/// itself. This makes toggleUI() and related functions toggle the
	/// sub window's visibility.
	QWidget* editor() override
	{
		return m_pluginSubWindow.get();
	}
private:
	std::unique_ptr<QMdiSubWindow> m_pluginSubWindow;
};



VestigeInstrument::VestigeInstrument( InstrumentTrack * _instrument_track ) :
	Instrument(_instrument_track, &vestige_plugin_descriptor, nullptr, Flag::IsSingleStreamed | Flag::IsMidiBased),
	m_plugin( nullptr ),
	m_pluginMutex(),
	m_subWindow( nullptr ),
	m_scrollArea( nullptr ),
	m_parameters(this, [this](int index, float value) { if (m_plugin) { m_plugin->setParam(index, value); } }),
	p_subWindow( nullptr )
{
	// now we need a play-handle which cares for calling play()
	auto iph = new InstrumentPlayHandle(this, _instrument_track);
	Engine::audioEngine()->addPlayHandle( iph );

	connect( ConfigManager::inst(), SIGNAL( valueChanged(QString,QString,QString) ),
			 this, SLOT( handleConfigChange(QString, QString, QString) ),
			 Qt::QueuedConnection );
}




VestigeInstrument::~VestigeInstrument()
{
	if (p_subWindow != nullptr) {
		delete p_subWindow;
		p_subWindow = nullptr;
	}

	Engine::audioEngine()->removePlayHandlesOfTypes( instrumentTrack(),
				PlayHandle::Type::NotePlayHandle
				| PlayHandle::Type::InstrumentPlayHandle );
	closePlugin();
}




void VestigeInstrument::loadSettings(const QDomElement& settings)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	const auto plugin = settings.attribute("plugin");
	if (plugin.isEmpty()) { return; }
	if (!m_plugin || m_pluginDLL != PathUtil::toShortestRelative(plugin)) { loadFile(plugin); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin) { return; }
	try
	{
		m_plugin->loadSettings(settings);
		m_parameters.restore(m_plugin->parameterDump(), settings);
		if (gui::getGUI())
		{
			if (!instrumentTrack()->isPreviewMode() && settings.attribute("guivisible").toInt()) { m_plugin->showUI(); }
			else { m_plugin->hideUI(); }
		}
	}
	catch (const std::exception& error) { collectErrorForUI(QString::fromUtf8(error.what())); }
}

Plugin::NativeProgramPage VestigeInstrument::nativePrograms(int offset, int limit)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin) { throw std::runtime_error("Load a VST plugin before inspecting programs"); }
	return m_plugin->programPage(offset, limit);
}

Plugin::NativePresetInfo VestigeInstrument::inspectNativePreset(const QString& path)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_plugin->inspectPresetFile(path);
	return result;
}

Plugin::NativePresetInfo VestigeInstrument::loadNativePreset(const QString& path)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_plugin->loadPresetFile(path);
	m_parameters.synchronize(m_plugin->parameterDump());
	return result;
}

Plugin::NativePresetInfo VestigeInstrument::saveNativePreset(const QString& path, bool bank)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (!m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_plugin->savePresetFile(path, bank);
	return result;
}

bool VestigeInstrument::selectNativeProgram(int index, QString& error)
{
	try
	{
		m_parameters.checkOwner();
		if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
		QScopedValueRollback<bool> operation(m_parameterOperation, true);
		FileResourceIO::EventSuspension events;
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		QMutexLocker lock(&m_pluginMutex);
		if (!m_plugin) { throw std::runtime_error("Load a VST plugin before selecting a program"); }
		m_plugin->setProgram(index);
		m_parameters.synchronize(m_plugin->parameterDump());
		error.clear();
		return true;
	}
	catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); return false; }
}

void VestigeInstrument::refreshParameterModels()
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (m_plugin) { m_parameters.synchronize(m_plugin->parameterDump()); }
}

std::vector<Plugin::FileResource> VestigeInstrument::fileResources() const
{
	return {{"plugin", tr("VST plugin binary"), m_pluginDLL,
		QString::fromUtf8(descriptor()->supportedFileTypes).split(',')}};
}

bool VestigeInstrument::setFileResource(const QString& key, const QString& path, QString& error)
{
	if (key != "plugin") { error = tr("Unknown VeSTige file resource"); return false; }
	if (!QFileInfo(PathUtil::toAbsolute(path)).isFile()) { error = tr("VST plugin file does not exist"); return false; }
	loadFile(path);
	if (!m_plugin || m_plugin->failed()) { error = tr("The VST plugin could not be loaded"); return false; }
	error.clear();
	return true;
}

void VestigeInstrument::handleConfigChange(QString cls, QString attr, QString value)
{
    Q_UNUSED(cls); Q_UNUSED(attr); Q_UNUSED(value);
    // Disabled for consistency with VST effects that don't implement this. (#3786)
    // if ( cls == "ui" && attr == "vstembedmethod" )
    // {
    // 	reloadPlugin();
    // }
}

void VestigeInstrument::reloadPlugin()
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	closePlugin();
	loadFile( m_pluginDLL );
}




void VestigeInstrument::saveSettings(QDomDocument& document, QDomElement& settings)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	settings.setAttribute("plugin", PathUtil::toShortestRelative(m_pluginDLL));
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_pluginMutex);
	if (m_plugin)
	{
		m_plugin->saveSettings(document, settings);
		m_parameters.saveSettings(document, settings);
	}
}

QString VestigeInstrument::nodeName( void ) const
{
	return( vestige_plugin_descriptor.name );
}




void VestigeInstrument::loadFile(const QString& file)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const bool rename = (m_plugin && instrumentTrack()->name() == m_plugin->name()) ||
		instrumentTrack()->name() == InstrumentTrack::tr("Default preset") || instrumentTrack()->name() == displayName();
	if (m_plugin && instrumentTrack()->isPreviewMode() && m_pluginDLL == PathUtil::toShortestRelative(file)) { return; }
	closePlugin();
	m_pluginDLL = PathUtil::toShortestRelative(file);
	std::unique_ptr<gui::TextFloat> loading;
	if (gui::getGUI())
	{
		loading.reset(gui::TextFloat::displayMessage(tr("Loading plugin"),
			tr("Please wait while loading the VST plugin..."), PLUGIN_NAME::getIconPixmap("logo", 24, 24), 0));
	}
	try
	{
		QMutexLocker lock(&m_pluginMutex);
		auto plugin = std::make_unique<VstInstrumentPlugin>(m_pluginDLL);
		if (plugin->failed()) { throw std::runtime_error(tr("The VST plugin %1 could not be loaded.").arg(m_pluginDLL).toStdString()); }
		m_plugin = plugin.release();
		m_parameters.synchronize(m_plugin->parameterDump());
		if (gui::getGUI() && !instrumentTrack()->isPreviewMode())
		{
			m_plugin->createUI(nullptr);
			m_plugin->showUI();
		}
		if (rename) { instrumentTrack()->setName(m_plugin->name()); }
	}
	catch (const std::exception& error)
	{
		closePlugin();
		collectErrorForUI(QString::fromUtf8(error.what()));
		m_pluginDLL.clear();
	}
	emit dataChanged();
}

void VestigeInstrument::play( SampleFrame* _buf )
{
	if (!m_pluginMutex.tryLock(Engine::getSong()->isExporting() ? -1 : 0)) {return;}
	const auto unlock = qScopeGuard([this] { m_pluginMutex.unlock(); });

	if( m_plugin == nullptr )
	{
		return;
	}

	m_plugin->process( nullptr, _buf );

}




bool VestigeInstrument::handleMidiEvent( const MidiEvent& event, const TimePos& time, f_cnt_t offset )
{
	QMutexLocker lock(&m_pluginMutex);
	if( m_plugin != nullptr )
	{
		m_plugin->processMidiEvent( event, offset );
	}

	return true;
}




void VestigeInstrument::closePlugin()
{
	m_parameters.checkOwner();
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	// Destroy views before their borrowed models and before the remote plugin.
	delete p_subWindow;
	p_subWindow = nullptr;
	QMutexLocker lock(&m_pluginMutex);
	m_parameters.clear();
	delete m_plugin;
	m_plugin = nullptr;
}

gui::PluginView * VestigeInstrument::instantiateView( QWidget * _parent )
{
	return new gui::VestigeInstrumentView( this, _parent );
}


namespace gui
{

VestigeInstrumentView::VestigeInstrumentView( Instrument * _instrument,
							QWidget * _parent ) :
	InstrumentViewFixedSize( _instrument, _parent ),
	lastPosInMenu (0)
{
	m_openPluginButton = new PixmapButton( this, "" );
	m_openPluginButton->setCheckable( false );
	m_openPluginButton->setCursor( Qt::PointingHandCursor );
	m_openPluginButton->move( 216, 81 );
	m_openPluginButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"select_file_active" ) );
	m_openPluginButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"select_file" ) );
	connect( m_openPluginButton, SIGNAL( clicked() ), this,
						SLOT( openPlugin() ) );
	m_openPluginButton->setToolTip(tr("Open VST plugin"));

	m_managePluginButton = new PixmapButton( this, "" );
	m_managePluginButton->setCheckable( false );
	m_managePluginButton->setCursor( Qt::PointingHandCursor );
	m_managePluginButton->move( 216, 101 );
	m_managePluginButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"controls_active" ) );
	m_managePluginButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"controls" ) );
	connect( m_managePluginButton, SIGNAL( clicked() ), this,
						SLOT( managePlugin() ) );
	m_managePluginButton->setToolTip(tr("Control VST plugin from LMMS host"));


	m_openPresetButton = new PixmapButton( this, "" );
	m_openPresetButton->setCheckable( false );
	m_openPresetButton->setCursor( Qt::PointingHandCursor );
	m_openPresetButton->move( 200, 224 );
	m_openPresetButton->setActiveGraphic( embed::getIconPixmap(
							"project_open", 20, 20 ) );
	m_openPresetButton->setInactiveGraphic( embed::getIconPixmap(
							"project_open", 20, 20 ) );
	connect( m_openPresetButton, SIGNAL( clicked() ), this,
						SLOT( openPreset() ) );
	m_openPresetButton->setToolTip(tr("Open VST plugin preset"));


	m_rolLPresetButton = new QPushButton(this);
	m_rolLPresetButton->setObjectName("btn-stepper-left");
	m_rolLPresetButton->setCursor(Qt::PointingHandCursor);
	m_rolLPresetButton->move( 190, 201 );
	connect( m_rolLPresetButton, SIGNAL( clicked() ), this,
						SLOT( previousProgram() ) );
	m_rolLPresetButton->setToolTip(tr("Previous (-)"));

	m_rolLPresetButton->setShortcut( Qt::Key_Minus );


	m_savePresetButton = new PixmapButton( this, "" );
	m_savePresetButton->setCheckable( false );
	m_savePresetButton->setCursor( Qt::PointingHandCursor );
	m_savePresetButton->move( 224, 224 );
	m_savePresetButton->setActiveGraphic( embed::getIconPixmap(
							"project_save", 20, 20  ) );
	m_savePresetButton->setInactiveGraphic( embed::getIconPixmap(
							"project_save", 20, 20  ) );
	connect( m_savePresetButton, SIGNAL( clicked() ), this,
						SLOT( savePreset() ) );
	m_savePresetButton->setToolTip(tr("Save preset"));


	m_rolRPresetButton = new QPushButton(this);
	m_rolRPresetButton->setObjectName("btn-stepper-right");
	m_rolRPresetButton->setCursor(Qt::PointingHandCursor);
	m_rolRPresetButton->move( 209, 201 );
	connect( m_rolRPresetButton, SIGNAL( clicked() ), this,
						SLOT( nextProgram() ) );
	m_rolRPresetButton->setToolTip(tr("Next (+)"));

	m_rolRPresetButton->setShortcut( Qt::Key_Plus );


	m_selPresetButton = new QPushButton(this);
	m_selPresetButton->setObjectName("btn-stepper-down");
	m_selPresetButton->setCursor(Qt::PointingHandCursor);
	m_selPresetButton->move(228, 201);

	auto menu = new QMenu;

	connect( menu, SIGNAL( aboutToShow() ), this, SLOT( updateMenu() ) );

	m_selPresetButton->setMenu(menu);

	m_toggleGUIButton = new QPushButton( tr( "Show/hide GUI" ), this );
	m_toggleGUIButton->setGeometry( 20, 130, 200, 24 );
	m_toggleGUIButton->setIcon( embed::getIconPixmap( "zoom" ) );
	m_toggleGUIButton->setFont(adjustedToPixelSize(m_toggleGUIButton->font(), LARGE_FONT_SIZE));
	connect( m_toggleGUIButton, SIGNAL( clicked() ), this,
							SLOT( toggleGUI() ) );

	auto note_off_all_btn = new QPushButton(tr("Turn off all "
											   "notes"),
		this);
	note_off_all_btn->setGeometry( 20, 160, 200, 24 );
	note_off_all_btn->setIcon( embed::getIconPixmap( "stop" ) );
	note_off_all_btn->setFont(adjustedToPixelSize(note_off_all_btn->font(), LARGE_FONT_SIZE));
	connect( note_off_all_btn, SIGNAL( clicked() ), this,
							SLOT( noteOffAll() ) );

	setAcceptDrops( true );
	_instrument2 = _instrument;
	_parent2 = _parent;
}


void VestigeInstrumentView::managePlugin( void )
{
	if ( m_vi->m_plugin != nullptr && m_vi->m_subWindow == nullptr ) {
		m_vi->p_subWindow = new ManageVestigeInstrumentView( _instrument2, _parent2, m_vi);
	} else if (m_vi->m_subWindow != nullptr) {
		if (m_vi->m_subWindow->widget()->isVisible() == false ) {
			m_vi->m_scrollArea->show();
			m_vi->m_subWindow->show();
		} else {
			m_vi->m_scrollArea->hide();
			m_vi->m_subWindow->hide();
		}
	}
}


void VestigeInstrumentView::updateMenu( void )
{

	// get all presets -
	if ( m_vi->m_plugin != nullptr )
	{
		try { m_vi->m_plugin->loadProgramNames(); }
		catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); return; }
		QWidget::update();

		const auto list1 = m_vi->m_plugin->allProgramNames();

     		QMenu * to_menu = m_selPresetButton->menu();
    		to_menu->clear();

			QVector<QAction*> presetActions(list1.size());

     		for (int i = 0; i < list1.size(); i++) {
			presetActions[i] = new QAction(this);
			connect(presetActions[i], SIGNAL(triggered()), this, SLOT(selPreset()));

        		presetActions[i]->setText(QString("%1. %2").arg(QString::number(i+1), list1.at(i)));
        		presetActions[i]->setData(i);
			if (i == lastPosInMenu) {
        			presetActions[i]->setIcon(embed::getIconPixmap( "sample_file", 16, 16 ));
			} else  presetActions[i]->setIcon(embed::getIconPixmap( "edit_copy", 16, 16 ));
			to_menu->addAction( presetActions[i] );
     		}

	}
}


void VestigeInstrumentView::modelChanged()
{
	m_vi = castModel<VestigeInstrument>();
}




void VestigeInstrumentView::openPlugin()
{
	FileDialog ofd( nullptr, tr( "Open VST plugin" ) );

	// set filters
	QStringList types;
#if defined(LMMS_BUILD_WIN32)
	types << tr("VST2 files (*.dll)");
#elif defined(LMMS_BUILD_LINUX)
#	if defined(LMMS_HAVE_VST_32) || defined(LMMS_HAVE_VST_64)
		types << tr("All VST files (*.dll *.so)")
			<< tr("Windows VST2 files (*.dll)");
#	endif
	types << tr("LinuxVST files (*.so)");
#endif

	ofd.setNameFilters(types);

	if( m_vi->m_pluginDLL != "" )
	{
		QString f = PathUtil::toAbsolute( m_vi->m_pluginDLL );
		ofd.setDirectory( QFileInfo( f ).absolutePath() );
		ofd.selectFile( QFileInfo( f ).fileName() );
	}
	else
	{
		ofd.setDirectory( ConfigManager::inst()->vstDir() );
	}

	if ( ofd.exec () == QDialog::Accepted )
	{
		if( ofd.selectedFiles().isEmpty() )
		{
			return;
		}
		Engine::audioEngine()->requestChangeInModel();

		if (m_vi->p_subWindow != nullptr) {
			delete m_vi->p_subWindow;
			m_vi->p_subWindow = nullptr;
		}

		m_vi->loadFile( ofd.selectedFiles()[0] );
		Engine::audioEngine()->doneChangeInModel();
		if( m_vi->m_plugin && m_vi->m_plugin->pluginWidget() )
		{
			m_vi->m_plugin->pluginWidget()->setWindowIcon(
									PLUGIN_NAME::getIconPixmap( "logo" ) );
		}
	}
}




void VestigeInstrumentView::openPreset()
{
	if (!m_vi->m_plugin) { return; }
	try
	{
		const auto path = m_vi->m_plugin->presetOpenPath();
		if (path.isEmpty()) { return; }
		m_vi->loadNativePreset(path);
		lastPosInMenu = m_vi->nativePrograms(0, 1).current;
		Engine::getSong()->setModified();
		QWidget::update();
	}
	catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VestigeInstrumentView::savePreset()
{
	if (!m_vi->m_plugin) { return; }
	try
	{
		const auto path = m_vi->m_plugin->presetSavePath();
		if (path.isEmpty()) { return; }
		m_vi->saveNativePreset(path, QFileInfo(path).suffix().compare("fxb", Qt::CaseInsensitive) == 0);
	}
	catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VestigeInstrumentView::changeProgram(int program, bool relative)
{
	try
	{
		const auto page = m_vi->nativePrograms(0, 1);
		if (page.total == 0) { return; }
		const int index = relative ? static_cast<int>(std::clamp<qint64>(static_cast<qint64>(page.current) + program, 0, page.total - 1)) : program;
		QString error;
		if (!m_vi->selectNativeProgram(index, error)) { m_vi->collectErrorForUI(error); return; }
		lastPosInMenu = index;
		Engine::getSong()->setModified();
		QWidget::update();
	}
	catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VestigeInstrumentView::nextProgram() { changeProgram(1, true); }
void VestigeInstrumentView::previousProgram() { changeProgram(-1, true); }
void VestigeInstrumentView::selPreset()
{
	if (auto action = qobject_cast<QAction*>(sender())) { changeProgram(action->data().toInt(), false); }
}


void VestigeInstrumentView::toggleGUI( void )
{
	if( m_vi == nullptr || m_vi->m_plugin == nullptr )
	{
		return;
	}
	m_vi->m_plugin->toggleUI();
}




void VestigeInstrumentView::noteOffAll( void )
{
	QMutexLocker lock(&m_vi->m_pluginMutex);
	if( m_vi->m_plugin != nullptr )
	{
		for( int key = 0; key <= MidiMaxKey; ++key )
		{
			m_vi->m_plugin->processMidiEvent( MidiEvent( MidiNoteOff, 0, key, 0 ), 0 );
		}
	}
}




void VestigeInstrumentView::dragEnterEvent( QDragEnterEvent * _dee )
{
	// For mimeType() and MimeType enum class
	using namespace Clipboard;

	if( _dee->mimeData()->hasFormat( mimeType( MimeType::StringPair ) ) )
	{
		QString txt = _dee->mimeData()->data(
						mimeType( MimeType::StringPair ) );
		if( txt.section( ':', 0, 0 ) == "vstplugin" )
		{
			_dee->acceptProposedAction();
		}
		else
		{
			_dee->ignore();
		}
	}
	else
	{
		_dee->ignore();
	}
}




void VestigeInstrumentView::dropEvent( QDropEvent * _de )
{
	QString type = StringPairDrag::decodeKey( _de );
	QString value = StringPairDrag::decodeValue( _de );
	if( type == "vstplugin" )
	{
		m_vi->loadFile( value );
		_de->accept();
		return;
	}
	_de->ignore();
}




void VestigeInstrumentView::paintEvent( QPaintEvent * )
{
	QPainter p( this );

	static auto s_artwork = PLUGIN_NAME::getIconPixmap("artwork");
	p.drawPixmap(0, 0, s_artwork);

	QString plugin_name = ( m_vi->m_plugin != nullptr ) ?
				m_vi->m_plugin->name()/* + QString::number(
						m_plugin->version() )*/
					:
				tr( "No VST plugin loaded" );
	QFont f = p.font();
	f.setBold( true );
	p.setFont(adjustedToPixelSize(f, DEFAULT_FONT_SIZE));
	p.setPen( QColor( 255, 255, 255 ) );
	p.drawText( 10, 100, plugin_name );

	p.setPen( QColor( 50, 50, 50 ) );
	p.drawText( 10, 211, tr( "Preset" ) );

//	m_pluginMutex.lock();
	if( m_vi->m_plugin != nullptr )
	{
		p.setPen( QColor( 0, 0, 0 ) );
		f.setBold( false );
		p.setFont(adjustedToPixelSize(f, SMALL_FONT_SIZE));
		p.drawText( 10, 114, tr( "by " ) +
					m_vi->m_plugin->vendorString() );
		p.setPen( QColor( 255, 255, 255 ) );
		p.drawText( 10, 225, m_vi->m_plugin->currentProgramName() );
	}

	if( m_vi->m_subWindow != nullptr )
	{
		m_vi->m_subWindow->setWindowTitle( m_vi->instrumentTrack()->name()
								+ tr( " - VST plugin control" ) );
	}
//	m_pluginMutex.unlock();
}




ManageVestigeInstrumentView::ManageVestigeInstrumentView( Instrument * _instrument,
							QWidget * _parent, VestigeInstrument * m_vi2 ) :
	InstrumentViewFixedSize( _instrument, _parent )
{
	m_vi = m_vi2;
	m_vi->m_scrollArea = new QScrollArea( this );
	widget = new QWidget(this);
	l = new QGridLayout( this );

	m_vi->m_subWindow = getGUI()->mainWindow()->addWindowedWidget(m_vi->m_scrollArea);
	m_vi->m_scrollArea->setFixedWidth(960);
	m_vi->m_scrollArea->setMinimumHeight(300);
	m_vi->m_subWindow->setWindowTitle( m_vi->instrumentTrack()->name()
								+ tr( " - VST plugin control" ) );
	m_vi->m_subWindow->setWindowIcon( PLUGIN_NAME::getIconPixmap( "logo" ) );
	m_vi->m_subWindow->setAttribute( Qt::WA_DeleteOnClose, false );


	l->setContentsMargins( 20, 10, 10, 10 );
	l->setVerticalSpacing( 10 );
	l->setHorizontalSpacing( 23 );

	m_syncButton = new QPushButton( tr( "VST Sync" ), this );
	connect( m_syncButton, SIGNAL( clicked() ), this,
							SLOT( syncPlugin() ) );

	l->addWidget( m_syncButton, 0, 0, 1, 2, Qt::AlignLeft );

	m_displayAutomatedOnly = new QPushButton( tr( "Automated" ), this );
	connect( m_displayAutomatedOnly, SIGNAL( clicked() ), this,
							SLOT( displayAutomatedOnly() ) );

	l->addWidget( m_displayAutomatedOnly, 0, 1, 1, 2, Qt::AlignLeft );


	m_closeButton = new QPushButton( tr( "    Close    " ), widget );
	connect( m_closeButton, SIGNAL( clicked() ), this,
							SLOT( closeWindow() ) );

	l->addWidget( m_closeButton, 0, 2, 1, 7, Qt::AlignLeft );


	for( int i = 0; i < 10; i++ )
	{
		l->addItem( new QSpacerItem( 68, 45, QSizePolicy::Fixed, QSizePolicy::Fixed ), 0, i );
	}

	m_vi->refreshParameterModels();
	const int paramCount = m_vi->m_parameters.size();
	m_vstKnobs.reserve(paramCount);
	for (int i = 0; i < paramCount; ++i)
	{
		auto model = m_vi->m_parameters.at(i);
		auto knob = new VstPluginKnob{m_vi->m_plugin, i, model->displayName(), widget};
		m_vstKnobs.push_back(knob);
		knob->setModel(model);
	}
	try { m_vi->m_plugin->loadParameterLabels(); m_vi->m_plugin->loadParameterDisplays(); }
	catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); }

	int i = 0;
	for( int lrow = 1; lrow < ( int( m_vi->m_parameters.size() / 10 ) + 1 ) + 1; lrow++ )
	{
		for( int lcolumn = 0; lcolumn < 10; lcolumn++ )
		{
			if( i < m_vi->m_parameters.size() )
			{
				l->addWidget(m_vstKnobs[i], lrow, lcolumn, Qt::AlignCenter);
			}
			i++;
		}
	}

	l->setRowStretch( ( int( m_vi->m_parameters.size() / 10) + 1), 1 );
	l->setColumnStretch( 10, 1 );

	widget->setLayout(l);
	widget->setAutoFillBackground(true);

	m_vi->m_scrollArea->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOn );
	m_vi->m_scrollArea->setPalette( QApplication::palette( m_vi->m_scrollArea ) );
	m_vi->m_scrollArea->setMinimumHeight( 64 );

	m_vi->m_scrollArea->setWidget( widget );

	m_vi->m_subWindow->show();
}




void ManageVestigeInstrumentView::closeWindow()
{
	m_vi->m_subWindow->hide();
}




void ManageVestigeInstrumentView::syncPlugin()
{
	try
	{
		m_vi->refreshParameterModels();
		if (m_vi->m_plugin) { m_vi->m_plugin->loadParameterLabels(); m_vi->m_plugin->loadParameterDisplays(); }
	}
	catch (const std::exception& error) { m_vi->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void ManageVestigeInstrumentView::displayAutomatedOnly( void )
{
	bool isAuto = QString::compare( m_displayAutomatedOnly->text(), tr( "Automated" ) ) == 0;

	for( int i = 0; i< m_vi->m_parameters.size(); i++ )
	{

		if( !( m_vi->m_parameters.at(i)->isAutomated() || m_vi->m_parameters.at(i)->controllerConnection() ) )
		{
			if (m_vstKnobs[i]->isVisible() && isAuto)
			{
				m_vstKnobs[i]->hide();
				m_displayAutomatedOnly->setText( "All" );
			} else {
				m_vstKnobs[i]->show();
				m_displayAutomatedOnly->setText( "Automated" );
			}
		}
	}
}


ManageVestigeInstrumentView::~ManageVestigeInstrumentView()
{
	if (m_vi->m_scrollArea != nullptr) {
		delete m_vi->m_scrollArea;
		m_vi->m_scrollArea = nullptr;
	}

	if ( m_vi->m_subWindow != nullptr ) {
		m_vi->m_subWindow->setAttribute(Qt::WA_DeleteOnClose);
		m_vi->m_subWindow->close();

		if ( m_vi->m_subWindow != nullptr )
			delete m_vi->m_subWindow;
		m_vi->m_subWindow = nullptr;
	}

	m_vi->p_subWindow = nullptr;
}






void ManageVestigeInstrumentView::dragEnterEvent( QDragEnterEvent * _dee )
{
	// For mimeType() and MimeType enum class
	using namespace Clipboard;

	if( _dee->mimeData()->hasFormat( mimeType( MimeType::StringPair ) ) )
	{
		QString txt = _dee->mimeData()->data(
						mimeType( MimeType::StringPair ) );
		if( txt.section( ':', 0, 0 ) == "vstplugin" )
		{
			_dee->acceptProposedAction();
		}
		else
		{
			_dee->ignore();
		}
	}
	else
	{
		_dee->ignore();
	}
}




void ManageVestigeInstrumentView::dropEvent( QDropEvent * _de )
{
	QString type = StringPairDrag::decodeKey( _de );
	QString value = StringPairDrag::decodeValue( _de );
	if( type == "vstplugin" )
	{
		m_vi->loadFile( value );
		_de->accept();
		return;
	}
	_de->ignore();
}




void ManageVestigeInstrumentView::paintEvent( QPaintEvent * )
{
	m_vi->m_subWindow->setWindowTitle( m_vi->instrumentTrack()->name()
					+ tr( " - VST plugin control" ) );
}


} // namespace gui


extern "C"
{

// necessary for getting instance out of shared lib
Q_DECL_EXPORT Plugin * lmms_plugin_main( Model *m, void * )
{
	return new VestigeInstrument( static_cast<InstrumentTrack *>( m ) );
}


}


} // namespace lmms
