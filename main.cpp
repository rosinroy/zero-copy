#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>

#include <nvbufsurface.h>

// ------------------- HELPER FUNCTIONS -------------------

int g_fd_socket = -1;
const char *SOCK_PATH = "/tmp/dmabuf_socket";


static int dup_cloexec(int fd) {
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

static bool try_map_nvbufsurface(GstBuffer* buffer,
                                 NvBufSurface** out_surf,
                                 GstMapInfo* out_map) {
    if (!buffer || !out_surf || !out_map) return false;

    GstMapInfo map = {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return false;
    if (!map.data || map.size < sizeof(NvBufSurface)) {
        gst_buffer_unmap(buffer, &map);
        return false;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(map.data);
    if ((addr % alignof(NvBufSurface)) != 0) {
        gst_buffer_unmap(buffer, &map);
        return false;
    }

    NvBufSurface* surf = reinterpret_cast<NvBufSurface*>(map.data);

    if (surf->batchSize == 0 ||
        surf->numFilled == 0 ||
        surf->numFilled > surf->batchSize) {
        gst_buffer_unmap(buffer, &map);
        return false;
    }

    *out_surf = surf;
    *out_map = map;
    return true;
}

static bool export_frame_fd(GstBuffer* buffer, int batch_id, int* out_fd) {

    NvBufSurface* surf = nullptr;
    GstMapInfo map = {};

    if (!try_map_nvbufsurface(buffer, &surf, &map))
        return false;

    if (batch_id < 0 || (uint32_t)batch_id >= surf->batchSize) {
        gst_buffer_unmap(buffer, &map);
        return false;
    }

    int fd = surf->surfaceList[batch_id].bufferDesc;
    int width  = surf->surfaceList[batch_id].width;
    int height = surf->surfaceList[batch_id].height;
    int pitch  = surf->surfaceList[batch_id].pitch;
    
    std::cout << "FD " << fd << std::endl;
    std::cout << "W H P" << width << " " << height << " " << pitch << std::endl;
    gst_buffer_unmap(buffer, &map);


    if (fd < 0) return false;

    int duped = dup_cloexec(fd);
    if (duped < 0) return false;

    *out_fd = duped;
    std::cout << "out_fd " << *out_fd << std::endl;
    return true;
}

bool send_fd(int sock, int fd)
{
    struct msghdr msg = {};
    struct iovec iov;
    char dummy = 'F';
    iov.iov_base = &dummy;
    iov.iov_len = 1;

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    *(int *)CMSG_DATA(cmsg) = fd;

    if (sendmsg(sock, &msg, 0) < 0) {
        perror("sendmsg");
        return false;
    }
    return true;
}


// ------------------- APPSINK CALLBACK -------------------

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer)
{
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    int dmabuf_fd = -1;

    if (export_frame_fd(buffer, 0, &dmabuf_fd)) {
        std::cout << "Got DMABUF FD = " << dmabuf_fd << std::endl;
        std::cout << "Producer sending FD = " << dmabuf_fd << std::endl;

        send_fd(g_fd_socket, dmabuf_fd);
        close(dmabuf_fd);
    } else {
        std::cerr << "Failed to get dmabuf fd" << std::endl;
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ------------------- MAIN -------------------

int main(int argc, char *argv[])
{

    // Create Unix domain socket for sending FDs
    unlink(SOCK_PATH);

    g_fd_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    bind(g_fd_socket, (struct sockaddr*)&addr, sizeof(addr));
    listen(g_fd_socket, 1);

    std::cout << "Waiting for consumer to connect on "
            << SOCK_PATH << std::endl;

    int client_fd = accept(g_fd_socket, nullptr, nullptr);
    std::cout << "Consumer connected!" << std::endl;

    // store client_fd somewhere global or static
    g_fd_socket = client_fd;   // reuse variable for simplicity


    gst_init(&argc, &argv);

    const char *pipeline_str =
    "rtspsrc location=rtsp://admin:admin123@192.168.134.218:554/1/1 latency=0 ! "
    "rtph264depay ! h264parse ! nvv4l2decoder ! "
    "nvvidconv ! "
    "video/x-raw(memory:NVMM), format=RGBA ! "
    "appsink name=sink emit-signals=true sync=false";

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);

    if (!pipeline) {
        std::cerr << "Pipeline error: "
                  << (error ? error->message : "unknown") << std::endl;
        return -1;
    }

    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    g_signal_connect(appsink, "new-sample",
                     G_CALLBACK(on_new_sample), nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::cout << "Running... Press Ctrl+C to exit" << std::endl;

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);

    return 0;
}
