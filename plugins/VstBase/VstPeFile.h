/* Bounded native VST binary classification. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PE_FILE_H
#define LMMS_VST_PE_FILE_H

#include <QFile>
#include <QtEndian>
#include <stdexcept>

namespace lmms
{
// Read only the DOS header and the PE signature/COFF header, without mapping an
// untrusted file or dereferencing its offsets. The platform loader still validates
// the executable itself. Layout: https://learn.microsoft.com/windows/win32/debug/pe-format
inline quint16 readVstPeMachine(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error(file.errorString().toStdString()); }
	const auto dos = file.read(64);
	if (dos.size() != 64 || !dos.startsWith("MZ"))
	{ throw std::runtime_error("VST binary has an invalid or truncated DOS header"); }
	const qint64 offset = qFromLittleEndian<quint32>(dos.constData() + 0x3c);
	const auto bytes = file.size();
	if (bytes < 24 || offset > bytes - 24 || !file.seek(offset))
	{ throw std::runtime_error("VST binary PE header is outside the file"); }
	const auto pe = file.read(24);
	if (pe.size() != 24 || pe.left(4) != QByteArray("PE\0\0", 4))
	{ throw std::runtime_error("VST binary has an invalid or truncated PE header"); }
	return qFromLittleEndian<quint16>(pe.constData() + 4);
}
} // namespace lmms
#endif
