#!/usr/bin/env bash
gcc kiss_fft.c -c -O3 -ffast-math -g
gcc kiss_fftr.c -c -O3 -ffast-math -g
gcc main.c -c -O3 -ffast-math -g
gcc main.o kiss_fft.o kiss_fftr.o -o fft -lSDL -lm -lasound -g
