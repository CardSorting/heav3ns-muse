/*
 * MixerView.cpp - effect-mixer-view for LMMS
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

#include "MixerView.h"
#include <algorithm>

#include <QHBoxLayout>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QStyle>
#include <QKeyEvent>
#include <QStackedLayout>
#include <QStackedWidget>

#include "EffectRackView.h"
#include "Engine.h"
#include "Fader.h"
#include "GuiApplication.h"
#include "Knob.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "Mixer.h"
#include "MixerChannelView.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "SendButtonIndicator.h"
#include "Song.h"
#include "SubWindow.h"
#include "TrackContainer.h" // For TrackContainer::TrackList typedef
#include "embed.h"

namespace lmms::gui
{


MixerView::MixerView(Mixer* mixer) :
	QWidget(),
	ModelView(nullptr, this),
	SerializingObjectHook(),
	m_mixer(mixer)
{
	mixer->setHook(this);

	//QPalette pal = palette();
	//pal.setColor(QPalette::Window, QColor(72, 76, 88));
	//setPalette(pal);

	setAutoFillBackground(true);

	setWindowTitle(tr("Mixer"));
	setWindowIcon(embed::getIconPixmap("mixer"));

	// main-layout
	auto ml = new QHBoxLayout{this};

	// Set margins
	ml->setContentsMargins(0, 4, 0, 0);

	// Channel area
	m_channelAreaWidget = new QWidget;
	chLayout = new QHBoxLayout(m_channelAreaWidget);
	chLayout->setSizeConstraint(QLayout::SetMinimumSize);
	chLayout->setSpacing(0);
	chLayout->setContentsMargins(0, 0, 0, 0);
	chLayout->setAlignment(Qt::AlignLeft);
	m_channelAreaWidget->setLayout(chLayout);

	// create rack layout before creating the first channel
	m_racksWidget = new QWidget;
	m_racksLayout = new QStackedLayout(m_racksWidget);
	m_racksLayout->setContentsMargins(0, 0, 0, 0);
	m_racksWidget->setLayout(m_racksLayout);

	// add master channel
	m_mixerChannelViews.resize(mixer->numChannels());
	MixerChannelView * masterView = new MixerChannelView(this, this, 0);
	m_mixerChannelViews[0] = masterView;
	connectToSoloAndMute(0);

	m_racksLayout->addWidget(m_mixerChannelViews[0]->m_effectRackView);

	ml->addWidget(masterView, 0);

	auto mixerChannelSize = masterView->sizeHint();

	// add mixer channels
	for (int i = 1; i < m_mixerChannelViews.size(); ++i)
	{
		m_mixerChannelViews[i] = new MixerChannelView(m_channelAreaWidget, this, i);
		connectToSoloAndMute(i);
		chLayout->addWidget(m_mixerChannelViews[i]);
		m_racksLayout->addWidget(m_mixerChannelViews[i]->m_effectRackView);
	}

	// add the scrolling section to the main layout
	// class solely for scroll area to pass key presses down
	class ChannelArea : public QScrollArea
	{
		public:
			ChannelArea(QWidget* parent, MixerView* mv) :
				QScrollArea(parent), m_mv(mv) {}
			~ChannelArea() override = default;
			void keyPressEvent(QKeyEvent* e) override
			{
				m_mv->keyPressEvent(e);
			}
		private:
			MixerView* m_mv;
	};
	channelArea = new ChannelArea(this, this);
	channelArea->setWidget(m_channelAreaWidget);
	channelArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	channelArea->setFrameStyle(QFrame::NoFrame);
	channelArea->setMinimumWidth(mixerChannelSize.width() * 6);
	channelArea->setWidgetResizable(true);

	int const scrollBarExtent = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
	channelArea->setMinimumHeight(mixerChannelSize.height() + scrollBarExtent);

	ml->addWidget(channelArea, 1);

	// show the add new mixer channel button
	auto newChannelBtn = new QPushButton(embed::getIconPixmap("new_channel"), QString(), this);
	newChannelBtn->setObjectName("newChannelBtn");
	newChannelBtn->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);
	newChannelBtn->setFixedWidth(mixerChannelSize.width());
	newChannelBtn->setFocusPolicy(Qt::NoFocus);
	connect(newChannelBtn, SIGNAL(clicked()), this, SLOT(addNewChannel()));
	ml->addWidget(newChannelBtn, 0);


	// add the stacked layout for the effect racks of mixer channels
	ml->addWidget(m_racksWidget);

	setCurrentMixerChannel(m_mixerChannelViews[0]);

	updateGeometry();

	connect(mixer, &Mixer::channelMoved, this, &MixerView::channelMoved);

	layout()->setSizeConstraint(QLayout::SetMinimumSize);

	// The presentation can also be attached to an already initialized engine
	// without constructing the complete application window hierarchy.
	if (auto gui = getGUI())
	{
		auto mainWindow = gui->mainWindow();
		connect(mainWindow, &MainWindow::periodicUpdate, this, &MixerView::updateFaders);
		mainWindow->addWindowedWidget(this);
		parentWidget()->setAttribute(Qt::WA_DeleteOnClose, false);
		parentWidget()->move(5, 310);
	}

	// we want to receive dataChanged-signals in order to update
	setModel(mixer);
}




int MixerView::addNewChannel()
{
	if (m_changingViews) { return -1; }
	QScopedValueRollback<bool> changing(m_changingViews, true);
	// add new mixer channel and redraw the form.
	Mixer * mix = getMixer();

	int newChannelIndex = mix->createChannel();
	m_mixerChannelViews.push_back(new MixerChannelView(m_channelAreaWidget, this, newChannelIndex));
	connectToSoloAndMute(newChannelIndex);
	chLayout->addWidget(m_mixerChannelViews[newChannelIndex]);
	m_racksLayout->addWidget(m_mixerChannelViews[newChannelIndex]->m_effectRackView);

	updateMixerChannel(newChannelIndex);

	return newChannelIndex;
}


void MixerView::refreshDisplay()
{
	if (m_changingViews) { return; }
	QScopedValueRollback<bool> changing(m_changingViews, true);
	refreshDisplayImpl();
}

void MixerView::refreshDisplayImpl()
{
	m_currentMixerChannel = m_mixerChannelViews[0];
	// delete all views and re-add them
	for (int i = 1; i<m_mixerChannelViews.size(); ++i)
	{
		// First disconnect from the solo/mute models.
		disconnectFromSoloAndMute(i);

		auto * mixerChannelView = m_mixerChannelViews[i];
		chLayout->removeWidget(mixerChannelView);
		m_racksLayout->removeWidget(mixerChannelView->m_effectRackView);

		delete mixerChannelView;
	}
	m_channelAreaWidget->adjustSize();

	// re-add the views
	m_mixerChannelViews.resize(getMixer()->numChannels());
	for (int i = 1; i < m_mixerChannelViews.size(); ++i)
	{
		m_mixerChannelViews[i] = new MixerChannelView(m_channelAreaWidget, this, i);
		connectToSoloAndMute(i);

		chLayout->addWidget(m_mixerChannelViews[i]);
		m_racksLayout->addWidget(m_mixerChannelViews[i]->m_effectRackView);
	}

	// set selected mixer channel to 0
	setCurrentMixerChannel(0);

	// update all mixer lines
	for (int i = 0; i < m_mixerChannelViews.size(); ++i)
	{
		updateMixerChannel(i);
	}

}


void MixerView::saveSettings(QDomDocument& doc, QDomElement& domElement)
{
	MainWindow::saveWidgetState(this, domElement);
}




void MixerView::loadSettings(const QDomElement& domElement)
{
	MainWindow::restoreWidgetState(this, domElement);
}





void MixerView::toggledSolo()
{
	updateAllMixerChannels();
}


void MixerView::toggledMute()
{
	updateAllMixerChannels();
}

Mixer* MixerView::getMixer() const
{
	return m_mixer;
}

void MixerView::updateAllMixerChannels()
{
	for (int i = 0; i < m_mixerChannelViews.size(); ++i)
	{
		m_mixerChannelViews[i]->update();
	}
}

void MixerView::connectToSoloAndMute(int channelIndex)
{
	disconnectFromSoloAndMute(channelIndex);
	const auto view = m_mixerChannelViews[channelIndex];
	auto * mixerChannel = getMixer()->mixerChannel(channelIndex);

	view->m_muteConnection = connect(&mixerChannel->m_muteModel, &BoolModel::dataChanged, this, &MixerView::toggledMute, Qt::DirectConnection);
	view->m_soloConnection = connect(&mixerChannel->m_soloModel, &BoolModel::dataChanged, this, &MixerView::toggledSolo, Qt::DirectConnection);
}

void MixerView::disconnectFromSoloAndMute(int channelIndex)
{
	const auto view = m_mixerChannelViews[channelIndex];
	disconnect(view->m_muteConnection);
	disconnect(view->m_soloConnection);
}


void MixerView::setCurrentMixerChannel(MixerChannelView* channel)
{
	// select
	m_currentMixerChannel = channel;
	m_racksLayout->setCurrentWidget(m_mixerChannelViews[channel->channelIndex()]->m_effectRackView);

	// set up send knob
	for (int i = 0; i < m_mixerChannelViews.size(); ++i)
	{
		updateMixerChannel(i);
	}
}


void MixerView::updateMixerChannel(int index)
{
	const auto mixer = getMixer();

	const auto currentIndex = m_currentMixerChannel->channelIndex();
	const auto thisLine = m_mixerChannelViews[index];
	thisLine->setToolTip(getMixer()->mixerChannel(index)->m_name);

	const auto sendModelCurrentToThis = mixer->channelSendModel(currentIndex, index);
	if (sendModelCurrentToThis == nullptr)
	{
		thisLine->m_sendKnob->setVisible(false);
		thisLine->m_sendArrow->setVisible(false);
	}
	else
	{
		thisLine->m_sendKnob->setVisible(true);
		thisLine->m_sendKnob->setModel(sendModelCurrentToThis);
		thisLine->m_sendArrow->setVisible(true);
	}

	const auto sendModelThisToCurrent = mixer->channelSendModel(index, currentIndex);
	if (sendModelThisToCurrent)
	{
		thisLine->m_receiveArrowOrSendButton->setVisible(true);
		thisLine->m_receiveArrowOrSendButton->setCurrentIndex(thisLine->m_receiveArrowStackedIndex);
	}
	else
	{
		thisLine->m_receiveArrowOrSendButton->setVisible(!mixer->isInfiniteLoop(currentIndex, index));
		thisLine->m_receiveArrowOrSendButton->setCurrentIndex(thisLine->m_sendButtonStackedIndex);
	}

	thisLine->m_sendButton->updateLightStatus();
	thisLine->m_renameLineEdit->setText(thisLine->elideName(thisLine->mixerChannel()->m_name));
	thisLine->update();
}


void MixerView::deleteChannel(int index)
{
	// can't delete master
	if (m_changingViews || index <= 0 || index >= m_mixerChannelViews.size()) { return; }
	QScopedValueRollback<bool> changing(m_changingViews, true);
	deleteChannelImpl(index);
}

void MixerView::deleteChannelImpl(int index)
{

	// remember selected line
	auto selected = m_currentMixerChannel;
	auto removed = m_mixerChannelViews[index];
	// Focus loss while retiring the widgets must not rename a shifted native
	// channel or access the removed final index.
	disconnect(removed->m_renameLineEdit, &QLineEdit::editingFinished, removed, &MixerChannelView::renameFinished);

	Mixer* mixer = getMixer();
	// delete the real channel
	if (!mixer->deleteChannel(index))
	{
		connect(removed->m_renameLineEdit, &QLineEdit::editingFinished, removed, &MixerChannelView::renameFinished, Qt::UniqueConnection);
		return;
	}
	disconnectFromSoloAndMute(index);

	chLayout->removeWidget(removed);
	m_racksLayout->removeWidget(removed->m_effectRackView);
	// delete MixerChannelView later to prevent a crash when deleting from context menu
	removed->m_effectRackView->hide();
	removed->m_effectRackView->setEnabled(false);
	removed->hide();
	removed->setEnabled(false);
	removed->deleteLater();
	m_channelAreaWidget->adjustSize();

	// make sure every channel knows what index it is
	for (int i = index + 1; i < m_mixerChannelViews.size(); ++i)
	{
		m_mixerChannelViews[i]->setChannelIndex(i - 1);
	}
	m_mixerChannelViews.remove(index);

	// Retain the selected channel object when an earlier channel was removed.
	if (selected == removed) { setCurrentMixerChannel(std::min(index, static_cast<int>(m_mixerChannelViews.size()) - 1)); }
	else { setCurrentMixerChannel(selected); }

}

void MixerView::deleteUnusedChannels()
{
	Mixer* mix = getMixer();

	// Check all channels except master, delete those with no incoming sends
	for (int i = m_mixerChannelViews.size() - 1; i > 0; --i)
	{
		if (!mix->isChannelInUse(i))
		{
			deleteChannel(i);
		}
	}
}

void MixerView::moveChannelLeft(int index)
{
	m_mixer->moveChannelLeft(index);
}

void MixerView::channelMoved(int from, int to)
{
	if (from <= 0 || to <= 0 || from >= m_mixerChannelViews.size() || to >= m_mixerChannelViews.size()) { return; }
	auto view = m_mixerChannelViews[from];
	chLayout->removeWidget(view);
	m_mixerChannelViews.move(from, to);
	chLayout->insertWidget(to - 1, view); // Master is outside the scrolling layout.
	for (int index = std::min(from, to); index <= std::max(from, to); ++index) { m_mixerChannelViews[index]->setChannelIndex(index); }
	setCurrentMixerChannel(m_currentMixerChannel);
}



void MixerView::moveChannelRight(int index)
{
	m_mixer->moveChannelRight(index);
}


void MixerView::renameChannel(int index)
{
	m_mixerChannelViews[index]->renameChannel();
}



void MixerView::keyPressEvent(QKeyEvent * e)
{
	auto adjustCurrentFader = [this](const Qt::KeyboardModifiers& modifiers, Fader::AdjustmentDirection direction)
	{
		auto* mixerChannel = currentMixerChannel();

		if (mixerChannel)
		{
			mixerChannel->fader()->adjust(modifiers, direction);
		}
	};

	switch(e->key())
	{
		case Qt::Key_Delete:
			deleteChannel(m_currentMixerChannel->channelIndex());
			break;
		case Qt::Key_Left:
			if (e->modifiers() & Qt::AltModifier)
			{
				moveChannelLeft(m_currentMixerChannel->channelIndex());
			}
			else
			{
				// select channel to the left
				setCurrentMixerChannel(m_currentMixerChannel->channelIndex() - 1);
			}
			break;
		case Qt::Key_Right:
			if (e->modifiers() & Qt::AltModifier)
			{
				moveChannelRight(m_currentMixerChannel->channelIndex());
			}
			else
			{
				// select channel to the right
				setCurrentMixerChannel(m_currentMixerChannel->channelIndex() + 1);
			}
			break;
		case Qt::Key_Up:
		case Qt::Key_Plus:
			adjustCurrentFader(e->modifiers(), Fader::AdjustmentDirection::Up);
			break;
		case Qt::Key_Down:
		case Qt::Key_Minus:
			adjustCurrentFader(e->modifiers(), Fader::AdjustmentDirection::Down);
			break;
		case Qt::Key_Insert:
			if (e->modifiers() & Qt::ShiftModifier)
			{
				addNewChannel();
			}
			break;
		case Qt::Key_Enter:
		case Qt::Key_Return:
		case Qt::Key_F2:
			renameChannel(m_currentMixerChannel->channelIndex());
			break;
		default:
			e->ignore();
			break;
	}
}



void MixerView::setCurrentMixerChannel(int channel)
{
	if (channel >= 0 && channel < m_mixerChannelViews.size())
	{
		setCurrentMixerChannel(m_mixerChannelViews[channel]);
	}
}



void MixerView::clear()
{
	if (m_changingViews) { return; }
	QScopedValueRollback<bool> changing(m_changingViews, true);
	for (auto i = m_mixerChannelViews.size() - 1; i > 0; --i) { deleteChannelImpl(i); }
	if (!getMixer()->clearChannel(0)) { return; }

	m_mixerChannelViews[0]->reset();

	refreshDisplayImpl();
}




void MixerView::updateFaders()
{
	Mixer * m = getMixer();

	for (int i = 0; i < m_mixerChannelViews.size(); ++i)
	{
		const float opl = m_mixerChannelViews[i]->m_fader->getPeak_L();
		const float opr = m_mixerChannelViews[i]->m_fader->getPeak_R();
		const float fallOff = 1.25;
		if (m->mixerChannel(i)->m_peakLeft >= opl/fallOff)
		{
			m_mixerChannelViews[i]->m_fader->setPeak_L(m->mixerChannel(i)->m_peakLeft);
			// Set to -1 so later we'll know if this value has been refreshed yet.
			m->mixerChannel(i)->m_peakLeft = -1;
		}
		else if (m->mixerChannel(i)->m_peakLeft != -1)
		{
			m_mixerChannelViews[i]->m_fader->setPeak_L(opl/fallOff);
		}

		if (m->mixerChannel(i)->m_peakRight >= opr/fallOff)
		{
			m_mixerChannelViews[i]->m_fader->setPeak_R(m->mixerChannel(i)->m_peakRight);
			// Set to -1 so later we'll know if this value has been refreshed yet.
			m->mixerChannel(i)->m_peakRight = -1;
		}
		else if (m->mixerChannel(i)->m_peakRight != -1)
		{
			m_mixerChannelViews[i]->m_fader->setPeak_R(opr/fallOff);
		}
	}
}


} // namespace lmms::gui
