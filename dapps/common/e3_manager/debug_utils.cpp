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
static const bool ENABLE_VERBOSE_DEBUG = true;     // Verbose console output
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
    std::cout << "\n=== Indication Metadata ===" << std::endl;
    for (const auto& [key, value] : payload.items()) {
        if (value.is_object()) {
            std::cout << key << ": " << value.dump() << std::endl;
        } else {
            std::cout << key << ": " << value << std::endl;
        }
    }
    std::cout << "=============================" << std::endl;
}

void ProcessHEstimatesDebug(const json& indication_payload, void* ran_shm_ptr) {
    if (!indication_payload.contains("h_estimates") || ran_shm_ptr == nullptr) {
        return;
    }

    uint16_t sfn = indication_payload.value("sfn", 0u);
    uint16_t slot = indication_payload.value("slot", 0u);
    uint64_t timestamp_ns = indication_payload.value("timestamp", 0ull);
    
    // Extract metadata
    uint8_t n_bs_ants = indication_payload.value("n_bs_ants", 0u);
    uint8_t n_layers = indication_payload.value("n_layers", 0u);
    uint16_t n_subcarriers = indication_payload.value("n_subcarriers", 0u);
    uint8_t n_dmrs_estimates = indication_payload.value("n_dmrs_estimates", 0u);
    
    const json& hest_data = indication_payload["h_estimates"];
    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);
    
    uint8_t hest_buf_idx = hest_data.value("hest_buffer_index", 0u);
    uint32_t hest_write_idx = hest_data.value("hest_write_index", 0u);
    uint32_t hest_data_size = hest_data.value("hest_data_size", 0u);
    
    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "\n=== H Estimates Debug Info (dApp) ===" << std::endl;
        std::cout << "SFN/Slot: " << sfn << "/" << slot << std::endl;
        std::cout << "Data index: " << hest_write_idx << " in buffer: " << (hest_buf_idx == 0 ? "ping" : "pong") << std::endl;
        std::cout << "H-estimates size: " << hest_data_size << " complex<float> samples" << std::endl;
    }
    
    if (hest_data_size == 0) {
        if (ENABLE_VERBOSE_DEBUG) {
            std::cout << "No H estimates data for this slot" << std::endl;
            std::cout << "==============================\n" << std::endl;
        }
        return;
    }
    
    // Calculate base addresses for both buffers
    size_t base_offset = sizeof(SharedMemoryHeader);
    size_t hest_base_offset = base_offset + (2 * header->fh_buffer_size) + (2 * header->pusch_buffer_size);
    
    if (ENABLE_VERBOSE_DEBUG) {
        uint8_t* base_ptr = reinterpret_cast<uint8_t*>(header + 1);
        const std::complex<float>* hest_buf0_ptr = reinterpret_cast<const std::complex<float>*>(
            static_cast<uint8_t*>(ran_shm_ptr) + hest_base_offset);
        const std::complex<float>* hest_buf1_ptr = reinterpret_cast<const std::complex<float>*>(
            static_cast<uint8_t*>(ran_shm_ptr) + hest_base_offset + header->hest_buffer_size);
        
        std::cout << "\nH-estimates Buffer memory layout:" << std::endl;
        std::cout << "Buffer base address (ping): " << (void*)hest_buf0_ptr << std::endl;
        std::cout << "Buffer base address (pong): " << (void*)hest_buf1_ptr << std::endl;
        std::cout << "Max samples per row: " << header->max_hest_samples_per_row << std::endl;
        std::cout << "Size per row: " << header->max_hest_samples_per_row * sizeof(std::complex<float>) << " bytes" << std::endl;
        
        std::cout << "\n=== Buffer Start Analysis ===" << std::endl;
        for (int buf = 0; buf < 2; buf++) {
            const std::complex<float>* buf_start = (buf == 0) ? hest_buf0_ptr : hest_buf1_ptr;
            std::cout << "Buffer " << buf << " (" << (buf == 0 ? "ping" : "pong") << ") first 10 complex values:" << std::endl;
            for (int i = 0; i < 10 && i < (int)header->max_hest_samples_per_row; i++) {
                std::cout << "  [" << i << "]: (" << std::fixed << std::setprecision(6) 
                          << buf_start[i].real() << "," << buf_start[i].imag() << "j)" << std::endl;
            }
            
            std::cout << "Buffer " << buf << " first sample from first 5 rows:" << std::endl;
            for (int row = 0; row < 5 && row < (int)header->num_hest_rows; row++) {
                const std::complex<float>* row_start = buf_start + (row * header->max_hest_samples_per_row);
                std::cout << "  Row " << row << ": (" << std::fixed << std::setprecision(6) 
                          << row_start[0].real() << "," << row_start[0].imag() << "j)" << std::endl;
            }
        }
        
        std::cout << "\n=== MEMORY LAYOUT VERIFICATION (dApp) ===" << std::endl;
        size_t header_size = sizeof(SharedMemoryHeader);
        size_t fh_total = 2 * header->fh_buffer_size;
        size_t pusch_total = 2 * header->pusch_buffer_size;
        size_t hest_total = 2 * header->hest_buffer_size;
        size_t expected_hest_offset = base_offset + fh_total + pusch_total;
        
        size_t shm_total_size = header_size + fh_total + pusch_total + hest_total;
        std::cout << "Shared memory total size: " << shm_total_size << " bytes" << std::endl;
        std::cout << "Header size: " << header_size << " bytes" << std::endl;
        std::cout << "Base pointer after header: " << (void*)base_ptr << std::endl;
        std::cout << "FH buffer size: " << header->fh_buffer_size << " bytes each (" << fh_total << " total)" << std::endl;
        std::cout << "PUSCH buffer size: " << header->pusch_buffer_size << " bytes each (" << pusch_total << " total)" << std::endl;
        std::cout << "HEST buffer size: " << header->hest_buffer_size << " bytes each (" << hest_total << " total)" << std::endl;
        std::cout << "Expected HEST start offset: " << expected_hest_offset << " bytes" << std::endl;
        
        size_t actual_hest0_offset = reinterpret_cast<size_t>(hest_buf0_ptr) - reinterpret_cast<size_t>(ran_shm_ptr);
        size_t actual_hest1_offset = reinterpret_cast<size_t>(hest_buf1_ptr) - reinterpret_cast<size_t>(ran_shm_ptr);
        
        std::cout << "Actual HEST buffer 0 offset: " << actual_hest0_offset << " bytes" << std::endl;
        std::cout << "Actual HEST buffer 1 offset: " << actual_hest1_offset << " bytes" << std::endl;
        
        if (actual_hest0_offset == expected_hest_offset) {
            std::cout << "[OK] HEST buffer 0 offset matches expected!" << std::endl;
        } else {
            std::cout << "[ERROR] HEST buffer 0 offset MISMATCH! Expected: " << expected_hest_offset 
                      << ", Actual: " << actual_hest0_offset << " (diff: " 
                      << (int64_t)actual_hest0_offset - (int64_t)expected_hest_offset << ")" << std::endl;
        }
        
        if (actual_hest1_offset == expected_hest_offset + header->hest_buffer_size) {
            std::cout << "[OK] HEST buffer 1 offset matches expected!" << std::endl;
        } else {
            std::cout << "[ERROR] HEST buffer 1 offset MISMATCH! Expected: " << (expected_hest_offset + header->hest_buffer_size)
                      << ", Actual: " << actual_hest1_offset << " (diff: " 
                      << (int64_t)actual_hest1_offset - (int64_t)(expected_hest_offset + header->hest_buffer_size) << ")" << std::endl;
        }
        std::cout << "=========================================" << std::endl;

        std::cout << "\n=== DATA TYPE INTERPRETATION VERIFICATION ===" << std::endl;
        std::cout << "std::complex<float> size: " << sizeof(std::complex<float>) << " bytes" << std::endl;
        std::cout << "hest_data_size: " << hest_data_size << " complex samples" << std::endl;
        std::cout << "Physical layout: [" << (int)n_dmrs_estimates << ", " << n_subcarriers 
                  << ", " << (int)n_bs_ants << ", " << (int)n_layers << "] (row-major)" << std::endl;
        std::cout << "Allocated subcarriers: " << n_subcarriers << " (" << n_subcarriers / 12 << " PRBs)" << std::endl;
        std::cout << "==========================================" << std::endl;
    }
    
    // Calculate offset to H estimates data
    size_t hest_buffer_offset = hest_buf_idx * header->hest_buffer_size;
    size_t row_offset = hest_write_idx * header->max_hest_samples_per_row * sizeof(std::complex<float>);
    size_t total_offset = hest_base_offset + hest_buffer_offset + row_offset;
    
    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "\n[dApp] Reading from shared memory:" << std::endl;
        std::cout << "  hest_buffer_index: " << (int)hest_buf_idx << std::endl;
        std::cout << "  hest_write_index: " << hest_write_idx << std::endl;
        std::cout << "  Offset calculation:" << std::endl;
        std::cout << "    base_offset: " << base_offset << " bytes" << std::endl;
        std::cout << "    hest_base_offset: " << hest_base_offset << " bytes" << std::endl;
        std::cout << "    hest_buffer_offset: " << hest_buffer_offset << " bytes" << std::endl;
        std::cout << "    row_offset: " << row_offset << " bytes (write_idx=" << hest_write_idx 
                  << " × max_per_row=" << header->max_hest_samples_per_row << " × 8)" << std::endl;
        std::cout << "    total_offset: " << total_offset << " bytes" << std::endl;
    }
    
    const std::complex<float>* hest_ptr = reinterpret_cast<const std::complex<float>*>(
        static_cast<const uint8_t*>(ran_shm_ptr) + total_offset);
    
    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "  Memory address: " << (void*)hest_ptr << std::endl;
        
        // Print first few samples
        const size_t samples_to_print = std::min(20u, hest_data_size);
        std::cout << "\nFirst " << samples_to_print << " H-estimate complex samples:" << std::endl;
        for (size_t i = 0; i < samples_to_print; ++i) {
            const auto& sample = hest_ptr[i];
            std::cout << "  [" << i << "]: "
                      << "(" << sample.real() << ", " << sample.imag() << "j) "
                      << "magnitude: " << std::abs(sample) << std::endl;
        }
        
        // Print last few samples if we have enough data
        if (hest_data_size > 20) {
            std::cout << "\nLast " << samples_to_print << " H-estimate complex samples:" << std::endl;
            for (size_t i = hest_data_size - samples_to_print; i < hest_data_size; ++i) {
                const auto& sample = hest_ptr[i];
                std::cout << "  [" << i << "]: "
                          << "(" << sample.real() << ", " << sample.imag() << "j) "
                          << "magnitude: " << std::abs(sample) << std::endl;
            }
        }
        
        // Calculate statistics
        float max_magnitude = 0.0f, avg_magnitude = 0.0f;
        for (size_t i = 0; i < hest_data_size; ++i) {
            float mag = std::abs(hest_ptr[i]);
            avg_magnitude += mag;
            if (mag > max_magnitude) max_magnitude = mag;
        }
        avg_magnitude /= hest_data_size;
        
        std::cout << "\nH Estimates Statistics:" << std::endl;
        std::cout << "  Average magnitude: " << avg_magnitude << std::endl;
        std::cout << "  Maximum magnitude: " << max_magnitude << std::endl;
        std::cout << "  Total bytes: " << hest_data_size * sizeof(std::complex<float>) << std::endl;
    }
    
    // Save data in binary format
    if (ENABLE_HEST_BINARY_SAVE) {
        SaveChannelEstimatesBinary(hest_ptr, hest_data_size, n_bs_ants, n_layers, 
                                   n_subcarriers, n_dmrs_estimates, sfn, slot, timestamp_ns);
    }
    
    // Save data in CSV format
    if (ENABLE_HEST_CSV_SAVE) {
        SaveChannelEstimatesCSV(hest_ptr, hest_data_size, n_bs_ants, n_layers, 
                                n_subcarriers, n_dmrs_estimates, sfn, slot, timestamp_ns);
    }
    
    if (ENABLE_VERBOSE_DEBUG) {
        std::cout << "==============================\n" << std::endl;
    }
}

void SaveChannelEstimatesCSV(
    const std::complex<float>* hest_ptr,
    uint32_t hest_data_size,
    uint8_t n_bs_ants,
    uint8_t n_layers,
    uint16_t n_subcarriers,
    uint8_t n_dmrs_estimates,
    uint16_t sfn,
    uint16_t slot,
    uint64_t timestamp_ns) {
    
    if (hest_ptr == nullptr || hest_data_size == 0 || n_bs_ants == 0) {
        return;
    }
    
    try {
        // Calculate samples per antenna
        int samples_per_ant = (n_layers * n_subcarriers * n_dmrs_estimates);
        
        if (samples_per_ant <= 0 || n_bs_ants * samples_per_ant > hest_data_size) {
            std::cout << "Skipping data save: dimension mismatch" << std::endl;
            return;
        }
        
        ensureOutputDir();
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
        
        std::stringstream filename_ss;
        filename_ss << "hest_" << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S") << "_"
                   << std::setfill('0') << std::setw(6) << ms.count() << "_"
                   << std::setfill('0') << std::setw(5) << sfn << "_"
                   << std::setfill('0') << std::setw(2) << slot << ".csv";
        
        std::string filepath = HEST_OUTPUT_DIR + "/" + filename_ss.str();
        
        std::ofstream csv_file(filepath);
        if (!csv_file.is_open()) {
            std::cerr << "Failed to open file for writing: " << filepath << std::endl;
            return;
        }
        
        // Write header
        csv_file << "# Channel Estimates Data for SFN.Slot: " << sfn << "." << slot << std::endl;
        csv_file << "# timestamp_ns: " << timestamp_ns << std::endl;
        csv_file << "# n_bs_ants=" << (int)n_bs_ants << ", n_layers=" << (int)n_layers 
                 << ", n_subcarriers=" << n_subcarriers << ", n_dmrs_estimates=" << (int)n_dmrs_estimates << std::endl;
        csv_file << "# Physical memory layout: [dmrs][subcarrier][antenna][layer] (row-major)" << std::endl;
        csv_file << "# CSV format: Each row is a subcarrier, columns are antennas (layer=0, dmrs=0)" << std::endl;
        
        csv_file << "subcarrier";
        for (int ant = 0; ant < n_bs_ants; ++ant) {
            csv_file << ",ant" << ant << "_magnitude";
        }
        csv_file << std::endl;
        
        // Write data for each subcarrier (layer 0, DMRS estimate 0)
        // Data is stored as [dmrs][subcarrier][antenna][layer], so for dmrs=0, layer=0:
        // Index formula: (0 * n_subcarriers * n_bs_ants * n_layers) + (subcarrier * n_bs_ants * n_layers) + (ant * n_layers) + 0
        // Simplifies to: ant + n_bs_ants * subcarrier (for single layer)
        for (int subcarrier = 0; subcarrier < n_subcarriers; ++subcarrier) {
            csv_file << subcarrier;
            
            for (int ant = 0; ant < n_bs_ants; ++ant) {
                // Index for [dmrs=0, subcarrier, antenna, layer=0]
                int idx = ant + n_bs_ants * subcarrier;
                
                float magnitude = 0.0f;
                if (idx < hest_data_size) {
                    magnitude = std::abs(hest_ptr[idx]);
                }
                
                csv_file << "," << std::fixed << std::setprecision(6) << magnitude;
            }
            csv_file << std::endl;
        }
        
        csv_file.close();
        
        std::cout << "Channel estimates data saved: " << filepath << std::endl;
        std::cout << "  Format: CSV with " << n_subcarriers << " subcarriers x " << (int)n_bs_ants << " antennas" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving channel estimates data: " << e.what() << std::endl;
    }
}

void SaveChannelEstimatesBinary(
    const std::complex<float>* hest_ptr,
    uint32_t hest_data_size,
    uint8_t n_bs_ants,
    uint8_t n_layers,
    uint16_t n_subcarriers,
    uint8_t n_dmrs_estimates,
    uint16_t sfn,
    uint16_t slot,
    uint64_t timestamp_ns) {
    
    if (hest_ptr == nullptr || hest_data_size == 0) {
        return;
    }
    
    try {
        ensureOutputDir();
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
        
        std::stringstream filename_ss;
        filename_ss << "hest_" << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S") << "_"
                   << std::setfill('0') << std::setw(6) << ms.count() << "_"
                   << std::setfill('0') << std::setw(5) << sfn << "_"
                   << std::setfill('0') << std::setw(2) << slot << ".bin";
        
        std::string filepath = HEST_OUTPUT_DIR + "/" + filename_ss.str();
        
        std::ofstream bin_file(filepath, std::ios::binary);
        if (!bin_file.is_open()) {
            std::cerr << "Failed to open binary file for writing: " << filepath << std::endl;
            return;
        }
        
        // Write header structure (for easy parsing later)
        struct BinaryHeader {
            uint32_t magic;              // Magic number: 0x48455354 ("HEST")
            uint32_t version;            // Format version: 1
            uint64_t timestamp_ns;       // Timestamp in nanoseconds
            uint16_t sfn;                // System Frame Number
            uint16_t slot;               // Slot number
            uint8_t n_bs_ants;          // Number of base station antennas
            uint8_t n_layers;           // Number of layers
            uint16_t n_subcarriers;     // Number of subcarriers
            uint8_t n_dmrs_estimates;   // Number of DMRS estimates
            uint8_t reserved1;          // Padding
            uint16_t reserved2;         // Padding
            uint32_t hest_data_size;    // Number of complex samples
            uint32_t data_offset;       // Offset to data (bytes from file start)
        } __attribute__((packed));
        
        BinaryHeader header;
        header.magic = 0x48455354;  // "HEST"
        header.version = 1;
        header.timestamp_ns = timestamp_ns;
        header.sfn = sfn;
        header.slot = slot;
        header.n_bs_ants = n_bs_ants;
        header.n_layers = n_layers;
        header.n_subcarriers = n_subcarriers;
        header.n_dmrs_estimates = n_dmrs_estimates;
        header.reserved1 = 0;
        header.reserved2 = 0;
        header.hest_data_size = hest_data_size;
        header.data_offset = sizeof(BinaryHeader);
        
        // Write header
        bin_file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        
        // Write raw H-estimates data (complex<float> = 2 floats per sample = 8 bytes)
        bin_file.write(reinterpret_cast<const char*>(hest_ptr), 
                      hest_data_size * sizeof(std::complex<float>));
        
        bin_file.close();
        
        if (ENABLE_VERBOSE_DEBUG) {
            std::cout << "H-estimates binary data saved: " << filepath << std::endl;
            std::cout << "  Format: Binary with " << hest_data_size << " complex<float> samples ("
                      << (hest_data_size * sizeof(std::complex<float>)) << " bytes data + " 
                      << sizeof(BinaryHeader) << " bytes header)" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving binary channel estimates: " << e.what() << std::endl;
    }
}

} // namespace e3

