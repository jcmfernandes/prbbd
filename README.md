# PRBBD

A Persistent RAM Backed Block Device Driver.

## What is it?

Plainly, PRBBD is a ramdisk that uses a defined memory range (through `ioremap`). Useful when you want maintain an in-memory filesystem across warm starts (i.e., when the RAM state is preserved... obviously!).

## Typical usage

You will most likely want to cap the amount of memory "seen" by the kernel through the `mem` kernel argument.

Lets say our imaginary system has 1024 MiB of RAM and we want a 128 MiB PRBBD device. We would pass `mem=896M` to the kernel and `prbbd=pramdisk,896M,128M` to PRBBD.

## Contact

mail youknowwhat joaofernandes putadothere eu
