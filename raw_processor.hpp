#pragma once
#include "device.hpp"



#define _SUPPRESS_SAMPLES_MAX 512
#define SUPPRESS_HISTORY_MAX    8
#define SUPPRESS_WINDOW_MAX    12
#define SUPPRESS_POST_MAX       8
#define I_RANGE_D_LENGTH        3
#define _I_RANGE_MISSING        8

#define SUPPRESS_MODE_OFF  0
#define SUPPRESS_MODE_MEAN 1
#define SUPPRESS_MODE_NAN  3


#define SUPPRESS_SAMPLES_MAX _SUPPRESS_SAMPLES_MAX
#define I_RANGE_MISSING      _I_RANGE_MISSING


struct js_stream_buffer_calibration_s
{
	float current_offset[8];
	float current_gain[8];
	float voltage_offset[2];
	float voltage_gain[2];
};


typedef void (*raw_processor_cbk_fn)(void* user_data, float cal_i, float cal_v, uint8_t bits);


class RawProcessor {
public:
	float   d_cal[_SUPPRESS_SAMPLES_MAX][2];     // as i, v
	uint8_t d_bits[_SUPPRESS_SAMPLES_MAX];     // packed bits : 7 : 6 = 0, 5 = voltage_lsb, 4 = current_lsb, 3 : 0 = i_range
	float   d_history[SUPPRESS_HISTORY_MAX][2];  // as i, v
	uint8_t d_history_idx;

	js_stream_buffer_calibration_s _cal;

	raw_processor_cbk_fn _cbk_fn;
	void* _cbk_user_data;

	int32_t  is_skipping;
	uint32_t _idx_out;
	uint64_t sample_count;
	uint64_t sample_missing_count;  // based upon sample_id
	uint64_t skip_count;            // number of sample skips
	uint64_t sample_sync_count;     // based upon alternating 0 / 1 pattern
	uint64_t contiguous_count;      //

	uint8_t _i_range_last;
	int32_t _suppress_samples_pre;     // the number of samples to use before range change
	int32_t _suppress_samples_window;  // the total number of samples to suppress after range change
	int32_t _suppress_samples_post;

	int32_t suppress_count; // the suppress counter, 1 = replace previous
	uint8_t _suppress_mode;

	uint16_t sample_toggle_last;
	uint16_t sample_toggle_mask;
	uint8_t _voltage_range;

	uint16_t* bulk_raw;
	float* bulk_cal;
	uint8_t* bulk_bits;
	uint32_t  bulk_index;
	uint32_t  bulk_length;  // in samples
	RawProcessor();
	void callback_set(raw_processor_cbk_fn cbk, void* user_data);
	void reset(void);
	void calibration_set(js_stream_buffer_calibration_s cal);
	void calibration_set(
		std::vector<float> current_offset,
		std::vector<float> current_gain,
		std::vector<float> voltage_offset,
		std::vector<float> voltage_gain);

	void process(uint16_t raw_i, uint16_t raw_v);
	void _history_insert(float cal_i, float cal_v);
};
