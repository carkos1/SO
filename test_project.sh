#!/bin/bash

# Compile
echo "Compiling..."
make
if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

# Clean previous run
rm -f input_pipe DEI_Emergency.log output.txt
ipcrm -a 2>/dev/null

# Start Server
echo "Starting Server..."
./dei_emergency > output.txt 2>&1 &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for startup
sleep 2

# Function to send command
send_cmd() {
    echo "$1" > input_pipe
    echo "Sent: $1"
}

# 1. Test Stats (Empty)
echo "--- Testing SIGUSR1 (Empty Stats) ---"
kill -SIGUSR1 $SERVER_PID
sleep 1

# 2. Test Single Patient
echo "--- Testing Single Patient ---"
send_cmd "PatientA 1 1 1"
sleep 2

# 3. Test Dynamic Triage Scaling
echo "--- Testing Dynamic Triage (Scale Down) ---"
send_cmd "TRIAGE=2"
sleep 1
echo "--- Testing Dynamic Triage (Scale Up) ---"
send_cmd "TRIAGE=6"
sleep 1

# 4. Test Queue Limit (Max 10)
echo "--- Testing Queue Limit ---"
for i in {1..15}; do
    send_cmd "P$i 1 1 1"
done
sleep 5

# 5. Test Stats (Populated)
echo "--- Testing SIGUSR1 (Populated Stats) ---"
kill -SIGUSR1 $SERVER_PID
sleep 1

# 6. Wait for Doctor Shift (Config=5s)
echo "--- Waiting for Doctor Shift Rotation (6s) ---"
sleep 6

# 7. Shutdown
echo "--- Testing Shutdown (SIGINT) ---"
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "--- Test Finished ---"
echo "Check output.txt for details."
