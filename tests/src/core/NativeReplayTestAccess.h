/* Durable receipt assertions. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <QJsonObject>
#include <QJsonDocument>
#include <QtTest>

// Check the replay marker and every original receipt field. Keep the Qt test
// failure at the call site and evaluate the request only once.
#define VERIFY_NATIVE_REPLAY(actual, expected) \
	do { \
		auto nativeReplayResponse_ = (actual); \
        QVERIFY2(nativeReplayResponse_.take("replayed").toBool(), qPrintable(QString("A cached response must report replayed=true: ") + QString::fromUtf8(QJsonDocument(nativeReplayResponse_).toJson(QJsonDocument::Compact)))); \
		QCOMPARE(nativeReplayResponse_, (expected)); \
	} while (false)
