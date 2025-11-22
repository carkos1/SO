#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <semaphore.h>
#include <stdarg.h>

// Config structure
typedef struct {
    int TRIAGE_QUEUE_MAX;
    int TRIAGE;
    int DOCTORS;
    int SHIFT_LENGTH;
    int MSQ_WAIT_MAX;
} Config;

// Patient structure
typedef struct {
    int arrival_number;
    char name[100];
    int triage_time;
    int attendance_time;
    int priority;
    time_t arrival_time;
    time_t triage_start_time;
    time_t triage_end_time;
    time_t attendance_start_time;
    time_t attendance_end_time;
} Patient;

// Message structure 
typedef struct {
    long mtype;
    Patient patient;
} PatientMessage;

// Shared memory structure
typedef struct {
    int total_patients_triaged;
    int total_patients_attended;
    double total_wait_before_triage;
    double total_wait_after_triage;
    double total_time_in_system;
    pthread_mutex_t mutex;
} Statistics;


Config config;
int shm_id;
Statistics *stats;
int msq_id;
pthread_t *triage_threads;
int running = 1;


void read_config(const char *filename);
void setup();
void cleanup();
void *triage_thread(void *arg);
void doctor_process(int doctor_id);
void sigint(int sig);
void sigusr1(int sig);
void log_event(const char *format, ...);

int main() {
    printf("=== DEI Emergency System Starting ===\n");
    
    // Read config.txt
    read_config("config.txt");
    
    
    setup();

    signal(SIGINT, sigint);
    signal(SIGUSR1, sigusr1);
    
    // Create triage threads
    triage_threads = malloc(config.TRIAGE * sizeof(pthread_t));
    for (int i = 0; i < config.TRIAGE; i++) {
        pthread_create(&triage_threads[i], NULL, triage_thread, (void *)(long)i);
        log_event("Triage thread %d created\n", i);
    }
    
    // Create doctor processes
    for (int i = 0; i < config.DOCTORS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            doctor_process(i);
            exit(0);
        } else {
            log_event("Doctor process %d created with PID: %d\n", i, pid);
        }
    }
    
    printf("System running. Press Ctrl+C to stop.\n");
    while (running) {
        sleep(1);
    }
    
    // Cleanup
    cleanup();
    printf("=== DEI Emergency System Stopped ===\n");
    return 0;
}

void read_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening config file");
        exit(1);
    }
    
    char line[100];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "TRIAGE_QUEUE_MAX")) {
            sscanf(line, "TRIAGE_QUEUE_MAX = %d", &config.TRIAGE_QUEUE_MAX);
        } else if (strstr(line, "TRIAGE")) {
            sscanf(line, "TRIAGE = %d", &config.TRIAGE);
        } else if (strstr(line, "DOCTORS")) {
            sscanf(line, "DOCTORS = %d", &config.DOCTORS);
        } else if (strstr(line, "SHIFT_LENGTH")) {
            sscanf(line, "SHIFT_LENGTH = %d", &config.SHIFT_LENGTH);
        } else if (strstr(line, "MSQ_WAIT_MAX")) {
            sscanf(line, "MSQ_WAIT_MAX = %d", &config.MSQ_WAIT_MAX);
        }
    }
    fclose(file);
    
    printf("Configuration loaded:\n");
    printf("TRIAGE_QUEUE_MAX: %d\n", config.TRIAGE_QUEUE_MAX);
    printf("TRIAGE: %d\n", config.TRIAGE);
    printf("DOCTORS: %d\n", config.DOCTORS);
    printf("SHIFT_LENGTH: %d\n", config.SHIFT_LENGTH);
    printf("MSQ_WAIT_MAX: %d\n", config.MSQ_WAIT_MAX);
}

void setup() {
    // Create shared memory 
    key_t shm_key = ftok("/tmp", 'S');
    shm_id = shmget(shm_key, sizeof(Statistics), IPC_CREAT | 0666);
    if (shm_id == -1) {
        perror("shmget failed");
        exit(1);
    }
    
    stats = (Statistics *)shmat(shm_id, NULL, 0);
    if (stats == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }
    
    // Initialize statistics in shm
    stats->total_patients_triaged = 0;
    stats->total_patients_attended = 0;
    stats->total_wait_before_triage = 0;
    stats->total_wait_after_triage = 0;
    stats->total_time_in_system = 0;
    pthread_mutex_init(&stats->mutex, NULL);
    
    // Create message queue
    key_t msq_key = ftok("/tmp", 'M');
    msq_id = msgget(msq_key, IPC_CREAT | 0666);
    if (msq_id == -1) {
        perror("msgget failed");
        exit(1);
    }
    
    printf("IPC mechanisms setup complete\n");
}

void cleanup() {
    // Clean shared memory
    shmdt(stats);
    shmctl(shm_id, IPC_RMID, NULL);
    
    // Clean message queue
    msgctl(msq_id, IPC_RMID, NULL);
    
    // Free threads
    free(triage_threads);
    
    printf("IPC cleanup complete\n");
}

void *triage_thread(void *arg) {
    int thread_id = (int)(long)arg;
    
    while (running) {
        //test
        Patient patient;
        patient.arrival_time = time(NULL);
        patient.triage_start_time = time(NULL);
        patient.priority = 1; 

        sleep(1); // Simulation to be substituted with actual handling in the future
        
        patient.triage_end_time = time(NULL);
        
        // Update statistics
        pthread_mutex_lock(&stats->mutex);
        stats->total_patients_triaged++;
        stats->total_wait_before_triage += 
            difftime(patient.triage_start_time, patient.arrival_time);
        pthread_mutex_unlock(&stats->mutex);
        
        // Send message to doctors
        PatientMessage msg;
        msg.mtype = patient.priority; 
        msg.patient = patient;
        
        if (msgsnd(msq_id, &msg, sizeof(Patient), 0) == -1) {
            perror("msgsnd failed");
        }
        
        log_event("Triage thread %d processed patient\n", thread_id);
        
        sleep(1); 
    }
    
    return NULL;
}

void doctor_process(int doctor_id) {
    time_t shift_start = time(NULL);
    
    while (running) {
        // Check if shift ended
        if (difftime(time(NULL), shift_start) > config.SHIFT_LENGTH) {
            log_event("Doctor %d shift ended\n", doctor_id);
            break;
        }
        
        // Receive patient from message queue
        PatientMessage msg;
        if (msgrcv(msq_id, &msg, sizeof(Patient), -4, IPC_NOWAIT) != -1) {
            msg.patient.attendance_start_time = time(NULL); // Priority( 1 - HIGH, 4 - LOW)
            
            sleep(1); // Simulation to be substituted with actual handling in the future
            
            msg.patient.attendance_end_time = time(NULL);
            
            // Update statistics
            pthread_mutex_lock(&stats->mutex);
            stats->total_patients_attended++;
            stats->total_wait_after_triage += difftime(msg.patient.attendance_start_time, msg.patient.triage_end_time);
            stats->total_time_in_system += difftime(msg.patient.attendance_end_time, msg.patient.arrival_time);
            pthread_mutex_unlock(&stats->mutex);
            
            log_event("Doctor %d attended patient with priority %ld\n", 
                     doctor_id, msg.mtype);
        }
        
        sleep(1); // Prevent busy waiting
    }
}

void sigint(int sig) {
    (void)sig; // Suppress unused parameter warning
    printf("\nReceived SIGINT - Starting graceful shutdown...\n");
    running = 0;
    
    // Wait for threads to finish
    for (int i = 0; i < config.TRIAGE; i++) {
        pthread_join(triage_threads[i], NULL);
    }
    
    printf("Shutdown complete\n");
}

void sigusr1(int sig) {
    (void)sig; // Suppress unused parameter warning
    printf("\n=== STATISTICS ===\n");
    pthread_mutex_lock(&stats->mutex);
    
    printf("Total patients triaged: %d\n", stats->total_patients_triaged);
    printf("Total patients attended: %d\n", stats->total_patients_attended);
    
    if (stats->total_patients_triaged > 0) {
        printf("Average wait before triage: %.2f seconds\n", stats->total_wait_before_triage / stats->total_patients_triaged);
    }
    
    if (stats->total_patients_attended > 0) {
        printf("Average wait after triage: %.2f seconds\n", stats->total_wait_after_triage / stats->total_patients_attended);
        printf("Average total time in system: %.2f seconds\n", stats->total_time_in_system / stats->total_patients_attended);
    }
    
    pthread_mutex_unlock(&stats->mutex);
    printf("=============================\n");
}

void log_event(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    
    //TODO: Write to mmp file
    va_end(args);
}