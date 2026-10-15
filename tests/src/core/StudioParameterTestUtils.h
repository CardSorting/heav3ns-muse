/* Collect actual native parameter pages for existing subsystem tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_PARAMETER_TEST_UTILS_H
#define LMMS_STUDIO_PARAMETER_TEST_UTILS_H
#include "StudioCapabilities.h"

namespace lmms::test
{
inline QJsonObject executeWithAllParameters(const QString& operation, const QJsonObject& arguments = {})
{
	if (operation != "instrument.inspect" && operation != "parameter.list")
	{ return StudioCapabilities::execute(operation, arguments); }
	auto response = StudioCapabilities::execute(operation, arguments);
	if (!response.value("ok").toBool()) { return response; }
	auto result = response.value("result").toObject();
	auto parameters = result.value("parameters").toArray();
	auto page = operation == "instrument.inspect" ? result.value("parameterPage").toObject() : result;
	const auto total = page.value("total");
	while (page.value("hasMore").toBool())
	{
		const int offset = page.value("nextOffset").toInt();
		if (offset <= page.value("offset").toInt()) { return {{"ok", false}, {"error", "Parameter page did not advance"}}; }
		auto nextArguments = arguments; nextArguments.insert("offset", offset);
		const auto next = StudioCapabilities::execute(operation, nextArguments);
		if (!next.value("ok").toBool()) { return next; }
		const auto nextResult = next.value("result").toObject();
		page = operation == "instrument.inspect" ? nextResult.value("parameterPage").toObject() : nextResult;
		if (page.value("total") != total) { return {{"ok", false}, {"error", "Native parameter layout changed while paging"}}; }
		for (const auto item : nextResult.value("parameters").toArray()) { parameters.append(item); }
	}
	// This aggregate is test data. Raw native responses are checked in StudioParameterPagingTest.
	result.insert("parameters", parameters); result.insert("testCollectedAllPages", true);
	response.insert("result", result); return response;
}
}
#endif
