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

#include "joulescope.hpp"

using namespace std;

GUID g_guids[] = {
	// alpha
	{ 0x99a06894, 0x3518, 0x41a5, { 0xa2, 0x07, 0x85, 0x19, 0x74, 0x6d, 0xa8, 0x9f } },
	// beta
	{ 0x576d606f, 0xf3de, 0x4e4e, { 0x8a, 0x87, 0x06, 0x5b, 0x9f, 0xd2, 0x1e, 0xb0 } }
};
size_t g_num_guids = sizeof(g_guids) / sizeof(GUID);

void
Joulescope::open(wstring path)
{
	m_path = path;
	if (m_path.empty())
	{
		throw runtime_error("Could not find a joulescope");
	}
	else
	{
		m_device.open(m_path);
		update_extio();
		update_settings();
		m_calibration = calibration_read_raw();
		m_open = true;
	}
}

void
Joulescope::close(void)
{
	m_device.close();
	m_open = false;
}

bool
Joulescope::is_open(void)
{
	return m_open;
}

bool
Joulescope::is_powered(void)
{
	return (m_state.settings.i_range == JoulescopeState::IRange::AUTO);
}

bool
Joulescope::is_tracing(void)
{
	return (m_state.settings.streaming != JoulescopeState::Streaming::OFF);
}

void
Joulescope::power_on(bool on)
{
	if (on)
	{
		m_state.extio.current_lsb = JoulescopeState::CurrentLsb::GPI0;
		m_state.settings.i_range = JoulescopeState::IRange::AUTO;
	}
	else
	{
		m_state.extio.current_lsb = JoulescopeState::CurrentLsb::NORMAL;
		m_state.settings.i_range = JoulescopeState::IRange::OFF;
	}
	update_extio();
	update_settings();
}

unsigned int
Joulescope::get_voltage(void)
{
	vector<UCHAR> ctr;
	cout << "Getting voltage\n";
	ctr = m_device.control_transfer_in_sync(
		BMREQUEST_TO_DEVICE,
		BMREQUEST_VENDOR,
		(UCHAR)JoulescopeRequest::STATUS,
		0,
		0,
		104
	);
	if (ctr.size() != 104) {
		throw runtime_error("Queried status wasn't 104 bytes.");
	}
	uint32_t mv = (unsigned)ctr[83];
	mv <<= 8;
	mv |= (unsigned)ctr[82];
	mv <<= 8;
	mv |= (unsigned)ctr[81];
	mv <<= 8;
	mv |= (unsigned)ctr[80];
	float v = (float)mv / (float)(1ul << 17);
	v *= 1000;
	return (unsigned int)v;
}

void
Joulescope::streaming_on(bool on)
{
	if (on)
	{
		if (m_raw_buffer_ptr == nullptr)
		{
			throw runtime_error("Joulescope needs a raw buffer pointer");
		}
		m_state.settings.streaming = JoulescopeState::Streaming::NORMAL;
		update_settings();
		UINT transfers_outstanding =    8; // the maximum number of USB transfers issued simultaneously [8]
		UINT transfer_length       =  256; // the USB transfer length in packets [256]
		m_device.read_stream_start(
			STREAMING_ENDPOINT_ID,
			transfers_outstanding,
			transfer_length * BULK_IN_LENGTH,
			m_raw_buffer_ptr
		);
	}
	else
	{
		m_device.read_stream_stop(STREAMING_ENDPOINT_ID);
		m_state.settings.streaming = JoulescopeState::Streaming::OFF;
		update_settings();
	}
}

void
Joulescope::update_extio(void)
{
	vector<UCHAR> buffer(24);

	buffer[0] = PACKET_VERSION;
	buffer[1] = (UCHAR)buffer.size();
	buffer[2] = (UCHAR)JoulescopePacketType::EXTIO;
	buffer[3] = 0;

	buffer[4] = 0;
	buffer[5] = 0;
	buffer[6] = 0;
	buffer[7] = 0;

	buffer[8] = 0; // flags
	buffer[9] = (UCHAR)m_state.extio.trigger_source;
	buffer[10] = (UCHAR)m_state.extio.current_lsb;
	buffer[11] = (UCHAR)m_state.extio.voltage_lsb;

	buffer[12] = (UCHAR)m_state.extio.gpi0;
	buffer[13] = (UCHAR)m_state.extio.gpi1;
	buffer[14] = 0; // uart_tx mapping reserved
	buffer[15] = 0;

	buffer[16] = 0;
	buffer[17] = 0;
	buffer[18] = 0;
	buffer[19] = 0;

	// TODO: Replace this with parameters['io_voltage']
	// io_voltage : 5000mV = 0x00001388
	buffer[20] = 0x88;
	buffer[21] = 0x13;
	buffer[22] = 0x00;
	buffer[23] = 0x00;

	m_device.control_transfer_out_sync(
		BMREQUEST_TO_DEVICE,
		BMREQUEST_VENDOR,
		(UCHAR)JoulescopeRequest::EXTIO,
		0,
		0,
		buffer
	);
}

void
Joulescope::update_settings(void)
{
	vector<UCHAR> buffer(16);

	buffer[0] = PACKET_VERSION;
	buffer[1] = (UCHAR)buffer.size();
	buffer[2] = (UCHAR)JoulescopePacketType::SETTINGS;
	buffer[3] = 0;

	buffer[4] = 0;
	buffer[5] = 0;
	buffer[6] = 0;
	buffer[7] = 0;

	buffer[8] = (UCHAR)m_state.settings.sensor_power;
	buffer[9] = (UCHAR)m_state.settings.i_range;
	buffer[10] = (UCHAR)m_state.settings.source;
	buffer[11] = (UCHAR)m_state.settings.options;

	buffer[12] = (UCHAR)m_state.settings.streaming;
	buffer[13] = 0;
	buffer[14] = 0;
	buffer[15] = 0;

	m_device.control_transfer_out_sync(
		BMREQUEST_TO_DEVICE,
		BMREQUEST_VENDOR,
		(UCHAR)JoulescopeRequest::SETTINGS,
		0,
		0,
		buffer
	);
}

js_stream_buffer_calibration_s
Joulescope::calibration_read_raw(void)
{
	vector<UCHAR> data;
	data = m_device.control_transfer_in_sync(
		BMREQUEST_TO_DEVICE,
		BMREQUEST_VENDOR,
		(UCHAR)JoulescopeRequest::CALIBRATION,
		1, // 1:active, 0:factory
		0,
		32 // datafile.HEADER_LENGTH
	);
	if (data.size() < 32)
	{
		throw runtime_error("Queried calibration data is too small.");
	}
	CalibrationHeader *hdr = (CalibrationHeader*)data.data();
	//cout << "Calibration Length : " << hdr->length << endl;
	//cout << "Version : " << (int)hdr->file_version << endl;
	// TODO: Check CRC32
	//cout << "CRC32 : 0x" << hex << hdr->crc32 << dec << endl;
	uint64_t length = hdr->length;
	string cal_raw;
	while (cal_raw.size() < length) {
		data = m_device.control_transfer_in_sync(
			BMREQUEST_TO_DEVICE,
			BMREQUEST_VENDOR,
			(UCHAR)JoulescopeRequest::CALIBRATION,
			1, // 1:active, 0:factory
			0,
			4096
		);
		//cout << "Bytes read during JSON scan " << data.size() << endl;
		cal_raw.insert(cal_raw.end(), data.begin(), data.end());
	}
	//cout << cal_raw << endl;
	size_t ajs_pos = cal_raw.find("AJS");
	if (ajs_pos == string::npos)
	{
		throw runtime_error("Calibration JSON missing 'AJS' field");
	}
	uint32_t tag_length = *((uint32_t*)(cal_raw.data() + ajs_pos + 4));
	size_t tag_start = ajs_pos + 4 + 4;
	string json = cal_raw.substr(tag_start, tag_length);
	//cout << json << endl;
	// JSON doesn't support NaN; nor does JsonCpp, so we use a crazy big float and fix later
	size_t nanpos = json.find("NaN");
	while (nanpos != string::npos)
	{
		// There's no way gain/offset will be this high, so we'll make it NAN later
		json.replace(nanpos, 3, "1e20");
		nanpos = json.find("NaN");
	}
	//cout << json << endl;
	Json::Value root;
	const auto rawJsonLength = static_cast<int>(json.length());
	JSONCPP_STRING err;
	Json::CharReaderBuilder builder;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(json.c_str(), json.c_str() + rawJsonLength, &root, &err))
	{
		throw runtime_error(err);
	}
	js_stream_buffer_calibration_s cal;
#	define HACK_NAN(x) (((x) > 1e19) ? NAN : (x))
	for (int i(0); i < 2; ++i)
	{
		cal.voltage_gain[i] = HACK_NAN(root["voltage"]["gain"][i].asFloat());
		cal.voltage_offset[i] = HACK_NAN(root["voltage"]["offset"][i].asFloat());
	}
	for (int i(0); i < 8; ++i)
	{
		cal.current_gain[i] = HACK_NAN(root["current"]["gain"][i].asFloat());
		cal.current_offset[i] = HACK_NAN(root["current"]["offset"][i].asFloat());
	}
	return cal;
}

vector<wstring>
Joulescope::guid_to_paths(GUID* pGuid)
{
	HDEVINFO                         DeviceInfoSet;
	DWORD                            DeviceIndex;
	DWORD                            RequiredSize;
	SP_DEVINFO_DATA                  DeviceInfoData;
	SP_DEVICE_INTERFACE_DATA         DeviceInterfaceData;
	SP_DEVICE_INTERFACE_DETAIL_DATA* pDeviceInterfaceDetailData;
	DWORD                            RequiredPropertyKeyCount;
	vector<wstring>                  paths;

	DeviceInfoSet = SetupDiGetClassDevs(
		pGuid,
		NULL,
		NULL,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	ZeroMemory(&DeviceInfoData, sizeof(SP_DEVINFO_DATA));
	DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
	ZeroMemory(&DeviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
	DeviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	DeviceIndex = 0;
	while (SetupDiEnumDeviceInterfaces(
		DeviceInfoSet,
		NULL,
		pGuid,
		DeviceIndex,
		&DeviceInterfaceData))
	{
		DeviceIndex++;
		RequiredSize = 0;
		SetupDiGetDeviceInterfaceDetail(
			DeviceInfoSet,
			&DeviceInterfaceData,
			NULL,
			0,
			&RequiredSize,
			NULL);
		// This is the malloc that needs to be freed.
		pDeviceInterfaceDetailData = (SP_DEVICE_INTERFACE_DETAIL_DATA*)malloc(RequiredSize);
		pDeviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
		if (SetupDiGetDeviceInterfaceDetail(
			DeviceInfoSet,
			&DeviceInterfaceData,
			pDeviceInterfaceDetailData,
			RequiredSize,
			&RequiredSize,
			&DeviceInfoData))
		{
			SetupDiGetDevicePropertyKeys(
				DeviceInfoSet,
				&DeviceInfoData,
				NULL,
				0,
				&RequiredPropertyKeyCount,
				0);
			// TODO: Error check on previous call
			paths.push_back(pDeviceInterfaceDetailData->DevicePath);
		}
		free(pDeviceInterfaceDetailData);
	}
	if (DeviceInfoSet)
	{
		SetupDiDestroyDeviceInfoList(DeviceInfoSet);
	}
	return paths;
}

vector<wstring>
Joulescope::scan(void)
{
	vector<wstring> all_paths;
	for (size_t idx = 0; idx < g_num_guids; ++idx)
	{
		vector<wstring> paths = guid_to_paths(&(g_guids[idx]));
		all_paths.insert(all_paths.end(), paths.begin(), paths.end());
	}
	return all_paths;
}

/**
 * Returns a Windows USB path to a Joulescope specified by the serial number,
 * or if the serial number is not included (or empty), the first joulescope
 * found.
 *
 * If no joulescopes are found, it returns an empty wide string.
 */
wstring
Joulescope::find_joulescope_by_serial_number(string serial_number)
{
	vector<wstring> tokens;
	wstring         wserial_number(serial_number.begin(), serial_number.end());
	vector<wstring> paths = scan();
	if (serial_number == "" && !paths.empty())
	{
		return paths.front();
	}
	else
	{
		for (size_t i = 0; i < paths.size(); ++i)
		{
			// Caution: microsoft does not recommend this
			// https://github.com/jetperch/js110_statistics/blob/55a9817bb3e6ddc9627c1bcc3ba7702bdad0554b/source/js110_statistics.c#L180
			boost::split(tokens, paths[i], boost::is_any_of("#"));
			if (tokens.size() > 2)
			{
				if (tokens[2] == wserial_number)
				{
					return paths[i];
				}
			}
		}
	}
	return L"";
}

