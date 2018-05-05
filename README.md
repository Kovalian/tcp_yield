# Yield TCP

A Less-than-Best-Effort TCP congestion control mechanism implemented for Linux. Full description to come. 

Note that this module has only been tested with Linux 4.4.15 and is not guaranteed to work with other versions.

## Instructions
The Makefile contains the necessary rule to compile Yield against the running kernel:
> make

Run the install rule to move resulting modules to the library and update dependencies (root access will be required):
> make install

Once copied, the modules can be loaded and selected like any other TCP congestion control modules using:
> modprobe tcp_yield \
> sysctl net.ipv4.tcp_congestion_control=yield