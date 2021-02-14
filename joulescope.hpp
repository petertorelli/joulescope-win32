/**
 * Copyright 2021 Peter Torelli
 * Copyright 2020 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "device.hpp"
#include "raw_processor.hpp"
#include "raw_buffer.hpp"
#include "json/json.h"
#include "joulescope_packet.hpp"
#include <boost\algorithm\string.hpp>

#define PACKET_VERSION        1u
#define MAX_SAMPLE_RATE       2'000'000u // 2 4-byte floats per second = 2MS/sec
#define STREAMING_ENDPOINT_ID 2u

// See joulescope/driver.py
enum class JoulescopeRequest
{
	LOOPBACK_WVALUE = 1, // USB testing 
	LOOPBACK_BUFFER = 2, // USB testing 
	SETTINGS = 3, // Configure operation, incl. start streaming
	STATUS = 4, // Get current status (GET only)
	SENSOR_BOOTLOADER = 5, // Sensor bootloader operations
	CONTROLLER_BOOTLOADER = 6, // Request reboot into the controller bootloader
	SERIAL_NUMBER = 7, // Request the 16-bit unique serial number
	CALIBRATION = 8, // Request calibration. wIndex 0=factory, 1=active
	EXTIO = 9, // Get/set the external GPI/O settings
	INFO = 10, // Get device information metadata JSON string
	TEST_MODE = 11  // Enter a test mode
};


/**
 * See `datafile.py` for more details:
 * magic = b'\xd3tagfmt \r\n \n  \x1a\x1c'
 */
struct CalibrationHeader
{
	uint8_t  magic[16];
	uint64_t length;
	uint8_t  reserverd[3];
	uint8_t  file_version;
	uint32_t crc32;
};


// Obviously this is an incomplete list of all settings
// TODO: Fill this out to match the Python driver
struct JoulescopeState
{
	enum class TriggerSource { AUTO = 0, GPI0 = 2, GPI1 = 3 };
	enum class CurrentLsb { NORMAL = 0, GPI0 = 2, GPI1 = 3 };
	enum class VoltageLsb { NORMAL = 0, GPI0 = 2, GPI1 = 3 };
	// IRange controls the MOSFET that connects +IN to +OUT
	enum class IRange { AUTO = 0x80, OFF = 0 };
	// SensorPower controls the internal sensor-side power
	enum class SensorPower { OFF = 0, ON = 1 };
	enum class Streaming { NORMAL = 3, OFF = 0 };
	enum class Options { DEFAULT = 0};
	enum class Source { RAW = 0xc0 };
	JoulescopeState()
	{
		extio.trigger_source = TriggerSource::AUTO;
		extio.current_lsb = CurrentLsb::NORMAL;
		extio.voltage_lsb = VoltageLsb::NORMAL;
		extio.gpi0 = 0;
		extio.gpi1 = 0;
		settings.i_range = IRange::OFF;
		settings.sensor_power = SensorPower::ON;
		settings.streaming = Streaming::OFF;
		settings.options = Options::DEFAULT;
		settings.source = Source::RAW;
	}
	struct
	{
		TriggerSource trigger_source;
		CurrentLsb current_lsb;
		VoltageLsb voltage_lsb;
		uint8_t gpi0;
		uint8_t gpi1;
	} extio;
	struct
	{
		IRange      i_range;
		SensorPower sensor_power;
		Streaming   streaming;
		Options     options;
		Source      source;
	} settings;
};

class Joulescope
{
public:
	// Initialization
	void open(std::wstring path);
	void close(void);
	bool is_open(void);
	bool is_powered(void);
	bool is_tracing(void);
	// Control
	void power_on(bool on);
	void streaming_on(bool on, RawBuffer *raw_buffer = nullptr);
	// Device discovery
	std::wstring find_joulescope_by_serial_number(std::string serial_number = "");
	std::vector<std::wstring> scan(void);
	// 2-second stat update voltage, in mV
	unsigned int get_voltage(void);
private:
	js_stream_buffer_calibration_s calibration_read_raw(void);
	std::vector<std::wstring> guid_to_paths(GUID* pGuid);
	/**
	 * IMPORTANT NOTE: These functions are not thread safe.
	 *
	 * Calling an update_* function with streaming on (e.g., an endpoint is
	 * instantiated and pending) will cause a race condition if there is another
	 * thread calling the WinUsbDevice::process() function. That's because the
	 * update_* functions are synchronous and call process() as well, which causes
	 * a race and invalid memory conditions. Stopping the read stream removes
	 * the endpoint and all of its pending transactions by calling the WinUsb
	 * functions to abort the pipe and drain the pending requests. It is a
	 * synchronous cleanup which needs to happen before calling update_*.
	 */
	void update_extio(void);
	void update_settings(void);
public:
	WinUsbDevice m_device;
	js_stream_buffer_calibration_s m_calibration;
private:
	JoulescopeState m_state;
	std::wstring m_path;
	bool m_open;
};
