/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_SF2_SYNTH_STATE_H
#define LMMS_SF2_SYNTH_STATE_H

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <fluidsynth.h>

namespace lmms
{

// An unpublished native synth owns independent settings. Destroy the synth
// before its settings, including when construction stops at an earlier stage.
struct Sf2SynthState
{
	using SettingsOwner = std::unique_ptr<fluid_settings_t, decltype(&delete_fluid_settings)>;
	using SynthOwner = std::unique_ptr<fluid_synth_t, decltype(&delete_fluid_synth)>;
	using SettingsFactory = fluid_settings_t* (*)();
	using SynthFactory = fluid_synth_t* (*)(fluid_settings_t*);
	enum class Error { None, InvalidSampleRate, SettingsAllocation, SettingsCopy, Polyphony, SynthAllocation,
		BankLoad, BankSelection };

	SettingsOwner settings{nullptr, delete_fluid_settings};
	SynthOwner synth{nullptr, delete_fluid_synth};
	int sampleRate = 0;
	int polyphony = 0;
	int fontId = -1;

	Sf2SynthState() = default;
	Sf2SynthState(Sf2SynthState&&) = default;
	Sf2SynthState& operator=(Sf2SynthState&&) = delete;
	Sf2SynthState(const Sf2SynthState&) = delete;
	Sf2SynthState& operator=(const Sf2SynthState&) = delete;

	bool loadBank(const char* path, int channel, int bank, int program, Error& error)
	{
		error = Error::None;
		if (!synth || !path) { error = Error::BankLoad; return false; }
		fontId = fluid_synth_sfload(synth.get(), path, false);
		if (fontId < 0 || !fluid_synth_get_sfont_by_id(synth.get(), fontId))
		{
			error = Error::BankLoad; return false;
		}
		if (fluid_synth_program_select(synth.get(), channel, fontId, bank, program) != FLUID_OK)
		{
			error = Error::BankSelection; return false;
		}
		return true;
	}

	// Factories are injectable for native allocation-failure tests; production
	// always uses FluidSynth. No current synth, bank or voice is changed here.
	static std::optional<Sf2SynthState> create(fluid_settings_t* current, double outputRate, Error& error,
		SettingsFactory settingsFactory = new_fluid_settings, SynthFactory synthFactory = new_fluid_synth)
	{
		error = Error::None;
		if (!std::isfinite(outputRate) || outputRate < 1 || outputRate > std::numeric_limits<int>::max())
		{
			error = Error::InvalidSampleRate; return std::nullopt;
		}
		Sf2SynthState candidate;
		candidate.settings.reset(settingsFactory());
		if (!candidate.settings) { error = Error::SettingsAllocation; return std::nullopt; }
		if (current)
		{
			SettingsCopy copy{current, candidate.settings.get()};
			fluid_settings_foreach(current, &copy, copySetting);
			if (!copy.valid) { error = Error::SettingsCopy; return std::nullopt; }
		}
		if (fluid_settings_getint(candidate.settings.get(), "synth.polyphony", &candidate.polyphony) != FLUID_OK
			|| candidate.polyphony <= 0)
		{
			error = Error::Polyphony; return std::nullopt;
		}
		// Preserve the existing resampling fallback if this native version does
		// not accept the requested engine rate: read its actual configured rate.
		fluid_settings_setnum(candidate.settings.get(), "synth.sample-rate", outputRate);
		double nativeRate = 0;
		if (fluid_settings_getnum(candidate.settings.get(), "synth.sample-rate", &nativeRate) != FLUID_OK
			|| !std::isfinite(nativeRate) || nativeRate < 1 || nativeRate > std::numeric_limits<int>::max())
		{
			error = Error::InvalidSampleRate; return std::nullopt;
		}
		candidate.sampleRate = static_cast<int>(nativeRate);
		candidate.synth.reset(synthFactory(candidate.settings.get()));
		if (!candidate.synth) { error = Error::SynthAllocation; return std::nullopt; }
		return std::optional<Sf2SynthState>{std::move(candidate)};
	}

private:
	struct SettingsCopy
	{
		fluid_settings_t* source;
		fluid_settings_t* destination;
		bool valid = true;
	};

	static void copySetting(void* opaque, const char* name, int type)
	{
		auto& copy = *static_cast<SettingsCopy*>(opaque);
		if (!copy.valid) { return; }
		switch (type)
		{
		case FLUID_NUM_TYPE:
		{
			double value = 0;
			copy.valid = fluid_settings_getnum(copy.source, name, &value) == FLUID_OK
				&& fluid_settings_setnum(copy.destination, name, value) == FLUID_OK;
			break;
		}
		case FLUID_INT_TYPE:
		{
			int value = 0;
			copy.valid = fluid_settings_getint(copy.source, name, &value) == FLUID_OK
				&& fluid_settings_setint(copy.destination, name, value) == FLUID_OK;
			break;
		}
		case FLUID_STR_TYPE:
		{
			char* value = nullptr;
			const int status = fluid_settings_dupstr(copy.source, name, &value);
			// fluid_free is required across Windows CRT/DLL boundaries, including
			// when a native string setting has a null value.
			const std::unique_ptr<char, decltype(&fluid_free)> owned(value, fluid_free);
			copy.valid = status == FLUID_OK && fluid_settings_setstr(copy.destination, name, value) == FLUID_OK;
			break;
		}
		case FLUID_SET_TYPE: break;
		default: copy.valid = false;
		}
	}
};

} // namespace lmms

#endif // LMMS_SF2_SYNTH_STATE_H
