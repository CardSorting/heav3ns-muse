/*
 * SampleThumbnail.cpp
 *
 * Copyright (c) 2024 Khoi Dau <casboi86@gmail.com>
 * Copyright (c) 2024 Sotonye Atemie <sakertooth@gmail.com>
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

#include "SampleThumbnail.h"

#include <QFileInfo>
#include <QLineF>
#include <QPainter>
#include <cmath>
#include <numeric>

#include "Sample.h"

namespace {
	constexpr auto MaxSampleThumbnailCacheSize = 32;
	constexpr auto AggregationPerZoomStep = 10;
}

namespace lmms::gui {

SampleThumbnail::Thumbnail::Thumbnail(std::vector<Peak> peaks, double samplesPerPeak)
	: m_peaks(std::move(peaks))
	, m_samplesPerPeak(samplesPerPeak)
{
}

SampleThumbnail::Thumbnail::Thumbnail(const float* buffer, size_t size, size_t width)
	: m_peaks(width)
	, m_samplesPerPeak(std::max(static_cast<double>(size) / width, 1.0))
{
	for (auto peakIndex = std::size_t{0}; peakIndex < width; ++peakIndex)
	{
		const auto beginSample = buffer + static_cast<size_t>(std::floor(peakIndex * m_samplesPerPeak));
		const auto endSample = buffer + static_cast<size_t>(std::ceil((peakIndex + 1) * m_samplesPerPeak));
		const auto [min, max] = std::minmax_element(beginSample, endSample);
		m_peaks[peakIndex] = Peak{*min, *max};
	}
}

SampleThumbnail::Thumbnail SampleThumbnail::Thumbnail::zoomOut(float factor) const
{
	assert(factor >= 1 && "Invalid zoom out factor");

	auto peaks = std::vector<Peak>(m_peaks.size() / factor);
	for (auto peakIndex = std::size_t{0}; peakIndex < peaks.size(); ++peakIndex)
	{
		const auto beginAggregationAt = m_peaks.begin() + static_cast<size_t>(std::floor(peakIndex * factor));
		const auto endAggregationAt = m_peaks.begin() + static_cast<size_t>(std::ceil((peakIndex + 1) * factor));
		peaks[peakIndex] = std::accumulate(beginAggregationAt, endAggregationAt, Peak{});
	}

	return Thumbnail{std::move(peaks), m_samplesPerPeak * factor};
}

SampleThumbnail::SampleThumbnail(const Sample& sample)
	: m_buffer(sample.buffer())
{
	if (!m_buffer || m_buffer->empty()) { return; }
	auto entry = SampleThumbnailEntry{sample.sampleFile(), QFileInfo{sample.sampleFile()}.lastModified()};
	if (!entry.filePath.isEmpty())
	{
		const auto it = s_sampleThumbnailCacheMap.find(entry);
		if (it != s_sampleThumbnailCacheMap.end())
		{
			m_thumbnailCache = it->second;
			return;
		}

		if (s_sampleThumbnailCacheMap.size() == MaxSampleThumbnailCacheSize)
		{
			const auto leastUsed = std::min_element(s_sampleThumbnailCacheMap.begin(), s_sampleThumbnailCacheMap.end(),
				[](const auto& a, const auto& b) { return a.second.use_count() < b.second.use_count(); });
			s_sampleThumbnailCacheMap.erase(leastUsed->first);
		}

		s_sampleThumbnailCacheMap[std::move(entry)] = m_thumbnailCache;
	}

	const auto flatBuffer = m_buffer->data()->data();
	const auto flatBufferSize = m_buffer->size() * DEFAULT_CHANNELS;
	m_thumbnailCache->emplace_back(flatBuffer, flatBufferSize, std::max<std::size_t>(1, flatBufferSize / AggregationPerZoomStep));

	while (m_thumbnailCache->back().width() >= AggregationPerZoomStep)
	{
		auto zoomedOutThumbnail = m_thumbnailCache->back().zoomOut(AggregationPerZoomStep);
		m_thumbnailCache->emplace_back(std::move(zoomedOutThumbnail));
	}
}

void SampleThumbnail::visualize(VisualizeParameters parameters, QPainter& painter) const
{
	const auto& sampleRect = parameters.sampleRect;
	const auto& viewportRect = parameters.viewportRect.isNull() ? sampleRect : parameters.viewportRect;

	const auto renderRect = sampleRect.intersected(viewportRect);
	if (renderRect.isEmpty() || !m_buffer || m_buffer->empty()) { return; }

	const auto sampleRange = parameters.sampleEnd - parameters.sampleStart;
	if (!std::isfinite(sampleRange) || sampleRange <= 0.0f || parameters.sampleStart < 0.0f
		|| parameters.sampleEnd > 1.0f || !std::isfinite(parameters.amplification)) { return; }

	const double targetThumbnailWidth = sampleRect.width() / static_cast<double>(sampleRange);
	const auto finerThumbnail = std::find_if(m_thumbnailCache->rbegin(), m_thumbnailCache->rend(),
		[&](const auto& thumbnail) { return thumbnail.width() >= targetThumbnailWidth; });

	const auto useOriginalBuffer = finerThumbnail == m_thumbnailCache->rend();

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);

	const auto width = useOriginalBuffer ? m_buffer->size() : static_cast<std::size_t>(finerThumbnail->width());
	const auto yScale = sampleRect.height() / 2.0 * parameters.amplification;
	for (auto x = renderRect.x(); x <= renderRect.right(); ++x)
	{
		double start = parameters.sampleStart + (x - sampleRect.x()) / targetThumbnailWidth;
		double end = parameters.sampleStart + (x + 1.0 - sampleRect.x()) / targetThumbnailWidth;
		if (parameters.reversed)
		{
			const auto previousStart = start; start = 1.0 - end; end = 1.0 - previousStart;
		}
		const auto beginIndex = static_cast<std::size_t>(std::clamp(std::floor(start * width), 0.0, static_cast<double>(width - 1)));
		const auto endIndex = static_cast<std::size_t>(std::clamp(std::ceil(end * width),
			static_cast<double>(beginIndex + 1), static_cast<double>(width)));
		auto peak = Thumbnail::Peak{};
		if (useOriginalBuffer)
		{
			peak = std::accumulate(m_buffer->begin() + beginIndex, m_buffer->begin() + endIndex, peak);
		}
		else { peak = std::accumulate(finerThumbnail->data() + beginIndex, finerThumbnail->data() + endIndex, peak); }
		const auto yMin = sampleRect.center().y() - peak.min * yScale;
		const auto yMax = sampleRect.center().y() - peak.max * yScale;
		painter.drawLine(QLineF(x, yMin, x, yMax));
	}

	painter.restore();
}

} // namespace lmms::gui
