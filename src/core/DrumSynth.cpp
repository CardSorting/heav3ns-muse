/*
 * DrumSynth.cpp - DrumSynth DS file renderer
 *
 * Copyright (c) 1998-2000 Paul Kellett (mda-vst.com)
 * Copyright (c) 2007 Paul Giblock <drfaygo/at/gmail.com>
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

#include "DrumSynth.h"

#include <QFile>
#include <QBuffer>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "FileResourceIO.h"
#include "lmms_math.h"

#ifdef _MSC_VER
// not #if LMMS_BUILD_WIN32 because we have strncasecmp in mingw
#define strcasecmp _stricmp
#endif // _MSC_VER

namespace lmms {

using namespace std;

// const int     Fs    =  44100;
const int MAX = 0;
const int ENV = 1;
const int PNT = 2;
const int dENV = 3;
const int NEXTT = 4;

int DrumSynth::LongestEnv()
{
	float l = 0.f;

	for (long e = 1; e < 7; e++) // 3
	{
		long eon = e - 1;
		if (eon > 2)
		{
			eon = eon - 1;
		}
		long p = 0;
		while (p + 1 < 32 && envpts[e][0][p + 1] >= 0.f)
		{
			p++;
		}
		envData[e][MAX] = envpts[e][0][p] * timestretch;
		if (!std::isfinite(envData[e][MAX])) { return 0; }
		if (chkOn[eon] == 1 && envData[e][MAX] > l)
		{
			l = envData[e][MAX];
		}
	}
	// l *= timestretch;

	const auto blocks = std::floor(static_cast<double>(l / 1200.f));
	if (blocks > (std::numeric_limits<int>::max() - 2400) / 1200) { return 0; }
	return 2400 + 1200 * static_cast<int>(blocks);
}

float DrumSynth::LoudestEnv()
{
	float loudest = 0.f;
	int i = 0;

	while (i < 5) // 2
	{
		if ((chkOn[i] == 1) && (sliLev[i] > loudest))
		{
			loudest = static_cast<float>(sliLev[i]);
		}
		i++;
	}
	return loudest * loudest;
}

void DrumSynth::UpdateEnv(int e, long t)
{
	const auto point = static_cast<int>(envData[e][PNT]);
	if (point >= 31)
	{
		// Keep native final-segment interpolation, but never advance past its sentinel.
		envData[e][ENV] = 0.f;
		envData[e][dENV] = 0.f;
		envData[e][NEXTT] = std::numeric_limits<float>::max();
		return;
	}
	// 0.2's added
	envData[e][NEXTT] = envpts[e][0][static_cast<long>(envData[e][PNT] + 1.f)] * timestretch; // get next point
	if (envData[e][NEXTT] < 0)
	{
		envData[e][NEXTT] = 442000 * timestretch; // if end point, hold
	}
	envData[e][ENV] = envpts[e][1][static_cast<long>(envData[e][PNT] + 0.f)] * 0.01f;		  // this level
	float endEnv = envpts[e][1][static_cast<long>(envData[e][PNT] + 1.f)] * 0.01f;			  // next level
	float dT = envData[e][NEXTT] - static_cast<float>(t);
	if (dT < 1.0)
	{
		dT = 1.0;
	}
	envData[e][dENV] = (endEnv - envData[e][ENV]) / dT;
	envData[e][PNT] = envData[e][PNT] + 1.0f;
}

void DrumSynth::GetEnv(int env, const char* sec, const char* key, QString ini)
{
	char en[256];
	int ep = 0;
	GetPrivateProfileString(sec, key, "0,0 100,0", en, sizeof(en), ini);
	char* cursor = en;
	while (*cursor)
	{
		while (std::isspace(static_cast<unsigned char>(*cursor))) { ++cursor; }
		if (!*cursor) { break; }
		if (ep >= 31) { m_valid = false; return; }
		char* end;
		const auto time = std::strtof(cursor, &end);
		if (end == cursor || *end != ',' || !std::isfinite(time) || time < 0.f
			|| (ep > 0 && time < envpts[env][0][ep - 1])) { m_valid = false; return; }
		cursor = end + 1;
		const auto level = std::strtof(cursor, &end);
		if (end == cursor || !std::isfinite(level)
			|| (*end && !std::isspace(static_cast<unsigned char>(*end)))) { m_valid = false; return; }
		envpts[env][0][ep] = time;
		envpts[env][1][ep] = level;
		++ep;
		cursor = end;
	}
	if (ep == 0) { m_valid = false; return; }
	envpts[env][0][ep] = -1.f;
	envData[env][MAX] = envpts[env][0][ep - 1];
}

float DrumSynth::waveform(float ph, int form)
{
	// sine
	if (form == 0) { return std::sin(ph); }
	// sine^2
	if (form == 1) { return std::abs(2.f * std::sin(0.5f * ph)) - 1.f; }
	// sawtooth with range [0, 1], used to generate triangle, sawtooth, and square
	auto saw01 = absFraction(ph / (2 * std::numbers::pi_v<float>));
	// triangle
	if (form == 2) { return 1.f - 4.f * std::abs(saw01 - 0.5f); }
	// sawtooth
	if (form == 3) { return 2.f * saw01 - 1.f; }
	// square
	return (saw01 < 0.5f) ? 1.f : -1.f;
}

std::size_t DrumSynth::GetPrivateProfileString(const char* sec,
	const char* key, const char* def, char* buffer, std::size_t size,
	QString file)
{
	Q_UNUSED(file);
	const auto maxlen = std::max(std::size_t{1}, size) - 1; // TODO C++26: Use std::sub_sat(size, 1)
	stringstream is;
	bool inSection = false;
	std::string line;

	// The native profile is read once, before any parameters or samples are used.
	is.str(string(m_profile.constData(), m_profile.size()));

	// If buffer[0] isn't overwritten after reading file, default value
	// `def` will be used.
	std::memset(buffer, '\0', size);

	while (std::getline(is, line))
	{
		QString error;
		if (!FileResourceIO::check(error)) { m_valid = false; break; }
		if (!inSection)
		{
			const auto open = line.find('['), close = line.find(']', open == std::string::npos ? 0 : open + 1);
			if (open != std::string::npos && close != std::string::npos
				&& !strcasecmp(line.substr(open + 1, close - open - 1).c_str(), sec)) { inSection = true; }
		}
		else
		{
			if (!line.empty() && line[0] == '[') { break; }
			const auto start = line.find_first_not_of(" \t=");
			if (start == std::string::npos) { continue; }
			const auto keyEnd = line.find_first_of(" \t=", start);
			if (keyEnd == std::string::npos) { continue; }
			if (!strcasecmp(line.substr(start, keyEnd - start).c_str(), key))
			{
				const auto valueStart = line.find_first_not_of(" \t=", keyEnd);
				if (valueStart != std::string::npos)
				{
					const auto valueEnd = line.find_last_not_of(" \t\r");
					const auto length = valueEnd >= valueStart ? valueEnd - valueStart + 1 : 0;
					// Comments retain their native display truncation; parameters must be complete.
					if (length > maxlen && strcasecmp(key, "Comment")) { m_valid = false; }
					std::memcpy(buffer, line.data() + valueStart, std::min(length, maxlen));
				}
				break;
			}
		}
	}

	// Use default value `def` if value is missing
	if (!buffer[0]) { std::strncpy(buffer, def, maxlen); }
	// Since nothing ever copies past `maxlen`, buffer must be null-terminated
	const auto len = std::strlen(buffer);
	return len;
}

int DrumSynth::GetPrivateProfileInt(const char* sec, const char* key, int def, QString file)
{
	char tmp[16];
	GetPrivateProfileString(sec, key, "", tmp, sizeof(tmp), file);
	if (tmp[0] == 0) { return def; }
	char* end; errno = 0;
	const auto value = std::strtol(tmp, &end, 10);
	if (end == tmp || errno == ERANGE || value < std::numeric_limits<int>::min()
		|| value > std::numeric_limits<int>::max()) { m_valid = false; return def; }
	return static_cast<int>(value);
}

float DrumSynth::GetPrivateProfileFloat(const char* sec, const char* key, float def, QString file)
{
	char tmp[16];
	GetPrivateProfileString(sec, key, "", tmp, sizeof(tmp), file);
	if (tmp[0] == 0) { return def; }
	char* end;
	const auto value = std::strtof(tmp, &end);
	if (end == tmp || !std::isfinite(value)) { m_valid = false; return def; }
	return value;
}

int DrumSynth::GetDSFileSamples(QString dsfile, int16_t*& wave, int channels, sample_rate_t Fs)
try
{
	using namespace std::numbers;
	if (wave != nullptr || channels <= 0 || Fs == 0 || Fs > std::numeric_limits<int>::max()) { return 0; }
	*this = DrumSynth{};
	QString error;
	if (!FileResourceIO::check(error)) { return 0; }
	QFile profile(dsfile);
	QBuffer cache(&m_profile);
	if (!profile.open(QIODevice::ReadOnly) || profile.size() <= 0 || !cache.open(QIODevice::WriteOnly)
		|| !FileResourceIO::stream(profile, &cache, nullptr, profile.size(), error)) { return 0; }
	cache.close(); profile.close();
	if (m_profile.contains('\0')) { return 0; }
	// input file
	char sec[32];
	char ver[32];
	char comment[256];
	int commentLen = 0;

	// generation
	long Length, tpos = 0, tplus, totmp, t, i, j;
	float x[3] = {0.f, 0.f, 0.f};
	float MasterTune;
	int MainFilter, HighPass;

	long NON, NT, TON, DiON, TDroop = 0, DStep;
	float a, b = 0.f, c = 0.f, d = 0.f, g, TT = 0.f, TL, NL, F1, F2;
	float TphiStart = 0.f, Tphi, TDroopRate, ddF, DAtten, DGain;

	long BON, BON2, BFStep, BFStep2, botmp;
	float BdF = 0.f, BdF2 = 0.f, BPhi, BPhi2, BF, BF2, BQ, BQ2, BL, BL2;

	long OON, OF1Sync = 0, OF2Sync = 0, OMode, OW1, OW2;
	float Ophi1, Ophi2, OF1, OF2, OL, Ot = 0 /*PG: init */, OBal1, OBal2, ODrive;
	float Ocf1, Ocf2, OcF, OcQ, OcA, Oc[6][2]; // overtone cymbal mode
	float Oc0 = 0.0f, Oc1 = 0.0f, Oc2 = 0.0f;

	float MFfb, MFtmp, MFres, MFin = 0.f, MFout = 0.f;
	float DownAve;
	long DownStart, DownEnd, jj;

	// try to read version from input file
	std::strcpy(sec, "General");
	GetPrivateProfileString(sec, "Version", "", ver, sizeof(ver), dsfile);
	ver[9] = 0;
	if ((strcasecmp(ver, "DrumSynth") != 0) // input fail
		|| (ver[11] != '1' && ver[11] != '2'))  // version fail
	{
		return 0;
	}

	// read master parameters
	GetPrivateProfileString(sec, "Comment", "", comment, sizeof(comment), dsfile);
	while ((comment[commentLen] != 0) && (commentLen < 254))
	{
		commentLen++;
	}
	if (commentLen == 0)
	{
		comment[0] = 32;
		comment[1] = 0;
		commentLen = 1;
	}
	comment[commentLen + 1] = 0;
	commentLen++;
	if ((commentLen % 2) == 1)
	{
		commentLen++;
	}

	timestretch = .01f * mem_time * GetPrivateProfileFloat(sec, "Stretch", 100.0, dsfile);
	if (timestretch < 0.2f)
	{
		timestretch = 0.2f;
	}
	if (timestretch > 10.f)
	{
		timestretch = 10.f;
	}
	// the unit of envelope lengths is a sample in 44100Hz sample rate, so correct it
	timestretch *= Fs / 44100.f;

	DGain = 1.0f; // leave this here!
	DGain = fastPow10f(0.05f * GetPrivateProfileFloat(sec, "Level", 0, dsfile));

	MasterTune = GetPrivateProfileFloat(sec, "Tuning", 0.0, dsfile);
	MasterTune = std::pow(1.0594631f, MasterTune + mem_tune);
	const auto filter = GetPrivateProfileInt(sec, "Filter", 0, dsfile);
	if (filter < 0 || filter > 1 || !std::isfinite(MasterTune) || !std::isfinite(DGain)) { return 0; }
	MainFilter = 2 * filter;
	MFres = 0.0101f * GetPrivateProfileFloat(sec, "Resonance", 0.0, dsfile);
	MFres = std::sqrt(MFres);

	HighPass = GetPrivateProfileInt(sec, "HighPass", 0, dsfile);
	GetEnv(7, sec, "FilterEnv", dsfile);

	// read noise parameters
	std::strcpy(sec, "Noise");
	chkOn[1] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	sliLev[1] = GetPrivateProfileInt(sec, "Level", 0, dsfile);
	NT = GetPrivateProfileInt(sec, "Slope", 0, dsfile);
	GetEnv(2, sec, "Envelope", dsfile);
	NON = chkOn[1];
	NL = static_cast<float>(sliLev[1]) * static_cast<float>(sliLev[1]) * mem_n;
	if (NT < 0)
	{
		a = 1.f + (NT / 105.f);
		d = -static_cast<float>(NT) / 105.f;
		g = (1.f + 0.0005f * NT * NT) * NL;
	}
	else
	{
		a = 1.f;
		b = -NT / 50.f;
		c = std::abs(static_cast<float>(NT)) / 100.f;
		g = NL;
	}

	// read tone parameters
	std::strcpy(sec, "Tone");
	chkOn[0] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	TON = chkOn[0];
	sliLev[0] = GetPrivateProfileInt(sec, "Level", 128, dsfile);
	TL = static_cast<float>(sliLev[0]) * static_cast<float>(sliLev[0]) * mem_t;
	GetEnv(1, sec, "Envelope", dsfile);
	if (!m_valid || (TON == 1 && envData[1][MAX] <= 0.f)) { return 0; }
	F1 = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F1", 200.0, dsfile) / Fs;
	if (std::abs(F1) < 0.001f)
	{
		F1 = 0.001f; // to prevent overtone ratio div0
	}
	F2 = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F2", 120.0, dsfile) / Fs;
	TDroopRate = GetPrivateProfileFloat(sec, "Droop", 0.f, dsfile);
	if (TDroopRate > 0.f)
	{
		TDroopRate = fastPow10f((TDroopRate - 20.0f) / 30.0f);
		TDroopRate = TDroopRate * -4.f / envData[1][MAX];
		TDroop = 1;
		F2 = F1 + ((F2 - F1) / (1.f - std::exp(TDroopRate * envData[1][MAX])));
		ddF = F1 - F2;
	}
	else
	{
		ddF = F2 - F1;
	}

	Tphi = GetPrivateProfileFloat(sec, "Phase", 90.f, dsfile) / 57.29578f; // degrees>radians

	// read overtone parameters
	std::strcpy(sec, "Overtones");
	chkOn[2] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	OON = chkOn[2];
	sliLev[2] = GetPrivateProfileInt(sec, "Level", 128, dsfile);
	OL = static_cast<float>(sliLev[2]) * static_cast<float>(sliLev[2]) * mem_o;
	GetEnv(3, sec, "Envelope1", dsfile);
	GetEnv(4, sec, "Envelope2", dsfile);
	OMode = GetPrivateProfileInt(sec, "Method", 2, dsfile);
	OF1 = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F1", 200.0, dsfile) / Fs;
	OF2 = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F2", 120.0, dsfile) / Fs;
	OW1 = GetPrivateProfileInt(sec, "Wave1", 0, dsfile);
	OW2 = GetPrivateProfileInt(sec, "Wave2", 0, dsfile);
	OBal2 = static_cast<float>(GetPrivateProfileInt(sec, "Param", 50, dsfile));
	ODrive = (OBal2 * OBal2 * OBal2) / 125000.0f;
	OBal2 *= 0.01f;
	OBal1 = 1.f - OBal2;
	Ophi1 = Tphi;
	Ophi2 = Tphi;
	if (MainFilter == 0)
	{
		MainFilter = GetPrivateProfileInt(sec, "Filter", 0, dsfile);
	}
	if ((GetPrivateProfileInt(sec, "Track1", 0, dsfile) == 1) && (TON == 1))
	{
		OF1Sync = 1;
		OF1 = OF1 / F1;
	}
	if ((GetPrivateProfileInt(sec, "Track2", 0, dsfile) == 1) && (TON == 1))
	{
		OF2Sync = 1;
		OF2 = OF2 / F1;
	}

	OcA = 0.28f + OBal1 * OBal1; // overtone cymbal mode
	OcQ = OcA * OcA;
	OcF = (1.8f - 0.7f * OcQ) * 0.92f; // multiply by env 2
	OcA *= 1.0f + 4.0f * OBal1;		   // level is a compromise!
	Ocf1 = 2 * pi_v<float> / OF1;
	Ocf2 = 2 * pi_v<float> / OF2;
	for (i = 0; i < 6; i++)
	{
		Oc[i][0] = Oc[i][1] = Ocf1 + (Ocf2 - Ocf1) * 0.2f * static_cast<float>(i);
	}

	// read noise band parameters
	std::strcpy(sec, "NoiseBand");
	chkOn[3] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	BON = chkOn[3];
	sliLev[3] = GetPrivateProfileInt(sec, "Level", 128, dsfile);
	BL = static_cast<float>(sliLev[3]) * static_cast<float>(sliLev[3]) * mem_b;
	BF = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F", 1000.0, dsfile) / Fs;
	BPhi = pi_v<float> / 4.f;
	GetEnv(5, sec, "Envelope", dsfile);
	BFStep = GetPrivateProfileInt(sec, "dF", 50, dsfile);
	BQ = static_cast<float>(BFStep);
	BQ = BQ * BQ / (10000.f - 6600.f * (std::sqrt(BF) - 0.19f));
	const auto step = (40.f - (BFStep / 2.5f)) / (BQ + 1.f + (1.f * BF));
	if (!std::isfinite(step) || step <= -1.f || step >= static_cast<double>(std::numeric_limits<int>::max())) { return 0; }
	BFStep = 1 + static_cast<int>(step);

	std::strcpy(sec, "NoiseBand2");
	chkOn[4] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	BON2 = chkOn[4];
	sliLev[4] = GetPrivateProfileInt(sec, "Level", 128, dsfile);
	BL2 = static_cast<float>(sliLev[4]) * static_cast<float>(sliLev[4]) * mem_b;
	BF2 = MasterTune * 2 * pi_v<float> * GetPrivateProfileFloat(sec, "F", 1000.0, dsfile) / Fs;
	BPhi2 = pi_v<float> / 4.f;
	GetEnv(6, sec, "Envelope", dsfile);
	BFStep2 = GetPrivateProfileInt(sec, "dF", 50, dsfile);
	BQ2 = static_cast<float>(BFStep2);
	BQ2 = BQ2 * BQ2 / (10000.f - 6600.f * (std::sqrt(BF2) - 0.19f));
	const auto step2 = (40 - (BFStep2 / 2.5)) / (BQ2 + 1 + (1 * BF2));
	if (!std::isfinite(step2) || step2 <= -1.0 || step2 >= static_cast<double>(std::numeric_limits<int>::max())) { return 0; }
	BFStep2 = 1 + static_cast<int>(step2);

	// read distortion parameters
	std::strcpy(sec, "Distortion");
	chkOn[5] = GetPrivateProfileInt(sec, "On", 0, dsfile);
	DiON = chkOn[5];
	const auto distortionRate = GetPrivateProfileInt(sec, "Rate", 0, dsfile);
	if (distortionRate < 0 || distortionRate >= 1200) { return 0; }
	DStep = 1 + distortionRate;
	if (DStep == 7) { DStep = 20; }
	if (DStep == 6) { DStep = 10; }
	if (DStep == 5) { DStep = 8; }

	clippoint = 32700;
	DAtten = 1.0f;

	if (DiON == 1)
	{
		const auto loudest = LoudestEnv();
		if (!std::isfinite(loudest) || loudest > std::numeric_limits<short>::max()) { return 0; }
		DAtten = DGain * static_cast<short>(loudest);
		if (!std::isfinite(DAtten) || DAtten < 0.f) { return 0; }
		clippoint = DAtten > 32700 ? 32700 : static_cast<short>(DAtten);
		DAtten = std::exp2(2.0 * GetPrivateProfileInt(sec, "Bits", 0, dsfile));
		DGain = DAtten * DGain * fastPow10f(0.05f * GetPrivateProfileInt(sec, "Clipping", 0, dsfile));
	}

	// prepare envelopes
	for (i = 1; i < 8; i++)
	{
		envData[i][NEXTT] = 0;
		envData[i][PNT] = 0;
	}
	Length = LongestEnv();
	if (!m_valid || Length <= 0 || !FileResourceIO::check(error) || !std::isfinite(DGain)
		|| !std::isfinite(DAtten) || DAtten <= 0.f || !std::isfinite(MFres)
		|| static_cast<std::size_t>(Length) > std::numeric_limits<std::size_t>::max() / sizeof(int16_t) / channels) { return 0; }
	auto output = std::make_unique<int16_t[]>(static_cast<std::size_t>(Length) * channels);
	std::size_t wavewords = 0;

	/*
	if(wavemode==0)
	{
	  //open output file
	  fp = fopen(wavfile, "wb");
	  if(!fp) {return 3;} //output fail

	   //set up INFO chunk
	  WI.list = 0x5453494C;
	  WI.listLength = 36 + commentLen;
	  WI.info = 0x4F464E49;
	  WI.isft = 0x54465349;
	  WI.isftLength = 16;
	  strcpy(WI.software, "DrumSynth v2.0 "); WI.software[15]=0;
	  WI.icmt = 0x544D4349;
	  WI.icmtLength = commentLen;

	  //write WAV header
	  WH.riff = 0x46464952;
	  WH.riffLength = 36 + (2 * Length) + 44 + commentLen;
	  WH.wave = 0x45564157;
	  WH.fmt = 0x20746D66;
	  WH.waveLength = 16;
	  WH.wFormatTag = WAVE_FORMAT_PCM;
	  WH.nChannels = 1;
	  WH.nSamplesPerSec = Fs;
	  WH.nAvgBytesPerSec = 2 * Fs;
	  WH.nBlockAlign = 2;
	  WH.wBitsPerSample = 16;
	  WH.data = 0x61746164;
	  WH.dataLength = 2 * Length;
	  fwrite(&WH, 1, 44, fp);
	}
	*/

	// generate
	tpos = 0;
	while (tpos < Length)
	{
		if (!FileResourceIO::check(error)) { return 0; }
		tplus = tpos + 1199;

		if (NON == 1) // noise
		{
			for (t = tpos; t <= tplus; t++)
			{
				if (t < envData[2][NEXTT])
				{
					envData[2][ENV] = envData[2][ENV] + envData[2][dENV];
				}
				else
				{
					UpdateEnv(2, t);
				}
				x[2] = x[1];
				x[1] = x[0];
				x[0] = fastRandInc(-1.f, 1.f);
				TT = a * x[0] + b * x[1] + c * x[2] + d * TT;
				DF[t - tpos] = TT * g * envData[2][ENV];
			}
			if (t >= envData[2][MAX])
			{
				NON = 0;
			}
		}
		else
		{
			for (j = 0; j < 1200; j++)
			{
				DF[j] = 0.f;
			}
		}

		if (TON == 1) // tone
		{
			TphiStart = Tphi;
			if (TDroop == 1)
			{
				for (t = tpos; t <= tplus; t++)
				{
					phi[t - tpos] = F2 + (ddF * std::exp(t * TDroopRate));
				}
			}
			else
			{
				for (t = tpos; t <= tplus; t++)
				{
					phi[t - tpos] = F1 + (t / envData[1][MAX]) * ddF;
				}
			}
			for (t = tpos; t <= tplus; t++)
			{
				totmp = t - tpos;
				if (t < envData[1][NEXTT])
				{
					envData[1][ENV] = envData[1][ENV] + envData[1][dENV];
				}
				else
				{
					UpdateEnv(1, t);
				}
				Tphi = Tphi + phi[totmp];
				DF[totmp] += TL * envData[1][ENV] * std::sin(Tphi); // overflow?
			}
			if (t >= envData[1][MAX])
			{
				TON = 0;
			}
		}
		else
		{
			for (j = 0; j < 1200; j++)
			{
				phi[j] = F2; // for overtone sync
			}
		}

		if (BON == 1) // noise band 1
		{
			for (t = tpos; t <= tplus; t++)
			{
				if (t < envData[5][NEXTT])
				{
					envData[5][ENV] = envData[5][ENV] + envData[5][dENV];
				}
				else
				{
					UpdateEnv(5, t);
				}
				if ((t % BFStep) == 0)
				{
					BdF = fastRandInc(-0.5f, 0.5f);
				}
				BPhi = BPhi + BF + BQ * BdF;
				botmp = t - tpos;
				DF[botmp] = DF[botmp] + std::cos(BPhi) * envData[5][ENV] * BL;
			}
			if (t >= envData[5][MAX])
			{
				BON = 0;
			}
		}

		if (BON2 == 1) // noise band 2
		{
			for (t = tpos; t <= tplus; t++)
			{
				if (t < envData[6][NEXTT])
				{
					envData[6][ENV] = envData[6][ENV] + envData[6][dENV];
				}
				else
				{
					UpdateEnv(6, t);
				}
				if ((t % BFStep2) == 0)
				{
					BdF2 = fastRandInc(-0.5f, 0.5f);
				}
				BPhi2 = BPhi2 + BF2 + BQ2 * BdF2;
				botmp = t - tpos;
				DF[botmp] = DF[botmp] + std::cos(BPhi2) * envData[6][ENV] * BL2;
			}
			if (t >= envData[6][MAX])
			{
				BON2 = 0;
			}
		}

		for (t = tpos; t <= tplus; t++)
		{
			if (OON == 1) // overtones
			{
				if (t < envData[3][NEXTT])
				{
					envData[3][ENV] = envData[3][ENV] + envData[3][dENV];
				}
				else
				{
					if (t >= envData[3][MAX]) // wait for OT2
					{
						envData[3][ENV] = 0;
						envData[3][dENV] = 0;
						envData[3][NEXTT] = 999999;
					}
					else
					{
						UpdateEnv(3, t);
					}
				}
				//
				if (t < envData[4][NEXTT])
				{
					envData[4][ENV] = envData[4][ENV] + envData[4][dENV];
				}
				else
				{
					if (t >= envData[4][MAX]) // wait for OT1
					{
						envData[4][ENV] = 0;
						envData[4][dENV] = 0;
						envData[4][NEXTT] = 999999;
					}
					else
					{
						UpdateEnv(4, t);
					}
				}
				//
				TphiStart = TphiStart + phi[t - tpos];
				if (OF1Sync == 1)
				{
					Ophi1 = TphiStart * OF1;
				}
				else
				{
					Ophi1 = Ophi1 + OF1;
				}
				if (OF2Sync == 1)
				{
					Ophi2 = TphiStart * OF2;
				}
				else
				{
					Ophi2 = Ophi2 + OF2;
				}
				Ot = 0.0f;
				switch (OMode)
				{
				case 0: // add
					Ot = OBal1 * envData[3][ENV] * waveform(Ophi1, OW1);
					Ot = OL * (Ot + OBal2 * envData[4][ENV] * waveform(Ophi2, OW2));
					break;

				case 1: // FM
					Ot = ODrive * envData[4][ENV] * waveform(Ophi2, OW2);
					Ot = OL * envData[3][ENV] * waveform(Ophi1 + Ot, OW1);
					break;

				case 2: // RM
					Ot = (1 - ODrive / 8) + (((ODrive / 8) * envData[4][ENV]) * waveform(Ophi2, OW2));
					Ot = OL * envData[3][ENV] * waveform(Ophi1, OW1) * Ot;
					break;

				case 3: // 808 Cymbal
					for (j = 0; j < 6; j++)
					{
						Oc[j][0] += 1.0f;

						if (Oc[j][0] > Oc[j][1])
						{
							Oc[j][0] -= Oc[j][1];
							Ot = OL * envData[3][ENV];
						}
					}
					Ocf1 = envData[4][ENV] * OcF; // filter freq
					Oc0 += Ocf1 * Oc1;
					Oc1 += Ocf1 * (Ot + Oc2 - OcQ * Oc1 - Oc0); // bpf
					Oc2 = Ot;
					Ot = Oc1;
					break;
				}
			}

			if (MainFilter == 1) // filter overtones
			{
				if (t < envData[7][NEXTT])
				{
					envData[7][ENV] = envData[7][ENV] + envData[7][dENV];
				}
				else
				{
					UpdateEnv(7, t);
				}

				MFtmp = envData[7][ENV];
				if (MFtmp > 0.2f)
				{
					MFfb = 1.001f - fastPow10f(MFtmp - 1);
				}
				else
				{
					MFfb = 0.999f - 0.7824f * MFtmp;
				}

				MFtmp = Ot + MFres * (1.f + (1.f / MFfb)) * (MFin - MFout);
				MFin = MFfb * (MFin - MFtmp) + MFtmp;
				MFout = MFfb * (MFout - MFin) + MFin;

				DF[t - tpos] = DF[t - tpos] + (MFout - (HighPass * Ot));
			}
			else if (MainFilter == 2) // filter all
			{
				if (t < envData[7][NEXTT])
				{
					envData[7][ENV] = envData[7][ENV] + envData[7][dENV];
				}
				else
				{
					UpdateEnv(7, t);
				}

				MFtmp = envData[7][ENV];
				if (MFtmp > 0.2f)
				{
					MFfb = 1.001f - fastPow10f(MFtmp - 1);
				}
				else
				{
					MFfb = 0.999f - 0.7824f * MFtmp;
				}

				MFtmp = DF[t - tpos] + Ot + MFres * (1.f + (1.f / MFfb)) * (MFin - MFout);
				MFin = MFfb * (MFin - MFtmp) + MFtmp;
				MFout = MFfb * (MFout - MFin) + MFin;

				DF[t - tpos] = MFout - (HighPass * (DF[t - tpos] + Ot));
			}
			// PG: Ot is uninitialized
			else
			{
				DF[t - tpos] = DF[t - tpos] + Ot; // no filter
			}
		}

		if (DiON == 1) // bit resolution
		{
			for (j = 0; j < 1200; j++)
			{
				const auto quantized = DF[j] / DAtten;
				if (!std::isfinite(quantized) || static_cast<double>(quantized) < std::numeric_limits<int>::min()
					|| static_cast<double>(quantized) > std::numeric_limits<int>::max()) { return 0; }
				DF[j] = DGain * static_cast<int>(quantized);
			}

			for (j = 0; j < 1200; j += DStep) // downsampling
			{
				DownAve = 0;
				DownStart = j;
				DownEnd = std::min(j + DStep - 1, 1199L);
				for (jj = DownStart; jj <= DownEnd; jj++)
				{
					DownAve = DownAve + DF[jj];
				}
				DownAve = DownAve / (DownEnd - DownStart + 1);
				for (jj = DownStart; jj <= DownEnd; jj++)
				{
					DF[jj] = DownAve;
				}
			}
		}
		else
		{
			for (j = 0; j < 1200; j++)
			{
				DF[j] *= DGain;
			}
		}

		for (j = 0; j < 1200; j++) // clipping + output
		{
			if (!std::isfinite(DF[j])) { return 0; }
			if (DF[j] > clippoint)
			{
				output[wavewords++] = clippoint;
			}
			else if (DF[j] < -clippoint)
			{
				output[wavewords++] = -clippoint;
			}
			else
			{
				output[wavewords++] = static_cast<short>(DF[j]);
			}

			for (int c = 1; c < channels; c++)
			{
				output[wavewords] = output[wavewords - 1];
				wavewords++;
			}
		}

		tpos = tpos + 1200;
	}

	/*
	if(wavemode==0)
	{
	  fwrite(wave, 2, Length, fp);  //write data
	  fwrite(&WI,  1, 44, fp); //write INFO chunk
	  fwrite(&comment, 1, commentLen, fp);
	  fclose(fp);
	}
	wavemode = 0; //force compatibility!!
	*/

	if (!FileResourceIO::check(error)) { return 0; }
	wave = output.release();
	return Length;
}
catch (const std::bad_alloc&) { return 0; }
catch (const std::length_error&) { return 0; }

} // namespace lmms
