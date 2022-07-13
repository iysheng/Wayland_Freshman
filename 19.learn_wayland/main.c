#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include "xdg-shell-client-protocol.h"

struct my_output {
    struct wl_compositor * compositor;
    struct wl_shm * shm;
	struct xdg_wm_base *xdg_wm_base;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_surface *wl_surface;
	struct wl_buffer *wl_buffer;
    float offset;
    uint32_t last_frame;
};

static int
set_cloexec_or_close(int fd)
{
        long flags;

        if (fd == -1)
                return -1;

        flags = fcntl(fd, F_GETFD);
        if (flags == -1)
                goto err;

		/* 设置自动关闭这个 fd 的 flags */
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
                goto err;

        return fd;

err:
        close(fd);
        return -1;
}

static int
create_shm_file(void)
{
#define NAME_TEMPLATE    "/wl_s1-XXXXXX"
	char name[64] = {0};
	const char *path;
	int fd;

	path = getenv("XDG_RUNTIME_DIR");
	if (path)
	{
		strcpy(name, path);
	}
	strcat(name, NAME_TEMPLATE);
	printf("name=%s\n", name);

	/* 根据模板创建临时文件句柄 */
	fd = mkstemp(name);
    if (fd >= 0) {
        fd = set_cloexec_or_close(fd);
        unlink(name);
		return fd;
    }

	return -1;
}

int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	int ret;
	if (fd < 0)
		return -1;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer* draw_frame(struct my_output *state)
{
	const int width = 100, height = 100;
    int stride = width * 4;
    int size = stride * height;
    int offset = state->offset;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Draw checkerboxed background */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + offset + y + offset / 8 * 8) % 16 < 8)
                data[y * width + x] = 0xFF666666;
            else
                data[y * width + x] = 0xFFEEEEEE;
        }
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

void test_format(void *data,
	       struct wl_shm *wl_shm,
	       uint32_t format)
{
	static int i = 0;
	printf("format[%d]=%x\n", i++, format);
}

struct wl_shm_listener shm_listener = {
    .format = test_format,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct my_output *state = (struct my_output *)data;
	printf("interface: '%s', version: %d, name: %d\n",
			interface, version, name);
	if (!strcmp(interface, wl_compositor_interface.name))
	{
		/* 绑定混合器 */
		state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
		printf("绑定混合器\n");
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(
            registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(state->shm, &shm_listener, NULL);
		printf("绑定内存管理器\n");
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);
		printf("绑定 xdg_wm_base\n");
    }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name)
{
	// This space deliberately left blank
}

static const struct wl_registry_listener
registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void
xdg_surface_configure(void *data,
        struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct my_output *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	/* Destroy this callback */
	wl_callback_destroy(cb);

	/* Request another frame */
	struct my_output *state = data;
	cb = wl_surface_frame(state->wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

	/* Update scroll amount at 24 pixels per second */
	if (state->last_frame != 0) {
		int elapsed = time - state->last_frame;
		state->offset += elapsed / 1000.0 * 24;
	}

	/* Submit a frame for this event */
	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(state->wl_surface);

	state->last_frame = time;
}

static const struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display = wl_display_connect(NULL);
    struct my_output state = {0};
	struct wl_surface *surface = NULL;

	if (display)
	{
		printf("Create connection success\n");
	}
	else
	{
		printf("Failed create connection to server\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	wl_display_roundtrip(display);

	if (state.compositor)
	{
		surface = wl_compositor_create_surface(state.compositor);
		state.wl_surface = surface;
		/* TODO create xdg surface */

		/* TODO create xdg toplevel */
	}

	if (state.shm)
	{
    }

	if (!surface)
	{
		printf("invalid surface\n");
		return -2;
	}

	state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "AExample client");

    wl_surface_commit(state.wl_surface);
	printf("buffer commit is done\n");

    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

	/* 处理接收到的 events */
	while (-1 != wl_display_dispatch(display))
	{
#if 0
		sleep(10);
        wl_surface_attach(state.wl_surface, buffer, 0, 0);
		memset(pixels, 0x550000, stride * height);
        wl_surface_commit(state.wl_surface);
		sleep(1);
		printf("show 550000\n");

		memset(pixels, 0x005500, stride * height);
        wl_surface_commit(state.wl_surface);
		sleep(1);
		printf("show 5500\n");

		memset(pixels, 0x000055, stride * height);
        wl_surface_commit(state.wl_surface);
		sleep(1);
		printf("show 55\n");
#endif
	}

	return 0;
}

