#pragma once

#include <Windows.h>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <fstream>

using namespace std;

#define MAX_OVERLAPPED_WRITES 8 // don't change; using mask in save_acc()
#define MAX_PAGE_SIZE (64 * 1024) // in floats

#define QUEUE_BYTES_EVENT 1
#define QUEUE_PAGE_EVENT 0

class FileWriter
{
public:
	FileWriter()
	{
		m_events[0] = CreateEvent(NULL, TRUE, FALSE, NULL);
		m_events[1] = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	bool m_observe_timestamps = false;
	size_t m_total_samples = 0;
	size_t m_total_nan = 0;
	void add(float i, float v, uint8_t bits);
	void open(string fn);
	void close(void);
	void wait(DWORD msec);
	unsigned int samplerate(void)
	{
		return m_sample_rate;
	}
	void samplerate(unsigned int rate, unsigned int max)
	{
		m_sample_rate = rate;
		m_samples_per_downsample = max / m_sample_rate;
	}
	float nanpct(void)
	{
		if (m_total_samples == 0)
		{
			return 0.0f;
		}
		return m_total_nan / m_total_samples * 100.0f;
	}
	vector<float> m_timestamps;
private:
	HANDLE        m_events[2];
	HANDLE        m_file_handle = NULL;
	float         m_acc = 0;
	size_t        m_total_accumulated = 0;
	size_t        m_samples_per_downsample = 2'000'000 / 1000;
	unsigned int  m_sample_rate = 1000;
	OVERLAPPED    m_ov[MAX_OVERLAPPED_WRITES];
	OVERLAPPED    m_overlapped; // For queue_bytes
	float         m_pages[MAX_OVERLAPPED_WRITES][MAX_PAGE_SIZE];
	unsigned      m_head = 0;
	unsigned      m_tail = 0;
	unsigned      m_buffer_pos = 0;
	uint64_t      m_file_offset = 0;
	bool          m_last_gpi0 = false;

	void gpi0_check(bool& last, bool current);
	void save_acc(void);
	void queue_page(unsigned page, unsigned len);
	void queue_bytes(LPCVOID bytes, unsigned length);
};
