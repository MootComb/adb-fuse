#!/bin/bash

# Create mount base directory
mkdir -p /run/media/$USER

# Set ownership
chown $USER:$USER /run/media/$USER

# Build
make

# Run FUSE daemon
./fuse
