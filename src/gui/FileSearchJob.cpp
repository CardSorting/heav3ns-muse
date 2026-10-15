/*
 * FileSearchJob.cpp
 *
 * Copyright (c) 2025 saker <sakertooth@gmail.com>
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

#include "FileSearchJob.h"


#include "ThreadPool.h"

namespace lmms::gui {
FileSearchJob::FileSearchJob(QObject* parent)
	: QObject(parent)
{
}

FileSearchJob::~FileSearchJob()
{
	if (m_task.valid())
	{
		m_stop.test_and_set(std::memory_order_acquire);
		m_task.get();
	}
}

void FileSearchJob::search(Task task)
{
	if (m_task.valid())
	{
		m_stop.test_and_set(std::memory_order_acquire);
		m_task.get();
		m_stop.clear(std::memory_order_release);
	}

	const auto fn = [this, task = std::move(task)] { runSearch(std::move(task)); };
	m_task = ThreadPool::instance().enqueue(std::move(fn));
}

void FileSearchJob::runSearch(Task task)
{
	emit started();
	FileSearch::Callbacks callbacks;
	callbacks.cancelled = [this] { return m_stop.test(std::memory_order_relaxed); };
	callbacks.match = [this](const QString& path) { emit foundMatch(path); };
	FileSearch::scan(std::move(task), callbacks);
	emit finished();
}

} // namespace lmms::gui
