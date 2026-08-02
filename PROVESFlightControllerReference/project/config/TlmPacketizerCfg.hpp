/*
 * TlmPacketizerComponentImplCfg.hpp
 *
 *  Created on: Dec 10, 2017
 *      Author: tim
 */

// \copyright
// Copyright 2009-2015, by the California Institute of Technology.
// ALL RIGHTS RESERVED.  United States Government Sponsorship
// acknowledged.

#ifndef SVC_TLMPACKETIZER_TLMPACKETIZERCOMPONENTIMPLCFG_HPP_
#define SVC_TLMPACKETIZER_TLMPACKETIZERCOMPONENTIMPLCFG_HPP_

#include <Fw/FPrimeBasicTypes.hpp>

namespace Svc {
static const FwChanIdType MAX_PACKETIZER_PACKETS = 32;
static const FwChanIdType TLMPACKETIZER_NUM_TLM_HASH_SLOTS =
    15;  // !< Number of slots in the hash table.
         // Works best when set to about twice the number of components producing telemetry
static const FwChanIdType TLMPACKETIZER_HASH_MOD_VALUE =
    999;  // !< The modulo value of the hashing function.
          // Should be set to a little below the ID gaps to spread the entries around

static const FwChanIdType TLMPACKETIZER_HASH_BUCKETS =
    260;  // !< Buckets assignable to a hash slot.
          // Buckets must be >= number of telemetry channels in system

static const FwChanIdType MAX_PACKETIZER_CHANNELS =
    235;  // !< Must be >= number of non-omitted telemetry channels in system

static const FwChanIdType TLMPACKETIZER_MAX_MISSING_TLM_CHECK =
    25;  // !< Maximum number of missing telemetry channel checks
}  // namespace Svc

#endif /* SVC_TLMPACKETIZER_TLMPACKETIZERCOMPONENTIMPLCFG_HPP_ */
