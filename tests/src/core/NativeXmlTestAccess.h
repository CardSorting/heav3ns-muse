/* Native XML content comparison for persistence fixtures. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <QDomNode>
#include <QJsonArray>
#include <QJsonObject>

namespace lmms::test
{
// Preserve node content and child order while ignoring XML attribute order.
inline QJsonObject xmlState(const QDomNode& node)
{
	QJsonObject attributes;
	const auto entries = node.attributes();
	for (int i = 0; i < entries.count(); ++i) { const auto entry = entries.item(i); attributes.insert(entry.nodeName(), entry.nodeValue()); }
	QJsonArray children;
	for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) { children.append(xmlState(child)); }
	return {{"type", int(node.nodeType())}, {"name", node.nodeName()}, {"value", node.nodeValue()}, {"attributes", attributes}, {"children", children}};
}
}
