/*
 * EffectRackView.cpp - view for effectChain model
 *
 * Copyright (c) 2006-2007 Danny McRae <khjklujn@netscape.net>
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

#include "EffectRackView.h"

#include <QApplication>
#include <QAction>
#include <QPushButton>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

#include "DeprecationHelper.h"
#include "EffectSelectDialog.h"
#include "EffectView.h"
#include "GroupBox.h"


namespace lmms::gui
{

EffectRackView::EffectRackView( EffectChain* model, QWidget* parent ) :
	QWidget( parent ),
	ModelView( nullptr, this )
{
	auto mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(5, 5, 5, 5);

	m_effectsGroupBox = new GroupBox( tr( "EFFECTS CHAIN" ) );
	mainLayout->addWidget( m_effectsGroupBox );

	auto effectsLayout = new QVBoxLayout(m_effectsGroupBox);
	effectsLayout->setSpacing( 0 );
	effectsLayout->setContentsMargins( 2, m_effectsGroupBox->titleBarHeight() + 2, 2, 2 );

	m_scrollArea = new QScrollArea;
	m_scrollArea->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOn );
	m_scrollArea->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
	m_scrollArea->setPalette( QApplication::palette( m_scrollArea ) );
	m_scrollArea->setFrameStyle( QFrame::NoFrame );
	m_scrollArea->setWidget( new QWidget );

	effectsLayout->addWidget( m_scrollArea );

	auto addButton = new QPushButton;
	addButton->setText( tr( "Add effect" ) );
	addButton->setFocusPolicy(Qt::NoFocus);

	effectsLayout->addWidget( addButton );

	connect( addButton, SIGNAL(clicked()), this, SLOT(addEffect()));


	m_lastY = 0;

	setModel( model );
}



EffectRackView::~EffectRackView()
{
	clearViews();
}





void EffectRackView::clearViews()
{
	while( m_effectViews.size() )
	{
		EffectView * e = m_effectViews[m_effectViews.size() - 1];
		m_effectViews.pop_back();
		delete e;
	}
}




void EffectRackView::moveUp( EffectView* view )
{
	if (!fxChain() || !m_effectViews.contains(view)) { return; }
	fxChain()->moveUp(view->effect()); // Native dataChanged updates the view order.
}




void EffectRackView::moveDown( EffectView* view )
{
	if (!fxChain() || !m_effectViews.contains(view)) { return; }
	fxChain()->moveDown(view->effect());
}




void EffectRackView::deletePlugin( EffectView* view )
{
	if (!fxChain() || !m_effectViews.contains(view)) { return; }
	// The chain notification retires the view while its effect is still alive.
	// A refused detach leaves both the native effect and its view intact.
	auto detached = fxChain()->takeEffect(view->effect());
	if (detached) { detached.release()->deleteLater(); }
}




void EffectRackView::update()
{
	if (!fxChain()) { clearViews(); return; }
	QWidget * w = m_scrollArea->widget();
	auto unused = m_effectViews;
	QVector<EffectView*> ordered;
	ordered.reserve(static_cast<int>(fxChain()->effects().size()));

	for (const auto effect : fxChain()->effects())
	{
		const auto found = std::find_if(unused.begin(), unused.end(), [effect](const auto view) { return view->model() == effect; });
		if (found != unused.end())
		{
			ordered.append(*found);
			unused.erase(found);
		}
		else
		{
			auto view = new EffectView(effect, w);
			connect(view, &EffectView::movedUp, this, &EffectRackView::moveUp);
			connect(view, &EffectView::movedDown, this, &EffectRackView::moveDown);
			connect(view, &EffectView::deletedPlugin, this, [this, pending = QPointer<EffectView>(view)] {
				if (pending) { deletePlugin(pending.data()); }
			}, Qt::QueuedConnection);

			QAction* moveUpAction = new QAction(view);
			moveUpAction->setShortcut(keySequence(Qt::Key_Up, Qt::AltModifier));
			moveUpAction->setShortcutContext(Qt::WidgetShortcut);
			connect(moveUpAction, &QAction::triggered, view, &EffectView::moveUp);
			view->addAction(moveUpAction);

			QAction* moveDownAction = new QAction(view);
			moveDownAction->setShortcut(keySequence(Qt::Key_Down, Qt::AltModifier));
			moveDownAction->setShortcutContext(Qt::WidgetShortcut);
			connect(moveDownAction, &QAction::triggered, view, &EffectView::moveDown);
			view->addAction(moveDownAction);

			view->show();
			ordered.append(view);
		}
	}

	m_effectViews = std::move(ordered);
	for (const auto view : unused) { delete view; }

	const int EffectViewMargin = 3;
	m_lastY = EffectViewMargin;

	for (const auto view : m_effectViews)
	{
		view->move(EffectViewMargin, m_lastY);
		m_lastY += view->height();
	}

	w->setFixedSize(EffectView::DEFAULT_WIDTH + 2 * EffectViewMargin, m_lastY);

	QWidget::update();
}




void EffectRackView::addEffect()
{
	const QPointer<EffectRackView> owner(this);
	QPointer<EffectChain> chain = fxChain();
	if (!chain) { return; }
	const QPointer<EffectSelectDialog> dialog(new EffectSelectDialog(this));
	const auto cleanupDialog = qScopeGuard([&] { delete dialog.data(); }); dialog->exec();

	if (!owner || !dialog || dialog->result() == QDialog::Rejected || !chain || owner->fxChain() != chain.data())
	{
		return;
	}

	const QPointer<Effect> fx(dialog->instantiateSelectedPlugin(chain.data()));
	const auto cleanupEffect = qScopeGuard([&] { if (fx && !fx->effectChain()) { delete fx.data(); } });
	if (!owner || !chain || !fx || owner->fxChain() != chain.data() || fx->effectChain()) { return; }
	auto candidate = std::unique_ptr<Effect>(fx.data());
	if (!chain->appendEffect(candidate)) { return; }
	if (!owner || !chain || !fx || owner->fxChain() != chain.data()) { return; }

	// Find the effectView, and show the controls
	for (const auto& effectView : owner->m_effectViews)
	{
		if (effectView->effect() == fx)
		{
			effectView->editControls();
			break;
		}
	}


}




void EffectRackView::modelChanged()
{
	if (!fxChain()) { clearViews(); return; }
	m_effectsGroupBox->setModel( &fxChain()->m_enabledModel );
	connect( fxChain(), SIGNAL(aboutToClear()), this, SLOT(clearViews()));
	update();
}




QSize EffectRackView::sizeHint() const
{
	// Use the formula from InstrumentTrackWindow.cpp
	return QSize{EffectRackView::DEFAULT_WIDTH, 254 /* INSTRUMENT_HEIGHT */ - 4 - 1};
}




} // namespace lmms::gui
