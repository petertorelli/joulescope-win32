#include "raw_buffer.hpp"

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
	while (num_pkts--)
	{
		add_pkt(pkts++);
	}
	return false;
}

void RawBuffer::add_pkt(JoulescopePacket* pkt)
{
	++m_total_pkts;
	UINT16 delta = pkt->pkt_index - m_last_pkt_index;
	if (delta > 1)
	{
		m_total_dropped_pkts += delta;
		while (delta-- > 1)
		{
			copy_raw_samples((UINT32 *)BADPACKET);
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
	CopyMemory(&(m_raw[m_raw_pos]), samples,
		JS110_SAMPLES_PER_PACKET * sizeof(UINT32));
	m_raw_pos += JS110_SAMPLES_PER_PACKET;
	if (m_raw_pos >= MAX_RAW_SAMPLES)
	{
		// This means we couldn't call the RawProcesor fast enough.
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
	for (size_t j(0); j < m_raw_pos; ++j)
	{
		uint16_t v = (m_raw[j] >> 16) & 0xFFFF;
		uint16_t i = m_raw[j] & 0xFFFF;
		m_raw_processor->process(i, v);
	}
	m_raw_pos = 0;
	return false;
}
