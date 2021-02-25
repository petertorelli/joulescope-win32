#pragma once
#include <cinttypes>

#define JS110_SAMPLES_PER_PACKET 126u

enum class JoulescopePacketType
{
	SETTINGS = 1,
	STATUS = 2,
	EXTIO = 3,
	INFO = 4
};

struct JoulescopePacket {
	uint8_t buffer_type;
	uint8_t status;
	uint16_t length;
	uint16_t pkt_index;
	uint16_t usb_frame_index;
	uint32_t samples[126];
};