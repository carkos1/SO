#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PIPE_NAME "input_pipe"

int main() {
    printf("=== Hospital Client ===\n");
    printf("Connecting to %s...\n", PIPE_NAME);

    // Open pipe for writing
    // We use O_WRONLY. The server must be running and have the pipe open.
    int fd = open(PIPE_NAME, O_WRONLY);
    if (fd == -1) {
        perror("Error opening pipe. Make sure ./dei_emergency is running");
        exit(1);
    }

    printf("Connected!\n");
    printf("Enter patient details: Name TriageTime AttendanceTime Priority\n");
    printf("Example: John 2 5 1\n");
    printf("Commands: TRIAGE=X (to scale triage threads)\n");
    printf("Type 'quit' to exit.\n\n");

    char buffer[256];
    while (1) {
        printf("> ");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;

        // Remove newline to check for commands
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "quit") == 0) break;
        if (strlen(buffer) == 0) continue;

        // Add newline back because the server uses fgets()
        strcat(buffer, "\n");

        if (write(fd, buffer, strlen(buffer)) == -1) {
            perror("Write failed");
            break;
        }
    }

    close(fd);
    printf("Client exiting.\n");
    return 0;
}
