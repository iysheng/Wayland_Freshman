/////////////////////
// \author JackeyLea
// \date 
// \note 以关系内存方式显示一个空白窗口
/////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

/*  */
static struct wl_display *display = NULL;

/* 窗口合成器 */
static struct wl_compositor *compositor = NULL;

struct wl_surface *surface;
struct wl_shell *shell;
struct wl_shell_surface *shell_surface;
struct wl_shm *shm;
struct wl_buffer *buffer;

void *shm_data;

int WIDTH = 480;
int HEIGHT = 360;

#define BIND_WL_REG(registry, ptr, id, intf, n) \
	do                                          \
	{                                           \
		(ptr) = (typeof(ptr))wl_registry_bind(  \
			registry, id, intf, n);             \
	} while (0)

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
create_tmpfile_cloexec(char *tmpname)
{
        int fd;

#ifdef HAVE_MKOSTEMP
        fd = mkostemp(tmpname, O_CLOEXEC);
        if (fd >= 0)
                unlink(tmpname);
#else
        /* 根据 template 创建一个唯一的临时文件，并打开这个文件返回这个文件的 fd 句柄 */
        fd = mkstemp(tmpname);
        if (fd >= 0) {
                fd = set_cloexec_or_close(fd);
                unlink(tmpname);
        }
#endif
        return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.

 * 创建一个给定大小的新的、唯一的、匿名的文件，
 * 并为其返回文件描述符。文件描述符被设置为cloexec。
 * 该文件立即适合于mmap()的给定大小的偏移量为零。
 * 
 * 该文件不应该像磁盘那样有永久备份存储，
 * 但如果XDG_RUNTIME_DIR在操作系统中没有正确实现，可能有。
 * 
 * 该文件名将从文件系统中删除。
 * 
 * 该文件适用于通过使用SCM_RIGHTS方法通过Unix套接字传输文件描述符来实现进程之间的缓冲区共享。
 */
int os_create_anonymous_file(off_t size)
{
	static const char template[] = "/weston-shared-XXXXXX";
	const char *path;
	char *name;
	int fd;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path)
	{
		errno = ENOENT;
		return -1;
	}

	name = malloc(strlen(path) + sizeof(template));
	if (!name)
		return -1;

	/* 创建这个名字模板 */
	strcpy(name, path);
	strcat(name, template);
	printf("%s\n", name);

	fd = create_tmpfile_cloexec(name);

	free(name);

	if (fd < 0)
		return -1;

	/* 截断文件到指定大小 */
	if (ftruncate(fd, size) < 0)
	{
		close(fd);
		return -1;
	}

	return fd;//其中：fd是临时文件，大小为size，用于mmap用。
}

// void buffer_release(void *data, struct wl_buffer *buffer)
// {
// 	LPPAINTBUFFER lpBuffer = data;
// 	lpBuffer->busy = 0;
// }

// static const struct wl_buffer_listener buffer_listener = 
// {
// 	.release = buffer_release
// };

static struct wl_buffer *
create_buffer()
{
	struct wl_shm_pool *pool;
	int stride = WIDTH * 4; // 4 bytes per pixel
	int size = stride * HEIGHT;
	int fd;
	struct wl_buffer *buff;

	fd = os_create_anonymous_file(size);
	if (fd < 0)
	{
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
			size);
		exit(1);
	}

	/* 将指定的文件映射到内存 */
	shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm_data == MAP_FAILED)
	{
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		exit(1);
	}

	/* 创建一个 wl_shm_pool 对象，这个对象可以用来创建依托共享内存的缓冲区对象
	 * 服务器将 mmap 指定文件的描述符以及指定大小的空间，用作池的后背缓冲区？？
	 * */
	pool = wl_shm_create_pool(shm, fd, size);
	/* 冲池子中创建一个 buffer 缓冲区 */
	buff = wl_shm_pool_create_buffer(pool, 0,
									 WIDTH, HEIGHT,
									 stride,
									 WL_SHM_FORMAT_XRGB8888);
	//wl_buffer_add_listener(buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	return buff;
}

static void
create_window()
{
	/* 创建了后背缓冲区？？ */
	buffer = create_buffer();

	/* 将指定的内存关联到 surface
	 * surface 的大小会根据 wl_buffer 的内容重新计算
	 * */
	wl_surface_attach(surface, buffer, 0, 0);
	//wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
	/* 提交内容到 surface */
	wl_surface_commit(surface);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	//struct display *d = data;
	//	d->formats |= (1 << format);
	fprintf(stderr, "Format %d\n", format);
}

struct wl_shm_listener shm_listener = {
	.format = shm_format};

static void
global_registry_handler(void *data, struct wl_registry *registry,
						uint32_t id, const char *interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0)
	{
		/* 绑定到这个 compositor, 即获取窗口合成器指针到 compositor */
		BIND_WL_REG(registry, compositor, id, &wl_compositor_interface, 1);
	}
	else if (strcmp(interface, "wl_shell") == 0)
	{
		/* 绑定到全局变量 shell, 支持窗口操作功能 */
		BIND_WL_REG(registry, shell, id, &wl_shell_interface, 1);
	}
	else if (strcmp(interface, "wl_shm") == 0)
	{
		/* 绑定全局变量 shm, 内存管理器 */
		BIND_WL_REG(registry, shm, id, &wl_shm_interface, 1);
		wl_shm_add_listener(shm, &shm_listener, NULL);
	}
}

static void
global_registry_remover(void *data, struct wl_registry *registry,
						uint32_t id)
{
	printf("Got a registry losing event for %d\n", id);
}

static const struct wl_registry_listener registry_listener = {
	/* 服务器发送给客户端通知有新的 object */
	global_registry_handler,
	/* 服务器发送给客户端通知有 object 无效 */
	global_registry_remover,
};

/* 绘制图形 */
static void
paint_pixels() {
    int n;
    uint32_t *pixel = shm_data;

    fprintf(stderr, "Painting pixels\n");
	/* 绘制内容到显示缓冲区 */
    for (n =0; n < WIDTH*HEIGHT; n++) {
    	*pixel++ = 0xff0000;//红色
    }
}

int main(int argc, char **argv)
{
	display = wl_display_connect(NULL);
	if (display == NULL)
	{
		fprintf(stderr, "Can't connect to display\n");
		exit(1);
	}
	printf("connected to display\n");

	/* 全局对象注册表，全局对象需要使用该表获取 */
	struct wl_registry *registry = wl_display_get_registry(display);

	wl_registry_add_listener(registry, &registry_listener, NULL);

	/* 处理接收到的 events */
	wl_display_dispatch(display);
	/* 阻塞直到 client 所有的 request 都被 server 处理 */
	wl_display_roundtrip(display);

	if (compositor == NULL)
	{
		fprintf(stderr, "Can't find compositor\n");
		exit(1);
	}
	else
	{
		fprintf(stderr, "Found compositor\n");
	}

	/* 根据 compositor 创建一个 surface */
	surface = wl_compositor_create_surface(compositor);
	if (surface == NULL)
	{
		fprintf(stderr, "Can't create surface\n");
		exit(1);
	}
	else
	{
		fprintf(stderr, "Created surface\n");
	}

	/* 获取这个 shell surface */
	shell_surface = wl_shell_get_shell_surface(shell, surface);
	if (shell_surface == NULL)
	{
		fprintf(stderr, "Can't create shell surface\n");
		exit(1);
	}
	else
	{
		fprintf(stderr, "Created shell surface\n");
	}
	wl_shell_surface_set_toplevel(shell_surface);
	//wl_shell_suface_add_listener(shell_surface,&shell_surface_listener,NULL);

	create_window();
	paint_pixels();

	while(wl_display_dispatch(display)!=-1){
		;
	}

	wl_display_disconnect(display);
	printf("disconnected from display\n");

	exit(0);
}
