// Copyright (c) 2011-2015 The Cryptonote developers
// Copyright (c) 2014-2015 XDN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <vector>

#include "crypto/hash.h"

namespace cryptonote
{
    typedef std::uint64_t difficulty_type;

    bool check_hash(const crypto::hash &hash, difficulty_type difficulty);
}
