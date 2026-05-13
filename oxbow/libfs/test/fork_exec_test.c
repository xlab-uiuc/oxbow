#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc, char *argv[])
{
	pid_t pid;
	int status;

	printf("Parent process (PID: %d) starting...\n", getpid());

	pid = fork();
	if (pid < 0) {
		// Fork failed
		perror("fork");
		exit(1);
	} else if (pid == 0) {
		// Child process
		printf("Child process (PID: %d) executing ls...\n", getpid());

		char *args[] = { "ls", "-l", NULL };

		sleep(5);
		execvp("ls", args);

		// If execvp returns, it means it failed
		perror("execvp");
		exit(1);
	} else {
		// Parent process
		printf("Parent process waiting for child (PID: %d)...\n", pid);
		wait(&status);

		if (WIFEXITED(status)) {
			printf("Child process exited with status: %d\n",
			       WEXITSTATUS(status));
		} else {
			printf("Child process did not exit normally\n");
		}
	}

	return 0;
}