# joulescope-win32

A basic CLI for the Jetperch Joulescope JS110 under native Win32 libraries. The current goal of this project is to provide a driver for EEMBC's benchmark framework, so it may appear a bit opinionated (e.g., timestamp behavior). This may (or may not) change in the future.

# Usage

Starting the program initiates a simple command-line interface. It is intended to be used through a bidrectional pipe/IPC, rather than a user typing instructions. Here are the commands:

```
help
deinit - De-initialize the current JS110.
exit - De-initialize (if necessary) and exit.
help - Print this help.
init - [serial] Find the first JS110 (or by serial #) and initialize it.
power - [on|off] Get/set output power state.
rate - Set the sample rate to an integer multiple of 1e6.
timer - [on|off] Get/set timestamping state.
trace - [on [path prefix]|off] Get/set tracing and save files in 'path/prefix' (quote if 'path' uses spaces).
voltage - Report the internal 2s voltage average in mv.
```

The output energy file format is:
~~~
1 UInt8 - Trace version
1 Float32LE - Sample rate, in Hz
N Float32LE - N energy samples, in Joules
~~~

The timestamp format is a list of JSON array of floating point times in seconds.

# Quick Overview

The file `device.cpp` is effectively a line-by-line translation of the Matt Liberty's Joulescope Python-driver for Windows, the same is true of the `raw_processor.cpp` file. The `joulescope.cpp` file slims down the functionality provided by the Python driver. The `main.cpp` file controls the CLI and issues commands to the driver object, and writes to the output files.

The CLI downsamples based on `samplerate`, which is a command that can get or set the final rate in Hertz. The `current_lsb` is used as a falling-edge counter, which generates an `m-lap-us-\d+` message on each falling edge. Be sure to ground gpi0 when developing to avoid spurious messages.

The sampling code spins in its own thread until the command line parser thread recieves an `exit` or `stop-trace` command.

# Copyright

All joulescope-win32 code is released under the permissive Apache 2.0 license. See the License File for details.

The original Python code is Copyright (c) by [JetPerch](https://github.com/jetperch/pyjoulescope) and released under Apache 2.0. Many thanks for Matt Liberty's feedback on the port and general camraderie!

The Json library used is Copyright (c) 2007-2010 by Baptiste Lepilleur and The JsonCpp Authors

