/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2009 New York University
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Adrian S.-W. Tam <adrian.sw.tam@gmail.com>
 */

#include <stdint.h>
#include <iostream>
#include "ns3/buffer.h"
#include "ns3/address-utils.h"
#include "ns3/log.h"
#include "SRC-header.h"

NS_LOG_COMPONENT_DEFINE("SRCHeader");

namespace ns3
{

    NS_OBJECT_ENSURE_REGISTERED(SRCHeader);

    SRCHeader::SRCHeader()
    {
        dport = 0;
        bolt = {0};
    }

    SRCHeader::~SRCHeader()
    {
    }

    TypeId
    SRCHeader::GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::SRCHeader")
                                .SetParent<Header>()
                                .AddConstructor<SRCHeader>();
        return tid;
    }

    TypeId
    SRCHeader::GetInstanceTypeId(void) const
    {
        return GetTypeId();
    }

    void SRCHeader::Print(std::ostream &os) const
    {
        return;
    }
    uint32_t SRCHeader::GetSerializedSize(void) const
    {
        return sizeof(bolt) + sizeof(dport);
    }
    void SRCHeader::Serialize(Buffer::Iterator start) const
    {
        start.WriteU16(dport);
        start.WriteU32(bolt.q_size_and_rate);
        start.WriteU8(bolt.flags);
        start.WriteU32(bolt.tx);
        // printf("srcheader:serialize:\n");
        // printf("%d %d %d %d %d\n", dport, bolt.q_size_and_rate >> 8, bolt.q_size_and_rate & 0xff, (int)bolt.flags, bolt.tx);
    }
    uint32_t SRCHeader::Deserialize(Buffer::Iterator start)
    {
        dport = start.ReadLsbtohU16();
        bolt.q_size_and_rate = start.ReadU32();
        bolt.flags = start.ReadU8();
        bolt.tx = start.ReadU32();
        // printf("srcheader:deserialize:\n");
        // printf("%d %d %d %d %d\n", dport, bolt.q_size_and_rate >> 8, bolt.q_size_and_rate & 0xff, (int)bolt.flags, bolt.tx);
        return GetSerializedSize();
    }
}; // namespace ns3
