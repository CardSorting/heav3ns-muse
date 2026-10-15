/* Native scoped linked-control serialization.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_LINKED_CONTROLS_H
#define LMMS_NATIVE_LINKED_CONTROLS_H
#include <QDomDocument>
#include <QList>
#include <QPointer>
#include <memory>
#include <vector>
#include "lmms_export.h"

namespace lmms
{
class AutomatableModel;
class Clip;
class Track;
class Note;
class LMMS_EXPORT NativeLinkedControls
{
public:
	class LMMS_EXPORT SaveScope
	{
	public:
		explicit SaveScope(QDomDocument& document);
		SaveScope(QDomDocument& document, const QList<AutomatableModel*>& models);
		~SaveScope();
		SaveScope(const SaveScope&) = delete;
		SaveScope& operator=(const SaveScope&) = delete;
		bool finish(QDomElement parent, QString& error);
	private:
		struct State;
		std::unique_ptr<State> m_state;
		SaveScope* m_previous;
		friend class NativeLinkedControls;
	};
	class LMMS_EXPORT RestoreScope
	{
	public:
		explicit RestoreScope(const QDomDocument& document);
		~RestoreScope();
		RestoreScope(const RestoreScope&) = delete;
		RestoreScope& operator=(const RestoreScope&) = delete;
		bool finish(const QDomElement& parent, QString& error);
	private:
		struct State;
		std::unique_ptr<State> m_state;
		RestoreScope* m_previous;
		friend class NativeLinkedControls;
	};
	static QList<AutomatableModel*> projectModels();
	static QList<AutomatableModel*> trackModels(Track* track, bool includeClips = false);
	static QList<AutomatableModel*> clipModels(Clip* clip);
	static QList<AutomatableModel*> noteModels(const Note* note);
	static void unlinkInternalGroups(const QList<AutomatableModel*>& models);
	static bool isRestoring(const QDomDocument& document);
	// Native control effects must not reinterpret stored values as user actions
	// while a document's complete linked state is being restored.
	static bool isRestoring();
	static void unlinkProjectModels();
	static bool needsReference(const AutomatableModel* model, const QDomDocument& document);
	static void recordSaved(AutomatableModel* model, QDomDocument& document, QDomElement element);
	static void recordLoaded(AutomatableModel* model, const QDomElement& element);
	static void recordInlineOwner(AutomatableModel* model, const QDomDocument& document, const QDomElement& owner);
private:
	friend class NativeControlLinkJournal;
	static bool applyGroups(const std::vector<std::vector<QPointer<AutomatableModel>>>& groups, QString& error);
};
}
#endif
