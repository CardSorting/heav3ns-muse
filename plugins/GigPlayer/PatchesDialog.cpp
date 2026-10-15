/*
 * PatchesDialog.cpp - display GIG patches (based on Sf2 patches_dialog.cpp)
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail/dot/com>
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


#include "PatchesDialog.h"
#include <QHeaderView>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "Song.h"
#include "TrackContainer.h"

namespace lmms::gui
{
namespace
{
class PatchItem final : public QTreeWidgetItem
{
public:
	using QTreeWidgetItem::QTreeWidgetItem;
	bool operator<(const QTreeWidgetItem& other) const override
	{
		const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
		return column == 0 ? text(column).toInt() < other.text(column).toInt() : text(column) < other.text(column);
	}
};
}

class PatchesDialog::ViewGuard
{
public:
	explicit ViewGuard(PatchesDialog* owner) : m_owner(owner), m_banks(owner->m_banksView), m_programs(owner->m_programsView), m_ok(owner->m_acceptButton), m_epoch(owner->m_epoch) { }
	bool alive() const { return m_owner && m_owner->m_epoch == m_epoch && m_banks && m_programs && m_ok
		&& m_owner->m_bankListView == m_banks.data() && m_owner->m_progListView == m_programs.data() && m_owner->m_okButton == m_ok.data(); }
	bool ready(bool controls = true) const { return alive() && m_owner->contextValid(controls) && alive(); }
	void refuse() const { if (m_owner && m_owner->m_epoch == m_epoch) { m_owner->invalidate(); } }
private:
	QPointer<PatchesDialog> m_owner;
	QPointer<QTreeWidget> m_banks, m_programs;
	QPointer<QPushButton> m_ok;
	std::uint64_t m_epoch;
};

PatchesDialog::PatchesDialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags)
{
	setupUi(this);
	m_banksView = m_bankListView; m_programsView = m_progListView; m_acceptButton = m_okButton;
	m_progListView->header()->setStretchLastSection(true);
	m_bankListView->setSortingEnabled(true); m_progListView->setSortingEnabled(true);
	m_bankListView->sortItems(0, Qt::AscendingOrder); m_progListView->sortItems(0, Qt::AscendingOrder);
	connect(m_bankListView, &QTreeWidget::currentItemChanged, this, &PatchesDialog::bankChanged);
	connect(m_progListView, &QTreeWidget::currentItemChanged, this, &PatchesDialog::progChanged);
	connect(m_progListView, &QTreeWidget::itemActivated, this, &PatchesDialog::accept);
	connect(m_okButton, &QPushButton::clicked, this, &PatchesDialog::accept);
	connect(m_cancelButton, &QPushButton::clicked, this, &PatchesDialog::reject);
}

bool PatchesDialog::actorsValid() const
{
	return m_instrument && m_track && !m_track->isRetiring() && m_track->instrument() == m_instrument.data()
		&& m_instrument->isFromTrack(m_track.data()) && m_container && m_track->trackContainer() == m_container.data()
		&& std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) != m_container->tracks().end()
		&& m_song && Engine::getSong() == m_song.data() && m_song->replacementGeneration() == m_project && m_song->projectFileName() == m_projectPath
		&& m_mixer && Engine::mixer() == m_mixer.data() && m_mixer->replacementGeneration() == m_routing
		&& m_engine && Engine::audioEngine() == m_engine.data() && m_engine->outputSampleRate() == m_rate;
}

bool PatchesDialog::contextValid(bool controls)
{
	const QPointer<PatchesDialog> owner(this); const auto epoch = m_epoch; const auto native = m_instrument;
	if (m_invalid || !actorsValid() || !m_bankModel || !m_programModel
		|| (controls && (m_bankModel->storedValue() != m_bank || m_programModel->storedValue() != m_program))) { return false; }
	try
	{
		const auto resources = native->fileResources();
		return owner && owner->m_epoch == epoch && !owner->m_invalid && owner->actorsValid() && owner->m_bankModel && owner->m_programModel
			&& (!controls || (owner->m_bankModel->storedValue() == owner->m_bank && owner->m_programModel->storedValue() == owner->m_program))
			&& resources.size() == 1 && resources[0].identity == owner->m_identity && resources[0].path == owner->m_path && !resources[0].unresolved;
	}
	catch (...) { return false; }
}

bool PatchesDialog::setup(GigInstrument* instrument, const QString& channelName)
{
	const auto previousIdentity = m_identity; // Release retired QObject-backed resources after all scoped continuations.
	const QString caption(channelName); ++m_epoch; const auto epoch = m_epoch; const QPointer<PatchesDialog> owner(this); ViewGuard view(this);
	m_invalid = true; m_closed = false; m_preparing = true; m_committing = false;
	const auto preparing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_preparing = false; } });
	try
	{
		for (const auto& connection : m_connections) { QObject::disconnect(connection); } m_connections.clear();
		m_instrument = instrument; m_track = instrument ? instrument->instrumentTrack() : nullptr; m_container = m_track ? m_track->trackContainer() : nullptr;
		m_song = Engine::getSong(); m_mixer = Engine::mixer(); m_engine = Engine::audioEngine();
		m_project = m_song ? m_song->replacementGeneration() : 0; m_routing = m_mixer ? m_mixer->replacementGeneration() : 0;
		m_projectPath = m_song ? m_song->projectFileName() : QString{}; m_rate = m_engine ? m_engine->outputSampleRate() : 0;
		if (!view.alive() || !actorsValid()) { view.refuse(); return false; }
		auto bank = instrument->childModel("bank"); if (!view.alive() || !actorsValid()) { view.refuse(); return false; } m_bankModel = bank;
		auto program = instrument->childModel("patch"); if (!view.alive() || !actorsValid()) { view.refuse(); return false; } m_programModel = program;
		if (!m_bankModel || !m_programModel) { view.refuse(); return false; }
		m_bank = m_bankModel->storedValue(); m_program = m_programModel->storedValue(); m_programs = instrument->programSnapshot();
		const auto resources = instrument->fileResources();
		if (!view.alive() || !actorsValid() || resources.size() != 1 || !resources[0].identity || resources[0].unresolved || m_programs.empty()) { view.refuse(); return false; }
		m_identity = resources[0].identity; m_path = resources[0].path; m_invalid = false;
		if (!view.ready()) { view.refuse(); return false; }
		const auto invalid = [this, epoch] { if (m_epoch == epoch) { invalidate(); } };
		m_connections.append(connect(instrument, &QObject::destroyed, this, invalid));
		m_connections.append(connect(instrument, &GigInstrument::fileLoading, this, invalid));
		m_connections.append(connect(instrument, &GigInstrument::fileChanged, this, invalid));
		m_connections.append(connect(m_song.data(), &Song::projectFileNameChanged, this, invalid));
		m_connections.append(connect(m_engine.data(), &AudioEngine::sampleRateChanged, this, invalid));
		const auto changed = [this, epoch] { if (m_epoch == epoch && !m_committing) { invalidate(); } };
		m_connections.append(connect(m_bankModel.data(), &AutomatableModel::dataChanged, this, changed));
		m_connections.append(connect(m_programModel.data(), &AutomatableModel::dataChanged, this, changed));
		setWindowTitle(caption + tr(" - GIG patches")); if (!view.ready()) { view.refuse(); return false; }
		m_bankListView->clear(); if (!view.ready()) { view.refuse(); return false; }
		const auto available = m_programs; int previous = -1;
		for (const auto& item : available)
		{
			if (item.bank != previous) { new PatchItem(m_bankListView, {QString::number(item.bank)}); previous = item.bank; if (!view.ready()) { view.refuse(); return false; } }
		}
		int selectedBank = static_cast<int>(m_bank), selected = static_cast<int>(m_program); bool found = false;
		for (const auto& item : available) { if (item.bank == selectedBank && item.program == selected) { found = true; break; } }
		if (!found) { selectedBank = available.front().bank; selected = available.front().program; }
		const auto banks = m_bankListView->findItems(QString::number(selectedBank), Qt::MatchExactly, 0);
		m_bankListView->setCurrentItem(banks.isEmpty() ? nullptr : banks.front()); if (!view.ready()) { view.refuse(); return false; }
		if (!fillPrograms(selectedBank, selected) || !view.ready()) { view.refuse(); return false; }
		m_preparing = false; stabilizeForm(); return view.ready();
	}
	catch (...) { view.refuse(); return false; }
}

bool PatchesDialog::selectedProgram(int& bank, int& program) const
{
	const auto bankItem = m_bankListView->currentItem(), programItem = m_progListView->currentItem(); if (!bankItem || !programItem) { return false; }
	bool bankOk = false, programOk = false; bank = bankItem->text(0).toInt(&bankOk); program = programItem->text(0).toInt(&programOk);
	return bankOk && programOk && std::any_of(m_programs.begin(), m_programs.end(), [=](const Instrument::Program& item) { return item.bank == bank && item.program == program; });
}

bool PatchesDialog::fillPrograms(int bank, int preferredProgram)
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return false; } const auto available = m_programs;
	m_progListView->clear(); if (!view.ready()) { view.refuse(); return false; }
	QSet<int> seen;
	for (const auto& item : available)
	{
		if (item.bank == bank && !seen.contains(item.program)) { seen.insert(item.program); new PatchItem(m_progListView, {QString::number(item.program), item.name.isEmpty() ? tr("<no name>") : item.name}); if (!view.ready()) { view.refuse(); return false; } }
	}
	const auto preferred = m_progListView->findItems(QString::number(preferredProgram), Qt::MatchExactly, 0);
	m_progListView->setCurrentItem(preferred.isEmpty() ? m_progListView->topLevelItem(0) : preferred.front());
	if (!view.ready()) { view.refuse(); return false; } return true;
}

void PatchesDialog::bankChanged()
{
	if (m_preparing) { return; } ViewGuard view(this); if (!view.ready()) { view.refuse(); return; }
	const auto selected = m_bankListView->currentItem(); if (!selected) { stabilizeForm(); return; } const int bank = selected->text(0).toInt();
	const QPointer<PatchesDialog> owner(this); const auto epoch = m_epoch; m_preparing = true;
	const auto preparing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_preparing = false; } });
	if (!fillPrograms(bank, static_cast<int>(m_program)) || !view.ready()) { view.refuse(); return; } m_preparing = false; stabilizeForm();
}

void PatchesDialog::progChanged(QTreeWidgetItem*, QTreeWidgetItem*) { if (!m_preparing) { stabilizeForm(); } }
void PatchesDialog::stabilizeForm()
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; } int bank = 0, program = 0; const bool valid = selectedProgram(bank, program);
	m_okButton->setEnabled(valid); if (!view.ready()) { view.refuse(); }
}

void PatchesDialog::accept()
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; } int bank = 0, program = 0; if (!selectedProgram(bank, program)) { return; }
	if (m_bank == bank && m_program == program) { m_closed = true; m_invalid = true; ++m_epoch; QDialog::accept(); return; }
	const QPointer<PatchesDialog> owner(this); const auto epoch = m_epoch; const auto native = m_instrument; m_committing = true;
	const auto committing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_committing = false; } });
	try
	{
		QString error; const bool accepted = native->selectProgram(bank, program, error);
		if (!view.ready(false)) { view.refuse(); return; }
		if (!accepted || !m_bankModel || !m_programModel || m_bankModel->storedValue() != bank || m_programModel->storedValue() != program) { invalidate(); return; }
		m_bank = bank; m_program = program; m_committing = false; m_closed = true; m_invalid = true; ++m_epoch; QDialog::accept();
	}
	catch (...) { view.refuse(); }
}

void PatchesDialog::invalidate() { if (m_closed) { return; } m_closed = true; m_invalid = true; ++m_epoch; QDialog::reject(); }
void PatchesDialog::reject() { invalidate(); }
} // namespace lmms::gui
