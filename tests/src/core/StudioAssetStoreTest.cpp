/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <limits>
#include "DataFile.h"
#include "FileResourceSnapshot.h"
#include "StudioAssetStore.h"
#include "GigResourceFixture.h"

namespace lmms
{

class StudioAssetStoreTest : public QObject
{
	Q_OBJECT
	static bool write(const QString& path, const QByteArray& bytes)
	{
		QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
	}
private slots:
	void gigGroupsKeepIdenticalRootsWithDifferentCompanionBytesSeparate()
	{
		using namespace test::gigResource;
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("bank.gig");
		QVERIFY(write(source, root({"arbitrary.gx01"})));
		QVERIFY(write(directory.filePath("arbitrary.gx01"), companion("first")));
		QString error; auto first = FileResourceSnapshot::captureGig(source, error); QVERIFY2(first, qPrintable(error));
		QVERIFY(write(directory.filePath("arbitrary.gx01"), companion("other")));
		auto second = FileResourceSnapshot::captureGig(source, error); QVERIFY2(second, qPrintable(error));
		QCOMPARE(first->sha256(), second->sha256()); QVERIFY(first->groupDigest() != second->groupDigest());
		QVERIFY(QFile::remove(source)); QVERIFY(QFile::remove(directory.filePath("arbitrary.gx01")));
		QJsonObject a, b, duplicate; const auto store = directory.filePath("assets");
		QVERIFY(StudioAssetStore::preserve(first->path(), store, "gig", a, error, first.get()));
		QVERIFY(StudioAssetStore::preserve(second->path(), store, "gig", b, error, second.get()));
		QVERIFY(a.value("path") != b.value("path")); QCOMPARE(a.value("sha256"), b.value("sha256"));
		QVERIFY(StudioAssetStore::validReferences({a, b})); QVERIFY(StudioAssetStore::verify(directory.path(), {a, b}, error));
		QVERIFY(StudioAssetStore::preserve(first->path(), store, "gig", duplicate, error, first.get())); QCOMPARE(duplicate, a);
		QCOMPARE(QDir(store).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);
		const auto group = QFileInfo(directory.filePath(a.value("path").toString())).absoluteDir();
		QVERIFY(write(group.filePath("arbitrary.gx01"), companion("third")));
		QVERIFY(!StudioAssetStore::verify(directory.path(), {a, b}, error));
		QVERIFY(!StudioAssetStore::preserve(first->path(), store, "gig", duplicate, error, first.get())); QVERIFY(duplicate.isEmpty());
		QFile altered(group.filePath("arbitrary.gx01")); QVERIFY(altered.open(QIODevice::ReadOnly)); QCOMPARE(altered.readAll(), companion("third"));
	}

	void gigDocumentsPreserveAcceptedLayoutsInBothSaveModes_data()
	{
		QTest::addColumn<bool>("checkpoint"); QTest::addColumn<bool>("corrupt");
		QTest::newRow("bundle") << false << false;
		QTest::newRow("checkpoint") << true << false;
		QTest::newRow("bundle-corrupt-companion") << false << true;
		QTest::newRow("checkpoint-corrupt-companion") << true << true;
	}

	void gigDocumentsPreserveAcceptedLayoutsInBothSaveModes()
	{
		using namespace test::gigResource;
		QFETCH(bool, checkpoint); QFETCH(bool, corrupt); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("same.gig"); const auto pool = directory.filePath("pool.gx01");
		QVERIFY(write(source, root({"pool.gx01"}, true))); QVERIFY(write(pool, companion("wave")));
		QVERIFY(write(directory.filePath("same.gx99"), companion("impulse")));
		QString error; auto accepted = FileResourceSnapshot::captureGig(source, error); QVERIFY2(accepted, qPrintable(error));
		DataFile document(DataFile::Type::SongProject); auto node = document.createElement("gigplayer");
		node.setAttribute("src", source); document.content().appendChild(node);
		{ DataFile::ResourceSourceScope scope(document); DataFile::registerResourceSnapshot(document, node, "src", accepted); }
		QVERIFY(QFile::remove(source)); QVERIFY(QFile::remove(pool)); QVERIFY(QFile::remove(directory.filePath("same.gx99")));
		if (corrupt) { QVERIFY(write(QFileInfo(accepted->path()).absoluteDir().filePath("pool.gx01"), companion("edit"))); }
		const auto destination = directory.filePath(checkpoint ? "assets" : "resources"); QVERIFY(QDir().mkdir(destination));
		QJsonArray references; QCOMPARE(document.copyResources(destination, checkpoint ? &references : nullptr), !corrupt);
		if (corrupt)
		{
			QCOMPARE(node.attribute("src"), source); QVERIFY(references.isEmpty());
			QVERIFY(QDir(destination).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty()); return;
		}
		const auto relative = node.attribute("src"); QVERIFY(relative.startsWith(checkpoint ? "local:assets/" : "local:resources/"));
		const auto restored = directory.filePath(relative.mid(6)); QVERIFY(accepted->verifyCopy(restored, error));
		auto recaptured = FileResourceSnapshot::captureGig(restored, error); QVERIFY2(recaptured, qPrintable(error));
		QCOMPARE(recaptured->groupDigest(), accepted->groupDigest());
		if (checkpoint)
		{
			QCOMPARE(references.size(), 1); QVERIFY(document.matchesAssetReferences(references));
			QVERIFY(StudioAssetStore::verify(directory.path(), references, error));
		}
	}

	void gigDocumentsWithoutSnapshotBindingsDiscoverDependenciesAndKeepCollidingBanks()
	{
		using namespace test::gigResource;
		QTemporaryDir directory; QVERIFY(directory.isValid());
		QVERIFY(QDir().mkpath(directory.filePath("one"))); QVERIFY(QDir().mkpath(directory.filePath("two")));
		DataFile document(DataFile::Type::SongProject);
		for (const auto& name : {QString("one"), QString("two")})
		{
			const auto bank = directory.filePath(name + "/bank.gig");
			QVERIFY(write(bank, root({"same.gx01"}))); QVERIFY(write(directory.filePath(name + "/same.gx01"), companion(name.toUtf8())));
			auto node = document.createElement("gigplayer"); node.setAttribute("src", bank); document.content().appendChild(node);
		}
		const auto resources = directory.filePath("resources"); QVERIFY(QDir().mkdir(resources));
		QVERIFY(document.copyResources(resources));
		const auto nodes = document.elementsByTagName("gigplayer"); QCOMPARE(nodes.size(), 2);
		const auto a = nodes.at(0).toElement().attribute("src"), b = nodes.at(1).toElement().attribute("src"); QVERIFY(a != b);
		QString error;
		auto first = FileResourceSnapshot::captureGig(directory.filePath(a.mid(6)), error); QVERIFY2(first, qPrintable(error));
		auto second = FileResourceSnapshot::captureGig(directory.filePath(b.mid(6)), error); QVERIFY2(second, qPrintable(error));
		QCOMPARE(first->sha256(), second->sha256()); QVERIFY(first->groupDigest() != second->groupDigest());
	}

	void gigManifestsRejectMissingMembersUnsafeNamesAndLayoutDigestChanges()
	{
		using namespace test::gigResource;
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto bank = directory.filePath("bank.gig");
		QVERIFY(write(bank, root({"pool.gx01"}))); QVERIFY(write(directory.filePath("pool.gx01"), companion()));
		QString error; auto accepted = FileResourceSnapshot::captureGig(bank, error); QVERIFY2(accepted, qPrintable(error));
		QJsonObject valid; QVERIFY(StudioAssetStore::preserve(accepted->path(), directory.filePath("assets"), "gig", valid, error, accepted.get()));
		for (const auto& name : {QString("../escape"), QString("/escape"), QString("BANK.GIG"), QString("renamed.gx01")})
		{
			auto bad = valid; auto members = bad.value("members").toArray(); auto changed = members.at(1).toObject();
			changed.insert("name", name); members[1] = changed; bad.insert("members", members);
			QVERIFY(!StudioAssetStore::validReferences({bad}));
		}
		auto missing = valid; auto members = missing.value("members").toArray(); members.removeLast(); missing.insert("members", members);
		QVERIFY(!StudioAssetStore::validReferences({missing}));
		auto mismatch = valid; mismatch.insert("sha256", QString(64, 'a')); QVERIFY(!StudioAssetStore::validReferences({mismatch}));
		const auto group = QFileInfo(directory.filePath(valid.value("path").toString())).absoluteDir();
		QVERIFY(QFile::remove(group.filePath("pool.gx01"))); QVERIFY(!StudioAssetStore::verify(directory.path(), {valid}, error));
	}

	void acceptedFingerprintChecksCopiedStreamBeforePublication_data()
	{
		QTest::addColumn<int>("alteration");
		QTest::newRow("same-size-bytes") << 0;
		QTest::newRow("truncated") << 1;
		QTest::newRow("grown") << 2;
	}

	void acceptedFingerprintChecksCopiedStreamBeforePublication()
	{
		QFETCH(int, alteration); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("input.wav"); const QByteArray bytes(130007, 'a'); QVERIFY(write(source, bytes));
		QString error; auto accepted = FileResourceSnapshot::capture(source, error); QVERIFY2(accepted, qPrintable(error));
		const auto store = directory.filePath("assets"); QJsonObject original, rejected;
		QVERIFY(StudioAssetStore::preserve(source, store, "wav", original, error, accepted.get()));
		QCOMPARE(original.value("sha256").toString(), accepted->sha256());
		const auto changed = alteration == 0 ? QByteArray(bytes.size(), 'b')
			: alteration == 1 ? bytes.left(bytes.size() - 1) : bytes + "b";
		QVERIFY(write(source, changed)); QVERIFY(accepted->verify(error));
		QVERIFY(!StudioAssetStore::preserve(source, store, "wav", rejected, error, accepted.get()));
		QVERIFY(!error.isEmpty()); QVERIFY(rejected.isEmpty());
		QCOMPARE(QDir(store).entryList(QDir::Files | QDir::Hidden).size(), 1);
		QVERIFY(StudioAssetStore::verify(directory.path(), {original}, error));
		QVERIFY(StudioAssetStore::preserve(accepted->path(), store, "wav", rejected, error, accepted.get()));
		QCOMPARE(rejected, original);
	}

	void nativeDocumentSnapshotLeaseAndFingerprintCoverBothSaveModes_data()
	{
		QTest::addColumn<bool>("checkpoint"); QTest::addColumn<bool>("corrupt");
		QTest::newRow("bundle-valid") << false << false;
		QTest::newRow("bundle-corrupt") << false << true;
		QTest::newRow("checkpoint-valid") << true << false;
		QTest::newRow("checkpoint-corrupt") << true << true;
	}

	void nativeDocumentSnapshotLeaseAndFingerprintCoverBothSaveModes()
	{
		QFETCH(bool, checkpoint); QFETCH(bool, corrupt);
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.wav");
		const QByteArray bytes(130007, 'a'); QVERIFY(write(source, bytes)); QString error;
		auto snapshot = FileResourceSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		const auto path = snapshot->path(), checksum = snapshot->sha256();
		std::weak_ptr<const FileResourceSnapshot> lease(snapshot);
		const auto destination = directory.filePath(checkpoint ? "assets" : "resources"); QVERIFY(QDir().mkdir(destination));
		{
			DataFile document(DataFile::Type::SongProject); auto node = document.createElement("sampleclip");
			node.setAttribute("src", source); document.content().appendChild(node);
			{ DataFile::ResourceSourceScope scope(document); DataFile::registerResourceSnapshot(document, node, "src", snapshot); }
			snapshot.reset(); QVERIFY(!lease.expired()); QVERIFY(QFile::exists(path)); QVERIFY(QFile::remove(source));
			if (corrupt) { QVERIFY(write(path, QByteArray(bytes.size(), 'b'))); }
			QJsonArray references;
			QCOMPARE(document.copyResources(destination, checkpoint ? &references : nullptr), !corrupt);
			if (corrupt)
			{
				QCOMPARE(node.attribute("src"), source); QVERIFY(references.isEmpty());
				QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::Hidden).size(), 0);
			}
			else
			{
				const auto relative = node.attribute("src"); QVERIFY(relative.startsWith("local:"));
				QFile copied(directory.filePath(relative.mid(6))); QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), bytes);
				if (checkpoint)
				{
					QCOMPARE(references.size(), 1); QCOMPARE(references.at(0).toObject().value("sha256").toString(), checksum);
					QVERIFY(document.matchesAssetReferences(references)); QVERIFY(StudioAssetStore::verify(directory.path(), references, error));
				}
			}
		}
		QVERIFY(lease.expired()); QVERIFY(!QFile::exists(path));
	}

	void reusedCopiesCannotBypassCapturedFingerprint_data()
	{
		QTest::addColumn<bool>("checkpoint");
		QTest::newRow("bundle") << false;
		QTest::newRow("checkpoint") << true;
	}

	void reusedCopiesCannotBypassCapturedFingerprint()
	{
		QFETCH(bool, checkpoint); QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("input.wav"); const QByteArray bytes(130007, 'a'); QVERIFY(write(source, bytes));
		QString error; auto snapshot = FileResourceSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		QVERIFY(write(snapshot->path(), QByteArray(bytes.size(), 'b'))); QVERIFY(QFile::remove(source));
		DataFile document(DataFile::Type::SongProject);
		// AFP is visited first in the resource table and has a plain source path.
		// The captured sample clip then reaches the copier's reused-path branch.
		auto raw = document.createElement("audiofileprocessor"); raw.setAttribute("src", snapshot->path()); document.content().appendChild(raw);
		auto captured = document.createElement("sampleclip"); captured.setAttribute("src", source); document.content().appendChild(captured);
		{ DataFile::ResourceSourceScope scope(document); DataFile::registerResourceSnapshot(document, captured, "src", snapshot); }
		const auto destination = directory.filePath(checkpoint ? "assets" : "resources"); QVERIFY(QDir().mkdir(destination));
		QJsonArray references;
		QVERIFY(!document.copyResources(destination, checkpoint ? &references : nullptr));
		QCOMPARE(raw.attribute("src"), snapshot->path()); QCOMPARE(captured.attribute("src"), source); QVERIFY(references.isEmpty());
	}

	void identicalBytesShareAFileAndCorruptionIsRejected()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto a = directory.filePath("a.wav"), b = directory.filePath("b.wav");
		const QByteArray bytes(130000, 'a'); QVERIFY(write(a, bytes)); QVERIFY(write(b, bytes));
		const auto store = directory.filePath("assets");
		QJsonObject first, second; QString error;
		QVERIFY2(StudioAssetStore::preserve(a, store, "wav", first, error), qPrintable(error));
		QVERIFY(StudioAssetStore::preserve(b, store, "wav", second, error));
		QCOMPARE(first, second); QCOMPARE(first.value("bytes").toDouble(), double(bytes.size()));
		QCOMPARE(QDir(store).entryList(QDir::Files).size(), 1);
		QVERIFY(StudioAssetStore::verify(directory.path(), {first}, error));
		const auto owned = directory.filePath(first.value("path").toString());
		QVERIFY(write(owned, QByteArray(bytes.size(), 'b')));
		QVERIFY(!StudioAssetStore::verify(directory.path(), {first}, error));
		QVERIFY(!StudioAssetStore::preserve(a, store, "wav", second, error));
		QFile altered(owned); QVERIFY(altered.open(QIODevice::ReadOnly));
		QCOMPARE(altered.readAll(), QByteArray(bytes.size(), 'b'));
	}

	void referencesRejectTraversalHashMismatchAndInvalidSizes()
	{
		const QString hash(64, 'a');
		const QJsonObject valid{{"path", "assets/" + hash + ".sf2"}, {"sha256", hash}, {"bytes", 123}};
		QVERIFY(StudioAssetStore::validReferences({valid}));
		for (const auto& path : {"../assets/" + hash + ".sf2", "assets/../" + hash + ".sf2", "assets/" + hash + "/file"})
		{
			auto bad = valid; bad.insert("path", path); QVERIFY(!StudioAssetStore::validReferences({bad}));
		}
		for (const auto number : {-1., .5, 9007199254740992., std::numeric_limits<double>::quiet_NaN()})
		{
			auto bad = valid; bad.insert("bytes", number); QVERIFY(!StudioAssetStore::validReferences({bad}));
		}
		auto bad = valid; bad.insert("sha256", QString(64, 'b'));
		QVERIFY(!StudioAssetStore::validReferences({bad}));
		QVERIFY(!StudioAssetStore::validReferences({valid, valid}));
	}

	void nativeDocumentPathsMustMatchAllDeclaredAssets()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("bank.sf2"); QVERIFY(write(source, "Accepted native input"));
		DataFile document(DataFile::Type::SongProject);
		auto node = document.createElement("sf2player"); node.setAttribute("src", source);
		document.content().appendChild(node);
		QJsonArray references;
		QVERIFY(document.copyResources(directory.filePath("assets"), &references));
		QCOMPARE(references.size(), 1); QVERIFY(document.matchesAssetReferences(references));
		node.setAttribute("src", source); QVERIFY(!document.matchesAssetReferences(references));
		node.setAttribute("src", "local:assets/not-declared.sf2"); QVERIFY(!document.matchesAssetReferences(references));
		node.setAttribute("src", ""); QVERIFY(!document.matchesAssetReferences(references));
	}
};

} // namespace lmms

QTEST_GUILESS_MAIN(lmms::StudioAssetStoreTest)
#include "StudioAssetStoreTest.moc"
