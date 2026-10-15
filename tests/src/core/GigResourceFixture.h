/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TEST_GIG_RESOURCE_FIXTURE_H
#define LMMS_TEST_GIG_RESOURCE_FIXTURE_H

#include <QByteArray>
#include <QStringList>
#include <QtEndian>
#include <algorithm>

// Structural RIFF fixtures for dependency capture. These deliberately contain
// no playable instruments; they do not validate libgig synthesis or rendering.
namespace lmms::test::gigResource
{
inline QByteArray uint32(quint32 value, bool bigEndian = false)
{
	QByteArray result(4, '\0');
	if (bigEndian) { qToBigEndian(value, reinterpret_cast<uchar*>(result.data())); }
	else { qToLittleEndian(value, reinterpret_cast<uchar*>(result.data())); }
	return result;
}
inline QByteArray chunk(const QByteArray& id, const QByteArray& data, bool bigEndian = false)
{
	return id + uint32(data.size(), bigEndian) + data + (data.size() % 2 ? QByteArray(1, '\0') : QByteArray());
}
inline QByteArray riff(const QByteArray& chunks, bool bigEndian = false)
{
	return (bigEndian ? "RIFX" : "RIFF") + uint32(chunks.size() + 4, bigEndian) + "DLS " + chunks;
}
inline QByteArray id() { return QByteArray::fromHex("123456789abcdef00102030405060708"); }
inline QByteArray encodedId(QByteArray identifier, bool bigEndian)
{
	if (bigEndian)
	{
		std::reverse(identifier.begin(), identifier.begin() + 4);
		std::reverse(identifier.begin() + 4, identifier.begin() + 6);
		std::reverse(identifier.begin() + 6, identifier.begin() + 8);
	}
	return identifier;
}
inline QByteArray root(const QStringList& companions, bool convolution = false, bool bigEndian = false)
{
	QByteArray declarations = uint32(companions.size(), bigEndian);
	for (const auto& name : companions)
	{
		auto encoded = name.toUtf8(); encoded += QByteArray(128 - encoded.size(), '\0');
		declarations += encoded + encodedId(id(), bigEndian);
	}
	auto chunks = chunk("JUNK", "Odd", bigEndian) + chunk("xfil", declarations, bigEndian);
	if (convolution) { chunks += chunk("doxf", uint32(1, bigEndian) + QByteArray(128, '\0') + encodedId(id(), bigEndian), bigEndian); }
	return riff(chunks, bigEndian);
}
inline QByteArray companion(const QByteArray& payload = "sample pool", const QByteArray& identifier = id(), bool bigEndian = false)
{
	return riff(chunk("dlid", encodedId(identifier, bigEndian), bigEndian) + chunk("JUNK", payload, bigEndian), bigEndian);
}
} // namespace lmms::test::gigResource
#endif
