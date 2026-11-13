#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "common/assert.h"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/page_table.h"
#include "common/scope_exit.h"

namespace Common {

bool PageTable::DirectTables() noexcept {
    int mode = direct_tables.load(std::memory_order_acquire);
    if (mode < 0) {
        static std::once_flag once;
        std::call_once(once, [] {
            const char* option = std::getenv("EDEN_JIT_TABLE");
            if (option != nullptr && option[0] == '0') direct_tables_requested.store(false);
            const bool direct = direct_tables_requested.load() && SparseTablesAvailable();
            direct_tables.store(direct ? 1 : 0, std::memory_order_release);
            std::printf("EDEN_JIT_TABLE direct=%d\n", direct ? 1 : 0);
        });
        mode = direct_tables.load(std::memory_order_acquire);
    }
    return mode == 1;
}

PageTable::PageTable() = default;

PageTable::~PageTable() noexcept = default;

void PageTable::Resize(std::size_t address_space_width_in_bits, std::size_t page_bits) {
    auto const num_page_table_entries = 1ULL << (address_space_width_in_bits - page_bits);
    if (DirectTables()) {
        ASSERT(num_page_table_entries * sizeof(PageEntryData) <= DIRECT_TABLE_OFFSET);
        constexpr std::size_t mirror = DIRECT_TABLE_OFFSET / sizeof(PageEntryData);
        entries.ResizeAndClear(mirror + num_page_table_entries);
        entries.mirror_index = mirror;
    } else {
        entries.ResizeAndClear(num_page_table_entries);
        entries.mirror_index = 0;
    }
    current_address_space_width_in_bits = address_space_width_in_bits;
    current_page_bits = page_bits;
}

} // namespace Common
