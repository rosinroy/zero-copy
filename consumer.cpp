#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <string>
#include <cstdio>

const char *SOCK_PATH = "/tmp/dmabuf_socket";

// Receive a file descriptor over Unix socket
int recv_fd(int sock)
{
    struct msghdr msg = {};
    char buf[1];
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    if (recvmsg(sock, &msg, 0) <= 0) {
        perror("recvmsg");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) {
        std::cerr << "No control message received\n";
        return -1;
    }

    int fd = *(int *)CMSG_DATA(cmsg);
    return fd;
}

int main()
{
    // ---- IMPORTANT: RGBA CONFIG ----
    const int WIDTH = 2560;
    const int HEIGHT = 1440;
    const int PITCH = WIDTH * 4;          // ✅ 4 bytes per pixel (R,G,B,A)
    const size_t FRAME_SIZE = PITCH * HEIGHT;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    std::cout << "Connecting to producer..." << std::endl;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }
    std::cout << "Connected!\n";

    int frame = 0;

    while (true) {
        int fd = recv_fd(sock);
        if (fd < 0) {
            std::cerr << "Producer disconnected or error\n";
            break;
        }

        // Map the full RGBA frame
        void *addr = mmap(nullptr, FRAME_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            perror("mmap");
            close(fd);
            continue;
        }

        std::string filename = "frame_" + std::to_string(frame) + ".rgba";
        FILE *f = fopen(filename.c_str(), "wb");

        if (!f) {
            perror("fopen");
            munmap(addr, FRAME_SIZE);
            close(fd);
            continue;
        }

        unsigned char *base = (unsigned char*)addr;

        // Write row by row respecting pitch
        for (int i = 0; i < HEIGHT; i++) {
            fwrite(base + i * PITCH, 1, WIDTH * 4, f);  // ✅ 4 bytes per pixel
        }

        fclose(f);
        munmap(addr, FRAME_SIZE);
        close(fd);

        std::cout << "Dumped: " << filename << std::endl;
        frame++;
    }

    close(sock);
    return 0;
}
