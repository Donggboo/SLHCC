#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "qbb-net-device.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include "qbb-header.h"
#include "NT-header.h"
#include "SRC-header.h"

#include <cmath>

namespace ns3
{

	TypeId SwitchNode::GetTypeId(void)
	{
		static TypeId tid = TypeId("ns3::SwitchNode")
								.SetParent<Node>()
								.AddConstructor<SwitchNode>()
								.AddAttribute("EcnEnabled",
											  "Enable ECN marking.",
											  BooleanValue(false),
											  MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
											  MakeBooleanChecker())
								.AddAttribute("CcMode",
											  "CC mode.",
											  UintegerValue(0),
											  MakeUintegerAccessor(&SwitchNode::m_ccMode),
											  MakeUintegerChecker<uint32_t>())
								.AddAttribute("AckHighPrio",
											  "Set high priority for ACK/NACK or not",
											  UintegerValue(1),
											  MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
											  MakeUintegerChecker<uint32_t>())
								.AddAttribute("MaxRtt",
											  "Max Rtt of the network",
											  UintegerValue(9000),
											  MakeUintegerAccessor(&SwitchNode::m_maxRtt),
											  MakeUintegerChecker<uint32_t>());
		return tid;
	}

	SwitchNode::SwitchNode()
	{
		m_ecmpSeed = m_id;
		id = m_id;
		m_node_type = 1;
		m_mmu = CreateObject<SwitchMmu>();
		for (uint32_t i = 0; i < pCnt; i++)
			for (uint32_t j = 0; j < pCnt; j++)
				for (uint32_t k = 0; k < qCnt; k++)
					m_bytes[i][j][k] = 0;
		for (uint32_t i = 0; i < pCnt; i++)
			m_txBytes[i] = 0;
		for (uint32_t i = 0; i < pCnt; i++)
			m_lastPktSize[i] = m_lastPktTs[i] = 0;
		for (uint32_t i = 0; i < pCnt; i++)
			m_u[i] = 0;
		for (uint32_t i = 0; i < pCnt; i++)
		{
			last_sm_time[i] = 0;
			sm_token[i] = 0;
			pur_token[i] = 0;
		}
		m_ackHighPrio = 1;
		use_APOLLO_route_table = false;
	}

	int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch)
	{

		// look up entries
		auto entry = m_rtTable.find(ch.dip);
		// no matching entry
		if (entry == m_rtTable.end())
		{
			return -1;
		}

		// entry found
		auto &nexthops = entry->second;
		// printf("%d\n",use_fat_tree_route_table);
		if (use_fat_tree_route_table && ch.l3Prot == 0x11)
		{
			if (fat_tree_route_table.find(ch.dip >> 8 & 0xffff) != fat_tree_route_table.end())
			{
				int idx = fat_tree_route_table[ch.dip >> 8 & 0xffff];
				return idx;
			}
		}
		if (use_APOLLO_route_table && ch.l3Prot == 0x11)
		{
			
			int idx = APOLLO_route_table[(ch.sip >> 8) & 0xffff][(ch.dip >> 8) & 0xffff][ch.udp.sport];
			if (idx != 0)
			{
				if (((ch.sip >> 8) & 0xffff) == 1)
				{
					;
					// printf("%d %d %d %d %d\n", id, (ch.sip >> 8) & 0xffff, (ch.dip >> 8) & 0xffff, ch.udp.sport, idx);
				}
				if (idx != 0)
					return idx;
			}
		}
		// pick one next hop based on hash
		union
		{
			uint8_t u8[4 + 4 + 2 + 2];
			uint32_t u32[3];
		} buf;
		buf.u32[0] = ch.sip;
		buf.u32[1] = ch.dip;
		if (ch.l3Prot == 0x6)
			buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
		else if (ch.l3Prot == 0x11)
			buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
		else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)
			buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);

		uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
		// printf("%d\n",nexthops[idx]);
		return nexthops[idx];
	}

	void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex)
	{
		Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
		if (m_mmu->CheckShouldPause(inDev, qIndex))
		{
			device->SendPfc(qIndex, 0);
			m_mmu->SetPause(inDev, qIndex);
		}
	}
	void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex)
	{
		Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
		if (m_mmu->CheckShouldResume(inDev, qIndex))
		{
			device->SendPfc(qIndex, 1);
			m_mmu->SetResume(inDev, qIndex);
		}
	}

	void SwitchNode::SendToDev(Ptr<Packet> p, CustomHeader &ch)
	{
		int idx = GetOutDev(p, ch);
		if (idx >= 0)
		{
			NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

			// determine the qIndex
			uint32_t qIndex;
			if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || ch.l3Prot == 0xFD || ch.l3Prot == 0xFB || ch.l3Prot == 0xFA)
			{ // QCN or PFC or NACK or NT or SRC, go highest priority
				qIndex = 0;
			}
			else
			{
				qIndex = (ch.l3Prot == 0x06 ? 1 : ch.udp.pg); // if TCP, put to queue 1
			}
			//  admission control
			FlowIdTag t;
			p->PeekPacketTag(t);
			uint32_t inDev = t.GetFlowId();
			if (qIndex != 0)
			{ // not highest priority
				if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize()) && m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize()))
				{ // Admission control
					m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize());
					m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize());
				}
				else
				{
					return; // Drop
				}
				CheckAndSendPfc(inDev, qIndex);
			}

			if (m_ccMode == 11 && ch.l3Prot == 0x11 && ch.udp.ih.rate != 0)
			{
				sendNT(p, idx);
			}
			if (m_ccMode == 13 && ch.l3Prot == 0x11)
			{
				Bolt(p, idx, inDev, qIndex);
			}
			m_bytes[inDev][idx][qIndex] += p->GetSize();
			m_devices[idx]->SwitchSend(qIndex, p, ch);
		}
		else
		{
			return; // Drop
		}
	}

	uint32_t SwitchNode::EcmpHash(const uint8_t *key, size_t len, uint32_t seed)
	{
		uint32_t h = seed;
		if (len > 3)
		{
			const uint32_t *key_x4 = (const uint32_t *)key;
			size_t i = len >> 2;
			do
			{
				uint32_t k = *key_x4++;
				k *= 0xcc9e2d51;
				k = (k << 15) | (k >> 17);
				k *= 0x1b873593;
				h ^= k;
				h = (h << 13) | (h >> 19);
				h += (h << 2) + 0xe6546b64;
			} while (--i);
			key = (const uint8_t *)key_x4;
		}
		if (len & 3)
		{
			size_t i = len & 3;
			uint32_t k = 0;
			key = &key[i - 1];
			do
			{
				k <<= 8;
				k |= *key--;
			} while (--i);
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
		}
		h ^= len;
		h ^= h >> 16;
		h *= 0x85ebca6b;
		h ^= h >> 13;
		h *= 0xc2b2ae35;
		h ^= h >> 16;
		return h;
	}

	void SwitchNode::SetEcmpSeed(uint32_t seed)
	{
		m_ecmpSeed = seed;
	}

	void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx)
	{
		uint32_t dip = dstAddr.Get();
		m_rtTable[dip].push_back(intf_idx);
	}

	void SwitchNode::ClearTable()
	{
		m_rtTable.clear();
	}

	// This function can only be called in switch mode
	bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch)
	{
		// printf("%d %d\n",packet->GetSize(),Simulator::Now().GetTimeStep());
		SendToDev(packet, ch);
		// printf("%d\n",id);
		return true;
	}

	void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p)
	{
		FlowIdTag t;
		p->PeekPacketTag(t);
		if (qIndex != 0)
		{
			uint32_t inDev = t.GetFlowId();
			m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
			m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize());
			m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
			if (m_ecnEnabled)
			{
				bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
				if (egressCongested)
				{
					PppHeader ppp;
					Ipv4Header h;
					p->RemoveHeader(ppp);
					p->RemoveHeader(h);
					h.SetEcn((Ipv4Header::EcnType)0x03);
					p->AddHeader(h);
					p->AddHeader(ppp);
				}
			}
			// CheckAndSendPfc(inDev, qIndex);
			CheckAndSendResume(inDev, qIndex);
		}
		if (1)
		{
			uint8_t *buf = p->GetBuffer();
			if (buf[PppHeader::GetStaticSize() + 9] == 0x11)
			{
				IntHeader *ih = (IntHeader *)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
				Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
				// if (dev->GetQueue()->GetNBytesTotal() > 2000)
				/// printf("%ld %ld %d %d\n", Simulator::Now().GetTimeStep(), id, dev->GetQueue()->GetNBytesTotal(), GetId());
				if (m_ccMode == 3)
				{ // HPCC
					// printf("nhop:%d\n",ih->nhop);
					ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
				}
				else if (m_ccMode == 10)
				{ // HPCC-PINT
					uint64_t t = Simulator::Now().GetTimeStep();
					uint64_t dt = t - m_lastPktTs[ifIndex];
					if (dt > m_maxRtt)
						dt = m_maxRtt;
					uint64_t B = dev->GetDataRate().GetBitRate() / 8; // Bps
					uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
					double newU;

					/**************************
					 * approximate calc
					 *************************/
					int b = 20, m = 16, l = 20; // see log2apprx's paremeters
					int sft = logres_shift(b, l);
					double fct = 1 << sft;				 // (multiplication factor corresponding to sft)
					double log_T = log2(m_maxRtt) * fct; // log2(T)*fct
					double log_B = log2(B) * fct;		 // log2(B)*fct
					double log_1e9 = log2(1e9) * fct;	 // log2(1e9)*fct
					double qterm = 0;
					double byteTerm = 0;
					double uTerm = 0;
					if ((qlen >> 8) > 0)
					{
						int log_dt = log2apprx(dt, b, m, l);		  // ~log2(dt)*fct
						int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
						qterm = pow(2, (
										   log_dt + log_qlen + log_1e9 - log_B - 2 * log_T) /
										   fct) *
								256;
						// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
					}
					if (m_lastPktSize[ifIndex] > 0)
					{
						int byte = m_lastPktSize[ifIndex];
						int log_byte = log2apprx(byte, b, m, l);
						byteTerm = pow(2, (
											  log_byte + log_1e9 - log_B - log_T) /
											  fct);
						// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
					}
					if (m_maxRtt > dt && m_u[ifIndex] > 0)
					{
						int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l);				 // ~log2(T-dt)*fct
						int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
						uTerm = pow(2, (
										   log_T_dt + log_u - log_T) /
										   fct) /
								8192;
						// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
					}
					newU = qterm + byteTerm + uTerm;

#if 1
					/**************************
					 * accurate calc
					 *************************/
					double weight_ewma = double(dt) / m_maxRtt;
					double u;
					if (m_lastPktSize[ifIndex] == 0)
						u = 0;
					else
					{
						double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
						u = (qlen / m_maxRtt + txRate) * 1e9 / B;
					}
// newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
// printf(" %lf\n", newU);
#endif

					/************************
					 * update PINT header
					 ***********************/
					uint16_t power = Pint::encode_u(newU);
					if (power > ih->GetPower())
						ih->SetPower(power);

					m_u[ifIndex] = newU;
				}
			}
		}
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		ch.getInt = 1; // parse INT header
		p->PeekHeader(ch);
		if (ch.l3Prot == 0x11 && use_APOLLO_route_table)
		{
			m_txBytes[ifIndex] += p->GetSize() - 48;
			// printf("%d\n", p->GetSize());
		}
		else
		{
			m_txBytes[ifIndex] += p->GetSize();
		}
		// p->g
		m_lastPktSize[ifIndex] = p->GetSize();
		m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
	}

	int SwitchNode::logres_shift(int b, int l)
	{
		static int data[] = {0, 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
		return l - data[b];
	}

	int SwitchNode::log2apprx(int x, int b, int m, int l)
	{
		int x0 = x;
		int msb = int(log2(x)) + 1;
		if (msb > m)
		{
			x = (x >> (msb - m) << (msb - m));
#if 0
		x += + (1 << (msb - m - 1));
#else
			int mask = (1 << (msb - m)) - 1;
			if ((x0 & mask) > (rand() & mask))
				x += 1 << (msb - m);
#endif
		}
		return int(log2(x) * (1 << logres_shift(b, l)));
	}

	void SwitchNode::sendNT(Ptr<Packet> p, uint32_t ifIndex)
	{
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		ch.getInt = 1; // parse INT header
		p->PeekHeader(ch);
		Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
		Ptr<Packet> newp = Create<Packet>(0);
		NTHeader nt;
		nt.rate = ch.udp.ih.rate;
		uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
		uint8_t *buf = p->GetBuffer();
		IntHeader *ih = (IntHeader *)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6];
		if (ih->u == 253 && qlen == 0)
		{
			return;
		}
		if (qlen > 0)
		{
			ih->u = 253;
		}
		if (qlen == 0)
		{
			uint64_t t = Simulator::Now().GetTimeStep();
			uint64_t dt = t - m_lastPktTs[ifIndex];
			double txRate = m_lastPktSize[ifIndex] / double(dt);
			uint64_t B = dev->GetDataRate().GetBitRate() / 8; // Bps
			double u = txRate * 1e9 / B;
			u = std::min(.99, u);
			u_int8_t mu = (u_int8_t)(u * (1 << 8));
			if (mu <= ih->u)
			{
				return;
			}
			ih->u = mu;
		}
		nt.dport = ch.udp.sport;
		nt.ih.Set(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], qlen, dev->GetDataRate().GetBitRate());
		newp->AddHeader(nt);
		Ipv4Header ipv4h; // Prepare IPv4 header
		ipv4h.SetProtocol(0xFB);
		uint32_t sip = 0x0b000001 + ((m_id / 256) * 0x00010000) + ((m_id % 256) * 0x00000100);
		ipv4h.SetSource(Ipv4Address(sip));
		ipv4h.SetDestination(Ipv4Address(ch.sip));
		Ipv4Address dip = Ipv4Address(ch.sip);
		ipv4h.SetPayloadSize(0);
		ipv4h.SetTtl(64);
		newp->AddHeader(ipv4h);
		PppHeader ppp;
		ppp.SetProtocol(0x0021);
		newp->AddHeader(ppp);
		CustomHeader chh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		newp->PeekHeader(chh);
		newp->AddPacketTag(FlowIdTag(ifIndex));
		SendToDev(newp, chh);
	}
	void SwitchNode::CalculateSupplyToken(Ptr<Packet> p, uint32_t ifIndex)
	{
		Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
		uint64_t dt = Simulator::Now().GetTimeStep() - last_sm_time[ifIndex];
		last_sm_time[ifIndex] = Simulator::Now().GetTimeStep();
		uint64_t supply = ((dev->GetDataRate().GetBitRate() / 8) * dt) / 1000000000;
		uint64_t d = p->GetSize();
		sm_token[ifIndex] = sm_token[ifIndex] + supply - d;
		sm_token[ifIndex] = std::min(sm_token[ifIndex], (uint64_t)(1000));
	}
	void SwitchNode::Bolt(Ptr<Packet> p, uint32_t ifIndex, uint32_t indev, uint32_t qIndex)
	{
		// printf("bolt\n");
		CalculateSupplyToken(p, ifIndex);
		Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		p->PeekHeader(ch);
		uint8_t *buf = p->GetBuffer();
		IntHeader *ih = (IntHeader *)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6];
		// printf("%d %d %d %d\n", m_mmu->ingress_bytes[indev][qIndex], ch.udp.ih.bolt.flags & (1 << 5), ch.udp.ih.bolt.flags & (1 << 6), __LINE__);
		if (m_mmu->ingress_bytes[indev][qIndex] >= 2000)
		{
			if ((((int)ch.udp.ih.bolt.flags) & (1 << 6)) == 0)
			{
				SRCHeader srcheader;
				srcheader.dport = ch.udp.sport;
				srcheader.bolt.tx = ch.udp.ih.bolt.tx;
				srcheader.bolt.q_size_and_rate = m_mmu->ingress_bytes[indev][qIndex] << 8;
				srcheader.bolt.q_size_and_rate |= (dev->GetDataRate().GetBitRate() / 5000000000);
				Ptr<Packet> srcpkt = Create<Packet>(0);
				srcpkt->AddHeader(srcheader);
				Ipv4Header ipv4h; // Prepare IPv4 header
				ipv4h.SetProtocol(0xFA);
				uint32_t sip = 0x0b000001 + ((m_id / 256) * 0x00010000) + ((m_id % 256) * 0x00000100);
				ipv4h.SetSource(Ipv4Address(sip));
				ipv4h.SetDestination(Ipv4Address(ch.sip));
				ipv4h.SetPayloadSize(0);
				ipv4h.SetTtl(64);
				srcpkt->AddHeader(ipv4h);
				PppHeader ppp;
				ppp.SetProtocol(0x0021);
				srcpkt->AddHeader(ppp);
				CustomHeader chh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
				srcpkt->PeekHeader(chh);
				srcpkt->AddPacketTag(FlowIdTag(ifIndex));
				// printf("%d %d %d %d\n", chh.l3Prot, chh.bolt.tx, chh.bolt.q_size_and_rate >> 8, __LINE__);
				SendToDev(srcpkt, chh);
			}
			ih->bolt.flags &= ~(1u << 5);
			ih->bolt.flags |= (1u << 6);
		}
		else if ((ch.udp.ih.bolt.flags & (1u << 3)) != 0)
		{
			if ((ch.udp.ih.bolt.flags & (1u << 4)) == 0)
			{
				pur_token[ifIndex]++;
			}
		}
		else if ((ch.udp.ih.bolt.flags & (1u << 5)) != 0)
		{
			if (pur_token[ifIndex] > 0)
			{
				pur_token[ifIndex] = pur_token[ifIndex] - 1;
			}
			else if (sm_token[ifIndex] > 1000)
			{
				sm_token[ifIndex] -= 1000;
			}
			else
			{
				ih->bolt.flags &= ~(1u << 5);
			}
		}
		// printf("%d %d %d %d\n", m_mmu->ingress_bytes[indev][qIndex], ch.udp.ih.bolt.flags & (1 << 5), ch.udp.ih.bolt.flags & (1 << 6), __LINE__);
		//  printf("bolt:end\n");
	}

} /* namespace ns3 */
