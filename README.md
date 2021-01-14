# joulescope-win32

A basic CLI for the Jetperch Joulescope JS110 under native Win32 libraries. The current goal of this project is to provide a driver for EEMBC's IoTConnect framework, so it may appear a bit opinionated (e.g., timestamp behavior). This may (or may not) change in the future.

# Usage

Starting the program initiates a simple command-line interface. It is intended to be used through a bidrectional pipe, rather than a user typing instructions. Here are the commands:

1. `exit` - De-initialize the hardware and exit the app.
2. `init [SERIAL#]` - Initialize a JS110 by serial number, or the first one found.
3. `deinit` - Stop tracing, release the USB interface, close any files (does not exit).
4. `power [on|off]` - Turn on power to the device under test, or indicate status
6. `trace-start [path] [prefix] ` - Begin logging trace samples at the requested sample rate at a path starting with a prefix and ending with `-energy.bin`. Path defaults to "." and prefix "js110".
7. `trace-stop` - Stop tracing and close the trace file.
8. `timestamps [on|off]` - Log samples with GPIO0 falling edge to the file `[path]/[prefix]-timestamps.json`. Multiple falling edges in a downsample are counted as one for that sample.
10. `samplerate [HZ]` - Set the sample rate in Hz, or report back current rate. Must be a factor of 1,000,000.

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

