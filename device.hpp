#pragma once

#include <iostream>
#include <deque>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <ctime>
#include <iomanip>

#include <Windows.h>
#include <SetupAPI.h>
#include "WinUsb.h"

#pragma comment (lib, "Setupapi.lib")
#pragma comment (lib, "WinUsb.lib")

template<typename ... Args> std::string string_format(const std::string& format, Args ... args);

//#define ENDPOINT_PERFSTATS
#include <chrono>
#include <numeric>
#include <algorithm>

// TODO: Cleanup logging and messaging.

#if 0
#	define DBG(x) { cout << x << endl; }
#else
#	define DBG(x) {}
#endif

#if 0
#	define LOG(x) { cout << x << endl; }
#else
#	define LOG(x) {}
#endif

// Is this a USB thing or a Joulescope thing
#define BULK_IN_LENGTH 512u // see usb/__init__.py

/**
 * This is mixed up with DWORD GetLastError and Python NONE.
 */
enum class DeviceEvent
{
	ENDPOINT_CALLBACK_STOP            = -1,  // a callback indicated that streaming should stop
	UNDEFINED                         =  0,
	COMMUNICATION_ERROR               =  1,  // an communicate error that prevents this device from functioning, such as device removal
	ENDPOINT_CALLBACK_EXCEPTION       =  2,  // a callback threw an exception
	FORCE_CAST_FROM_GETLASTERROR_BUG,        // GetLastError is used in ControlTransferAsync._issue
	NONE                                     // for Python compatibility with None
};

// TODO: This should pass a reference, otherwise we end up doing a lot of copying
typedef bool (*EndpointIn_data_fn_t)(std::vector<UCHAR>);
typedef bool (*EndpointIn_process_fn_t)(void);
typedef void (*EndpointIn_stop_fn_t)(int, std::string);

/**
 * This is a wrapper around Windows OVERLAPPED object that contains
 * its own STL buffer container. Each overlapped transfer has an
 * event associated with it, and is used either by ReadPipe or
 * ControlTransfer. Note that before using each OVERLAPPED, the
 * event handle needs to be reset and the structure zeroed.
 */
struct TransferOverlapped
{
	TransferOverlapped(HANDLE _event, size_t _size) : m_event(_event)
	{
		m_size = _size;
		m_buffer.resize(m_size);
		reset();
	};
	LPOVERLAPPED ov_ptr(void) {
		return &m_ov;
	}
	void reset(void)
	{
		ZeroMemory(&m_ov, sizeof(m_ov));
		m_ov.hEvent = m_event;
		// TODO: Revisit the Python, it properly resizes this buffer duringcontrol transfers
		// TODO: It is unlikely we will have a bug here since there are limited ctrl transfers
		// TODO: but if we exceed 4096B it might fail. 
		// TODO: This should be sized according to the request!
		m_buffer.resize(m_size);
	};
	OVERLAPPED         m_ov;
	std::vector<UCHAR> m_buffer;
	HANDLE             m_event;
	size_t             m_size;
};

/**
 * This was the sole conversion problem between Python and C++. Using
 * pass-by-reference and creating copies of the in-flight OVERLAPPED
 * structures creates havoc and exceptions deep in the NTDLL. This can
 * be seen by looking the Python code: it creates a member object
 * called m_ptr that points to m_ov, but in C++ on pass by reference
 * and then copying to/from the pending/free containers, m_ptr
 * eventually points to unused memory, unless it is fixed on every
 * copy, and I'm sure I could overload the copy constructor to do that
 * but it was easier to just use an array of pointers. Plus, every time
 * you do a copy, you're also copying vector<UCHAR> m_data. Why
 * do that?
 */
typedef std::deque<TransferOverlapped*> TransferOverlappedDeque;

class EndpointIn
{
public:
	EndpointIn(
		HANDLE _winusb,
		UCHAR _pipe_id,
		UINT _transfers,
		UINT _block_size,
		EndpointIn_data_fn_t data_fn,
		EndpointIn_process_fn_t process_fn,
		EndpointIn_stop_fn_t stop_fn
	);
private:
	void _open(void);
	void _close(void);
	bool _issue(TransferOverlapped*);
	bool _pend(void);
	bool _expire(void);
	void _cancel(void);
	void _halt(DeviceEvent, std::string);
public:
	bool process(void);
	bool process_signal(void);
	void start(void);
	void stop(void);
	// needed by WinUsbDevice event updater
	HANDLE event(void) { return m_event; }
	// needed public in WinUsbDevice::process()
	UCHAR m_pipe_id;
	// needed public in WinUsbDevice::process()
	DeviceEvent m_stop_code;
private:
	HANDLE m_winusb;
	HANDLE m_event;
	TransferOverlappedDeque m_overlapped_free;
	TransferOverlappedDeque m_overlapped_pending;
	UINT m_transfers;
	UINT m_transfer_size;
	EndpointIn_data_fn_t m_data_fn;
	EndpointIn_process_fn_t m_process_fn;
	EndpointIn_stop_fn_t m_stop_fn;
	UINT m_process_transfers;
	enum class state_e { ST_IDLE = 0, ST_RUNNING, ST_STOPPING };
	state_e m_state;
	std::string m_stop_message;
	ULONG m_byte_count_this;
	ULONG m_byte_count_total;
	ULONG m_transfer_count;
	ULONG m_transfer_expire_max;

#ifdef ENDPOINT_PERFSTATS
	struct PerfStats
	{
		PerfStats()
		{
			data_fn_time.reserve(100'000);
			process_fn_time.reserve(100'000);
		}
		// Arrays of deltas so we can do max/min/avg on close
		std::vector<float> data_fn_time;
		std::vector<float> process_fn_time;
	} m_perf_stats;
	void print_stats_to_console(void)
	{
		double avg, max, min;
		size_t n;
		
		std::cout << std::setprecision(3);

		n = m_perf_stats.data_fn_time.size();
		min = *std::min_element(
			m_perf_stats.data_fn_time.begin(),
			m_perf_stats.data_fn_time.end());
		avg = std::accumulate(
			m_perf_stats.data_fn_time.begin(),
			m_perf_stats.data_fn_time.end(), 0.0) / n;
		max = *std::max_element(
			m_perf_stats.data_fn_time.begin(),
			m_perf_stats.data_fn_time.end());
		avg *= 1e6;
		min *= 1e6;
		max *= 1e6;
		std::cout << "   data_fn [" << min << ", " << avg << ", " << max << "] n=" << n << std::endl;

		n = m_perf_stats.process_fn_time.size();
		min = *std::min_element(
			m_perf_stats.process_fn_time.begin(),
			m_perf_stats.process_fn_time.end());
		avg = std::accumulate(
			m_perf_stats.process_fn_time.begin(),
			m_perf_stats.process_fn_time.end(), 0.0) / n;
		max = *std::max_element(
			m_perf_stats.process_fn_time.begin(),
			m_perf_stats.process_fn_time.end());
		avg *= 1e6;
		min *= 1e6;
		max *= 1e6;
		std::cout << "process_fn [" << min << ", " << avg << ", " << max << "] n=" << n << std::endl;

		std::cout << std::defaultfloat;

	}
#endif
};

// pipe_id -> EndpointIn
typedef std::map <UCHAR, EndpointIn> EndpointInMap;

typedef void (*event_callback_fn_t)(DWORD stop_code, std::string msg);

struct ControlTransferResponse
{
	ControlTransferResponse(
		WINUSB_SETUP_PACKET _setup_packet,
		DeviceEvent         _result,
		std::vector<UCHAR>  _data // TODO is this a time-wasting deep-copy?
	) : setup_packet(_setup_packet),
		result(_result),
		data(_data)
	{};
	WINUSB_SETUP_PACKET setup_packet;
	DeviceEvent         result;
	std::vector<UCHAR>  data;
};

/**
 * We provide two functions for synchronous (blocking) control transfer. In
 * order to accomplish this, we need to be able to pass a pointer to a member
 * function. This becomes a type conflict when non-member-functions are used.
 * To avoid this, the sync functions use a lambda wrapper, which requires the
 * callback function to be defined as a type `std::function`.
 *
 * N.B.: I've never used `std::function` before, so this code is suspect.
 */
//typedef void (*ControlTransferAsync_cbk_fn)(ControlTransferResponse);
// TODO: Pass a reference to avoid large (failed!) copies of m_data
typedef std::function<void(ControlTransferResponse)> ControlTransferAsync_cbk_fn;

struct ControlTransferAsync_Command
{
	ControlTransferAsync_Command(
		ControlTransferAsync_cbk_fn _cbk_fn,
		WINUSB_SETUP_PACKET         _setup_packet,
		std::vector<UCHAR>          _buffer // TODO: is this a time-wasting deep-copy?
	) : cbk_fn(_cbk_fn),
		setup_packet(_setup_packet),
		buffer(_buffer)
	{};
	ControlTransferAsync_cbk_fn cbk_fn;
	WINUSB_SETUP_PACKET         setup_packet;
	std::vector<UCHAR>          buffer;
};

class ControlTransferAsync {
public:
	ControlTransferAsync(HANDLE _winusb);
	HANDLE event(void);
	void open(void);
	void close(void);
	bool pend(ControlTransferAsync_cbk_fn, WINUSB_SETUP_PACKET, std::vector<UCHAR>);
	void process(void);
	// needed public by WinUsbDevice::_abort
	DeviceEvent m_stop_code;
private:
	void _close_event(void);
	bool _issue(void);
	void _finish(ControlTransferAsync_Command);

	HANDLE m_winusb;
	HANDLE m_event;
	TransferOverlapped *m_overlapped;
	// TODO: is buffer being passed by value, reference, or copy? man C++ confuses me... deep copy?
	std::deque<ControlTransferAsync_Command> m_commands;
	time_t m_time_start;
};

class WinUsbDevice {
public:
	WinUsbDevice(void)
	{
		m_file = NULL;
		m_winusb = NULL;
		m_interface = 0;
		m_endpoints.clear();
		m_event_list.resize(MAXIMUM_WAIT_OBJECTS);
		m_event_callback_fn = nullptr;
		m_control_transfer = nullptr;
		m_event_list_count = 0; // not in the python constructor
		_update_event_list();
	};
	void open(std::wstring _path, event_callback_fn_t* event_callback_fn = nullptr);
	void close(void);
	void _update_event_list(void);
	std::wstring path(void) { return m_path; };
	// TODO: split serial #
	std::wstring serial_number(void) { return m_path; };
	bool control_transfer_out(
		ControlTransferAsync_cbk_fn cbk_fn,
		UCHAR Recipient,
		UCHAR Type,
		UCHAR Request,
		UINT Value,
		UINT Index,
		std::vector<UCHAR> data);
	bool control_transfer_in(
		ControlTransferAsync_cbk_fn cbk_fn,
		UCHAR Recipient,
		UCHAR Type,
		UCHAR Request,
		UINT Value,
		UINT Index,
		USHORT Length);
	bool control_transfer_out_sync(
		UCHAR Recipient,
		UCHAR Type,
		UCHAR Request,
		UINT Value,
		UINT Index,
		std::vector<UCHAR> data);
	std::vector<UCHAR> control_transfer_in_sync(
		UCHAR Recipient,
		UCHAR Type,
		UCHAR Request,
		UINT Value,
		UINT Index,
		USHORT Length);
	void read_stream_start(
		UCHAR endpoint_id,
		UINT transfers,
		UINT block_size,
		EndpointIn_data_fn_t data_fn,
		EndpointIn_process_fn_t process_fn,
		EndpointIn_stop_fn_t stop_fn);
	void read_stream_stop(UCHAR endpoint_id);

	void _abort(int stop_code, std::string msg);
	void process(float timeout);
private:
	std::wstring m_path;
	HANDLE m_file;
	HANDLE m_winusb;
	UINT m_interface; //type?
	EndpointInMap m_endpoints;
	std::vector<HANDLE> m_event_list;
	event_callback_fn_t* m_event_callback_fn;
	ControlTransferAsync* m_control_transfer;
	DWORD m_event_list_count; // not in python constructor
};
