#pragma once
#include <cstdint>
#include <vector>

#include "bytes.h"
#include "prx.h"

namespace mgspo {

// Decompresses a zlib stream (RFC 1950/1951). Returns false if the data is damaged.
bool zlibInflate(const uint8_t* data, size_t size, Bytes& out);

// The stage modules ship in a directory with a "_zar" file: a 4-byte size followed by a zlib stream
// holding a small archive of the stage's data. One member, "<stage>.rlc", is a relocation table that
// the game applies itself after loading the module, to point the module's references at the main
// executable. Returns the table's bytes, or an empty vector if there is none.
Bytes findLinkTable(const Bytes& zar);

// Applies a link table found by findLinkTable. Entries are (offset, info) pairs with the usual PRX
// relocation types; "offset segment" selects one of the module's own segments and "value segment"
// one of the main executable's segments (mainSegBases, absolute addresses).
// If targets is given, the addresses in the main executable that the table makes calls (R_26) or
// address loads (HI16/LO16 pairs) point at are appended to it.
void applyLinkTable(PrxImage& img, const Bytes& table, const std::vector<uint32_t>& mainSegBases,
                    std::vector<uint32_t>* targets = nullptr);

}  // namespace mgspo
