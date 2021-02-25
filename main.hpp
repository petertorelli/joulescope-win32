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

#include "joulescope.hpp"
#include "raw_processor.hpp"
#include "raw_buffer.hpp"
#include "file_writer.hpp"
#include <fstream>
#include <filesystem>
#include <iomanip>

#include <csignal>

#include <boost\algorithm\string\trim_all.hpp>
#include <boost\tokenizer.hpp>


typedef boost::escaped_list_separator<char> delim_t;
typedef boost::tokenizer<delim_t> tokenizer_t;

struct Command
{
	void(*func)(std::vector<std::string>);
	std::string desc;
};

typedef std::map<std::string, Command> CommandTable;

void cmd_debug(std::vector<std::string>);
void cmd_deinit(std::vector<std::string>);
void cmd_exit(std::vector<std::string>);
void cmd_help(std::vector<std::string>);
void cmd_init(std::vector<std::string>);
void cmd_power(std::vector<std::string>);
void cmd_timer(std::vector<std::string>);
void cmd_trace(std::vector<std::string>);
void cmd_rate(std::vector<std::string>);
void cmd_voltage(std::vector<std::string>);
