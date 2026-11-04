#!/usr/bin/env sh
# Build the steno engine for your PC (no ESP32, no ESP-IDF needed). Output: tools/hosttest/hostt
set -e
cd "$(dirname "$0")/../.."
gcc -std=c11 -O1 -Wall -fsanitize=address,undefined \
    -Icomponents/steno_core/include -Icomponents/steno_core \
    components/steno_core/steno_*.c tools/hosttest/host_main.c \
    -o tools/hosttest/hostt
echo "built tools/hosttest/hostt"
