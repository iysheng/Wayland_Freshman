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
#include "text-input-unstable-v1-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#include <assert.h>

struct my_xkb {
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
};

struct my_output {
    struct wl_compositor * compositor;
    struct wl_shm * shm;
	struct xdg_wm_base *xdg_wm_base;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_surface *wl_surface;
	struct wl_buffer *wl_buffer;
    struct wl_seat *wl_seat;
    struct wl_pointer *wl_pointer;
    struct wl_keyboard *wl_keyboard;
    float offset;
    uint32_t last_frame;
    int32_t width;
    int32_t height;
    struct my_xkb xkb;
    struct zwp_text_input_manager_v1 *zwp_text_input_manager_v1;
    struct zwp_text_input_v1 *text_input;
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
	//printf("name=%s\n", name);

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
    const int width = state->width, height = state->height;
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

static void wl_pointer_enter(void *data,
	      struct wl_pointer *wl_pointer,
	      uint32_t serial,
	      struct wl_surface *surface,
	      wl_fixed_t surface_x,
	      wl_fixed_t surface_y)
{
    printf("catch wl pointer enter event,(%f,%f) serial:%d\n", wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y), serial);
}

static void wl_pointer_leave(void *data,
	      struct wl_pointer *wl_pointer,
	      uint32_t serial,
	      struct wl_surface *surface)
{
}

static void wl_pointer_motion(void *data,
	       struct wl_pointer *wl_pointer,
	       uint32_t time,
	       wl_fixed_t surface_x,
	       wl_fixed_t surface_y)
{
}

static void wl_pointer_button(void *data,
	       struct wl_pointer *wl_pointer,
	       uint32_t serial,
	       uint32_t time,
	       uint32_t button,
	       uint32_t state)
{
    printf("catch button event:%d\n", button);
}

static struct wl_pointer_listener wl_pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
};

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
       uint32_t format,
       int32_t fd,
       uint32_t size)
{
    struct my_output *state = (struct my_output *)data;
    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    assert(map_shm != MAP_FAILED);
    //printf("map share memory for keyboard:%s\n", map_shm);
    /* 生成 keymap */
    state->xkb.keymap = xkb_keymap_new_from_string(state->xkb.context, map_shm,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);
    close(fd);
    state->xkb.state = xkb_state_new(state->xkb.keymap);
    /* 这里为什么要执行这个 keymap 和 state 的 unref ???
     * 取消，之前 xkb 键的映射，以防合成器在运行时候更改键位
     * */
    xkb_keymap_unref(state->xkb.keymap);
    xkb_state_unref(state->xkb.state);
}

void wl_keyboard_enter(void *data,
	      struct wl_keyboard *wl_keyboard,
	      uint32_t serial,
	      struct wl_surface *surface,
	      struct wl_array *keys)
{
    printf("catch keyboard enter\n");
    struct my_output *state = (struct my_output *)data;
    uint32_t *key;
    wl_array_for_each(key, keys) {
         char buf[128];
         xkb_keysym_t sym = xkb_state_key_get_one_sym(
                         state->xkb.state, *key + 8);
         xkb_keysym_get_name(sym, buf, sizeof(buf));
         fprintf(stderr, "sym: %-12s (%d), ", buf, sym);
         xkb_state_key_get_utf8(state->xkb.state,
                         *key + 8, buf, sizeof(buf));
         fprintf(stderr, "utf8: '%s'\n", buf);
    }
}

void wl_keyboard_leave(void *data,
	      struct wl_keyboard *wl_keyboard,
	      uint32_t serial,
	      struct wl_surface *surface)
{
    printf("catch keyboard leave\n");
}

void wl_keyboard_key(void *data,
	    struct wl_keyboard *wl_keyboard,
	    uint32_t serial,
	    uint32_t time,
	    uint32_t key,
	    uint32_t state)
{
    printf("catch keyboard key event\n");
    struct my_output *my_state = (struct my_output *)data;
    char buf[128];

    printf("key=%d\n", key);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(
                    my_state->xkb.state, key + 8);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    fprintf(stderr, "sym: %-12s (%d), ", buf, sym);
    xkb_state_key_get_utf8(my_state->xkb.state,
                    key + 8, buf, sizeof(buf));
    fprintf(stderr, "utf8: '%s'\n", buf);
}

void wl_keyboard_modifiers(void *data,
		  struct wl_keyboard *wl_keyboard,
		  uint32_t serial,
		  uint32_t mods_depressed,
		  uint32_t mods_latched,
		  uint32_t mods_locked,
		  uint32_t group)
{
    printf("catch keyboard modifiers event\n");
}

void wl_keyboard_repeat_info(void *data,
		    struct wl_keyboard *wl_keyboard,
		    int32_t rate,
		    int32_t delay)
{
    printf("catch keyboard repeat_info event\n");
}

static struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void wl_seat_capabilities(void *data,
		     struct wl_seat *wl_seat,
		     uint32_t capabilities)
{
    struct my_output *state = (struct my_output *)data;
    printf("wl_seat capabilities=%x\n", capabilities);
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
    {
        printf("Wow it's support keyboard\n");
        state->wl_keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(state->wl_keyboard, &wl_keyboard_listener, state);
    }

    if (capabilities & WL_SEAT_CAPABILITY_POINTER)
    {
        printf("Wow it's support pointer\n");
        state->wl_pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(state->wl_pointer, &wl_pointer_listener, state);
    }

    if (capabilities & WL_SEAT_CAPABILITY_TOUCH)
    {
        printf("Wow it's support touch board\n");
    }
}

static void wl_seat_name(void *data,
	     struct wl_seat *wl_seat,
	     const char *name)
{
    printf("wl_seat name=%s\n", name);
}

void zwp_text_input_v1_enter(void *data,
	      struct zwp_text_input_v1 *zwp_text_input_v1,
	      struct wl_surface *surface)
{
}

void zwp_text_input_v1_modifiers_map(void *data,
		      struct zwp_text_input_v1 *zwp_text_input_v1,
		      struct wl_array *map)
{
}

void zwp_text_input_v1_preedit_cursor(void *data,
		       struct zwp_text_input_v1 *zwp_text_input_v1,
		       int32_t index)
{

}

void zwp_text_input_v1_preedit_styling(void *data,
			struct zwp_text_input_v1 *zwp_text_input_v1,
			uint32_t index,
			uint32_t length,
			uint32_t style)
{

}

void zwp_text_input_v1_language(void *data,
		 struct zwp_text_input_v1 *zwp_text_input_v1,
		 uint32_t serial,
		 const char *language)
{

}

void zwp_text_input_v1_text_direction(void *data,
		       struct zwp_text_input_v1 *zwp_text_input_v1,
		       uint32_t serial,
		       uint32_t direction)
{
}

static struct zwp_text_input_v1_listener zwp_text_input_v1_listener = {
    .enter = zwp_text_input_v1_enter,
    .modifiers_map = zwp_text_input_v1_modifiers_map,
    .preedit_styling = zwp_text_input_v1_preedit_styling,
    .preedit_cursor = zwp_text_input_v1_preedit_cursor,
    .language = zwp_text_input_v1_language,
    .text_direction = zwp_text_input_v1_text_direction,
};

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
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
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->wl_seat = wl_registry_bind(
            registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(state->wl_seat, &wl_seat_listener, state);
		printf("绑定 wl_seat\n");
	} else if (strcmp(interface, zwp_text_input_manager_v1_interface.name) == 0) {
        state->zwp_text_input_manager_v1 = wl_registry_bind(
            registry, name, &zwp_text_input_manager_v1_interface, 1);
        state->text_input = zwp_text_input_manager_v1_create_text_input(state->zwp_text_input_manager_v1);
        zwp_text_input_v1_add_listener(state->text_input, &zwp_text_input_v1_listener, state);
        printf("绑定 zwp_text_input_v1\n");
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
		//state->offset += elapsed / 1000.0 * 24;
		state->offset += 0; //elapsed / 1000.0 * 24;
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

void xdg_toplevel_configure(void *data,
		  struct xdg_toplevel *xdg_toplevel,
		  int32_t width,
		  int32_t height,
		  struct wl_array *states)
{
    struct my_output * state =  (struct my_output *)data;
    if (width == 0 || height == 0)
    {
        printf("Oh no zero return\n");
        return;
    }

    state->width = width;
    state->height = height;
}

void xdg_toplevel_close(void *data,
	      struct xdg_toplevel *xdg_toplevel)
{
}

static struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display = wl_display_connect(NULL);
    struct my_output state = {0};
	struct wl_surface *surface = NULL;

    state.width = 900;
    state.height = 900;
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
    state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
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

	state.xdg_surface = xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "AExample client");
    xdg_toplevel_add_listener(state.xdg_toplevel, &xdg_toplevel_listener, &state);
    //xdg_toplevel_set_fullscreen(state.xdg_toplevel, state.wl);

    wl_surface_commit(state.wl_surface);
	printf("buffer commit is done\n");

    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

    zwp_text_input_v1_show_input_panel(state.text_input);
    zwp_text_input_v1_activate(state.text_input, state.wl_seat, state.wl_surface);
    printf("show keyboard virtual\n");
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

