# 📘 Code Explanation & Documentation: DEI Emergency System

This document serves as the comprehensive manual for the Hospital Simulator (`main.c`). It explains the logic using simple metaphors and provides detailed "DocString" style documentation for every function.

---

## 🏥 The Hospital Metaphor (How it works)

Imagine this program is a busy **Hospital**.

1.  **The Administrator (`main`)**:
    *   **Role:** The boss who runs the show.
    *   **Job:** Sits at the front desk, watches the **Front Door** (`input_pipe`), and manages the staff.
    *   **Action:** If a doctor leaves (shift ends), the Administrator immediately hires a new one. If the waiting room gets too full, they hire a temporary doctor.

2.  **The Front Door (`input_pipe`)**:
    *   **Role:** The entry point.
    *   **Job:** A special tube where patients arrive (e.g., "John 2 5 1") and commands are dropped (e.g., "TRIAGE=10").

3.  **The Receptionists (`triage_thread`)**:
    *   **Role:** The first point of contact.
    *   **Job:** They take patients from the door, assess them (simulate triage time), and send them to the **Waiting Room**.
    *   **Metaphor:** If there are no patients, they sit and drink coffee (wait on a condition variable).

4.  **The Waiting Room (`Message Queue`)**:
    *   **Role:** The holding area.
    *   **Job:** Patients sit here waiting for a doctor. It's organized by priority—sicker patients (Priority 1) sit in the front row.

5.  **The Doctors (`doctor_process`)**:
    *   **Role:** The medical staff.
    *   **Job:** They grab the next patient from the Waiting Room, treat them (simulate attendance time), and then look for the next one.
    *   **Shifts:** They work for a specific time (e.g., 5 seconds) and then go home.

6.  **The Dashboard (`Shared Memory`)**:
    *   **Role:** The statistics board.
    *   **Job:** A big whiteboard on the wall where everyone writes down numbers: "Patients Treated: 50", "Avg Wait: 2s".

7.  **The Scribe (`log_event` / MMF)**:
    *   **Role:** The record keeper.
    *   **Job:** Writes every event into a permanent **Log Book** (Memory Mapped File) instantly.

---

## 📚 Function Documentation (DocStrings)

Here is the detailed explanation for every function in the code.

### `int main()`
*   **Description:** The entry point and "Administrator" of the system.
*   **Responsibilities:**
    1.  Calls `setup()` to build the infrastructure.
    2.  Hires initial staff (threads and processes).
    3.  Enters a loop to read from `input_pipe`.
    4.  Parses patient data and adds them to the `triage_queue`.
    5.  Handles dynamic scaling commands (`TRIAGE=X`).
    6.  Respawns doctors who finish their shifts.
*   **Metaphor:** The Hospital Administrator sitting at the control desk.

### `void setup()`
*   **Description:** Initializes all IPC (Inter-Process Communication) mechanisms.
*   **Details:**
    *   Creates **Shared Memory** for statistics.
    *   Creates **Message Queue** for the doctor-patient handover.
    *   Creates **Named Pipe** for input.
    *   Sets up **Memory Mapped File** for logging.
*   **Metaphor:** The construction crew building the hospital before it opens.

### `void cleanup()`
*   **Description:** Releases all resources before the program exits.
*   **Details:** Removes SHM, MSQ, Pipe, and unmaps the log file.
*   **Metaphor:** The demolition crew cleaning up the site after the hospital closes.

### `void read_config(const char *filename)`
*   **Description:** Reads configuration parameters from a text file.
*   **Parameters:** `filename` - Path to `config.txt`.
*   **Updates:** Global `config` structure (Doctors count, Shift length, etc.).
*   **Metaphor:** Reading the hospital's rulebook.

### `void init_triage_queue()`
*   **Description:** Initializes the internal queue used by the main thread to pass patients to triage threads.
*   **Details:** Allocates memory for the buffer and initializes mutexes and condition variables.
*   **Metaphor:** Setting up the velvet ropes and ticket counter at the reception.

### `void *triage_thread(void *arg)`
*   **Description:** The code running in each Triage Thread.
*   **Logic:**
    1.  Waits for a patient in `triage_queue`.
    2.  Simulates triage time (`sleep`).
    3.  Updates "Wait Before Triage" statistics.
    4.  Sends the patient to the **Message Queue**.
    5.  Checks if the queue is full (`MSQ_WAIT_MAX`) and signals for help if needed.
*   **Metaphor:** A receptionist processing a patient and sending them to the waiting room.

### `void doctor_process(int doctor_id, int is_temp)`
*   **Description:** The code running in each Doctor Process.
*   **Parameters:**
    *   `doctor_id`: ID of the doctor (0 to N).
    *   `is_temp`: Flag (1 if temporary, 0 if regular).
*   **Logic:**
    1.  Checks if shift time has exceeded `SHIFT_LENGTH`.
    2.  If `is_temp`, checks if load is low enough to quit.
    3.  Receives a patient from **Message Queue** (Priority -4 means types 1, 2, 3, 4).
    4.  Simulates treatment time (`sleep`).
    5.  Updates "Wait After Triage" and "Total Time" statistics.
*   **Metaphor:** A doctor treating patients until their shift ends.

### `void spawn_doctor(int id, int is_temp)`
*   **Description:** Forks a new process to act as a doctor.
*   **Details:** Updates the `doctor_pids` array so the main loop can track the new process.
*   **Metaphor:** Hiring a new doctor and giving them an ID badge.

### `void log_event(const char *format, ...)`
*   **Description:** Thread-safe logging function.
*   **Details:**
    *   Prints to `stdout` (Screen).
    *   Writes to the Memory Mapped File buffer.
    *   Uses a mutex to prevent garbled text when multiple people write at once.
*   **Metaphor:** The Scribe writing in the official logbook.

### `void sigint(int sig)`
*   **Description:** Handler for `SIGINT` (Ctrl+C).
*   **Action:** Sets `running = 0`, causing all loops to exit gracefully.
*   **Metaphor:** The fire alarm sounding, telling everyone to finish up and leave.

### `void sigusr1(int sig)`
*   **Description:** Handler for `SIGUSR1`.
*   **Action:** Prints the current statistics from Shared Memory to the screen.
*   **Metaphor:** The Administrator asking for a status report.

### `void sigchld_handler(int sig)`
*   **Description:** Handler for `SIGCHLD`.
*   **Action:** Sets a flag `child_terminated = 1`. The main loop sees this and calls `waitpid` to clean up zombies and respawn doctors.
*   **Metaphor:** A pager beeping to say a doctor has clocked out.

### `void sigusr2_handler(int sig)`
*   **Description:** Handler for `SIGUSR2`.
*   **Action:** Sets `need_temp_doctor = 1`. The main loop sees this and spawns a temporary doctor.
*   **Metaphor:** An emergency button pressed when the waiting room is overflowing.
