/*
 * TrackConteintObject.h - declaration of Clip class
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_CLIP_H
#define LMMS_CLIP_H

#include <optional>
#include <memory>

#include <QColor>

#include "AutomatableModel.h"


namespace lmms
{

class Track;

namespace gui
{

class ClipView;
class TrackView;

} // namespace gui


class LMMS_EXPORT Clip : public Model, public JournallingObject
{
	Q_OBJECT
	mapPropertyFromModel(bool,isMuted,setMuted,m_mutedModel);
	mapPropertyFromModel(bool,isSolo,setSolo,m_soloModel);
public:
	//! @brief Creates a new clip for the given track @p track.
	//! @param track The track that will contain the new object
	Clip(Track* track);
	~Clip() override; //!< Destroys the given clip.
	QDomElement saveJournalState(QDomDocument& document, QDomElement& parent) override;
	bool restoreJournalState(const QDomElement& state, QString& error) override;
	BoolModel* mutedModel() { return &m_mutedModel; }
	BoolModel* soloModel() { return &m_soloModel; }
	const BoolModel* mutedModel() const { return &m_mutedModel; }
	const BoolModel* soloModel() const { return &m_soloModel; }

	inline Track * getTrack() const
	{
		return m_track;
	}

	inline const QString & name() const
	{
		return m_name;
	}

	inline void setName( const QString & name )
	{
		m_name = name;
		emit dataChanged();
	}

	QString displayName() const override
	{
		return name();
	}


	inline const TimePos & startPosition() const
	{
		return m_startPosition;
	}

	inline TimePos endPosition() const
	{
		const int sp = m_startPosition;
		return sp + m_length;
	}

	inline const TimePos & length() const
	{
		return m_length;
	}


	bool hasTrackContainer() const;

	bool isInPattern() const;

	bool manuallyResizable() const;

	//! @brief Set whether a clip has been resized yet by the user or the knife tool.
	//!
	//! If a clip has been resized previously, it will not automatically resize when editing it.
	void setAutoResize(const bool r) { m_autoResize = r; }

	bool getAutoResize() const
	{
		return m_autoResize;
	}

	auto color() const -> const std::optional<QColor>& { return m_color; }
	void setColor(const std::optional<QColor>& color);

	//! @brief Move this Clip's position in time
	//!
	//! If the clip has moved, update its position. We also add a journal entry for undo and update the display.
	//!
	//! @param pos The new position of the clip.
	virtual void movePosition(const TimePos& pos);

	//! @brief Change the length of this Clip
	//!
	//! If the clip's length has changed, update it. We also add a journal entry for undo and update the display.
	//!
	//! @param length The new length of the clip.
	virtual void changeLength(const TimePos& length);
	virtual void updateLength() {};
	struct EditOptions
	{
		std::optional<int> position, length, offset;
		std::optional<bool> autoResize, muted;
		std::optional<QString> name;
	};
	// Checked compound metadata/geometry edit through the native virtual setters.
	// A returned failure after callbacks can retain edits; the studio runtime
	// supplies the broader project recovery boundary.
	bool edit(const EditOptions& options, bool& changed, QString& error);

	virtual gui::ClipView * createView( gui::TrackView * tv ) = 0;

	inline void selectViewOnCreate( bool select )
	{
		m_selectViewOnCreate = select;
	}

	inline bool getSelectViewOnCreate()
	{
		return m_selectViewOnCreate;
	}

	//! @brief Returns true if and only if `a->startPosition()` < `b->startPosition()`
	static bool comparePosition(const Clip* a, const Clip* b);

	TimePos startTimeOffset() const;
	virtual void setStartTimeOffset(const TimePos& startTimeOffset);

	//! @brief Copies the state of a clip to another clip
	static void copyStateTo(Clip* src, Clip* dst);

	//! @brief Creates a copy of this clip
	//! @return Pointer to the new clip object
	virtual Clip* clone() = 0;
	//! Copy native state to a compatible track on the model owner thread.
	//! PatternStore destinations require an existing cell to replace. Other
	//! destinations append a clip. The destination track owns the returned clip.
	Clip* copyTo(Track* destination, Clip* replacement = nullptr) const;
	//! Split at a relative tick position, preserving native offset semantics.
	//! Returns the new right-hand clip, or nullptr for an invalid boundary.
	Clip* splitAt(const TimePos& relativePosition);

public slots:
	void toggleMute(); //!< Mutes this Clip

signals:
	void lengthChanged();
	void positionChanged();
	void destroyedClip();
	void colorChanged();

protected:

	//! @brief Creates a duplicate clip of the one provided.
	//! @param other The clip object which will be copied.
	Clip(const Clip& other);
	Clip(const Clip& other, Track* destination);
	Clip* cloneNative() const;
	void saveControlSettings(QDomDocument& document, QDomElement& element, const QString& muteKey = "muted");
	void loadControlSettings(const QDomElement& element, const QString& muteKey = "muted");

private:
	void saveCopyControlSettings(QDomDocument& document, QDomElement& element, const QString& muteKey = "muted");
	void loadCopyControlSettings(const QDomElement& element, const QString& muteKey = "muted");
	void requireIdleForCopy() const;
	QDomDocument captureCopyControls() const;
	Clip* copyToImpl(Track* destination, Clip* replacement, bool legacyClone) const;
	std::unique_ptr<Clip> constructCopy(Track* destination) const;
	Track * m_track;
	QString m_name;

	TimePos m_startPosition;
	TimePos m_length;
	TimePos m_startTimeOffset;

	BoolModel m_mutedModel;
	BoolModel m_soloModel;
	bool m_autoResize = true;

	bool m_selectViewOnCreate;

	std::optional<QColor> m_color;

	friend class ClipView;
	friend class Track;
	friend class NativeClipTransfer;
	friend class InlineAutomation;
};


} // namespace lmms

#endif // LMMS_CLIP_H
