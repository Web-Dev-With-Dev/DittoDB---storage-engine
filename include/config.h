/**
 * @file config.h
 * @brief Global configuration constants, type definitions, and sentinel values for DittoDB.
 */

#pragma once

#include <cstddef>   
#include <cstdint>  

/**
 * @brief Fixed size of each database storage page in bytes (4 KB).
 */
constexpr std::size_t PAGE_SIZE = 4096;

/**
 * @brief Logical page identifier type.
 */
using page_id_t = std::int32_t;

/**
 * @brief Sentinel value indicating an invalid or unallocated page ID.
 */
constexpr page_id_t INVALID_PAGE_ID = -1;

/**
 * @brief Buffer pool frame index identifier type.
 */
using frame_id_t = std::size_t;

/**
 * @brief Sentinel value indicating an invalid or unassigned frame ID.
 */
constexpr frame_id_t INVALID_FRAME_ID = static_cast<frame_id_t>(-1);

/**
 * @brief Log sequence number (LSN) type for Write-Ahead Logging (WAL) and ARIES recovery.
 */
using lsn_t = std::int32_t;

/**
 * @brief Sentinel value indicating an invalid or unassigned LSN.
 */
constexpr lsn_t INVALID_LSN = -1;
