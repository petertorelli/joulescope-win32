# joulescope-win32
A basic CLI for the Jetperch Joulescope JS110 for native Win32

The file `device.cpp` is effectively a line-by-line translation of the Matt Liberty's Joulescope Python-driver for Windows. The `joulescope.cpp` file slims down the functionality to what I need for my project. Lastly, `main.cpp` invokes a simple Win32 CLI that writes data to a file for future processing.

The CLI downsamples based on `samplerate`, which is a command that can get or set the final rate in Hertz. The `current_lsb` is used as a falling-edge counter, which generates an `m-lap-us-\d+` message on each falling edge. Be sure to ground gpi0 when developing to avoid spurious messages.

This is alpha quality code, there is sparse commenting and no help screen. Important areas of attention start with `TODO` in the comments.
