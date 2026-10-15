/* Native user-edit history for linked control topology.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_CONTROL_LINK_JOURNAL_H
#define LMMS_NATIVE_CONTROL_LINK_JOURNAL_H
#include <QDomDocument>
#include <QList>
#include "lmms_export.h"
namespace lmms
{
class AutomatableModel;
class LMMS_EXPORT NativeControlLinkJournal
{
public:
	static bool link(AutomatableModel* model, AutomatableModel* peer, QString& error);
	static bool unlink(AutomatableModel* model, QString& error);
	static QDomElement counterpart(AutomatableModel* model, const QDomElement& state, QDomDocument& document, QDomElement parent, QString& error);
	static bool restore(AutomatableModel* model, const QDomElement& state, QString& error);
private:
	struct Snapshot;
	static bool edit(AutomatableModel* model, AutomatableModel* peer, bool joining, QString& error);
	static Snapshot capture(const QList<AutomatableModel*>& models, QString& error);
	static Snapshot read(const QDomElement& element, QString& error);
	static bool matches(const Snapshot& state, bool values);
	static QDomElement write(const Snapshot& state, QDomDocument& document, const QString& tag);
	static bool decode(AutomatableModel* model, const QDomElement& state, Snapshot& desired, Snapshot& expected, QString& error);
	static void rollback(const Snapshot& state) noexcept;
};
}
#endif
