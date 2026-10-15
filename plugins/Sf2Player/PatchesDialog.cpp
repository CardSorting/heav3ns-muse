/*
 * PatchesDialog.cpp - display sf2 patches
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
#include <QKeyEvent>
#include <QRegularExpression>
#include "embed.h"
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
bool restorePreview(const QPointer<Sf2Instrument>& native, const Sf2Instrument::ProgramPreviewToken& token) noexcept
{
	if (!native || !token) { return false; }
	try { QString error; return native->restoreProgramPreview(token, error); } catch (...) { return false; }
}

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
	explicit ViewGuard(PatchesDialog* owner) : m_owner(owner), m_banks(owner->m_banksView), m_programs(owner->m_programsView), m_ok(owner->m_acceptButton), m_filter(owner->m_filter), m_source(owner->m_source), m_proxy(owner->m_proxy), m_selection(owner->m_selection), m_epoch(owner->m_epoch) { }
	bool alive() const { return m_owner && m_owner->m_epoch == m_epoch && m_banks && m_programs && m_ok
		&& m_filter && m_source && m_proxy && m_selection && m_owner->m_bankListView == m_banks.data() && m_owner->m_progListView == m_programs.data()
		&& m_owner->m_okButton == m_ok.data() && m_owner->m_filterEdit == m_filter.data() && m_programs->model() == m_proxy.data()
		&& m_proxy->sourceModel() == m_source.data() && m_programs->selectionModel() == m_selection.data(); }
	bool ready(bool controls = true) const { return alive() && m_owner->contextValid(controls) && alive(); }
	void refuse() const { if (m_owner && m_owner->m_epoch == m_epoch) { m_owner->invalidate(); } }
private:
	QPointer<PatchesDialog> m_owner;
	QPointer<QTreeWidget> m_banks;
	QPointer<QTableView> m_programs;
	QPointer<QLineEdit> m_filter;
	QPointer<QStandardItemModel> m_source;
	QPointer<QSortFilterProxyModel> m_proxy;
	QPointer<QItemSelectionModel> m_selection;
	QPointer<QPushButton> m_ok;
	std::uint64_t m_epoch;
};

PatchesDialog::PatchesDialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags)
{
	setupUi(this); m_source = new QStandardItemModel(this); m_proxy = new QSortFilterProxyModel(this);
	m_banksView = m_bankListView; m_programsView = m_progListView; m_acceptButton = m_okButton; m_filter = m_filterEdit;
	// Configure bank list view
	auto bankHeader = m_bankListView->header();
	bankHeader->setSectionResizeMode(0, QHeaderView::Stretch);
	bankHeader->setStretchLastSection(true);
	bankHeader->resizeSection(0, 30);

	m_splitter->setStretchFactor(0, 2);
	m_splitter->setStretchFactor(1, 6);

	// Configure program list models
	m_source->setHorizontalHeaderLabels({tr("Patch"), tr("Name")});
	m_proxy->setSourceModel(m_source.data());
	m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
	m_proxy->setFilterKeyColumn(1); // "Name" column
	m_proxy->setDynamicSortFilter(true);

	// Configure program list view
	m_progListView->setModel(m_proxy.data());
	m_progListView->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_progListView->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_progListView->setSelectionMode(QAbstractItemView::SingleSelection);
	m_progListView->setSortingEnabled(true);
	m_progListView->sortByColumn(0, Qt::AscendingOrder); // Numeric patch order

	constexpr int RowHeight = 18;
	auto progVHeader = m_progListView->verticalHeader();
	progVHeader->setSectionResizeMode(QHeaderView::Fixed);
	progVHeader->setMinimumSectionSize(RowHeight);
	progVHeader->setMaximumSectionSize(RowHeight);
	progVHeader->setDefaultSectionSize(RowHeight);
	progVHeader->hide();

	auto progHeader = m_progListView->horizontalHeader();
	progHeader->setDefaultAlignment(Qt::AlignLeft);
	progHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	progHeader->setSectionResizeMode(1, QHeaderView::Stretch);
	progHeader->setSectionsMovable(false);
	progHeader->setStretchLastSection(true);

	// Initial sort order...
	m_bankListView->setSortingEnabled(true);
	m_bankListView->sortItems(0, Qt::AscendingOrder);

	m_filterEdit->setPlaceholderText(tr("Search"));
	m_filterEdit->setClearButtonEnabled(true);
	m_filterEdit->addAction(embed::getIconPixmap("zoom"), QLineEdit::LeadingPosition);

	// Configure focus (only allow for search bar and dialog buttons)
	m_filterEdit->setFocus();
	m_filterEdit->setFocusPolicy(Qt::StrongFocus);
	m_bankListView->setFocusPolicy(Qt::NoFocus);
	m_progListView->setFocusPolicy(Qt::NoFocus);

	m_selection = m_progListView->selectionModel();
	connect(m_filterEdit, &QLineEdit::textChanged, this, &PatchesDialog::filterChanged);
	connect(m_bankListView, &QTreeWidget::currentItemChanged, this, &PatchesDialog::bankChanged);
	connect(m_selection.data(), &QItemSelectionModel::currentRowChanged, this, &PatchesDialog::progChanged);
	connect(m_progListView, &QTableView::doubleClicked, this, &PatchesDialog::accept);
	connect(m_okButton, &QPushButton::clicked, this, &PatchesDialog::accept);
	connect(m_cancelButton, &QPushButton::clicked, this, &PatchesDialog::reject);
}

PatchesDialog::~PatchesDialog()
{
	const auto native = m_instrument; const auto token = std::move(m_previewToken); m_closed = true; m_invalid = true; ++m_epoch;
	for (const auto& connection : m_connections) { QObject::disconnect(connection); }
	restorePreview(native, token);
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

bool PatchesDialog::setup(Sf2Instrument* instrument, const QString& channelName)
{
	const auto previousIdentity = m_identity; // Release retired QObject-backed resources after all scoped continuations.
	const QString caption(channelName); ++m_epoch; const auto epoch = m_epoch; const QPointer<PatchesDialog> owner(this); ViewGuard view(this);
	const auto priorNative = m_instrument; const auto priorPreview = std::move(m_previewToken);
	m_invalid = true; m_closed = false; m_preparing = true; m_committing = false;
	const auto preparing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_preparing = false; } });
	try
	{
		restorePreview(priorNative, priorPreview); if (!view.alive()) { return false; }
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
		const auto effective = instrument->currentProgramModels(); if (!view.ready()) { view.refuse(); return false; }
		const auto invalid = [this, epoch] { if (m_epoch == epoch) { invalidate(); } };
		m_connections.append(connect(instrument, &QObject::destroyed, this, invalid));
		m_connections.append(connect(instrument, &Sf2Instrument::fileLoading, this, invalid));
		m_connections.append(connect(instrument, &Sf2Instrument::fileChanged, this, invalid));
		m_connections.append(connect(m_song.data(), &Song::projectFileNameChanged, this, invalid));
		m_connections.append(connect(m_engine.data(), &AudioEngine::sampleRateChanged, this, invalid));
		const auto changed = [this, epoch] { if (m_epoch == epoch && !m_committing) { invalidate(); } };
		m_connections.append(connect(m_bankModel.data(), &AutomatableModel::dataChanged, this, changed));
		m_connections.append(connect(m_programModel.data(), &AutomatableModel::dataChanged, this, changed));
		setWindowTitle(caption + tr(" - SoundFont patches")); if (!view.ready()) { view.refuse(); return false; }
		m_filterEdit->clear(); if (!view.ready()) { view.refuse(); return false; }
		m_proxy->setFilterRegularExpression(QRegularExpression{}); if (!view.ready()) { view.refuse(); return false; }
		m_bankListView->clear(); if (!view.ready()) { view.refuse(); return false; }
		const auto available = m_programs; int previous = -1;
		for (const auto& item : available)
		{
			if (item.bank != previous) { new PatchItem(m_bankListView, {QString::number(item.bank)}); previous = item.bank; if (!view.ready()) { view.refuse(); return false; } }
		}
		int selectedBank = effective.bank, selected = effective.program; bool found = false;
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
	const auto bankItem = m_bankListView->currentItem(); const auto selected = m_proxy->mapToSource(m_progListView->currentIndex());
	if (!bankItem || !selected.isValid()) { return false; } bool bankOk = false, programOk = false;
	bank = bankItem->text(0).toInt(&bankOk); program = m_source->index(selected.row(), 0).data().toInt(&programOk);
	return bankOk && programOk && std::any_of(m_programs.begin(), m_programs.end(), [=](const Instrument::Program& item) { return item.bank == bank && item.program == program; });
}

bool PatchesDialog::fillPrograms(int bank, int preferredProgram)
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return false; } const auto available = m_programs;
	m_source->setRowCount(0); if (!view.ready()) { view.refuse(); return false; } QSet<int> seen;
	for (const auto& item : available)
	{
		if (item.bank != bank || seen.contains(item.program)) { continue; } seen.insert(item.program);
		auto number = std::make_unique<QStandardItem>(); auto name = std::make_unique<QStandardItem>(item.name); number->setData(item.program, Qt::DisplayRole);
		QList<QStandardItem*> row{number.get(), name.get()}; number.release(); name.release(); m_source->appendRow(row);
		if (!view.ready()) { view.refuse(); return false; }
	}
	QModelIndex selected;
	for (int row = 0; row < m_source->rowCount(); ++row) { if (m_source->index(row, 0).data().toInt() == preferredProgram) { selected = m_proxy->mapFromSource(m_source->index(row, 0)); break; } }
	if (!selected.isValid() && m_proxy->rowCount() > 0) { selected = m_proxy->index(0, 0); }
	m_selection->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	if (!view.ready()) { view.refuse(); return false; } return true;
}

void PatchesDialog::bankChanged()
{
	if (m_preparing) { return; } ViewGuard view(this); if (!view.ready()) { view.refuse(); return; }
	const auto selected = m_bankListView->currentItem(); if (!selected) { stabilizeForm(); return; } const int bank = selected->text(0).toInt();
	const QPointer<PatchesDialog> owner(this); const auto epoch = m_epoch; m_preparing = true;
	const auto preparing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_preparing = false; } });
	if (!fillPrograms(bank, static_cast<int>(m_program)) || !view.ready()) { view.refuse(); return; } m_preparing = false; previewSelection();
}

void PatchesDialog::progChanged(const QModelIndex&, const QModelIndex&) { if (!m_preparing) { previewSelection(); } }

void PatchesDialog::previewSelection()
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; } int bank = 0, program = 0;
	const auto native = m_instrument;
	if (!selectedProgram(bank, program))
	{
		const auto token = std::move(m_previewToken); restorePreview(native, token); if (!view.ready()) { view.refuse(); return; } stabilizeForm(); return;
	}
	try
	{
		Sf2Instrument::ProgramPreviewToken token; QString error; const bool previewed = native->previewProgram(bank, program, error, token);
		if (!view.ready()) { restorePreview(native, token); view.refuse(); return; }
		if (!previewed) { invalidate(); return; } m_previewToken = token; stabilizeForm();
	}
	catch (...) { view.refuse(); }
}

void PatchesDialog::filterChanged(const QString& text)
{
	if (m_preparing) { return; } const QString query(text); ViewGuard view(this); if (!view.ready()) { view.refuse(); return; }
	try { m_proxy->setFilterRegularExpression(QRegularExpression(query, QRegularExpression::CaseInsensitiveOption)); if (!view.ready()) { view.refuse(); return; } diffSelectProgRow(0); }
	catch (...) { view.refuse(); }
}

void PatchesDialog::diffSelectProgRow(int offset)
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; }
	const int count = m_proxy->rowCount(); if (count <= 0) { previewSelection(); return; }
	const int row = qBound(0, m_selection->currentIndex().row() + offset, count - 1);
	m_selection->setCurrentIndex(m_proxy->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	if (!view.ready()) { view.refuse(); return; } const auto selected = m_selection->currentIndex(); m_progListView->scrollTo(selected);
	if (!view.ready()) { view.refuse(); return; } previewSelection();
}

void PatchesDialog::keyPressEvent(QKeyEvent* event)
{
	const auto key = event->key();
	if (key == Qt::Key_Up || key == Qt::Key_Down) { event->accept(); diffSelectProgRow(key == Qt::Key_Up ? -1 : 1); }
	else if (key == Qt::Key_Return || key == Qt::Key_Enter) { event->accept(); accept(); }
	else if (key == Qt::Key_Escape) { event->accept(); reject(); }
	else { QDialog::keyPressEvent(event); }
}

void PatchesDialog::stabilizeForm()
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; } int bank = 0, program = 0; const bool valid = selectedProgram(bank, program);
	m_okButton->setEnabled(valid); if (!view.ready()) { view.refuse(); }
}

void PatchesDialog::accept()
{
	ViewGuard view(this); if (!view.ready()) { view.refuse(); return; } int bank = 0, program = 0; if (!selectedProgram(bank, program)) { return; }
	const QPointer<PatchesDialog> owner(this); const auto epoch = m_epoch; const auto native = m_instrument; m_committing = true;
	const auto committing = qScopeGuard([owner, epoch] { if (owner && owner->m_epoch == epoch) { owner->m_committing = false; } });
	try
	{
		QString error; const bool accepted = native->selectProgram(bank, program, error);
		if (!view.ready(false)) { view.refuse(); return; }
		if (!accepted || !m_bankModel || !m_programModel || m_bankModel->storedValue() != bank || m_programModel->storedValue() != program) { invalidate(); return; }
		m_previewToken.reset(); m_bank = bank; m_program = program; m_committing = false; m_closed = true; m_invalid = true; ++m_epoch; QDialog::accept();
	}
	catch (...) { view.refuse(); }
}

void PatchesDialog::invalidate()
{
	if (m_closed) { return; } const auto native = m_instrument; const auto token = std::move(m_previewToken); const QPointer<PatchesDialog> owner(this);
	m_closed = true; m_invalid = true; const auto epoch = ++m_epoch; restorePreview(native, token);
	if (owner && owner->m_epoch == epoch) { owner->QDialog::reject(); }
}
void PatchesDialog::reject() { invalidate(); }
} // namespace lmms::gui
