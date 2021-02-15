#include "file_writer.hpp"

void
FileWriter::add(float i, float v, uint8_t bits)
{
	float e = (double)i * (double)v / 2.0f; // wait, isn't this 1.0?
	m_acc += e;
	++m_total_accumulated;
	if (m_total_accumulated == m_samples_per_downsample)
	{
		m_total_accumulated = 0;
		m_acc = 0;
		save_acc();
	}
	//gpi0_check(m_last_gpi0, ((bits >> 4) & 1) == 1);
}

void
FileWriter::open(string fn)
{
	cout << "Opening file " << fn << endl;
	m_file_handle = CreateFileA(
		fn.c_str(),
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_FLAG_OVERLAPPED,
		NULL);
	if (m_file_handle == INVALID_HANDLE_VALUE)
	{
		throw runtime_error("Unable to create FileWriter file handle");
	}
	for (size_t i = 0; i < MAX_OVERLAPPED_WRITES; ++i)
	{
		m_events[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	m_file_offset = 0;
}

void
FileWriter::close(void)
{
	// Write partial buffer and set signal to exit on completion w/timeout.
	CloseHandle(m_file_handle);
}

void
FileWriter::save_acc(void)
{
	unsigned saved_head;
	unsigned saved_len;
	m_pages[m_head][m_idx] = m_acc;
	++m_idx;
	if (m_idx == MAX_PAGE_SIZE)
	{
		// Using 'saved' so we can update m_head ASAP before queue_page
		saved_head = m_head;
		m_head = (m_head + 1) & 0x7;
		saved_len = m_idx;
		m_idx = 0;
		if (m_head == m_tail)
		{
			throw runtime_error("Ring-buffer exhausted");
		}
		queue_page(saved_head, saved_len);
	}
}

void
FileWriter::queue_page(unsigned page, unsigned len)
{
	ZeroMemory(&m_ov[page], sizeof(OVERLAPPED));
	m_ov[page].hEvent = m_events[page];
	m_ov[page].OffsetHigh = (m_file_offset >> 32) & 0xFFFF'FFFF;
	m_ov[page].Offset = m_file_offset & 0xFFFF'FFFF;
	if (!WriteFile(
		m_file_handle,
		(LPCVOID)m_pages[page],
		len * sizeof(float),
		NULL,
		(LPOVERLAPPED)&m_ov[page]))
	{
		DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING)
		{
			cout << "GetLastError() = " << err << endl;
			throw runtime_error("Failed to write overlapped");
		}
	}
	m_file_offset += len * sizeof(float);
}

void
FileWriter::wait(DWORD msec)
{
	DWORD ret = WaitForMultipleObjects(MAX_OVERLAPPED_WRITES, m_events, FALSE, msec);
	if (ret < MAXIMUM_WAIT_OBJECTS)
	{
		unsigned page =  ret - WAIT_OBJECT_0;
		if (page < MAX_OVERLAPPED_WRITES) {
			ResetEvent(m_events[page]);
		}
		cout << "Page write completed " << ret << " page=" << page << endl;
		m_tail = (m_tail + 1) & 0x7;
	}
}

/**
* the writer loops spins on waiting for the data ready event
* the device driver emits data ready event every time the process_fn
* callback moves to the next file queue circular buffer.
* the device driver grabs the mutex, advances the pointer
*
* maybe we don't need a mutex. the device advancs the g_head and the
* writer advances g_tail.
*/

/*

device::process()
loop
	subloop1 - call process() on all endpoints
	subloop2 - call process_signal() on all endpoints
	last task - if control response, process that


process() is called with a # of packets, the goal being to 
grab all the data from the endpoints and THEN signal process.
the work item here is a varaible # of 512 byte packets.

process_signal() - expectation here is that processing may
now begin on the data downloadd from this WaitOnMulitple...

FileWriterThread contains the data_fn() called by process()
and the process_fn() called by process_signal().

data_fn() fills the raw buffer. process_fn() fills the
cooked .. er, calibrated buffer. When a calibrated buffer fills
it is flushed to disk with an overlapped WriteFile. There are
MAX_OVERLAPPED_WRITES buffers with an m_head and m_tail pointer.

process_fn() moves m_head, flush to disk moves m_tail. This
implies the device driver thread moves m_head and the writer
thread moves m_tail, avoiding race conditions.
*/
/*

there are 8 buffers for calibrated data writes
there are 8 overlapped responses from async writes
they should move together

*/
#if 0

	
	
	*/



		/*
	
	double m_acc;
	size_t m_total_samples;
	size_t m_total_accumulated;
	size_t m_total_downsamples;
	bool m_last_gpi0;
	bool m_observe_timestamps;
	size_t m_sample_rate;
	vector<float> m_timestamps;
	void raw_processor_callback(void *puser, float I, float V, uint8_t bits)
	{
		double E = ((double)I * (double)V) / 2.0;
		m_acc += E;
		++m_total_accumulated;
		if (m_total_accumulated == m_total_downsamples)
		{
			add(m_acc);
			++m_total_samples;
			m_total_accumulated = 0;
			m_acc = 0;
		}
		gpi0_check(m_last_gpi0, ((bits >> 4) & 1) == 1);
	}
	void flush_processed_samples_to_disk(void)
	{
		// sizeof_t(float) had better be 4!
		//g_trace_file.write(reinterpret_cast<const char*>(g_cal_e), g_num_buffered_cal * 4);
		//cout << "Wrote " << g_num_buffered_cal << " processed samples to disk." << endl;
		//g_num_buffered_cal = 0;
	}

	inline void gpi0_check(bool& last, bool current)
	{
		float timestamp;
		// packed bits : 7 : 6 = 0, 5 = voltage_lsb, 4 = current_lsb, 3 : 0 = i_range
		if (last && !current)
		{
			if (m_observe_timestamps == true) {
				timestamp = (float)m_total_samples / (float)m_sample_rate;
				m_timestamps.push_back(timestamp);
				cout << "m-lap-us-" << (unsigned int)(timestamp * 1e6) << endl;
			}
		}
		last = current;
	}
	void write_timestamps(path g_pfx)
	{
		/*
		// Always write this file, even if no timestamps
		path fn = g_pfx;
		fn += g_sfx_timestamps;
		path fp = g_tmpdir / fn;
		fstream file;
		file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
		file.open(fp, ios::out);
		file << "[" << endl;
		for (size_t i(0); i < g_stats.m_timestamps.size(); ++i)
		{
			file << "\t" << g_stats.m_timestamps[i];
			if (i < (g_stats.m_timestamps.size() - 1))
			{
				file << ",";
			}
			file << endl;
		}
		file << "]" << endl;
		file.close();
		// Required by the framework
		cout
			<< "m-regfile-fn["
			<< g_pfx.string()
			<< g_sfx_timestamps.string()
			<< "]-type[etime]-name[js110]"
			<< endl;
			*/
	}
	FileWriterThread(HANDLE file_handle)
	{
		m_file_handle = file_handle;
		m_head = 0;
		m_tail = 0;
		m_ptr = 0;
		m_stop_event = CreateEventA(NULL, FALSE, FALSE, "EVENT_STOP_WRITING");
		m_events[MAX_OVERLAPPED_WRITES + EVENT_STOP_WRITING] = m_stop_event;
		m_raw_processor.callback_set(raw_processor_callback, nullptr);
	}
#endif

