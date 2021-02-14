#pragma once

#include <Windows.h>
#include "joulescope_packet.hpp"
#include <vector>
#include "raw_processor.hpp"

using namespace std;

#define MAX_RAW_SAMPLES (1024 * 1024)

class RawBuffer
{
public:
	bool add_data(vector<UCHAR>& data);
	bool process_data(void);
	void set_raw_processor(RawProcessor *ptr)
	{
		m_raw_processor = ptr;
	}
private:
	UINT16 m_last_pkt_index = 0;
	size_t m_total_dropped_pkts = 0;
	UINT32 m_raw[MAX_RAW_SAMPLES];
	size_t m_raw_ptr = 0;
	RawProcessor *m_raw_processor = nullptr;
	void add_packet(struct JoulescopePacket *pkt);
	void copy_raw_samples(UINT32 *samples);
};
