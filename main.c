#define _GNU_SOURCE
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
#include <errno.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/types.h>

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

// Triage Queue Structure
typedef struct {
    Patient *patients;
    int count;
    int in;
    int out;
    pthread_mutex_t mutex;
    pthread_cond_t can_produce;
    pthread_cond_t can_consume;
} TriageQueue;


Config config;
int shm_id;
Statistics *stats;
int msq_id;
pthread_t *triage_threads;
pid_t *doctor_pids; // Array to track doctor PIDs
int running = 1;
TriageQueue triage_queue;
char PIPE_NAME[] = "input_pipe";

// Logging globals
char *log_addr = NULL;
int log_fd = -1;
size_t log_size = 1024 * 1024 * 2; // 2MB
size_t log_offset = 0;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Signal flags
volatile sig_atomic_t child_terminated = 0;
volatile sig_atomic_t need_temp_doctor = 0;

void read_config(const char *filename);
void setup(); 
void cleanup(); 
void init_triage_queue();
void cleanup_triage_queue();
void *triage_thread(void *arg);
void doctor_process(int doctor_id, int is_temp);
void sigint(int sig);
void doctor_sigint(int sig);
void sigchld_handler(int sig); 
void sigusr1(int sig);
void sigusr2_handler(int sig); 
void log_event(const char *format, ...);
void spawn_doctor(int id, int is_temp); 

void prepare_log_fork() { pthread_mutex_lock(&log_mutex); }
void parent_log_fork() { pthread_mutex_unlock(&log_mutex); }
void child_log_fork() { pthread_mutex_unlock(&log_mutex); }

void setup() {
    // Initialize Triage Queue
    init_triage_queue();
    
    // Register atfork handlers for log_mutex
    pthread_atfork(prepare_log_fork, parent_log_fork, child_log_fork);

    // Setup MMF for logging
    log_fd = open("DEI_Emergency.log", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (log_fd == -1) {
        perror("Error opening log file");
        exit(1);
    }
    if (ftruncate(log_fd, log_size) == -1) {
        perror("Error truncating log file");
        exit(1);
    }
    log_addr = mmap(NULL, log_size, PROT_READ | PROT_WRITE, MAP_SHARED, log_fd, 0);
    if (log_addr == MAP_FAILED) {
        perror("Error mmapping log file");
        exit(1);
    }

    // Create Named Pipe
    if (mkfifo(PIPE_NAME, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo failed");
            exit(1);
        }
    }

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
    
    // Initialize mutex with PTHREAD_PROCESS_SHARED
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&stats->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Create message queue
    key_t msq_key = ftok("/tmp", 'M');
    msq_id = msgget(msq_key, IPC_CREAT | 0666);
    if (msq_id == -1) {
        perror("msgget failed");
        exit(1);
    }
    
    // Create doctor processes array
    doctor_pids = malloc(config.DOCTORS * sizeof(pid_t));
    if (!doctor_pids) {
        perror("malloc failed");
        exit(1);
    }
    
    printf("IPC mechanisms setup complete\n");
}

int main() {
    setbuf(stdout, NULL); // Disable stdout buffering
    printf("=== DEI Emergency System Starting ===\n");
    
    // Read config.txt
    read_config("config.txt");
    
    setup();

    signal(SIGINT, sigint);
    struct sigaction sa_usr1;
    sa_usr1.sa_handler = sigusr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = SA_RESTART; // Restart syscalls for stats
    sigaction(SIGUSR1, &sa_usr1, NULL);

    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = 0; // No SA_RESTART to interrupt fgets
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_usr2;
    sa_usr2.sa_handler = sigusr2_handler;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, NULL);
    
    // Create triage threads
    triage_threads = malloc(config.TRIAGE * sizeof(pthread_t));
    for (int i = 0; i < config.TRIAGE; i++) {
        pthread_create(&triage_threads[i], NULL, triage_thread, (void *)(long)i);
        log_event("Triage thread %d created\n", i);
    }
    
    // Create doctor processes
    for (int i = 0; i < config.DOCTORS; i++) {
        spawn_doctor(i, 0);
    }
    
    printf("System running. Waiting for patients on pipe '%s'...\n", PIPE_NAME);
    
    int fd = open(PIPE_NAME, O_RDWR);
    if (fd == -1) {
        perror("Cannot open pipe");
        cleanup();
        return 1;
    }
    
    FILE *pipe_fp = fdopen(fd, "r");
    char buffer[256];
    
    while (running) {
        if (fgets(buffer, sizeof(buffer), pipe_fp) == NULL) {
            if (errno == EINTR) {
                if (child_terminated) {
                    child_terminated = 0;
                    int status;
                    pid_t pid;
                    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                        for (int i = 0; i < config.DOCTORS; i++) {
                            if (doctor_pids[i] == pid) {
                                log_event("Doctor %d (PID %d) exited. Respawning...\n", i, pid);
                                spawn_doctor(i, 0);
                                break;
                            }
                        }
                    }
                    clearerr(pipe_fp);
                }
                if (need_temp_doctor) {
                    need_temp_doctor = 0;
                    log_event("High load detected. Spawning temporary doctor.\n");
                    spawn_doctor(config.DOCTORS, 1);
                }
                continue;
            }
            break;
        }
        
        // Remove newline
        buffer[strcspn(buffer, "\n")] = 0;
        
        if (strlen(buffer) == 0) continue;
        
        if (strncmp(buffer, "TRIAGE=", 7) == 0) {
            int new_triage = atoi(buffer + 7);
            if (new_triage > 0) {
                log_event("Command received: Change TRIAGE from %d to %d\n", config.TRIAGE, new_triage);
                
                if (new_triage > config.TRIAGE) {
                    // Scale UP
                    pthread_t *new_threads = realloc(triage_threads, new_triage * sizeof(pthread_t));
                    if (new_threads) {
                        triage_threads = new_threads;
                        for (int i = config.TRIAGE; i < new_triage; i++) {
                            pthread_create(&triage_threads[i], NULL, triage_thread, (void *)(long)i);
                            log_event("Triage thread %d created\n", i);
                        }
                        config.TRIAGE = new_triage;
                    } else {
                        perror("realloc failed");
                    }
                } else if (new_triage < config.TRIAGE) {
                    // Scale DOWN
                    config.TRIAGE = new_triage;
                    // Wake up threads so they can check the new limit and exit
                    pthread_mutex_lock(&triage_queue.mutex);
                    pthread_cond_broadcast(&triage_queue.can_consume);
                    pthread_mutex_unlock(&triage_queue.mutex);
                }
            }
        } else {
            // Parse Patient: Name TriageTime AttendanceTime Priority
            Patient p;
            memset(&p, 0, sizeof(p)); // Zero out struct
            p.arrival_time = time(NULL);
            
            char name[100];
            int t_time, a_time, prio;
            
            // Try parsing 4 items
            if (sscanf(buffer, "%s %d %d %d", name, &t_time, &a_time, &prio) == 4) {
                strncpy(p.name, name, sizeof(p.name)-1);
                p.name[sizeof(p.name)-1] = '\0'; // Ensure null termination
                p.triage_time = t_time;
                p.attendance_time = a_time;
                p.priority = prio;
                
                // Add to queue
                pthread_mutex_lock(&triage_queue.mutex);
                if (triage_queue.count >= config.TRIAGE_QUEUE_MAX) {
                    log_event("Queue full! Dropping patient %s\n", p.name);
                    pthread_mutex_unlock(&triage_queue.mutex);
                } else {
                    triage_queue.patients[triage_queue.in] = p;
                    triage_queue.in = (triage_queue.in + 1) % config.TRIAGE_QUEUE_MAX;
                    triage_queue.count++;
                    pthread_cond_signal(&triage_queue.can_consume);
                    pthread_mutex_unlock(&triage_queue.mutex);
                    log_event("Patient %s arrived\n", p.name);
                }
            } else {
                log_event("Invalid input format: %s\n", buffer);
            }
        }
    }
    
    fclose(pipe_fp);
    
    // Graceful Shutdown Logic
    printf("\nStarting graceful shutdown...\n");
    
    // 1. Wake up triage threads
    pthread_mutex_lock(&triage_queue.mutex);
    pthread_cond_broadcast(&triage_queue.can_consume);
    pthread_mutex_unlock(&triage_queue.mutex);
    
    // 2. Join triage threads
    for (int i = 0; i < config.TRIAGE; i++) {
        pthread_join(triage_threads[i], NULL);
    }
    printf("Triage threads finished.\n");
    
    // 3. Wait for doctors
    // Doctors received SIGINT (via group) and should be draining.
    // We just wait for them.
    while (wait(NULL) > 0);
    printf("Doctors finished.\n");
    
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

void init_triage_queue() {
    triage_queue.patients = malloc(sizeof(Patient) * config.TRIAGE_QUEUE_MAX);
    triage_queue.count = 0;
    triage_queue.in = 0;
    triage_queue.out = 0;
    pthread_mutex_init(&triage_queue.mutex, NULL);
    pthread_cond_init(&triage_queue.can_produce, NULL);
    pthread_cond_init(&triage_queue.can_consume, NULL);
}

void cleanup_triage_queue() {
    free(triage_queue.patients);
    pthread_mutex_destroy(&triage_queue.mutex);
    pthread_cond_destroy(&triage_queue.can_produce);
    pthread_cond_destroy(&triage_queue.can_consume);
}



void cleanup() {
    // Clean shared memory
    shmdt(stats);
    shmctl(shm_id, IPC_RMID, NULL);
    
    // Clean message queue
    msgctl(msq_id, IPC_RMID, NULL);
    
    // Free threads
    free(triage_threads);
    free(doctor_pids); // Free PID array
    
    // Cleanup Queue and Pipe
    cleanup_triage_queue();
    unlink(PIPE_NAME);

    // Cleanup MMF
    if (log_addr != MAP_FAILED) munmap(log_addr, log_size);
    if (log_fd != -1) close(log_fd);

    printf("IPC cleanup complete\n");
}

void *triage_thread(void *arg) {
    int thread_id = (int)(long)arg;
    
    while (running) {
        // Check if I should still exist (Dynamic scaling)
        if (thread_id >= config.TRIAGE) {
            log_event("Triage thread %d stopping due to scale down\n", thread_id);
            break;
        }

        Patient patient;
        
        // Get patient from queue
        pthread_mutex_lock(&triage_queue.mutex);
        while (triage_queue.count == 0 && running && thread_id < config.TRIAGE) {
            pthread_cond_wait(&triage_queue.can_consume, &triage_queue.mutex);
        }
        
        if (!running || thread_id >= config.TRIAGE) {
            pthread_mutex_unlock(&triage_queue.mutex);
            break;
        }

        patient = triage_queue.patients[triage_queue.out];
        triage_queue.out = (triage_queue.out + 1) % config.TRIAGE_QUEUE_MAX;
        triage_queue.count--;
        pthread_cond_signal(&triage_queue.can_produce);
        pthread_mutex_unlock(&triage_queue.mutex);

        log_event("Triage thread %d started triaging patient %s\n", thread_id, patient.name);
        
        patient.triage_start_time = time(NULL);
        
        // Simulate triage time
        if (patient.triage_time > 0) sleep(patient.triage_time);
        
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
        } else {
            // Check load
            struct msqid_ds buf;
            if (msgctl(msq_id, IPC_STAT, &buf) != -1) {
                if (buf.msg_qnum > (unsigned long)config.MSQ_WAIT_MAX) {
                    kill(getpid(), SIGUSR2);
                }
            }
        }
        
        log_event("Triage thread %d finished patient %s\n", thread_id, patient.name);
    }
    
    return NULL;
}

void spawn_doctor(int id, int is_temp) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        doctor_process(id, is_temp);
        exit(0);
    } else if (pid > 0) {
        if (!is_temp && id < config.DOCTORS) {
            doctor_pids[id] = pid;
        }
        log_event("%s Doctor process %d created with PID: %d\n", is_temp ? "Temp" : "Regular", id, pid);
    } else {
        perror("fork failed");
    }
}

void doctor_process(int doctor_id, int is_temp) {
    // Set specific signal handler for doctor
    signal(SIGINT, doctor_sigint);
    signal(SIGUSR1, SIG_IGN); // Ignore stats signal
    
    time_t shift_start = time(NULL);
    
    while (running) {
        // Check if shift ended
        if (difftime(time(NULL), shift_start) > config.SHIFT_LENGTH) {
            log_event("Doctor %d shift ended\n", doctor_id);
            break;
        }
        
        if (is_temp) {
            struct msqid_ds buf;
            if (msgctl(msq_id, IPC_STAT, &buf) != -1) {
                if (buf.msg_qnum < 0.8 * config.MSQ_WAIT_MAX) {
                    log_event("Temp Doctor %d finishing due to low load\n", doctor_id);
                    break;
                }
            }
        }
        
        // Receive patient from message queue
        PatientMessage msg;
        if (msgrcv(msq_id, &msg, sizeof(Patient), -4, IPC_NOWAIT) != -1) {
            msg.patient.attendance_start_time = time(NULL); // Priority( 1 - HIGH, 4 - LOW)
            
            // Simulate attendance time
            if (msg.patient.attendance_time > 0) sleep(msg.patient.attendance_time);
            
            msg.patient.attendance_end_time = time(NULL);
            
            // Update statistics
            pthread_mutex_lock(&stats->mutex);
            stats->total_patients_attended++;
            stats->total_wait_after_triage += difftime(msg.patient.attendance_start_time, msg.patient.triage_end_time);
            stats->total_time_in_system += difftime(msg.patient.attendance_end_time, msg.patient.arrival_time);
            pthread_mutex_unlock(&stats->mutex);
            
            log_event("Doctor %d attended patient %s (Priority %d)\n", 
                     doctor_id, msg.patient.name, msg.mtype);
        } else {
            if (errno != ENOMSG) {
                perror("msgrcv failed");
            }
        }
        
        sleep(1); // Prevent busy waiting
    }
    
    // Drain queue (Graceful shutdown)
    if (!is_temp) { // Temp doctors just exit
        log_event("Doctor %d draining queue...\n", doctor_id);
        PatientMessage msg;
        while (msgrcv(msq_id, &msg, sizeof(Patient), -4, IPC_NOWAIT) != -1) {
             msg.patient.attendance_start_time = time(NULL);
             if (msg.patient.attendance_time > 0) sleep(msg.patient.attendance_time);
             msg.patient.attendance_end_time = time(NULL);
             
             pthread_mutex_lock(&stats->mutex);
             stats->total_patients_attended++;
             stats->total_wait_after_triage += difftime(msg.patient.attendance_start_time, msg.patient.triage_end_time);
             stats->total_time_in_system += difftime(msg.patient.attendance_end_time, msg.patient.arrival_time);
             pthread_mutex_unlock(&stats->mutex);
             
             log_event("Doctor %d attended patient %s (Drain)\n", doctor_id, msg.patient.name);
        }
    }
    log_event("Doctor %d exiting\n", doctor_id);
}

void sigint(int sig) {
    (void)sig;
    running = 0;
    // The main loop will exit when fgets is interrupted by the signal (EINTR)
}

void doctor_sigint(int sig) {
    (void)sig;
    running = 0;
}

void sigchld_handler(int sig) {
    (void)sig;
    child_terminated = 1;
}

void sigusr2_handler(int sig) {
    (void)sig;
    need_temp_doctor = 1;
}

void sigusr1(int sig) {
    (void)sig;
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
    
    // Print to console
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    // Write to MMF
    pthread_mutex_lock(&log_mutex);
    if (log_addr && log_offset < log_size) {
        va_start(args, format);
        // Get current time
        time_t now = time(NULL);
        char time_str[26];
        ctime_r(&now, time_str);
        time_str[24] = '\0'; // Remove newline
        
        // Format: [Time] Message
        int written = snprintf(log_addr + log_offset, log_size - log_offset, "[%s] ", time_str);
        if (written > 0) log_offset += written;
        
        written = vsnprintf(log_addr + log_offset, log_size - log_offset, format, args);
        if (written > 0) log_offset += written;
        
        va_end(args);
    }
    pthread_mutex_unlock(&log_mutex);
}