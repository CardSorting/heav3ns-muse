/*
 * ControllerRackView.cpp - view for song's controllers
 *
 * Copyright (c) 2008-2009 Paul Giblock <drfaygo/at/gmail.com>
 * Copyright (c) 2010-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "ControllerRackView.h"

#include <QApplication>
#include <QAction>
#include <QPushButton>
#include <QScrollArea>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QPointer>
#include <QDebug>
#include <QScopeGuard>
#include <algorithm>
#include <exception>
#include <iterator>

#include "ControllerView.h"
#include "DeprecationHelper.h"
#include "embed.h"
#include "GuiApplication.h"
#include "LfoController.h"
#include "MainWindow.h"
#include "Song.h"
#include "SubWindow.h"

namespace lmms::gui
{

namespace
{
template<typename Callback>
void receiveControllerNotification(const QPointer<Song>& song, Callback&& callback) noexcept
{
	try { callback(); }
	catch (...)
	{
		if (!song || !song->reportControllerNotificationFailure(std::current_exception()))
		{ qWarning("Native controller rack receiver failed outside a lifecycle notification"); }
	}
}
}


ControllerRackView::ControllerRackView()
	: QWidget{}
	, m_nextIndex{0}
{
	setWindowIcon( embed::getIconPixmap( "controller" ) );
	setWindowTitle( tr( "Controller Rack" ) );

	m_scrollArea = new QScrollArea( this );
	m_scrollArea->setPalette( QApplication::palette( m_scrollArea ) );
	m_scrollArea->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );

	auto scrollAreaWidget = new QWidget(m_scrollArea);
	m_scrollAreaLayout = new QVBoxLayout( scrollAreaWidget );
	m_scrollAreaLayout->addStretch();
	scrollAreaWidget->setLayout( m_scrollAreaLayout );

	m_scrollArea->setWidget( scrollAreaWidget );
	m_scrollArea->setWidgetResizable( true );

	m_addButton = new QPushButton( this );
	m_addButton->setText( tr( "Add" ) );

	const QPointer<Song> song(Engine::getSong());
	connect(m_addButton, &QPushButton::clicked, this, [this, song] {
		receiveControllerNotification(song, [this] { addController(); });
	});
	connect(song.data(), &Song::controllerAdded, this, [this, song](Controller* controller) {
		receiveControllerNotification(song, [this, controller] { addController(controller); });
	});
	connect(song.data(), &Song::controllerRemoved, this, [this, song](Controller* controller) {
		receiveControllerNotification(song, [this, controller] { removeController(controller); });
	});
	connect(song.data(), &Song::controllerRosterChanged, this, [this, song] {
		receiveControllerNotification(song, [this] { removeRetiredControllers(); });
	});

	auto layout = new QVBoxLayout();
	layout->addWidget( m_scrollArea );
	layout->addWidget( m_addButton );
	this->setLayout( layout );

	SubWindow* subWin = getGUI()->mainWindow()->addWindowedWidget(this);

	setFixedWidth(350);
	setMinimumHeight(200);

	// No maximize button
	Qt::WindowFlags flags = subWin->windowFlags();
	flags &= ~Qt::WindowMaximizeButtonHint;
	subWin->setWindowFlags( flags );
	
	subWin->setAttribute( Qt::WA_DeleteOnClose, false );
	subWin->move( 680, 310 );
}




void ControllerRackView::saveSettings( QDomDocument & _doc,
							QDomElement & _this )
{
	MainWindow::saveWidgetState( this, _this );
}




void ControllerRackView::loadSettings( const QDomElement & _this )
{
	MainWindow::restoreWidgetState( this, _this );
}




void ControllerRackView::deleteController( ControllerView * _view )
{
	if (!_view || std::find(m_controllerViews.begin(), m_controllerViews.end(), _view) == m_controllerViews.end()) { return; }
	const QPointer<ControllerRackView> owner(this);
	const QPointer<Controller> c(_view->getController());
	if (!c) { return; }

	if( c->connectionCount() > 0 )
	{
		QMessageBox msgBox;
		msgBox.setIcon( QMessageBox::Question );
		msgBox.setWindowTitle( tr("Confirm Delete") );
		msgBox.setText( tr("Confirm delete? There are existing connection(s) "
				"associated with this controller. There is no way to undo.") );
		msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
		if( msgBox.exec() != QMessageBox::Ok )
		{
			return;
		}
	}

	if (!owner || !c) { return; }
	Song * song = Engine::getSong();
	if (song && song->containsController(c.data())) { song->removeController(c.data()); }
}

void ControllerRackView::moveUp(ControllerView* view)
{
	const auto storedView = std::find(m_controllerViews.begin(), m_controllerViews.end(), view);
	if (storedView == m_controllerViews.end() || storedView == m_controllerViews.begin()) { return; }

	const auto index = std::distance(m_controllerViews.begin(), storedView);

	std::swap(m_controllerViews[index - 1], m_controllerViews[index]);
	m_scrollAreaLayout->removeWidget(view);
	m_scrollAreaLayout->insertWidget(index - 1, view);
}

void ControllerRackView::moveDown(ControllerView* view)
{
	const auto storedView = std::find(m_controllerViews.begin(), m_controllerViews.end(), view);
	if (storedView == m_controllerViews.end() || std::next(storedView) == m_controllerViews.end()) { return; }
	moveUp(*std::next(storedView));
}

void ControllerRackView::addController(Controller* controller)
{
	// An earlier observer may already have removed/deleted this signal's
	// payload. Query roster identity before accessing the native object.
	if (!Engine::getSong()->containsController(controller)) { return; }
	if (controller->isRetiring()) { return; }
	for (const auto view : m_controllerViews) { if (view->getController() == controller) { return; } }
	QWidget * scrollAreaWidget = m_scrollArea->widget();

	auto controllerView = new ControllerView(controller, scrollAreaWidget);
	const QPointer<ControllerView> pending(controllerView);
	auto cleanup = qScopeGuard([pending] { delete pending.data(); });

	connect(controllerView, &ControllerView::movedUp, this, &ControllerRackView::moveUp);
	connect(controllerView, &ControllerView::movedDown, this, &ControllerRackView::moveDown);
	const QPointer<ControllerView> weakView(controllerView);
	const QPointer<Song> song(Engine::getSong());
	connect(controllerView, &ControllerView::removedController, this, [this, weakView, song] {
		receiveControllerNotification(song, [this, weakView] { if (weakView) { deleteController(weakView.data()); } });
	}, Qt::QueuedConnection);

	auto moveUpAction = new QAction(controllerView);
	moveUpAction->setShortcut(keySequence(Qt::Key_Up, Qt::AltModifier));
	moveUpAction->setShortcutContext(Qt::WidgetShortcut);
	connect(moveUpAction, &QAction::triggered, controllerView, &ControllerView::moveUp);
	controllerView->addAction(moveUpAction);

	auto moveDownAction = new QAction(controllerView);
	moveDownAction->setShortcut(keySequence(Qt::Key_Down, Qt::AltModifier));
	moveDownAction->setShortcutContext(Qt::WidgetShortcut);
	connect(moveDownAction, &QAction::triggered, controllerView, &ControllerView::moveDown);
	controllerView->addAction(moveDownAction);


	m_scrollAreaLayout->insertWidget( m_nextIndex, controllerView );
	m_controllerViews.append( controllerView );

	++m_nextIndex;
	cleanup.dismiss();
}




void ControllerRackView::removeController(Controller* removedController)
{
	// A deleted signal payload can have been replaced at the same address.
	if (Engine::getSong()->containsController(removedController)) { return; }
	ControllerView * viewOfRemovedController = 0;

	QVector<ControllerView *>::const_iterator end = m_controllerViews.end();
	for ( QVector<ControllerView *>::const_iterator it = m_controllerViews.begin(); it != end; ++it)
	{
		ControllerView *currentControllerView = *it;
		if ( currentControllerView->getController() == removedController )
		{
			viewOfRemovedController = currentControllerView;
			break;
		}
	}

	if (viewOfRemovedController )
	{
		m_controllerViews.erase( std::find( m_controllerViews.begin(),
					m_controllerViews.end(), viewOfRemovedController ) );

		--m_nextIndex;
		delete viewOfRemovedController;
	}
}

void ControllerRackView::removeRetiredControllers()
{
	const QPointer<ControllerRackView> owner(this);
	auto index = m_controllerViews.size();
	while (owner && index > 0)
	{
		index = std::min(index, owner->m_controllerViews.size());
		if (!index) { break; }
		const auto view = owner->m_controllerViews[--index];
		if (!view->getController()) { owner->m_controllerViews.removeAt(index); --owner->m_nextIndex; delete view; }
	}
}




void ControllerRackView::addController()
{
	// TODO: Eventually let the user pick from available controller types

	Engine::getSong()->addController( new LfoController( Engine::getSong() ) );

	// fix bug which always made ControllerRackView loose focus when adding
	// new controller
	setFocus();
}

} // namespace lmms::gui
