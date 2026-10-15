/* Native VST MIDI packet ownership. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include "VstMidiBuffer.h"

using namespace lmms;
namespace
{
const VstMidiEvent& eventAt(const VstEvents* packet, int index)
{
	VstEvent* pointer = nullptr;
	std::memcpy(&pointer, reinterpret_cast<const std::byte*>(packet) + offsetof(VstEvents, events) + index * sizeof(pointer), sizeof(pointer));
	return *reinterpret_cast<const VstMidiEvent*>(pointer);
}
}
class VstMidiBufferTest : public QObject
{
	Q_OBJECT
private slots:
	void denseBatchesRetainAllEventsAndStableOrdering_data()
	{
		QTest::addColumn<int>("count");
		QTest::newRow("empty") << 0; QTest::newRow("one") << 1;
		QTest::newRow("past-old-capacity") << 1500; QTest::newRow("large") << 8193;
	}
	void denseBatchesRetainAllEventsAndStableOrdering()
	{
		QFETCH(int, count); std::vector<VstMidiEvent> queue(count);
		for (int index = 0; index < count; ++index)
		{ auto& event = queue[index]; event = {}; event.type = kVstMidiType; event.deltaFrames = (count - index) % 32; event.noteLength = index; }
		VstMidiBuffer buffer; buffer.assign(queue); QVERIFY(queue.empty());
		// Reuse and grow the producer queue while the native consumer retains pointers.
		queue.resize(count + 100); const auto packet = buffer.data(); QVERIFY(packet); QCOMPARE(packet->numEvents, count); QVERIFY(!packet->reserved);
		std::vector<bool> seen(count, false); int previousFrame = -1, previousIndex = -1;
		for (int index = 0; index < count; ++index)
		{
			const auto& event = eventAt(packet, index); QCOMPARE(event.type, kVstMidiType);
			QVERIFY(event.noteLength >= 0 && event.noteLength < count); QVERIFY(!seen[event.noteLength]); seen[event.noteLength] = true;
			QVERIFY(event.deltaFrames >= previousFrame);
			if (event.deltaFrames == previousFrame) { QVERIFY(event.noteLength > previousIndex); }
			previousFrame = event.deltaFrames; previousIndex = event.noteLength;
		}
		QVERIFY(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
		queue.clear(); buffer.assign(queue); QCOMPARE(buffer.data()->numEvents, 0);
	}
};

QTEST_GUILESS_MAIN(VstMidiBufferTest)
#include "VstMidiBufferTest.moc"
