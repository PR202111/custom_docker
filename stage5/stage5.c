#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>   
#include <unistd.h>

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

    close(sync_pipe[1]); 

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

    /* --- INSIDE NETWORK CONFIGURATION --- */
    // Bring up the loopback interface
    system("ip link set lo up");
    // Configure the injected veth interface end and give it an IP address
    system("ip addr add 10.0.0.2/24 dev veth_child");
    system("ip link set veth_child up");
    // Route all outside traffic through the host gateway interface
    system("ip route add default via 10.0.0.1");
    /* ------------------------------------ */

    char *container_args[] = {"/bin/sh", NULL};
    printf("[Container] Launching Alpine Sh shell with network link active...\n\n");
    execvp(container_args[0], container_args);

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

    printf("[Host] Cloning process with Network, Mount, PID, and UTS isolation...\n");

    int container_pid = clone(
        container_main, 
        stack + STACK_SIZE, 
        CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
        NULL
    );
    
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

    int target_uid = 0; 
    int target_gid = 0;

    printf("[Host] Mapping User Namespaces (Container UID 0 -> Host UID %d)...\n", target_uid);

    char map_path[256];
    char map_str[256];
    snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_uid);
    write_file(map_path, map_str);

    snprintf(map_path, sizeof(map_path), "/proc/%d/setgroups", container_pid);
    if (access(map_path, F_OK) == 0) {
        write_file(map_path, "deny\n");
    }

    snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", container_pid);
    snprintf(map_str, sizeof(map_str), "0 %d 1\n", target_gid);
    write_file(map_path, map_str);

    /* --- STAGE 5: HOST SIDE NETWORK PLUMBING --- */
    printf("[Host] Plumbing virtual ethernet wires into container...\n");
    char cmd[512];

    // 1. Create a virtual ethernet cable pair
    system("ip link add veth_host type veth peer name veth_child");

    // 2. Push the child end of the cable directly into the container's network namespace
    snprintf(cmd, sizeof(cmd), "ip link set veth_child netns %d", container_pid);
    system(cmd);

    // 3. Configure the host side end of the cable
    system("ip addr add 10.0.0.1/24 dev veth_host");
    system("ip link set veth_host up");

    // 4. Enable NAT/IP Forwarding on the host so the container can reach outer web hardware
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE");
    
    // 5. Ensure the etc directory exists inside the rootfs jail and Write the nameserver directly into the jail's path
    mkdir("./rootfs/etc", 0755); 
    write_file("./rootfs/etc/resolv.conf", "nameserver 8.8.8.8\n");
    /* -------------------------------- */

    write(sync_pipe[1], "O", 1);
    close(sync_pipe[1]); 

    waitpid(container_pid, NULL, 0);


    printf("[Host] Tearing down network infrastructures...\n");
    
    // Bring it down first to release kernel locks, then delete it
    system("ip link set veth_host down 2>/dev/null");
    system("ip link del veth_host 2>/dev/null");
    
    system("iptables -t nat -D POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null");

    if (rmdir(cgroup_dir) != 0) {
        perror("[Host] Warning: Failed to clean up cgroup directory");
    }
    
    free(stack);
    printf("[Host] Cleaned up smoothly. Exiting.\n");
    return 0;
}