// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// Waveform types for sound synthesis
enum class Waveform { SQUARE, SAW, TRIANGLE, SINE };

/// ADSR amplitude envelope
struct ADSREnvelope {
    float attack_ms = 5;
    float decay_ms = 40;
    float sustain_level = 0.6f; // 0.0-1.0
    float release_ms = 80;
};

/// Low-frequency oscillator for parameter modulation
struct LFOParams {
    std::string target; // "freq", "amplitude", "duty"
    float rate = 0;     // Hz
    float depth = 0;    // amount of modulation
};

/// Parameter sweep (glide over step duration)
struct SweepParams {
    std::string target; // "freq"
    float end_value = 0;
};

/// Audio filter parameters
struct FilterParams {
    std::string type; // "lowpass", "highpass"
    float cutoff = 20000;
    float sweep_to = 0; // 0 = no sweep
};

/// Single step in a sound sequence
struct SoundStep {
    float freq_hz = 0; // 0 = pause
    float duration_ms = 0;
    Waveform wave = Waveform::SQUARE;
    float velocity = 0.8f; // 0.0-1.0
    ADSREnvelope envelope;
    LFOParams lfo;
    SweepParams sweep;
    FilterParams filter;
    bool is_pause = false;
    // Polyphonic chord (empty = monophonic, use freq_hz on voice 0)
    std::array<float, 4> chord_freqs{};
    uint8_t chord_count = 0; // 0 = monophonic
};

/// A named sound (sequence of steps)
struct SoundDefinition {
    std::string name;
    std::string description;
    std::vector<SoundStep> steps;
    int repeat = 1;
    float bpm = 0; // 0 = no BPM (durations in ms)
};

/// A complete theme containing named sounds
struct SoundTheme {
    std::string name;
    std::string description;
    int version = 1;
    // Default values applied when steps omit them
    Waveform default_wave = Waveform::SQUARE;
    float default_velocity = 0.8f;
    ADSREnvelope default_envelope;
    std::unordered_map<std::string, SoundDefinition> sounds;
};

/// JSON theme parser for sound definitions
class SoundThemeParser {
  public:
    /// Load theme from JSON file on disk
    /// @return parsed theme, or nullopt on error
    static std::optional<SoundTheme> load_from_file(const std::string& path);

    /// Load theme from JSON string (useful for testing)
    /// @return parsed theme, or nullopt on error
    static std::optional<SoundTheme> load_from_string(const std::string& json_str);

    /// Convert note name to frequency in Hz
    /// Supports C0-B8, sharps (C#4) and flats (Db4). A4 = 440 Hz.
    /// @return frequency in Hz, or 0 if invalid
    static float note_to_freq(const std::string& note);

    /// Convert musical duration notation to milliseconds
    /// Supports: "1n" (whole), "2n" (half), "4n" (quarter), "8n" (eighth),
    /// "16n" (sixteenth), "4n." (dotted quarter), "8t" (eighth triplet)
    /// @param dur Musical duration string
    /// @param bpm Tempo in beats per minute
    /// @return duration in ms, or 0 if invalid
    static float musical_duration_to_ms(const std::string& dur, float bpm);
};
