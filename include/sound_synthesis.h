// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sound_theme.h" // Waveform enum

#include <atomic>
#include <string>

namespace helix::audio {

/// Filter type enum — replaces std::string to eliminate data race between
/// sequencer thread (writer) and audio render thread (reader).
enum class FilterType : int { NONE = 0, LOWPASS = 1, HIGHPASS = 2 };

/// Convert string to FilterType (for theme JSON parsing)
FilterType filter_type_from_string(const std::string& type);

/// Biquad filter state (Direct Form II Transposed)
struct BiquadFilter {
    float b0 = 1, b1 = 0, b2 = 0;
    float a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    bool active = false;
    FilterType current_type = FilterType::NONE;
    float current_cutoff = 0;
    float current_sample_rate = 0;
};

/// Generate waveform samples into buffer.
/// @param phase Modified in-place for continuity across calls.
void generate_samples(float* buffer, int num_samples, int sample_rate, Waveform wave, float freq,
                      float amplitude, float duty_cycle, float& phase);

/// Compute biquad coefficients for lowpass or highpass.
void compute_biquad_coeffs(BiquadFilter& f, FilterType type, float cutoff, float sample_rate);

/// Apply filter to buffer in-place.
void apply_filter(BiquadFilter& f, float* buffer, int num_samples);

/// Recompute filter coefficients only if parameters changed.
void update_filter_if_needed(BiquadFilter& f, FilterType type, float cutoff, float sample_rate);

} // namespace helix::audio
