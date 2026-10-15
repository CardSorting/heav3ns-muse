/*
 * EffectChain.h - class for processing and effects chain
 *
 * Copyright (c) 2006-2008 Danny McRae <khjklujn/at/users.sourceforge.net>
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_EFFECT_CHAIN_H
#define LMMS_EFFECT_CHAIN_H

#include <memory>
#include <vector>
#include <QHash>
#include <QPointer>
#include "Model.h"
#include "SerializingObject.h"
#include "AutomatableModel.h"

namespace lmms
{

class AudioBuffer;
class Effect;

namespace gui
{

class EffectRackView;

} // namespace gui


class LMMS_EXPORT EffectChain : public Model, public SerializingObject
{
	Q_OBJECT
public:
	EffectChain( Model * _parent );
	~EffectChain() override;

	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;

	inline QString nodeName() const override
	{
		return "fxchain";
	}

	// Ownership transfers at publication, before observer callbacks. A rejected
	// append leaves the candidate with its caller.
	bool appendEffect(Effect* effect);
	bool appendEffect(std::unique_ptr<Effect>& effect);
	std::unique_ptr<Effect> takeEffect(Effect* effect);
	bool removeEffect(Effect* effect);
	bool moveEffect(int from, int to);
	bool moveDown(Effect* effect);
	bool moveUp(Effect* effect);
	bool processAudioBuffer(AudioBuffer& buffer);

	bool clear();
	bool isRetiring() const noexcept { return m_retiring; }

	// Native capability clients need the same ordered chain used by the audio engine.
	const std::vector<Effect*>& effects() const { return m_effects; }
	BoolModel* enabledModel() { return &m_enabledModel; }
	const BoolModel* enabledModel() const { return &m_enabledModel; }


private:
	using EffectList = std::vector<Effect*>;
	EffectList m_effects;
	bool m_changing = false;
	bool m_retiring = false;
	bool m_clearing = false;
	std::unique_ptr<QObject> m_lifecycleContext = std::make_unique<QObject>();
	QHash<Effect*, QMetaObject::Connection> m_lifecycleHooks;
	class ChangeGuard;
	bool canChange() const;
	void watchEffect(Effect* effect);
	void unwatchEffect(Effect* effect);
	std::unique_ptr<Effect> detachEffect(Effect* effect, bool releaseParent);
	bool clearImpl();

	BoolModel m_enabledModel;


	friend class gui::EffectRackView;
	friend class Mixer;


signals:
	void aboutToClear();

} ;

} // namespace lmms

#endif // LMMS_EFFECT_CHAIN_H
