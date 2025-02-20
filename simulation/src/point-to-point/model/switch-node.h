#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"

namespace ns3
{

	class Packet;

	class SwitchNode : public Node
	{
	public:
		static const uint32_t pCnt = 257; // Number of ports used
		static const uint32_t qCnt = 8;	  // Number of queues/priorities used
		uint32_t m_ecmpSeed;
		std::unordered_map<uint32_t, std::vector<int>> m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

		// monitor of PFC
		uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx

		uint64_t m_txBytes[pCnt]; // counter of tx bytes

		uint32_t m_lastPktSize[pCnt];
		uint64_t m_lastPktTs[pCnt]; // ns
		double m_u[pCnt];
		bool m_ecnEnabled;
		uint32_t m_ccMode;
		uint64_t m_maxRtt;
		uint32_t m_ackHighPrio; // set high priority for ACK/NACK
		int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
		void SendToDev(Ptr<Packet> p, CustomHeader &ch);
		static uint32_t EcmpHash(const uint8_t *key, size_t len, uint32_t seed);
		void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
		void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
		uint64_t id;
		Ptr<SwitchMmu> m_mmu;
		std::unordered_map<int, int> fat_tree_route_table;
		std::unordered_map<int, int> port_table;
		std::unordered_map<int, std::unordered_map<int, std::unordered_map<int, int>>> APOLLO_route_table;
		std::unordered_map<int, double> nbr_update_time;
		std::unordered_map<int, uint64_t> nbr_flow_time;
		uint64_t last_sm_time[pCnt];
		uint64_t sm_token[pCnt];
		uint64_t pur_token[pCnt];
		bool use_fat_tree_route_table = false;
		bool use_APOLLO_route_table;
		int sw_t = 0;
		static TypeId GetTypeId(void);
		SwitchNode();
		void SetEcmpSeed(uint32_t seed);
		void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
		void ClearTable();
		bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
		void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

		// for approximate calc in PINT
		int logres_shift(int b, int l);
		int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
		// LHCC
		void sendNT(Ptr<Packet> p, uint32_t ifIndex); // LHCC

		// bolt
		void CalculateSupplyToken(Ptr<Packet> p, uint32_t ifIndex);
		void Bolt(Ptr<Packet> p, uint32_t ifIndex, uint32_t indev, uint32_t qIndex);

		void addd_route(int sip, int dip, int sport, int idx);
	};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
