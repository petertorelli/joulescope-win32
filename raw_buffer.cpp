#include "raw_buffer.hpp"

#include <iostream>

INT32 BADPACKET[JS110_SAMPLES_PER_PACKET] = {
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/**
 * This is the primary incoming data stream from the device. Each USB
 * endpoint returns a number of 512B packets. These packets are stored
 * in the `m_raw` buffer, which is fixed size and can overflow. If the
 * indices of two adjacent packets is greater than one, it means packets
 * were dropped. Rather than skip them, copy a "bad packet" of 126 32-bit
 * -1 (0xFFFFFFFF), which is the same as "missing" samples. This retains
 * the timescale, rather than omitting them.
 */

bool RawBuffer::add_data(vector<UCHAR>& data)
{
	JoulescopePacket* pkts = (JoulescopePacket*)data.data();
	size_t num_pkts = data.size() / 512;
	//cout << "RawBuffer::add_data(), " << num_pkts << " packets\n";
	while (num_pkts--)
	{
		add_packet(pkts++);
	}
	return false;
}

void RawBuffer::add_packet(JoulescopePacket* pkt)
{
	UINT16 delta = pkt->pkt_index - m_last_pkt_index;
	if (delta > 1)
	{
		m_total_dropped_pkts += delta;
		while (delta-- > 1)
		{
			cout << "BAD PACKET\n";
			copy_raw_samples((UINT32*)BADPACKET);
		}
	}
	else
	{
		copy_raw_samples(pkt->samples);
	}
	m_last_pkt_index = pkt->pkt_index;
}

void RawBuffer::copy_raw_samples(UINT32 *samples)
{
	CopyMemory(&m_raw[m_raw_ptr], samples, JS110_SAMPLES_PER_PACKET * 4);
	m_raw_ptr += (JS110_SAMPLES_PER_PACKET * 4);
	if (m_raw_ptr >= MAX_RAW_SAMPLES)
	{
		throw runtime_error("Raw buffer overflow");
	}
}

/**
 * After the device driver has delivered the endpoint data using the
 * `data_fn()` callback, it calls `process_signal()` on each endpont.
 * We only need one, so the multiple calls are redundant. Processing
 * converts the raw samples to calibrated samples, downsamples,
 * looks for timestamps, and then queues the calibrated data to be
 * written asynchronously to a file with WriteFile and OVERLAPPED.
 */

bool RawBuffer::process_data(void)
{
	//cout << "RawBuffer::process_data\n";
	for (size_t j(0); j < m_raw_ptr; ++j)
	{
		uint32_t i = (m_raw[j] >> 16) & 0xFFFF;
		uint32_t v = m_raw[j] & 0xFFFF;
		m_raw_processor->process(i, v);
	}
	m_raw_ptr = 0;
	return false;
}
