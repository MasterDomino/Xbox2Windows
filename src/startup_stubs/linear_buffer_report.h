#pragma once

// What d3d12_xs.dll reports about slot 133's opt-in linear buffers (X2W_D3D12_LINEAR_BUFFERS,
// src/startup_stubs/linear_buffer_backing.h) to fixtures through x2w_d3d12_linear_buffer_info and
// x2w_d3d12_linear_buffer_totals. Plain layouts: the DLL and the fixtures include this file alike.

#include <cstdint>

struct X2wLinearBufferInfo {
    std::uint32_t path;             // 1 shared, 2 copied (a refused placement creates no resource)
    std::uint32_t use;              // 1 GPU-read, 2 GPU-written
    std::int32_t feature_hr;        // S_OK supported, DXGI_ERROR_UNSUPPORTED not, other: query failed; S_FALSE not queried
    std::int32_t share_hr;          // result of the sharing attempt; S_FALSE when not attempted
    std::uint64_t heap_generation;  // shared: generation of the opened heap
    void* backing;                  // the desktop ID3D12Resource
    std::uint64_t refreshes;        // copied: submissions that copied bytes into the backing
    std::uint64_t bytes_copied;     // copied: bytes copied from title memory into the backing
    std::uint64_t last_staging;     // copied: id of the staging buffer of the last refresh (0: none)
    std::uint64_t last_fence;       // copied: queue fence value after the last submission (0: none)
};

struct X2wLinearBufferTotals {
    std::uint64_t placements, shared, copied, refused;
    std::uint64_t feature_queries, heap_opens, heap_open_failures, heap_cache_hits, place_failures;
    std::uint64_t submissions, refreshes, bytes_copied, staging_created, staging_reused, unreadable;
    std::uint64_t submissions_refused;
    std::int32_t last_refusal_hr, last_submit_hr;
};
