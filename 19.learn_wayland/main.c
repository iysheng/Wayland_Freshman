#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>

struct my_output {
    struct wl_compositor * compositor;
    struct wl_shm * shm;
};


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
#define NAME_TEMPLATE    "/wl_shm-XXXXXX"
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
	//fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
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
    const int width = 100, height = 100;
    const int stride = width * 4;
    const int shm_pool_size = height * stride * 2;
    
    struct wl_shm_pool *pool;
    int fd = allocate_shm_file(shm_pool_size);
    uint8_t *pool_data = mmap(NULL, shm_pool_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	int index = 0;
    int offset = height * stride * index;
    uint32_t *pixels = (uint32_t *)&pool_data[offset];
    struct wl_buffer *buffer;

	wl_registry_add_listener(registry, &registry_listener, &state);

	wl_display_roundtrip(display);

	if (state.compositor)
	{
		surface = wl_compositor_create_surface(state.compositor);
	}

	if (state.shm)
	{
        pool = wl_shm_create_pool(state.shm, fd, shm_pool_size);
        buffer = wl_shm_pool_create_buffer(pool, offset,
            width, height, stride, WL_SHM_FORMAT_XRGB8888);
		for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8) {
              pixels[y * width + x] = 0xFF666666;
            } else {
              pixels[y * width + x] = 0xFFEEEEEE;
            }
          }
        }
    }
	if (!surface)
	{
		printf("invalid surface\n");
		return -2;
	}

	wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, UINT32_MAX, UINT32_MAX);
    wl_surface_commit(surface);
	printf("buffer commit is done\n");

	/* 处理接收到的 events */
	while (-1 != wl_display_dispatch(display))
	{

	}

	return 0;
}

