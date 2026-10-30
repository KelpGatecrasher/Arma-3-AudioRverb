AudioReverb (Convolution) - impulse responses
==============================================

openair\ holds eight recorded spaces from the OpenAIR library (CC BY 4.0, see ATTRIBUTION.txt).
By default (AR_irSource = 1) the convolution engine blends them using the scanner's environment
weights - e.g. 60% Alcuin courtyard + 40% Koli open terrain at the edge of a town - and matches
the blend's level to the environment. Mapping: CfgAudioReverb >> IRs in addons\ar_main\config.cpp.

    AR_irSource = 0;    // synthesized impulse responses instead
    AR_irSource = 1;    // OpenAIR recordings (default)

Audition one file everywhere (overrides the blend until cleared):

    ["irs\openair\hangar_t2.wav"] call AR_fnc_ir;
    [] call AR_fnc_ir;                         // back to the scanner

Your own files: PCM 16/24/32-bit or 32-bit float, mono or stereo, any sample rate (resampled),
up to 4.2 s (longer files are truncated). Remove the direct sound first (tools\prepare_irs.py
shows how), otherwise every sound plays twice. Check each file's licence before redistributing.
