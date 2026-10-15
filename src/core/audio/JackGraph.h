/* Shared native JACK graph name validation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_JACK_GRAPH_H
#define LMMS_JACK_GRAPH_H
#include <QByteArray>
#include <QString>
#include <cstddef>
#include <stdexcept>

namespace lmms::detail
{
inline QString jackGraphName(const char* name)
{
	if (!name) { throw std::runtime_error("JACK returned a missing port name/type"); }
	std::size_t size = 0;
	while (size < 4096 && name[size]) { ++size; }
	if (!size || size == 4096) { throw std::runtime_error("JACK port name/type exceeds the supported byte limit"); }
	const QByteArray bytes(name, static_cast<int>(size)); const auto text = QString::fromUtf8(bytes);
	if (text.toUtf8() != bytes) { throw std::runtime_error("JACK port name/type is not valid UTF-8"); }
	return text;
}

inline QByteArray jackRouteName(const QString& name)
{
	const auto encoded = name.toUtf8();
	if (name.isEmpty() || name.contains(QChar(0)) || encoded.size() >= 4096 || QString::fromUtf8(encoded) != name)
	{ throw std::invalid_argument("JACK routing requires an exact nonempty UTF-8 port name shorter than 4096 bytes"); }
	return encoded;
}
}
#endif
