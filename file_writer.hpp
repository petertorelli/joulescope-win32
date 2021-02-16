#pragma once

#include <Windows.h>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <fstream>

using namespace std;

#define MAX_OVERLAPPED_WRITES 8 // don't change; using mask in save_acc()
#define MAX_PAGE_SIZE (64 * 1024) // in floats

class FileWriter
{
public:
	void add(float i, float v, uint8_t bits);
	void open(string fn);
	void close(void);
	void wait(DWORD msec);
	bool m_observe_timestamps = false;
private:
	HANDLE        m_event;
	HANDLE        m_file_handle;
	float         m_acc = 0;
	size_t        m_total_accumulated = 0;
	size_t        m_total_samples = 0;
	size_t        m_samples_per_downsample = 1000;
	float         m_sample_rate = 2e6f / 1000.0f;
	OVERLAPPED    m_ov[MAX_OVERLAPPED_WRITES];
	OVERLAPPED    m_overlapped; // For queue_bytes
	float         m_pages[MAX_OVERLAPPED_WRITES][MAX_PAGE_SIZE];
	unsigned      m_head;
	unsigned      m_tail;
	unsigned      m_buffer_pos = 0;
	uint64_t      m_file_offset = 0;
	bool          m_last_gpi0 = false;
	vector<float> m_timestamps;

	void gpi0_check(bool& last, bool current);
	void save_acc(void);
	void queue_page(unsigned page, unsigned len);
	void queue_bytes(LPCVOID bytes, unsigned length);
	void write_timestamps(string fn);
};
