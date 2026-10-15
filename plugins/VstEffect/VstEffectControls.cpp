/*
 * VstEffectControls.cpp - controls for VST effect plugins
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <QFileInfo>
#include "VstEffectControls.h"

#include <QAction>
#include <QApplication>
#include <QDomElement>
#include <QGridLayout>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>

#include "embed.h"
#include "GuiApplication.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "Song.h"
#include <algorithm>
#include "FileResourceIO.h"
#include <QMutexLocker>
#include <QScopedValueRollback>
#include <stdexcept>
#include "MainWindow.h"
#include "SubWindow.h"
#include "VstEffectControlDialog.h"
#include "VstEffect.h"
#include "VstPlugin.h"

namespace lmms
{


VstEffectControls::VstEffectControls( VstEffect * _eff ) :
	EffectControls( _eff ),
	m_effect( _eff ),
	m_selPresetButton(nullptr),
	m_subWindow( nullptr ),
	m_scrollArea(nullptr),
	m_parameters(this, [this](int index, float value) { if (m_effect->m_plugin) { m_effect->m_plugin->setParam(index, value); } }),
	ctrHandle( nullptr ),
	lastPosInMenu (0),
	m_vstGuiVisible ( true )
//	m_presetLabel ( NULL )
{
}




VstEffectControls::~VstEffectControls()
{
	delete ctrHandle;
	ctrHandle = nullptr;
}




void VstEffectControls::loadSettings(const QDomElement& settings)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (!m_effect->m_plugin) { return; }
	try
	{
		m_vstGuiVisible = settings.attribute("guivisible").toInt();
		m_effect->m_plugin->loadSettings(settings);
		m_parameters.restore(m_effect->m_plugin->parameterDump(), settings);
	}
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }
}

Plugin::NativeProgramPage VstEffectControls::nativePrograms(int offset, int limit)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (!m_effect->m_plugin) { throw std::runtime_error("Load a VST plugin before inspecting programs"); }
	return m_effect->m_plugin->programPage(offset, limit);
}

Plugin::NativePresetInfo VstEffectControls::inspectNativePreset(const QString& path)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (!m_effect->m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_effect->m_plugin->inspectPresetFile(path);
	return result;
}

Plugin::NativePresetInfo VstEffectControls::loadNativePreset(const QString& path)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (!m_effect->m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_effect->m_plugin->loadPresetFile(path);
	m_parameters.synchronize(m_effect->m_plugin->parameterDump());
	return result;
}

Plugin::NativePresetInfo VstEffectControls::saveNativePreset(const QString& path, bool bank)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (!m_effect->m_plugin) { throw std::runtime_error("Load a VST plugin before using native preset files"); }
	const auto result = m_effect->m_plugin->savePresetFile(path, bank);
	return result;
}

bool VstEffectControls::selectNativeProgram(int index, QString& error)
{
	try
	{
		m_parameters.checkOwner();
		if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
		QScopedValueRollback<bool> operation(m_parameterOperation, true);
		FileResourceIO::EventSuspension events;
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		QMutexLocker lock(&m_effect->m_pluginMutex);
		if (!m_effect->m_plugin) { throw std::runtime_error("Load a VST plugin before selecting a program"); }
		m_effect->m_plugin->setProgram(index);
		m_parameters.synchronize(m_effect->m_plugin->parameterDump());
		error.clear();
		return true;
	}
	catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); return false; }
}

void VstEffectControls::refreshParameterModels()
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (m_effect->m_plugin) { m_parameters.synchronize(m_effect->m_plugin->parameterDump()); }
}

void VstEffectControls::saveSettings(QDomDocument& document, QDomElement& settings)
{
	m_parameters.checkOwner();
	if (m_parameterOperation) { throw std::runtime_error("VST parameter operation is already active"); }
	QScopedValueRollback<bool> operation(m_parameterOperation, true);
	settings.setAttribute("plugin", m_effect->m_key.attributes["file"]);
	FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QMutexLocker lock(&m_effect->m_pluginMutex);
	if (m_effect->m_plugin)
	{
		m_effect->m_plugin->saveSettings(document, settings);
		m_parameters.saveSettings(document, settings);
	}
}

int VstEffectControls::controlCount()
{
	// Even a plugin with no generic parameters can have a custom editor.
	return m_effect->m_plugin != nullptr ? 1 : 0;
}



gui::EffectControlDialog* VstEffectControls::createView()
{
	auto dialog = new gui::VstEffectControlDialog( this );
	dialog->togglePluginUI( m_vstGuiVisible );
	return dialog;
}




void VstEffectControls::managePlugin()
{
	if ( m_effect->m_plugin != nullptr && m_subWindow == nullptr ) {
		auto tt = new gui::ManageVSTEffectView(m_effect, this);
		ctrHandle = (QObject *)tt;
	} else if (m_subWindow != nullptr) {
		if (m_subWindow->widget()->isVisible() == false ) {
			m_scrollArea->show();
			m_subWindow->show();
		} else {
			m_scrollArea->hide();
			m_subWindow->hide();
		}
	}
}





void VstEffectControls::savePreset()
{
	if (!m_effect->m_plugin) { return; }
	try
	{
		const auto path = m_effect->m_plugin->presetSavePath();
		if (path.isEmpty()) { return; }
		saveNativePreset(path, QFileInfo(path).suffix().compare("fxb", Qt::CaseInsensitive) == 0);
	}
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VstEffectControls::updateMenu()
{

	// get all presets -
	if ( m_effect->m_plugin != nullptr )
	{
		try { m_effect->m_plugin->loadProgramNames(); }
		catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); return; }
		///QWidget::update();

		const auto list1 = m_effect->m_plugin->allProgramNames();

     		QMenu * to_menu = m_selPresetButton->menu();
    		to_menu->clear();

     		for (int i = 0; i < list1.size(); i++) {
				auto presetAction = new QAction(this);
				connect(presetAction, SIGNAL(triggered()), this, SLOT(selPreset()));

        		presetAction->setText(QString("%1. %2").arg(QString::number(i+1), list1.at(i)));
        		presetAction->setData(i);
			if (i == lastPosInMenu) {
        			presetAction->setIcon(embed::getIconPixmap( "sample_file", 16, 16 ));
			} else  presetAction->setIcon(embed::getIconPixmap( "edit_copy", 16, 16 ));
			to_menu->addAction( presetAction );
     		}

	}

}




void VstEffectControls::openPreset()
{
	if (!m_effect->m_plugin) { return; }
	try
	{
		const auto path = m_effect->m_plugin->presetOpenPath();
		if (path.isEmpty()) { return; }
		loadNativePreset(path);
		lastPosInMenu = nativePrograms(0, 1).current;
		Engine::getSong()->setModified();
	}
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VstEffectControls::changeProgram(int program, bool relative)
{
	try
	{
		const auto page = nativePrograms(0, 1);
		if (page.total == 0) { return; }
		const int index = relative ? static_cast<int>(std::clamp<qint64>(static_cast<qint64>(page.current) + program, 0, page.total - 1)) : program;
		QString error;
		if (!selectNativeProgram(index, error)) { m_effect->collectErrorForUI(error); return; }
		lastPosInMenu = index;
		Engine::getSong()->setModified();
	}
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void VstEffectControls::rollPreset() { changeProgram(1, true); }
void VstEffectControls::rolrPreset() { changeProgram(-1, true); }
void VstEffectControls::selPreset()
{
	if (auto action = qobject_cast<QAction*>(sender())) { changeProgram(action->data().toInt(), false); }
}


void VstEffectControls::paintEvent( QPaintEvent * )
{

}



namespace gui
{


ManageVSTEffectView::ManageVSTEffectView( VstEffect * _eff, VstEffectControls * m_vi ) :
	m_effect( _eff )
{
	m_vi2 = m_vi;
	widget = new QWidget();
	m_vi->m_scrollArea = new QScrollArea(widget);
	l = new QGridLayout( widget );

	m_vi->m_subWindow = getGUI()->mainWindow()->addWindowedWidget(nullptr, Qt::SubWindow |
			Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint);
	m_vi->m_subWindow->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
	m_vi->m_subWindow->setFixedSize( 960, 300);
	m_vi->m_subWindow->setWidget(m_vi->m_scrollArea);
	m_vi->m_subWindow->setWindowTitle( _eff->m_plugin->name() + tr( " - VST parameter control" ) );
	m_vi->m_subWindow->setWindowIcon( PLUGIN_NAME::getIconPixmap( "logo" ) );
	m_vi->m_subWindow->setAttribute(Qt::WA_DeleteOnClose, false);


	l->setContentsMargins( 20, 10, 10, 10 );
	l->setVerticalSpacing( 10 );
	l->setHorizontalSpacing( 23 );

	m_syncButton = new QPushButton( tr( "VST sync" ), widget );
	connect( m_syncButton, SIGNAL( clicked() ), this,
							SLOT( syncPlugin() ) );

	l->addWidget( m_syncButton, 0, 0, 1, 2, Qt::AlignLeft );

	m_displayAutomatedOnly = new QPushButton( tr( "Automated" ), widget );
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
		auto knob = new VstPluginKnob{m_effect->m_plugin.get(), i, model->displayName(), widget};
		m_vstKnobs.push_back(knob);
		knob->setModel(model);
	}
	try { m_effect->m_plugin->loadParameterLabels(); m_effect->m_plugin->loadParameterDisplays(); }
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }

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

	l->setRowStretch( ( int( m_vi->m_parameters.size() / 10 ) + 1 ), 1 );
	l->setColumnStretch( 10, 1 );

	widget->setLayout(l);
	widget->setAutoFillBackground(true);

	m_vi->m_scrollArea->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOn );
	m_vi->m_scrollArea->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
	m_vi->m_scrollArea->setPalette( QApplication::palette( m_vi->m_scrollArea ) );
	m_vi->m_scrollArea->setMinimumHeight( 64 );

	m_vi->m_scrollArea->setWidget( widget );

	m_vi->m_subWindow->show();
}




void ManageVSTEffectView::closeWindow()
{
	m_vi2->m_subWindow->hide();
}




void ManageVSTEffectView::syncPlugin()
{
	try
	{
		m_vi2->refreshParameterModels();
		if (m_effect->m_plugin) { m_effect->m_plugin->loadParameterLabels(); m_effect->m_plugin->loadParameterDisplays(); }
	}
	catch (const std::exception& error) { m_effect->collectErrorForUI(QString::fromUtf8(error.what())); }
}

void ManageVSTEffectView::displayAutomatedOnly()
{
	bool isAuto = QString::compare( m_displayAutomatedOnly->text(), tr( "Automated" ) ) == 0;

	for( int i = 0; i< m_vi2->m_parameters.size(); i++ )
	{

		if( !( m_vi2->m_parameters.at(i)->isAutomated() ||
					m_vi2->m_parameters.at(i)->controllerConnection() ) )
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






ManageVSTEffectView::~ManageVSTEffectView()
{
	if( m_vi2->m_scrollArea != nullptr )
	{
		delete m_vi2->m_scrollArea;
		m_vi2->m_scrollArea = nullptr;
	}

	if( m_vi2->m_subWindow != nullptr )
	{
		m_vi2->m_subWindow->setAttribute( Qt::WA_DeleteOnClose );
		m_vi2->m_subWindow->close();

		if( m_vi2->m_subWindow != nullptr )
		{
			delete m_vi2->m_subWindow;
		}
		m_vi2->m_subWindow = nullptr;
	}
	//delete m_vi2->m_subWindow;
	//m_vi2->m_subWindow = NULL;
}


} // namespace gui

} // namespace lmms
