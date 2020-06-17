#include "raw_processor.hpp"

using namespace std;

// experimentally determined charge coupling durations in samples at 2 MSPS
uint8_t SUPPRESS_MATRIX[9][9] = {
	//   0, 1, 2, 3, 4, 5, 6, 7, 8   // from this current select
	{0, 5, 5, 5, 5, 5, 6, 6, 0}, // to 0
	{3, 0, 5, 5, 5, 6, 7, 8, 0}, // to 1
	{4, 4, 0, 6, 6, 7, 7, 8, 0}, // to 2
	{4, 4, 4, 0, 6, 6, 7, 7, 0}, // to 3
	{4, 4, 4, 4, 0, 6, 7, 6, 0}, // to 4
	{4, 4, 4, 4, 4, 0, 7, 6, 0}, // to 5
	{4, 4, 4, 4, 4, 4, 0, 6, 0}, // to 6
	{0, 0, 0, 0, 0, 0, 0, 0, 0}, // to 7 (off)
	{0, 0, 0, 0, 0, 0, 0, 0, 0}, // to 8 (missing)
};


void
cal_init(js_stream_buffer_calibration_s* cal_ptr)
{
	for (size_t i(0); i < 8; ++i)
	{
		cal_ptr->current_offset[i] = 0.0f;
		cal_ptr->current_gain[i] = 1.0f;
	}
	cal_ptr->current_gain[7] = 0.0f; // always compute zero current when off
	for (size_t i(0); i < 2; ++i)
	{
		cal_ptr->voltage_offset[i] = 0.0f;
		cal_ptr->voltage_gain[i] = 1.0f;
	}
}

RawProcessor::RawProcessor() {
	// __cinit__
	cal_init(&_cal);
	_suppress_samples_pre = 2;
	_suppress_samples_window = 255;  // lookup = 'n'
	_suppress_samples_post = 2;
	_suppress_mode = SUPPRESS_MODE_MEAN;
	// __init__
	reset();
}

void
RawProcessor::callback_set(raw_processor_cbk_fn cbk, void* user_data) {
	_cbk_fn = cbk;
	_cbk_user_data = user_data;
}
/*
string supress_mode(void) {
@suppress_mode.setter
def suppress_mode(self, value) :
*/
void
RawProcessor::reset(void) {
	uint32_t idx;
	sample_count = 0;
	sample_missing_count = 0;
	is_skipping = 1;
	skip_count = 0;
	sample_sync_count = 0;
	contiguous_count = 0;

	suppress_count = 0;
	_i_range_last = 7;

	sample_toggle_last = 0;
	sample_toggle_mask = 0;
	_voltage_range = 0;
	_idx_out = 0;

	for (idx = 0; idx < SUPPRESS_HISTORY_MAX; ++idx)
	{
		d_history[idx][0] = 0.0;
		d_history[idx][1] = 0.0;
	}
	d_history_idx = 0;
}

void
RawProcessor::calibration_set(js_stream_buffer_calibration_s cal)
{
	_cal = cal;
	_cal.current_offset[7] = 0.0; // ASKMATT
	_cal.current_gain[7] = 0.0; // ASKMATT
}

void
RawProcessor::calibration_set(
	vector<float> current_offset,
	vector<float> current_gain,
	vector<float> voltage_offset,
	vector<float> voltage_gain)
{
	if (current_offset.size() < 7 || current_gain.size() << 7)
	{
		throw runtime_error("current calibration vector too small");
	}
	if (voltage_offset.size() < 2 || voltage_gain.size() << 2)
	{
		throw runtime_error("voltage calibration vector too small");
	}
	for (size_t i(0); i < 7; ++i)
	{
		_cal.current_offset[i] = current_offset[i];
		_cal.current_gain[i] = current_gain[i];
	}
	_cal.current_offset[7] = 0.0; // ASKMATT
	_cal.current_gain[7] = 0.0; // ASKMATT
	for (size_t i(0); i < 2; ++i)
	{
		_cal.voltage_offset[i] = voltage_offset[i];
		_cal.voltage_gain[i] = voltage_gain[i];
	}
}

void
RawProcessor::process(uint16_t raw_i, uint16_t raw_v)
{
	uint32_t is_missing;
	uint8_t suppress_window;
	int32_t suppress_idx;
	int32_t idx;
	int32_t suppress_filter_counter;
	uint8_t bits;
	uint8_t i_range;
	uint16_t sample_toggle_current;
	uint64_t sample_sync_count;
	float cal_i;
	float cal_v;
	
	is_missing = 0;

	if ((0xffff == raw_i) && (0xffff == raw_v))
	{
		is_missing = 1;
		i_range = _I_RANGE_MISSING; // missing sample
		sample_missing_count += 1;
		contiguous_count = 0;
		if (is_skipping == 0)
		{
			skip_count += 1;
			is_skipping = 1;
		}
	}
	else
	{
		i_range = ((raw_i & 0x0003) | ((raw_v & 0x0001) << 2));
		is_skipping = 0;
		contiguous_count += 1;
	}
	bits = (i_range & 0x0f) | ((raw_i & 0x0004) << 2) | ((raw_v & 0x0004) << 3);

	// process i_range for glitch suppression
	if (i_range != _i_range_last)
	{
		suppress_window = SUPPRESS_MATRIX[i_range][_i_range_last];
		if (suppress_window && _suppress_samples_window != 255)
		{
			suppress_window = _suppress_samples_window;
		}
		if (suppress_window)
		{
			idx = _idx_out + suppress_window + _suppress_samples_post;
			if (idx > suppress_count)
			{
				suppress_count = idx;
			}
		}
	}

	sample_toggle_current = (raw_v >> 1) & 0x1;
	raw_i = raw_i >> 2;
	raw_v = raw_v >> 2;
	sample_sync_count = (sample_toggle_current ^ sample_toggle_last ^ 1) & sample_toggle_mask;
	if (sample_sync_count && is_missing == 0) {
		skip_count += 1;
		is_skipping = 1;
		sample_sync_count += 1;
	}
	sample_toggle_last = sample_toggle_current;
	sample_toggle_mask = 0x1;

	if (i_range > 7) // missing sample
	{
		cal_i = NAN;
		cal_v = NAN;
	}
	else
	{
		cal_i = (float)raw_i;
		cal_i += _cal.current_offset[i_range];
		cal_i *= _cal.current_gain[i_range];
		cal_v = (float)raw_v;
		cal_v += _cal.voltage_offset[_voltage_range];
		cal_v *= _cal.voltage_gain[_voltage_range];
	}

	if (_idx_out < _SUPPRESS_SAMPLES_MAX)
	{
		d_bits[_idx_out] = bits;
		d_cal[_idx_out][0] = cal_i;
		d_cal[_idx_out][1] = cal_v;
	}

	// Suppress Joulescope range switching glitch (at least for now).
	if (suppress_count > 0)
	{
		//defer output until suppress computed
		if (suppress_count == 1)
		{
			// last sample, take action
			if (_idx_out >= _SUPPRESS_SAMPLES_MAX)
			{
				//log.warning('Suppression filter too long for actual data: %s > %s', _idx_out, _SUPPRESS_SAMPLES_MAX)
				while (_idx_out >= _SUPPRESS_SAMPLES_MAX)
				{
					_cbk_fn(_cbk_user_data, NAN, NAN, 0xff);
					_idx_out -= 1;
				}
			}

			if (SUPPRESS_MODE_MEAN == _suppress_mode)
			{
				suppress_filter_counter = 0;
				cal_i = 0;

				// sum samples over pre for mean computation
				idx = d_history_idx - _suppress_samples_pre;
				while (idx < 0)
				{
					idx += SUPPRESS_HISTORY_MAX;
				}
				for (suppress_idx = 0; suppress_idx < _suppress_samples_pre; ++suppress_idx)
				{
					while (idx >= SUPPRESS_HISTORY_MAX)
					{
						idx -= SUPPRESS_HISTORY_MAX;
					}
					cal_i += d_history[idx][0];
					suppress_filter_counter += 1;
					idx += 1;
				}

				// sum samples over post for mean computation
				for (idx = _idx_out + 1 - _suppress_samples_post; idx < _idx_out + 1; ++idx)
				{
					cal_i += d_cal[idx][0];
					suppress_filter_counter += 1;
				}

				if (suppress_filter_counter)
				{
					// compute mean
					cal_i = (float)(cal_i / suppress_filter_counter);
				}

				// update suppressed samples
				for (idx = 0; idx < _idx_out + 1 - _suppress_samples_post; ++idx)
				{
					sample_count += 1;
					_cbk_fn(_cbk_user_data, cal_i, d_cal[idx][1], d_bits[idx]);
					_history_insert(cal_i, d_cal[idx][1]);
				}
			}
			else if (SUPPRESS_MODE_NAN == _suppress_mode)
			{
				for (suppress_idx = 0; suppress_idx < _idx_out + 1 - _suppress_samples_post; ++suppress_idx)
				{
					sample_count += 1;
					_cbk_fn(_cbk_user_data, NAN, NAN, d_bits[suppress_idx]);
				}

			}
			else if (SUPPRESS_MODE_OFF == _suppress_mode)
			{
				for (suppress_idx = 0; suppress_idx < _idx_out + 1 - _suppress_samples_post; ++suppress_idx)
				{
					sample_count += 1;
					_cbk_fn(_cbk_user_data, d_cal[suppress_idx][0], d_cal[suppress_idx][1], d_bits[suppress_idx]);
					_history_insert(d_cal[suppress_idx][0], d_cal[suppress_idx][1]);
				}
			}
			else
			{
				throw runtime_error("unsupported suppress_mode");
			}

			// update post samples
			for (idx = _idx_out + 1 - _suppress_samples_post; idx < _idx_out + 1; ++idx)
			{
				sample_count += 1;
				_cbk_fn(_cbk_user_data, d_cal[idx][0], d_cal[idx][1], d_bits[idx]);
				_history_insert(d_cal[idx][0], d_cal[idx][1]);
			}
			_idx_out = 0;
		}
		else
		{
			// just skip, will fill in later
			_idx_out += 1;
		}
		suppress_count -= 1;
	}
	else
	{
		_history_insert(cal_i, cal_v);
		sample_count += 1;
		_cbk_fn(_cbk_user_data, cal_i, cal_v, bits);
		_idx_out = 0;
	}
	_i_range_last = i_range;
}

void
RawProcessor::_history_insert(float cal_i, float cal_v)
{
	// store history to circular buffer for _suppress_samples_pre
	d_history[d_history_idx][0] = cal_i;
	d_history[d_history_idx][1] = cal_v;
	d_history_idx += 1;
	if (d_history_idx >= SUPPRESS_HISTORY_MAX)
	{
		d_history_idx = 0;
	}
}
