/*
 * SampleBuffer.cpp - container-class SampleBuffer
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

#include "SampleBuffer.h"

#include <QDebug>
#include <QMessageBox>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "DataFile.h"
#include "FileResourceSnapshot.h"
#include "FileResourceIO.h"
#include "GuiApplication.h"
#include "PathUtil.h"
#include "SampleDecoder.h"

namespace lmms {

SampleBuffer::SampleBuffer(const SampleFrame* data, size_t numFrames, int sampleRate)
	: m_data(data, data + numFrames)
	, m_sampleRate(sampleRate)
{
}

SampleBuffer::SampleBuffer(std::vector<SampleFrame> data, int sampleRate, const QString& audioFile)
	: m_data(std::move(data))
	, m_audioFile(audioFile)
	, m_sampleRate(sampleRate)
{
}

void swap(SampleBuffer& first, SampleBuffer& second) noexcept
{
	using std::swap;
	swap(first.m_data, second.m_data);
	swap(first.m_audioFile, second.m_audioFile);
	swap(first.m_sourceSnapshot, second.m_sourceSnapshot);
	swap(first.m_sampleRate, second.m_sampleRate);
}

QString SampleBuffer::toBase64() const
{
	QString encoded, error;
	if (!toBase64(encoded, error)) { qWarning() << error; }
	return encoded;
}

bool SampleBuffer::toBase64(QString& encoded, QString& error) const
try
{
	encoded.clear();
	if (!FileResourceIO::check(error)) { return false; }
	if (m_sampleRate == 0 || m_sampleRate > static_cast<sample_rate_t>(std::numeric_limits<int>::max())
		|| m_data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
		|| m_data.size() > std::numeric_limits<std::size_t>::max() / sizeof(SampleFrame))
	{
		error = QObject::tr("Embedded sample exceeds native rate or frame limits"); return false;
	}
	using StringSize = decltype(QString{}.size());
	const auto bytes = m_data.size() * sizeof(SampleFrame);
	const auto groups = bytes / 3 + (bytes % 3 != 0);
	const auto maximumCharacters = std::min(static_cast<std::size_t>(std::numeric_limits<StringSize>::max() - 1),
		static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(QChar));
	if (groups > maximumCharacters / 4) { error = QObject::tr("Embedded sample exceeds native text limits"); return false; }
	QString result;
	// The complete output remains native project state; only encoder scratch work is bounded.
	constexpr std::size_t blockFrames = 1536;
	static_assert((blockFrames * sizeof(SampleFrame)) % 3 == 0);
	for (std::size_t offset = 0; offset < m_data.size();)
	{
		if (!FileResourceIO::check(error)) { return false; }
		const auto count = std::min(blockFrames, m_data.size() - offset);
		if (std::any_of(m_data.begin() + offset, m_data.begin() + offset + count,
			[](const SampleFrame& frame) { return frame.containsNaN() || frame.containsInf(); }))
		{
			error = QObject::tr("Embedded sample contains nonfinite audio"); return false;
		}
		const auto block = QByteArray::fromRawData(reinterpret_cast<const char*>(m_data.data() + offset),
			static_cast<int>(count * sizeof(SampleFrame))).toBase64();
		result.append(QString::fromLatin1(block));
		offset += count;
	}
	if (!FileResourceIO::check(error)) { return false; }
	encoded = std::move(result); error.clear(); return true;
}
catch (const std::bad_alloc&) { encoded.clear(); error = QObject::tr("Unable to allocate embedded sample encoding"); return false; }
catch (const std::length_error&) { encoded.clear(); error = QObject::tr("Embedded sample exceeds native encoding allocation limits"); return false; }

bool SampleBuffer::saveEmbeddedData(QDomDocument& document, QDomElement& element, const QString& attribute) const
{
	QString encoded, error;
	if (element.ownerDocument() != document || attribute.isEmpty())
	{
		error = QObject::tr("Embedded sample destination does not belong to its document");
	}
	else if (toBase64(encoded, error)) { element.setAttribute(attribute, encoded); return true; }
	DataFile::recordSerializationError(document, error); qWarning() << error; return false;
}

QString SampleBuffer::acceptedFile() const
{
	return m_sourceSnapshot ? m_sourceSnapshot->path() : QString{};
}

void SampleBuffer::registerResourceSource(const QDomDocument& document, const QDomElement& element,
	const QString& attribute, std::shared_ptr<const SampleBuffer> owner) const
{
	if (m_sourceSnapshot)
	{
		DataFile::registerResourceSnapshot(document, element, attribute, m_sourceSnapshot,
			owner.get() == this ? std::move(owner) : std::shared_ptr<const SampleBuffer>{});
	}
}

auto SampleBuffer::emptyBuffer() -> std::shared_ptr<const SampleBuffer>
{
	static auto s_buffer = std::make_shared<const SampleBuffer>();
	return s_buffer;
}

std::shared_ptr<const SampleBuffer> SampleBuffer::fromFile(const QString& filePath)
{
	if (filePath.isEmpty()) { return SampleBuffer::emptyBuffer(); }

	const auto absolutePath = PathUtil::toAbsolute(filePath);
	if (auto retained = FileResourceSnapshot::retainedSample(absolutePath)) { return retained; }
	const auto storedPath = PathUtil::toShortestRelative(filePath);

	QString captureError;
	auto snapshot = FileResourceSnapshot::capture(absolutePath, captureError);
	auto result = snapshot ? SampleDecoder::decode(snapshot->path()) : std::optional<SampleDecoder::Result>{};

	if (!result)
	{
		// TODO: Improve error handling. We dont always want to show a message box on failure when there is a GUI (e.g.
		// when loading the project), and this function also shouldn't be concerned with handling the error.
		if (gui::getGUI())
		{
			QMessageBox::warning(nullptr, QObject::tr("Failed to load sample"),
				QObject::tr("The sample may be corrupted or unsupported."));
		}
		else
		{
			if (!captureError.isEmpty()) { qWarning() << captureError; }
			qWarning() << QObject::tr(
				"Failed to load sample at path %1, the file may not exist, be corrupted, or is unsupported.")
							  .arg(absolutePath);
		}

		return SampleBuffer::emptyBuffer();
	}

	auto& [data, sampleRate] = *result;
	auto buffer = std::make_shared<SampleBuffer>(std::move(data), sampleRate, storedPath);
	buffer->m_sourceSnapshot = std::move(snapshot);
	return buffer;
}

std::shared_ptr<const SampleBuffer> SampleBuffer::fromBase64(const QString& str, int sampleRate)
{
	QString error;
	auto buffer = fromBase64(str, sampleRate, error);
	if (!buffer)
	{
		// TODO: Improve error handling. We dont always want to show a message box on failure when there is a GUI (e.g.
		// when loading the project), and this function also shouldn't be concerned with handling the error.
		if (gui::getGUI())
		{
			QMessageBox::warning(
				nullptr, QObject::tr("Failed to load sample"), error);
		}
		else
		{
			qWarning() << error;
		}

		return SampleBuffer::emptyBuffer();
	}
	return buffer;
}

std::shared_ptr<const SampleBuffer> SampleBuffer::fromBase64(const QString& str, int sampleRate, QString& error)
try
{
	static_assert(std::is_trivially_copyable_v<SampleFrame>);
	static_assert(sizeof(SampleFrame) == DEFAULT_CHANNELS * sizeof(sample_t));
	if (!FileResourceIO::check(error)) { return {}; }
	if (sampleRate <= 0) { error = QObject::tr("Invalid embedded sample rate"); return {}; }
	constexpr int encodedBlock = 16384; // Complete Base64 groups, bounded native Qt scratch storage.
	QByteArray encoded, pending;
	encoded.reserve(encodedBlock + 2);
	std::vector<SampleFrame> data;
	int padding = 0;
	const auto flush = [&] {
		if (!FileResourceIO::check(error)) { return false; }
		if (encoded.size() % 4 == 1) { error = QObject::tr("Invalid embedded sample Base64 length"); return false; }
		const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
		if (!decoded) { error = QObject::tr("Invalid embedded sample Base64 encoding"); return false; }
		pending.append(decoded.decoded);
		const auto frames = static_cast<std::size_t>(pending.size()) / sizeof(SampleFrame);
		if (frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) - data.size()
			|| frames > data.max_size() - data.size())
		{
			error = QObject::tr("Embedded sample exceeds native frame limits"); return false;
		}
		const auto begin = data.size();
		data.resize(begin + frames);
		if (frames > 0) { std::memcpy(data.data() + begin, pending.constData(), frames * sizeof(SampleFrame)); }
		if (std::any_of(data.begin() + begin, data.end(), [](const SampleFrame& frame) {
			return frame.containsNaN() || frame.containsInf(); }))
		{
			error = QObject::tr("Embedded sample contains nonfinite audio"); return false;
		}
		pending.remove(0, static_cast<int>(frames * sizeof(SampleFrame)));
		encoded.clear(); return FileResourceIO::check(error);
	};
	for (auto index = decltype(str.size()){0}; index < str.size(); ++index)
	{
		if (index % encodedBlock == 0 && !FileResourceIO::check(error)) { return {}; }
		const auto character = str.at(index).unicode();
		// Native XML/clipboard line wrapping remains valid; other characters must be real Base64.
		if (character == ' ' || character == '\t' || character == '\r' || character == '\n'
			|| character == '\f' || character == '\v') { continue; }
		if (character == '=')
		{
			if (++padding > 2) { error = QObject::tr("Invalid embedded sample Base64 padding"); return {}; }
		}
		else if (padding != 0 || !((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
			|| (character >= '0' && character <= '9') || character == '+' || character == '/'))
		{
			error = QObject::tr("Invalid embedded sample Base64 character or trailing data"); return {};
		}
		encoded.append(static_cast<char>(character));
		if (encoded.size() == encodedBlock && padding == 0 && !flush()) { return {}; }
	}
	if (!encoded.isEmpty() && !flush()) { return {}; }
	if (!pending.isEmpty()) { error = QObject::tr("Embedded sample has an incomplete native frame"); return {}; }
	if (!FileResourceIO::check(error)) { return {}; }
	error.clear(); return std::make_shared<SampleBuffer>(std::move(data), sampleRate);
}
catch (const std::bad_alloc&) { error = QObject::tr("Unable to allocate embedded native sample"); return {}; }
catch (const std::length_error&) { error = QObject::tr("Embedded sample exceeds native allocation limits"); return {}; }

} // namespace lmms
