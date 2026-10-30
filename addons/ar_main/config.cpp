class CfgPatches
{
	class AR_Main
	{
		name = "AudioReverb";
		author = "AudioReverb";
		version = "0.3.0-conv";
		units[] = {};
		weapons[] = {};
		requiredVersion = 2.0;
		requiredAddons[] = {"A3_Data_F_Decade_Loadorder"};
	};
};

class CfgFunctions
{
	class AR
	{
		tag = "AR";
		class Core
		{
			file = "\ar_main\functions";
			class settings { preInit = 1; };
			class init { postInit = 1; };
			class ext {};
			class scan {};
			class mix {};
			class debug {};
			class audition {};
			class taps {};
			class ir {};
			class listener {};
			class voice {};
		};
	};
};

// Environment model. The scanner (AR_fnc_scan) measures the space around the listener, then
// AR_fnc_mix blends the presets below by weight and the extension glides to the result.
class CfgAudioReverb
{
	interval = 0.25;        // s between scans
	glide = 0.8;            // s to glide to a new environment
	glideFast = 0.3;        // s when indoor/outdoor flips
	rayLength = 50;         // m, horizontal/elevated rays used for classification
	echoLength = 150;       // m, same rays extended for echoes (max ~200: 1.2 s delay line)
	roofLength = 30;        // m, upward rays
	nearDistance = 15;      // m, a wall closer than this counts as enclosing
	objectScanEvery = 4;    // scans between nearestTerrainObjects passes (trees/houses)
	objectRadius = 60;      // m
	treesForFullForest = 40;
	housesForFullUrban = 15;
	vehicleClosedBlend = 0.85;

	// values[] = {room, roomHF, roomRolloff, decayTime, decayHFRatio, reflections, reflectionsDelay,
	//             reverb, reverbDelay, diffusion, density, hfReference, send}
	// First 12 are XAudio2 I3DL2 units (mB / s / % / Hz); send is the linear gain from the game's
	// 3D mix into the reverb bus (0..4).
	// Shapes start from the XAUDIO2FX_I3DL2_PRESET_* tables noted on each line, but room is 0
	// instead of the standard -1000: the send is the level control, and stacking both left the
	// tail ~35 dB under the dry sound (inaudible). Measured offline: Hangar at send 1 puts the bus
	// output ~12 dB under its input peak. Use AR_fnc_audition to compare presets by ear.
	//
	// Outdoors the geometric echoes (class Echo) supply the early reflections, so the outdoor presets
	// turn the reverb's own early reflections off (-10000) and keep only a sparse, grainy late field
	// (low diffusion/density, longer pre-delay). A dense, diffuse field is what makes a space read
	// as "a room", which is why the old urban preset sounded indoors.
	class Presets
	{
		class Open        { values[] = {0, -2000, 0, 1.49, 0.50, -10000, 0.120, -1800, 0.100, 15,  40, 5000, 1.10}; }; // PLAIN, sparse
		class Forest      { values[] = {0, -3300, 0, 1.49, 0.54,  -3000, 0.162,  -800, 0.088, 79, 100, 5000, 0.80}; }; // FOREST (scattering is diffuse)
		class Urban       { values[] = {0, -1500, 0, 1.60, 0.45, -10000, 0.020, -2000, 0.060, 20,  35, 5000, 0.40}; }; // CITY, sparse late field (send -60%)
		class UrbanDense  { values[] = {0, -1000, 0, 1.40, 0.55, -10000, 0.010, -1300, 0.040, 35,  50, 5000, 0.40}; }; // ALLEY, sparse late field (send -60%)
		class IndoorSmall { values[] = {0,  -600, 0, 0.80, 0.83,  -400, 0.005,   300, 0.010,100, 100, 5000, 1.00}; }; // SMALLROOM, shorter decay
		class IndoorMedium{ values[] = {0,  -600, 0, 1.30, 0.83, -1000, 0.010,  -200, 0.020,100, 100, 5000, 1.10}; }; // MEDIUMROOM
		class IndoorLarge { values[] = {0,  -600, 0, 1.80, 0.70, -2000, 0.030, -1400, 0.060,100, 100, 5000, 1.60}; }; // LARGEHALL
		class Hangar      { values[] = {0, -1000, 0, 5.00, 0.23,  -602, 0.020,   198, 0.030,100, 100, 5000, 1.00}; }; // HANGAR, 10s decay halved
		class VehicleClosed{values[] = {0, -1500, 0, 0.30, 0.50,  -800, 0.002,  -800, 0.003,100, 100, 5000, 0.40}; }; // tiny damped cabin
	};

	// Geometric early echoes: one delay tap per scanner ray that hits a surface (AR_fnc_taps).
	class Echo
	{
		level = 1.5;              // overall echo gain (also scaled by AR_intensity)
		indoorScale = 0.2;        // echo level indoors relative to outdoors (the room reverb takes over)
		minDistance = 4;          // m, closer surfaces are left to the reverb (< 23 ms)
		referenceDistance = 8;    // m, spreading loss: gain = refl * ref / (ref + 2 * distance)
		reflectStructure = 0.7;   // buildings, walls
		reflectOther = 0.35;      // terrain, rocks, trees
		elevatedWeight = 0.5;     // rays angled 35 degrees up
		baseDamp = 0.15;          // HF loss on every reflection (0..0.95)
		airDampPer100m = 0.25;    // extra HF loss per 100 m of distance
		rearDamp = 0.35;          // extra HF loss for surfaces behind the listener (head shadow)
	};

	// Room size (m) -> indoor preset, interpolated between neighbours.
	indoorSizes[] = {3, 7, 14, 30};
	indoorPresets[] = {"IndoorSmall", "IndoorMedium", "IndoorLarge", "Hangar"};

	// TeamSpeak voice reverb (AR_fnc_voice): direct-speech audibility -> reverb send per player.
	class Voice
	{
		range = 20;               // m, TFAR's normal speaking range (scaled by tf_voiceVolume)
		cutoffExtra = 15;         // m, TFAR stops direct speech at range + 15
		fullLevelFraction = 0.5;  // full reverb send out to this fraction of the cutoff, then fades
		vehicleIsolation = 0.15;  // send multiplier when a closed vehicle separates you
	};

	// Convolution with recorded spaces (AR_irSource = 1): each preset's weight from the scanner
	// becomes that recording's share of a blended impulse response. Paths are relative to the
	// mod folder; "" = the synthesized IR. OpenAIR recordings, CC BY 4.0 - see irs\ATTRIBUTION.txt.
	class IRs
	{
		Open = "irs\openair\open_koli.wav";                   // Koli National Park, open terrain (T60 1.3 s)
		Forest = "irs\openair\forest_wheldrake.wav";          // Wheldrake Wood forest (1.7 s)
		Urban = "irs\openair\urban_alcuin.wav";               // Alcuin College courtyard (1.8 s)
		UrbanDense = "irs\openair\urbandense_trollers.wav";   // Troller's Gill limestone gorge (0.4 s)
		IndoorSmall = "irs\openair\indoorsmall_stairway.wav"; // stairwell (0.9 s)
		IndoorMedium = "irs\openair\indoormedium_typing.wav"; // Terry's Typing Room, 3500 m3 (2.4 s)
		IndoorLarge = "irs\openair\indoorlarge_warehouse.wav";// Terry's Factory Warehouse, 4500 m3 (6.6 s)
		Hangar = "irs\openair\hangar_t2.wav";                 // T2 Hangar, Yorkshire Air Museum (2.4 s)
		VehicleClosed = "";                                   // no recording: synthesized cabin
	};
};
