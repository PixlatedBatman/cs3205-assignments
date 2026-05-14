#!/bin/bash

# Check if input is provided

if [ -z "$1" ]; then
echo "Usage: ./run.sh [Q1|Q2|Q3]"
exit 1
fi

PREFIX=$1

# Validate input

if [[ "$PREFIX" != "Q1" && "$PREFIX" != "Q2" && "$PREFIX" != "Q3" ]]; then
echo "Invalid option. Use Q1, Q2, or Q3."
exit 1
fi

# Compile

g++ -std=c++11 ${PREFIX}_main.cpp ${PREFIX}_routing_algo.cpp -o rip

# Check if compilation succeeded

if [ $? -ne 0 ]; then
echo "Compilation failed."
exit 1
fi

# Run

./rip < input.txt > ${PREFIX}_output.txt
