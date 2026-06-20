#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <unistd.h>
#include <string.h>

#define STACK_SIZE (1024 * 1024)

int sync_pipe[2];

void write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[Host] FATAL: Failed to open file: %s\n", path);
        perror("[Host] Reason");
        exit(1);
    }
    if (fprintf(fp, "%s", value) < 0) {
        fprintf(stderr, "[Host] FATAL: Failed to write to file: %s\n", path);
        perror("[Host] Reason");
        fclose(fp);
        exit(1);
    }
    fclose(fp);
}

int container_main(void *arg) {
    char ch;
    // Close the write end of the synchronization wire
    close(sync_pipe[1]); 

    // Sleep until the parent process completes host-side plumbing
    if (read(sync_pipe[0], &ch, 1) != 1) {
        perror("[Container] Failed to sync with parent");
        return 1;
    }
    close(sync_pipe[0]);

    printf("[Container] Inside the container namespaces!\n");
    mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
    sethostname("c-container-demo", 16);

    
    if (chroot("./rootfs") != 0) {
        perror("[Container] chroot failed");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("[Container] chdir failed");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("[Container] Mounting /proc failed");
    }

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching native Ubuntu Bash shell...\n\n");
    execvp(container_args[0], container_args);

    perror("[Container] execvp failed");
    return 1;
}

int main() {
    if (pipe(sync_pipe) < 0) {
        perror("Pipe creation failed");
        exit(1);
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { 
        perror("Stack allocation failed");
        exit(1); 
    }

    printf("[Host] Cloning process with Mount, PID, User, and UTS isolation...\n");

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWUSER | SIGCHLD, 
        NULL
    );

    if (container_pid == -1) {
        perror("[Host] Clone failed");
        free(stack);
        exit(1);
    }

    // Parent closes read end of the pipe immediately
    close(sync_pipe[0]); 

    const char *cgroup_dir = "/sys/fs/cgroup/my_container";
    mkdir(cgroup_dir, 0755); 

    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "%s/memory.max", cgroup_dir);
    write_file(path_buf, "50M");

    snprintf(path_buf, sizeof(path_buf), "%s/pids.max", cgroup_dir);
    write_file(path_buf, "4");

    snprintf(path_buf, sizeof(path_buf), "%s/cgroup.procs", cgroup_dir);
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    write_file(path_buf, pid_str);

    /* --- USER NAMESPACE IDENTIFICATION SETTINGS --- */
    // Map identity settings to run as true root within the user namespace
    int target_uid = 0; 
    int target_gid = 0;

    printf("[Host] Mapping User Namespaces (Container UID 0 -> Host UID %d)...\n", target_uid);

    char map_path[256];
    char map_str[256];

    // *****************************************************************************************
    // Ques: try running whoami command 
    // with and without this code block to understand what is happening
    snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_uid);
    write_file(map_path, map_str);

    // Disable global setgroups context manipulation privileges
    snprintf(map_path, sizeof(map_path), "/proc/%d/setgroups", container_pid);
    if (access(map_path, F_OK) == 0) {
        write_file(map_path, "deny\n");
    }

    // Write the explicit container gid mapping parameters
    snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_gid);
    write_file(map_path, map_str);
    // *****************************************************************************************


    // Wake up the child process now that the host plumbing is complete
    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);

    if (rmdir(cgroup_dir) != 0) {
        perror("[Host] Warning: Failed to clean up cgroup directory");
    }
    free(stack);
    printf("[Host] Container has closed smoothly.\n");
    return 0;
}
