/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * E3 Indication Debug Handlers
 */

#include "e3_manager.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <complex>
#include <algorithm>
#include <sys/stat.h>

using json = nlohmann::json;

namespace e3 {

// Debug control flags
static const bool ENABLE_VERBOSE_DEBUG = false;     // Verbose console output
static const bool ENABLE_HEST_BINARY_SAVE = false;  // Save H-estimates to binary files
static const bool ENABLE_HEST_CSV_SAVE = false;     // Save H-estimates to CSV files

static const std::string HEST_OUTPUT_DIR = "/tmp/e3_debug"; // Output directory for H-estimates


// Ensure output directory exists
static void ensureOutputDir() {
    static bool created = false;
    if (!created) {
        mkdir(HEST_OUTPUT_DIR.c_str(), 0755);
        created = true;
    }
}

// FP16 to FP32 conversion (used only for IQ sample debug output)
static float fp16_to_fp32(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    
    if (exponent == 0) {
        // Subnormal or zero
        if (mantissa == 0) {
            return sign ? -0.0f : 0.0f;
        } else {
            float value = ldexpf(mantissa / 1024.0f, -14);
            return sign ? -value : value;
        }
    } else if (exponent == 31) {
        // Infinity or NaN
        if (mantissa == 0) {
            return sign ? -INFINITY : INFINITY;
        } else {
            return NAN;
        }
    } else {
        // Normal number
        float value = ldexpf((1024 + mantissa) / 1024.0f, exponent - 15);
        return sign ? -value : value;
    }
}

void ProcessIQSampleDebug(const json& indication_payload, void* ran_shm_ptr) {
    if (!ENABLE_VERBOSE_DEBUG) return;
    if (!indication_payload.contains("iq_samples") || ran_shm_ptr == nullptr) {
        return;
    }

    uint16_t sfn = indication_payload.value("sfn", 0u);
    uint16_t slot = indication_payload.value("slot", 0u);

    const json& iq_data = indication_payload["iq_samples"];
    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);

    uint8_t fh_buf_idx = iq_data.value("fh_buffer_index", 0u);
    uint32_t fh_write_idx = iq_data.value("fh_write_index", 0u);

    if (fh_write_idx == 0) return;

    std::cout << "\n=== IQ Samples Debug Info ===" << std::endl;
    std::cout << "SFN/Slot: " << sfn << "/" << slot << std::endl;
    std::cout << "Buffer index: " << (int)fh_buf_idx << ", Write index: " << fh_write_idx << std::endl;

    // Calculate offset
    size_t base_offset = sizeof(SharedMemoryHeader);
    size_t single_row_size_bytes = header->num_fh_samples * sizeof(int16_t);
    size_t buffer_base_offset = base_offset + (fh_buf_idx * header->fh_buffer_size);
    size_t row_offset = fh_write_idx * single_row_size_bytes;
    size_t total_offset = buffer_base_offset + row_offset;

    uint32_t num_complex_samples = header->num_fh_samples / 2;
    std::cout << "Num complex samples: " << num_complex_samples << " (FP16 I/Q pairs)" << std::endl;

    if (num_complex_samples == 0) {
        std::cout << "No IQ samples data for this slot\n==============================\n" << std::endl;
        return;
    }

    const int16_t* raw_data = reinterpret_cast<const int16_t*>(
        static_cast<const uint8_t*>(ran_shm_ptr) + total_offset);

    // Print first few samples
    const size_t samples_to_print = std::min((size_t)10, (size_t)num_complex_samples);
    std::cout << "\nFirst " << samples_to_print << " complex samples:" << std::endl;
    for (size_t i = 0; i < samples_to_print; ++i) {
        int16_t real_bits = raw_data[i*2];
        int16_t imag_bits = raw_data[i*2 + 1];
        
        uint16_t real_bits_u = *reinterpret_cast<const uint16_t*>(&real_bits);
        uint16_t imag_bits_u = *reinterpret_cast<const uint16_t*>(&imag_bits);
        
        float real_val = fp16_to_fp32(real_bits_u);
        float imag_val = fp16_to_fp32(imag_bits_u);
        
        std::cout << "  [" << i << "]: raw=(" << real_bits << ", " << imag_bits << ") "
                  << "float=(" << real_val << ", " << imag_val << "j) "
                  << "magnitude: " << std::abs(std::complex<float>(real_val, imag_val)) << std::endl;
    }

    if (num_complex_samples > 20) {
        std::cout << "\nLast " << samples_to_print << " complex samples:" << std::endl;
        for (size_t i = num_complex_samples - samples_to_print; i < num_complex_samples; ++i) {
            int16_t real_bits = raw_data[i*2];
            int16_t imag_bits = raw_data[i*2 + 1];
            
            uint16_t real_bits_u = *reinterpret_cast<const uint16_t*>(&real_bits);
            uint16_t imag_bits_u = *reinterpret_cast<const uint16_t*>(&imag_bits);
            
            float real_val = fp16_to_fp32(real_bits_u);
            float imag_val = fp16_to_fp32(imag_bits_u);
            
            std::cout << "  [" << i << "]: raw=(" << real_bits << ", " << imag_bits << ") "
                      << "float=(" << real_val << ", " << imag_val << "j) "
                      << "magnitude: " << std::abs(std::complex<float>(real_val, imag_val)) << std::endl;
        }
    }
    std::cout << "==============================\n" << std::endl;
}

void PrintMetadataDebug(const json& payload) {
    std::cout << "=== Indication Metadata ===" << std::endl;

    std::cout << "  Cell: {";
    bool first = true;
    for (const auto& [key, value] : payload.items()) {
        if (key == "ue_metrics") continue;
        if (!first) std::cout << ",";
        std::cout << "\"" << key << "\":" << value.dump();
        first = false;
    }
    std::cout << "}" << std::endl;

    if (payload.contains("ue_metrics") && payload["ue_metrics"].is_array()) {
        for (size_t i = 0; i < payload["ue_metrics"].size(); ++i) {
            std::cout << "  UE " << i << ": " << payload["ue_metrics"][i].dump() << std::endl;
        }
    }
    std::cout << "=============================" << std::endl;
}

void ProcessHEstimatesDebug(const json& indication_payload, void* ran_shm_ptr) {
    if (!ENABLE_VERBOSE_DEBUG && !ENABLE_HEST_BINARY_SAVE && !ENABLE_HEST_CSV_SAVE) return;
    if (!indication_payload.contains("h_estimates") || !indication_payload.contains("ue_metrics") ||
        ran_shm_ptr == nullptr) {
        return;
    }

    uint16_t sfn = indication_payload.value("sfn", 0u);
    uint16_t slot = indication_payload.value("slot", 0u);
    uint64_t timestamp_ns = indication_payload.value("timestamp", 0ull);
    uint8_t n_bs_ants = indication_payload.value("n_bs_ants", 0u);

    const json& hest_data = indication_payload["h_estimates"];
    const json& ue_metrics = indication_payload["ue_metrics"];
    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);

    uint8_t hest_buf_idx = hest_data.value("hest_buffer_index", 0u);
    uint32_t hest_write_idx = hest_data.value("hest_write_index", 0u);
    uint32_t hest_row_byte_off = hest_data.value("hest_row_byte_offset", 0u);
    if (hest_buf_idx >= 2 || hest_row_byte_off >= header->hest_buffer_size) {
        std::cerr << " OOB indication (buf_idx=" << +hest_buf_idx
                  << " offset=" << hest_row_byte_off
                  << " hest_buffer_size=" << header->hest_buffer_size << "), skipping." << std::endl;
        return;
    }
    if (hest_row_byte_off % sizeof(std::complex<float>) != 0) {
        std::cerr << " Misaligned hest_row_byte_offset=" << hest_row_byte_off
                  << " (not a multiple of " << sizeof(std::complex<float>)
                  << " bytes), skipping." << std::endl;
        return;
    }

    // Compute SHM row base pointer (v1.1.0: byte offset from indication, no fixed stride)
    size_t base_offset = sizeof(SharedMemoryHeader);
    size_t hest_base_offset = base_offset + (2 * header->fh_buffer_size) + (2 * header->pusch_buffer_size);
    size_t hest_buffer_offset = hest_buf_idx * header->hest_buffer_size;
    const size_t remaining_samples = (header->hest_buffer_size - hest_row_byte_off) / sizeof(std::complex<float>);
    const std::complex<float>* row_ptr = reinterpret_cast<const std::complex<float>*>(
        static_cast<const uint8_t*>(ran_shm_ptr) + hest_base_offset + hest_buffer_offset + hest_row_byte_off);

    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "\n=== H Estimates Debug (per-UE) ===" << std::endl;
        std::cout << "SFN/Slot: " << sfn << "/" << slot
                  << "  buffer: " << (hest_buf_idx == 0 ? "ping" : "pong")
                  << "  write_idx: " << hest_write_idx
                  << "  n_bs_ants: " << (int)n_bs_ants
                  << "  n_ue: " << ue_metrics.size() << std::endl;

        size_t fh_total = 2 * header->fh_buffer_size;
        size_t pusch_total = 2 * header->pusch_buffer_size;
        size_t hest_total = 2 * header->hest_buffer_size;
        size_t expected_hest_offset = base_offset + fh_total + pusch_total;
        size_t actual_hest0_offset = hest_base_offset;
        size_t actual_hest1_offset = hest_base_offset + header->hest_buffer_size;

        std::cout << "SHM layout: header=" << base_offset << " FH=" << fh_total
                  << " PUSCH=" << pusch_total << " HEST=" << hest_total
                  << " total=" << (base_offset + fh_total + pusch_total + hest_total) << " bytes" << std::endl;
        std::cout << "HEST offset: expected=" << expected_hest_offset
                  << " actual_buf0=" << actual_hest0_offset
                  << " actual_buf1=" << actual_hest1_offset
                  << " max_per_row=" << header->max_hest_samples_per_row
                  << (actual_hest0_offset == expected_hest_offset ? " [OK]" : " [MISMATCH]") << std::endl;
    }

    for (size_t i = 0; i < ue_metrics.size(); ++i) {
        const auto& ue = ue_metrics[i];
        uint16_t rnti = ue.value("rnti", 0u);
        uint8_t n_layers = ue.value("n_layers", 0u);
        uint16_t layer_offset = ue.value("layer_offset", 0u);
        uint16_t ue_grp_idx = ue.value("ue_grp_idx", 0u);
        uint32_t h_offset = ue.value("h_offset", 0u);
        uint32_t h_size = ue.value("h_size", 0u);
        uint16_t n_subcarriers = ue.value("n_subcarriers", 0u);
        uint8_t n_dmrs_estimates = ue.value("n_dmrs_estimates", 0u);

        if (h_size == 0) continue;
        if (static_cast<size_t>(h_offset) + h_size > remaining_samples) {
            std::cerr << " UE " << i << " OOB (h_offset=" << h_offset
                      << " h_size=" << h_size << " remaining=" << remaining_samples
                      << "), skipping." << std::endl;
            continue;
        }

        const std::complex<float>* grp_ptr = row_ptr + h_offset;

        if (ENABLE_VERBOSE_DEBUG) {
            std::cout << "  UE " << i << " rnti=" << rnti << " grp=" << ue_grp_idx
                      << " layers=" << (int)n_layers << " layer_off=" << layer_offset
                      << " h_offset=" << h_offset << " h_size=" << h_size
                      << " sc=" << n_subcarriers << " dmrs=" << (int)n_dmrs_estimates << std::endl;

            float max_mag = 0.0f, sum_mag = 0.0f;
            for (uint32_t s = 0; s < h_size; ++s) {
                float mag = std::abs(grp_ptr[s]);
                sum_mag += mag;
                if (mag > max_mag) max_mag = mag;
            }
            uint32_t n_layers_group = (n_subcarriers && n_bs_ants && n_dmrs_estimates)
                ? h_size / (n_subcarriers * n_bs_ants * n_dmrs_estimates) : 0;
            std::cout << "    avg_mag=" << std::fixed << std::setprecision(4) << (sum_mag / h_size)
                      << "  max_mag=" << max_mag
                      << "  bytes=" << h_size * sizeof(std::complex<float>)
                      << "  n_layers_group=" << n_layers_group << std::endl;

            const uint32_t N_PRINT = std::min(10u, h_size);
            std::cout << "    first " << N_PRINT << " samples:";
            for (uint32_t s = 0; s < N_PRINT; ++s) {
                std::cout << " (" << grp_ptr[s].real() << "," << grp_ptr[s].imag() << "j)";
            }
            std::cout << std::endl;
            if (h_size > N_PRINT) {
                std::cout << "    last  " << N_PRINT << " samples:";
                for (uint32_t s = h_size - N_PRINT; s < h_size; ++s) {
                    std::cout << " (" << grp_ptr[s].real() << "," << grp_ptr[s].imag() << "j)";
                }
                std::cout << std::endl;
            }
        }

        if (ENABLE_HEST_BINARY_SAVE) {
            SaveChannelEstimatesBinary(grp_ptr, h_size, n_bs_ants, n_layers, layer_offset,
                                       n_subcarriers, n_dmrs_estimates,
                                       rnti, ue_grp_idx, sfn, slot, timestamp_ns);
        }

        if (ENABLE_HEST_CSV_SAVE) {
            SaveChannelEstimatesCSV(grp_ptr, h_size, n_bs_ants, n_layers, layer_offset,
                                    n_subcarriers, n_dmrs_estimates,
                                    rnti, sfn, slot, timestamp_ns);
        }
    }

    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "==================================\n" << std::endl;
    }
}

void SaveChannelEstimatesCSV(
    const std::complex<float>* grp_ptr,
    uint32_t h_size,
    uint8_t n_bs_ants,
    uint8_t n_layers,
    uint16_t layer_offset,
    uint16_t n_subcarriers,
    uint8_t n_dmrs_estimates,
    uint16_t rnti,
    uint16_t sfn,
    uint16_t slot,
    uint64_t timestamp_ns) {

    if (grp_ptr == nullptr || h_size == 0 || n_bs_ants == 0 || n_subcarriers == 0 ||
        n_dmrs_estimates == 0) return;

    // Derive total layers in this group's tensor from known dimensions
    uint32_t n_layers_group = h_size / (n_subcarriers * n_bs_ants * n_dmrs_estimates);
    if (n_layers_group == 0) return;

    try {
        ensureOutputDir();

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

        std::stringstream filename_ss;
        filename_ss << "hest_" << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S") << "_"
                   << std::setfill('0') << std::setw(6) << us.count() << "_"
                   << std::setfill('0') << std::setw(5) << sfn << "_"
                   << std::setfill('0') << std::setw(2) << slot << "_"
                   << "rnti" << rnti << ".csv";

        std::string filepath = HEST_OUTPUT_DIR + "/" + filename_ss.str();

        std::ofstream csv_file(filepath);
        if (!csv_file.is_open()) {
            std::cerr << "Failed to open file for writing: " << filepath << std::endl;
            return;
        }

        csv_file << "# H Estimates for SFN.Slot: " << sfn << "." << slot << " RNTI: " << rnti << std::endl;
        csv_file << "# timestamp_ns: " << timestamp_ns << std::endl;
        csv_file << "# n_bs_ants=" << (int)n_bs_ants << ", n_layers=" << (int)n_layers
                 << ", layer_offset=" << layer_offset << ", n_layers_group=" << n_layers_group
                 << ", n_subcarriers=" << n_subcarriers << ", n_dmrs_estimates=" << (int)n_dmrs_estimates << std::endl;
        csv_file << "# Layout: [dmrs][subcarrier][antenna][layer] row-major. Showing first UE layer, dmrs=0." << std::endl;

        csv_file << "subcarrier";
        for (int ant = 0; ant < n_bs_ants; ++ant) {
            csv_file << ",ant" << ant << "_magnitude";
        }
        csv_file << std::endl;

        // Index: (dmrs * n_subcarriers * n_bs_ants * n_layers_group) + (sc * n_bs_ants * n_layers_group) + (ant * n_layers_group) + layer_offset
        for (int sc = 0; sc < n_subcarriers; ++sc) {
            csv_file << sc;
            for (int ant = 0; ant < n_bs_ants; ++ant) {
                uint32_t idx = (sc * n_bs_ants * n_layers_group) + (ant * n_layers_group) + layer_offset;
                float magnitude = (idx < h_size) ? std::abs(grp_ptr[idx]) : 0.0f;
                csv_file << "," << std::fixed << std::setprecision(6) << magnitude;
            }
            csv_file << std::endl;
        }

        csv_file.close();
        std::cout << "H-est CSV saved: " << filepath << " (" << n_subcarriers << " sc x " << (int)n_bs_ants << " ants)" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error saving channel estimates CSV: " << e.what() << std::endl;
    }
}

void SaveChannelEstimatesBinary(
    const std::complex<float>* grp_ptr,
    uint32_t h_size,
    uint8_t n_bs_ants,
    uint8_t n_layers,
    uint16_t layer_offset,
    uint16_t n_subcarriers,
    uint8_t n_dmrs_estimates,
    uint16_t rnti,
    uint16_t ue_grp_idx,
    uint16_t sfn,
    uint16_t slot,
    uint64_t timestamp_ns) {

    if (grp_ptr == nullptr || h_size == 0) return;

    try {
        ensureOutputDir();

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

        std::stringstream filename_ss;
        filename_ss << "hest_" << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S") << "_"
                   << std::setfill('0') << std::setw(6) << us.count() << "_"
                   << std::setfill('0') << std::setw(5) << sfn << "_"
                   << std::setfill('0') << std::setw(2) << slot << "_"
                   << "rnti" << rnti << ".bin";

        std::string filepath = HEST_OUTPUT_DIR + "/" + filename_ss.str();

        std::ofstream bin_file(filepath, std::ios::binary);
        if (!bin_file.is_open()) {
            std::cerr << "Failed to open binary file for writing: " << filepath << std::endl;
            return;
        }
        
        // Write header structure (for easy parsing later)
        struct BinaryHeader {
            uint32_t magic;
            uint32_t version;
            uint64_t timestamp_ns;
            uint16_t sfn;
            uint16_t slot;
            uint16_t rnti;
            uint16_t ue_grp_idx;
            uint8_t n_bs_ants;
            uint8_t n_layers;
            uint16_t layer_offset;
            uint16_t n_subcarriers;
            uint8_t n_dmrs_estimates;
            uint8_t reserved;
            uint32_t h_size;
            uint32_t data_offset;
        } __attribute__((packed));

        BinaryHeader hdr{};
        hdr.magic = 0x48455354;    // "HEST"
        hdr.version = 2;
        hdr.timestamp_ns = timestamp_ns;
        hdr.sfn = sfn;
        hdr.slot = slot;
        hdr.rnti = rnti;
        hdr.ue_grp_idx = ue_grp_idx;
        hdr.n_bs_ants = n_bs_ants;
        hdr.n_layers = n_layers;
        hdr.layer_offset = layer_offset;
        hdr.n_subcarriers = n_subcarriers;
        hdr.n_dmrs_estimates = n_dmrs_estimates;
        hdr.h_size = h_size;
        hdr.data_offset = sizeof(BinaryHeader);

        bin_file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        bin_file.write(reinterpret_cast<const char*>(grp_ptr),
                      h_size * sizeof(std::complex<float>));
        bin_file.close();

        if (ENABLE_VERBOSE_DEBUG) {
            std::cout << "H-est binary saved: " << filepath
                      << " (" << h_size << " samples, "
                      << (h_size * sizeof(std::complex<float>)) << " bytes)" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error saving binary channel estimates: " << e.what() << std::endl;
    }
}

} // namespace e3

