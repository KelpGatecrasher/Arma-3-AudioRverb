// AudioReverb phase 0 probe. Not for release: overrides vanilla HouseAttenuation with
// deliberately exaggerated values so it is obvious whether the engine applies it.

class CfgPatches
{
	class AR_Probe
	{
		name = "AudioReverb - Phase 0 Probe";
		author = "AudioReverb";
		units[] = {"AR_Probe_SUV_Dry", "AR_Probe_SUV_Small", "AR_Probe_SUV_Large"};
		weapons[] = {};
		requiredVersion = 2.0;
		requiredAddons[] = {"A3_Data_F_Decade_Loadorder"};
	};
};

class CfgFunctions
{
	class AR_Probe
	{
		tag = "AR";
		class Probe
		{
			file = "\ar_probe\functions";
			class dump {};
			class status {};
			class shot {};
		};
	};
};

// Flat EQ so only the Echo is heard. FXEQ gain 1 = 0 dB.
#define AR_FLAT_EQ(NAME) class NAME \
{ \
	center[] = {100, 800, 2000, 10000}; \
	bandwidth[] = {1, 1, 1, 1}; \
	gain[] = {1, 1, 1, 1}; \
}

// FXECHO ranges: WetDryMix 0..1, Feedback 0..1, Delay 1..2000 ms.
#define AR_ECHO_CHAIN(NAME, WET, FB, DELAY) class NAME \
{ \
	AR_FLAT_EQ(Equalizer0); \
	AR_FLAT_EQ(Equalizer1); \
	class Echo \
	{ \
		WetDryMix = WET; \
		Feedback = FB; \
		Delay = DELAY; \
	}; \
}

// Full XAUDIO2FX_REVERB_PARAMETERS set at extreme values. Static analysis says the engine
// parses these into Sound::XA2ReverEffectPars but has no effect that consumes them; if
// anything here is audible, that conclusion is wrong.
#define AR_HUGE_REVERB(NAME) class NAME \
{ \
	WetDryMix = 100; \
	ReflectionsDelay = 50; \
	ReverbDelay = 85; \
	RearDelay = 5; \
	PositionLeft = 6; \
	PositionRight = 6; \
	PositionMatrixLeft = 27; \
	PositionMatrixRight = 27; \
	EarlyDiffusion = 15; \
	LateDiffusion = 15; \
	LowEQGain = 8; \
	LowEQCutoff = 4; \
	HighEQGain = 8; \
	HighEQCutoff = 4; \
	RoomFilterFreq = 5000; \
	RoomFilterMain = 0; \
	RoomFilterHF = 0; \
	ReflectionsGain = 10; \
	ReverbGain = 20; \
	DecayTime = 20; \
	Density = 100; \
	RoomSize = 100; \
}

class CfgSoundEffects
{
	class AttenuationsEffects
	{
		// Engine-hardcoded name, used while the listener is inside a building_interior object.
		AR_ECHO_CHAIN(HouseAttenuation, 0.5, 0.5, 90);

		AR_ECHO_CHAIN(AR_Probe_Dry, 0, 0, 1);
		AR_ECHO_CHAIN(AR_Probe_Small, 0.35, 0.3, 25);
		AR_ECHO_CHAIN(AR_Probe_Large, 0.45, 0.6, 180);
	};

	class ReverbationEffects
	{
		AR_HUGE_REVERB(Default);
		AR_HUGE_REVERB(Generic);
		AR_HUGE_REVERB(HouseAttenuation);
		AR_HUGE_REVERB(CarAttenuation);
		AR_HUGE_REVERB(Hangar);
	};
};

class CfgVehicles
{
	class C_SUV_01_F;
	class AR_Probe_SUV_Dry: C_SUV_01_F
	{
		scope = 2;
		scopeCurator = 2;
		displayName = "[AR Probe] SUV - no echo";
		attenuationEffectType = "AR_Probe_Dry";
	};
	class AR_Probe_SUV_Small: AR_Probe_SUV_Dry
	{
		displayName = "[AR Probe] SUV - small echo";
		attenuationEffectType = "AR_Probe_Small";
	};
	class AR_Probe_SUV_Large: AR_Probe_SUV_Dry
	{
		displayName = "[AR Probe] SUV - large echo";
		attenuationEffectType = "AR_Probe_Large";
	};
};
