/*
 * See LICENSE file for copyright and license details.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <getopt.h>
#include <execinfo.h>
#include <limits.h>
#include <drm_fourcc.h>
#ifndef __FreeBSD__
#include <crypt.h>
#include <linux/input-event-codes.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <libinput.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <pixman-1/pixman.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <X11/Xlib.h>
#include <wlr/xwayland.h>
#endif

#include "util.h"

/* macros */
#define BARF(fmt, ...)		do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); exit(EXIT_FAILURE); } while (0)
#define EBARF(fmt, ...)		BARF(fmt ": %s", ##__VA_ARGS__, strerror(errno))
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define ROUND(X)                ((int)((X)+0.5))
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define MOVENODE(M, N, X, Y)    wlr_scene_node_set_position((N), (X) + (M)->m.x, (Y) + (M)->m.y)

#ifdef __FreeBSD__
#define BTN_MOUSE	  0x110
#define BTN_MIDDLE  0x112
#define BTN_JOYSTICK 0x120
#endif

/* enums */
enum { CurNormal, CurPressed , CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell, X11Managed, X11Unmanaged }; /* client types */
enum { LyrBg, LyrBottom, LyrTop, LyrOverlay, LyrBarBottom, LyrBarTop, LyrTile, LyrFloat, LyrNoFocus, LyrLock, NUM_LAYERS }; /* scene layers */
enum { BarBottomScene, BarTopScene , NUM_BAR_SCENES }; /* bar scenes */
enum { BarTagsText, BarLayoutText, BarStatusText, NUM_BAR_TEXTS }; /* texts on bar */
enum { BarSubstraceRect, BarUndertagsRect, BarSelectedTagRect, BarFocusedClientRect, BarInfoRect, NUM_BAR_RECTS }; /* rectangles on bar */
enum { LockImageEnter1, LockImageEnter2, LockImageWrong, LockImageClean, NUM_LOCK_IMAGES }; /* lock circle images */
#ifdef XWAYLAND
enum { NetWMWindowTypeDialog, NetWMWindowTypeSplash, NetWMWindowTypeToolbar,
	NetWMWindowTypeUtility, NetLast }; /* EWMH atoms */
#endif

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct {
	unsigned int type; /* XDGShell or X11* */
	struct wlr_box geom;  /* layout-relative, includes border */
	Monitor *mon;
	struct wlr_scene_node *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_node *scene_surface;
	struct wlr_scene_rect *fullscreen_bg; /* See setfullscreen() for info */
	struct wl_list link;
	struct wl_list flink;
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_hidden;
	struct wl_listener fullscreen;

	struct wlr_box prev;  /* layout-relative, includes border */
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener configure;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	unsigned int tags;
	int isfloating, isurgent, isfullscreen;
	uint32_t resize; /* configure serial of a pending resize */
	int ishidden;
	int isdevoured;
	int isclientonbarset;
} Client;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wl_list link;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} Keyboard;

typedef struct {
	unsigned int type; /* LayerShell */
	struct wlr_box geom;
	Monitor *mon;
	struct wlr_scene_node *scene;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

typedef struct {
	uint32_t singular_anchor;
	uint32_t anchor_triplet;
	int *positive_axis;
	int *negative_axis;
	int margin;
} Edge;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

typedef struct {
  cairo_surface_t *surface;
  struct wlr_readonly_data_buffer *buffer;
  struct wlr_scene_buffer *data;
} Surface;

typedef struct {
  Surface surface;
  struct wlr_box box;
  Client *c;

  unsigned long title_hash;

  int is_focused;
  int is_enabled;

	struct wl_list link;
} ClientOnBar;

typedef struct
{
  struct wlr_scene_rect *has_clients_rect;
  struct wlr_scene_rect *has_clients_not_focused_rect;
  int left;
  int right;
} Tag;

typedef struct {
	cairo_t *cairo;
	cairo_surface_t *surface;

  struct wlr_scene_node *scenes[NUM_BAR_SCENES];
  struct wlr_scene_rect *rects[NUM_BAR_RECTS];
  struct wlr_scene_rect *tags_has_client_rects[NUM_BAR_RECTS];
  Surface texts[NUM_BAR_TEXTS];
  Surface *layout_signs;
  Surface tray;

	struct wl_listener tray_update;
	struct wl_listener background_update;
	struct wl_listener status_update;

  struct wl_list clients;

	Tag *tags_info;
	int tags_border, layout_symbol_border, status_border;
	uint32_t applications_amount;
  unsigned int status_text_hash;
} Bar;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wlr_box m;      /* monitor area, layout-relative */
	struct wlr_box w;      /* window area, layout-relative */
	struct wl_list layers[6]; // LayerSurface::link
	const Layout *lt[2];
	
  Bar bar;
  Surface background_image;
  struct wlr_scene_node *background_scene;

	int layout_number;
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	double mfact;
	int nmaster;
	int un_map; /* If a map/unmap happened on this monitor, then this should be true */
  int round;
};

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
  int round;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x;
	int y;
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	Client *focused;

	struct wl_listener set_region;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
	int x;
	int y;
	int w;
	int h;
} Rule;

enum LockCircleDrawState{Nothing, Entering, Clear, Error, Disabled};

struct {
  struct wlr_scene_node *scene;
  char password[256];

	Surface images[NUM_LOCK_IMAGES];
	enum LockCircleDrawState state;

	int width, height;
	int len;
	int set;
} Lock;


/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyexclusive(struct wlr_box *usable_area, uint32_t anchor,
		int32_t exclusive, int32_t margin_top, int32_t margin_right,
		int32_t margin_bottom, int32_t margin_left);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void autostart(void);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void checkconstraintregion(void);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static void cleanupkeyboard(struct wl_listener *listener, void *data);
static void cleanupmon(struct wl_listener *listener, void *data);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpointerconstraint(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_input_device *device);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_input_device *device);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorwarptoconstrainthint(void);
static void destroyclientonbar(Client *c);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroysurface(Surface *surface);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void dragicondestroy(struct wl_listener *listener, void *data);
static unsigned long djb2hash(const char* str);
static void initbarrendering(Monitor *m);
static void focusclient(Client *c, int lift);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static void fullscreennotify(struct wl_listener *listener, void *data);
static Client *focustop(Monitor *m);
static void hideclient(struct wl_listener *listener, void *data);
static void incnmaster(const Arg *arg);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static void killclient(const Arg *arg);
static void lockactivate();
static void lockdeactivate();
static void lockstatechange(enum LockCircleDrawState state);
static void maplayersurfacenotify(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void monocle(Monitor *m);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void mouseclick(int button, int mod);
static void nextlayout(const Arg *arg);
static void nullsurface(Surface *s);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerconstraintsetregion(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void printstatus(void);
static void quit(const Arg *arg);
static void quitsignal(int signo);
static void rendertextonsurface(Monitor *m, Surface *surface, const char *text, int width, int height);
static void renderlockimages();
static void rendertray(Monitor *m);
static void renderclients(Monitor *m, struct timespec *now);
static void rendermon(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void screenlock(const Arg *arg);
static void scrollclients(const Arg *arg);
static Client *selclient(void);
static void setcursor(struct wl_listener *listener, void *data);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void sethidden(Client *c, int hidden);
static void setkblayout(Keyboard *kb, const struct xkb_rule_names *newrule);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setmon(Client *c, Monitor *m, unsigned int newtags);
static void setstatustext(Monitor *m, const char *text);
static void setup(void);
static void sigchld(int unused);
static void sigfaulthandler(int sig);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglecursor(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglekblayout(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void traymod(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updateclientbounds(Monitor *m);
static void updateclientonbar(Client *c);
static void updatebacks(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void updatestatusmessage(struct wl_listener *listener, void *data);
static void updatetrayicons(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void view(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static struct wlr_scene_node *xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
static void zoom(const Arg *arg);

/* variables */
static const char broken[] = "broken";
static const char *cursor_image = "left_ptr";
static pid_t child_pid = -1;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_node *layers[NUM_LAYERS];
static struct wlr_renderer *drw;
static struct wlr_allocator *alc;
static struct wlr_compositor *compositor;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wl_list stack;   /* stacking z-order */
static struct wlr_idle *idle;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_input_inhibit_manager *input_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

/* Relative cursor */
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wl_listener pointer_constraint_commit;
static PointerConstraint *active_constraint;
static pixman_region32_t active_confine;
static int active_confine_requires_warp;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_seat *seat;
static struct wl_list keyboards;
static unsigned int kblayout = 0;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener idle_inhibitor_create = {.notify = createidleinhibitor};
static struct wl_listener idle_inhibitor_destroy = {.notify = destroyidleinhibitor};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_input = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_xdg_surface = {.notify = createnotify};
static struct wl_listener new_layer_shell_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener drag_icon_destroy = {.notify = dragicondestroy};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static Atom getatom(xcb_connection_t *xc, const char *name);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
static Atom netatom[NetLast];
#endif

/* pango + cairo */
PangoFontDescription *pango_font_description;
int pango_font_symbol_width;
const char edwl_version[] = "edwl 0.9.2";
char *status_text = NULL;
cairo_surface_t *background_image_surface = NULL;

/* Additional flags and variables */
int cursor_is_hidden_in_games = 0;
pid_t *autorun_app = NULL;
size_t autorun_amount = 0;

/* configuration, allows nested code to access above variables */
#include "config.h"

/* attempt to encapsulate suck into one file */
#include "client.h"

/* tray functions */
#include "tray.h"

Tray *tray = NULL;

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };


/* function implementations */
void
applybounds(Client *c, struct wlr_box *bbox)
{
	if (!c->isfullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		/* try to set size hints */
		c->geom.width = MAX(min.width + (2 * c->bw), c->geom.width);
		c->geom.height = MAX(min.height + (2 * c->bw), c->geom.height);
		/* Some clients set them max size to INT_MAX, which does not violates
		 * the protocol but its innecesary, they can set them max size to zero. */
		if (max.width > 0 && !(2 * c->bw > INT_MAX - max.width)) // Checks for overflow
			c->geom.width = MIN(max.width + (2 * c->bw), c->geom.width);
		if (max.height > 0 && !(2 * c->bw > INT_MAX - max.height)) // Checks for overflow
			c->geom.height = MIN(max.height + (2 * c->bw), c->geom.height);
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width + 2 * c->bw <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height + 2 * c->bw <= bbox->y)
		c->geom.y = bbox->y;
}

void
applyexclusive(struct wlr_box *usable_area,
		uint32_t anchor, int32_t exclusive,
		int32_t margin_top, int32_t margin_right,
		int32_t margin_bottom, int32_t margin_left) {
	Edge edges[] = {
		{ // Top
			.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
			.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
			.positive_axis = &usable_area->y,
			.negative_axis = &usable_area->height,
			.margin = margin_top,
		},
		{ // Bottom
			.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = NULL,
			.negative_axis = &usable_area->height,
			.margin = margin_bottom,
		},
		{ // Left
			.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
			.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = &usable_area->x,
			.negative_axis = &usable_area->width,
			.margin = margin_left,
		},
		{ // Right
			.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
			.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = NULL,
			.negative_axis = &usable_area->width,
			.margin = margin_right,
		}
	};
	for (size_t i = 0; i < LENGTH(edges); i++) {
		if ((anchor == edges[i].singular_anchor || anchor == edges[i].anchor_triplet)
				&& exclusive + edges[i].margin > 0) {
			if (edges[i].positive_axis)
				*edges[i].positive_axis += exclusive + edges[i].margin;
			if (edges[i].negative_axis)
				*edges[i].negative_axis -= exclusive + edges[i].margin;
			break;
		}
	}
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	unsigned int i, newtags = 0;
	const Rule *r;
	Monitor *mon = selmon, *m;

	c->isfloating = client_is_float_type(c);
	if (!(appid = client_get_appid(c)))
		appid = broken;
	if (!(title = client_get_title(c)))
		title = broken;

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link)
				if (r->monitor == i++)
					mon = m;
			if (c->isfloating)
				resize(c, (struct wlr_box){
						r->x ? r->x + mon->w.x : mon->w.width / 2 - c->geom.width / 2 + mon->w.x,
						r->y ? r->y + mon->w.y : mon->w.height / 2 - c->geom.height / 2 + mon->w.y,
						r->w ? r->w : c->geom.width,
						r->h ? r->h : c->geom.height}, 1);
		}
	}
	wlr_scene_node_reparent(c->scene, layers[c->isfloating ? LyrFloat : LyrTile]);
	setmon(c, mon, newtags);
}

void
arrange(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		wlr_scene_node_set_enabled(c->scene, VISIBLEON(c, c->mon) && !c->ishidden);

	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *layersurface;
	struct wlr_box full_area = m->m;

	wl_list_for_each(layersurface, list, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
		struct wlr_layer_surface_v1_state *state = &wlr_layer_surface->current;
		struct wlr_box bounds;
		struct wlr_box box = {
			.width = state->desired_width,
			.height = state->desired_height
		};
		const uint32_t both_horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		const uint32_t both_vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

		/* Unmapped surfaces shouldn't have exclusive zone */
		if (!((LayerSurface *)wlr_layer_surface->data)->mapped
				|| exclusive != (state->exclusive_zone > 0))
			continue;

		bounds = state->exclusive_zone == -1 ? full_area : *usable_area;

		/* Horizontal axis */
		if ((state->anchor & both_horiz) && box.width == 0) {
			box.x = bounds.x;
			box.width = bounds.width;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
			box.x = bounds.x;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			box.x = bounds.x + (bounds.width - box.width);
		} else {
			box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
		}
		/* Vertical axis */
		if ((state->anchor & both_vert) && box.height == 0) {
			box.y = bounds.y;
			box.height = bounds.height;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			box.y = bounds.y;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
			box.y = bounds.y + (bounds.height - box.height);
		} else {
			box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
		}
		/* Margin */
		if ((state->anchor & both_horiz) == both_horiz) {
			box.x += state->margin.left;
			box.width -= state->margin.left + state->margin.right;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
			box.x += state->margin.left;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			box.x -= state->margin.right;
		}
		if ((state->anchor & both_vert) == both_vert) {
			box.y += state->margin.top;
			box.height -= state->margin.top + state->margin.bottom;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			box.y += state->margin.top;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
			box.y -= state->margin.bottom;
		}
		if (box.width < 0 || box.height < 0) {
			wlr_layer_surface_v1_destroy(wlr_layer_surface);
			continue;
		}
		layersurface->geom = box;

		if (state->exclusive_zone > 0)
			applyexclusive(usable_area, state->anchor, state->exclusive_zone,
					state->margin.top, state->margin.right,
					state->margin.bottom, state->margin.left);
		wlr_scene_node_set_position(layersurface->scene, box.x, box.y);
		wlr_layer_surface_v1_configure(wlr_layer_surface, box.width, box.height);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	LayerSurface *layersurface;
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
    arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	// Find topmost keyboard interactive layer, if such a layer exists
	for (i = 0; i < LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(layersurface,
				&m->layers[layers_above_shell[i]], link) {
			if (layersurface->layer_surface->current.keyboard_interactive &&
					layersurface->mapped) {
				/* Deactivate the focused client. */
				focusclient(NULL, 0);
				exclusive_focus = layersurface;
				client_notify_enter(layersurface->layer_surface->surface, wlr_seat_get_keyboard(seat));
				return;
			}
		}
	}
}

void
autostart(void)
{
  const char *const *c;
  int i = 0;

  for (c = autorun; *c; autorun_amount++, c++)
    while (*++c);

  if (autorun_amount == 0)
    return;

  autorun_app = ecalloc(autorun_amount, sizeof(pid_t));
  for (c = autorun; *c; i++, c++)
  {
    if ((autorun_app[i] = fork()) == 0)
    {
      setsid();
      execvp(*c, (char *const*)c);
      fprintf(stderr, "edwl: can't start %s\n", *c);
      _exit(EXIT_FAILURE);
    }
    while (*++c);
  }
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_event_pointer_axis *event = data;
	wlr_idle_notify_activity(idle, seat);
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

void mouseclick(int button, int mod) // mod - true, if MODKEY pressed
{
	/* coordinats in local mon */
	Client *selc = selclient();
	int local_x = cursor->x - selmon->m.x;
	int local_y = cursor->y - selmon->m.y;

	/* No button press on a mon with a fullscreen client */
	if (selc && selc->isfullscreen)
		return;

	/*  This events check if user clicked on a specific place on bar */
	if (local_y > selmon->m.height - barheight)
	{
		int selected_tag = 1;
		int tags_amount = sizeof(tags) / sizeof(tags[0]);

		/* checking that click was on a tags panel*/
		if ((local_x - selmon->w.x) < selmon->bar.layout_symbol_border)
		{
			/* getting and setting this tag */
			for (int i = 0; i < tags_amount; i++)
			{
				if (selmon->bar.tags_info[i].left <= local_x && selmon->bar.tags_info[i].right > local_x)
				{
					Arg arg;
					arg.ui = 1 << i;

					if (!mod)
						view(&arg); // if no mod - changing tag view
					else
						tag(&arg); // with mod - move current client to selected tag
				}
			}

			/* if it was a click on layer sign */
			if ((local_x - selmon->w.x) > selmon->bar.tags_border)
			{
				nextlayout(NULL);
			}
		}

		/* checking that click was on applications zone */
		if (local_x > selmon->bar.layout_symbol_border && local_x < selmon->w.width - selmon->bar.status_border )
		{
			ClientOnBar *cb;
			wl_list_for_each(cb, &selmon->bar.clients, link) {
				if (!cb->is_enabled)
					continue;

				if (local_x > cb->box.x  && local_x < cb->box.x + cb->box.width)
				{
          int hidden = cb->c->ishidden;
				  sethidden(cb->c, hidden ? 0 : 1);

          if (!hidden)
          {
            wlr_scene_node_set_enabled(cb->c->scene, hidden);
            wlr_scene_node_raise_to_top(cb->c->scene);
          }
				}
			}
		}

		/* checking that click was on a tray panel */
    if (local_x > selmon->m.width - tray->applications_amount * barheight)
    {
      int position = (int)((selmon->m.width - local_x) / barheight);
      if (position < tray->applications_amount)
      {
        tray_process_click(tray->applications_amount - position - 1, button, local_x, local_y);
      }
    }

		return;
	}
}

void
nextlayout(const Arg *)
{
	Arg arg;
	int next_layout_number = (selmon->layout_number + 1) % 3;

	arg.v = &layouts[next_layout_number];
	setlayout(&arg);
}

void
nullsurface(Surface *s)
{
  s->surface = NULL;
  s->buffer = NULL;
  s->data = NULL;
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_event_pointer_button *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c;
	const Button *b;

	wlr_idle_notify_activity(idle, seat);

	/* If lock is active - no actions allowed */
	if (Lock.set)
		return;

	switch (event->state) {
	case WLR_BUTTON_PRESSED:;
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && !client_is_unmanaged(c))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		if (event->button >= BTN_MOUSE && event->button < BTN_JOYSTICK)
		{
			// If no mods, than it's just a click
		  if (!mods)
		  {
		    mouseclick(event->button, 0);
		  }
			// If only MODKEY, than user wants to move a client
		  else if (CLEANMASK(mods) == MODKEY)
		  {
		    mouseclick(event->button, 1);
		  }
		}

		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		cursor_mode = CurPressed;
		break;
	case WLR_BUTTON_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (cursor_mode != CurNormal && cursor_mode != CurPressed) {
			cursor_mode = CurNormal;
			/* Clear the pointer focus, this way if the cursor is over a surface
			 * we will send an enter event after which the client will provide us
			 * a cursor surface */
			wlr_seat_pointer_clear_focus(seat);
      motionnotify(0, NULL, 0, 0, 0, 0);
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			setmon(grabc, selmon, 0);
			return;
		} else {
			cursor_mode = CurNormal;
		}

		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		Client *c;
		if (exclude == inhibitor->surface)
			continue;
		/* In case we can't get a client from the surface assume that it is
		 * visible, for example a layer surface */
		if (!(c = client_from_wlr_surface(inhibitor->surface))
				|| VISIBLEON(c, c->mon)) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_set_enabled(idle, NULL, !inhibited);
}

void
checkconstraintregion(void)
{
	struct wlr_pointer_constraint_v1 *constraint = active_constraint->constraint;
	pixman_region32_t *region = &constraint->region;
	Client *c = client_from_wlr_surface(constraint->surface);
	double sx, sy;
	if (active_confine_requires_warp && c) {
		active_confine_requires_warp = 0;

		sx = cursor->x + c->geom.x;
		sy = cursor->y + c->geom.y;

		if (!pixman_region32_contains_point(region,
				floor(sx), floor(sy), NULL)) {
			int nboxes;
			pixman_box32_t *boxes = pixman_region32_rectangles(region, &nboxes);
			if (nboxes > 0) {
				sx = (boxes[0].x1 + boxes[0].x2) / 2.;
				sy = (boxes[0].y1 + boxes[0].y2) / 2.;

				wlr_cursor_warp_closest(cursor, NULL,
					sx - c->geom.x, sy - c->geom.y);
			}
		}
	}

	/* A locked pointer will result in an empty region, thus disallowing all movement. */
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		pixman_region32_copy(&active_confine, region);
	} else {
		pixman_region32_clear(&active_confine);
	}
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(wlr_backend_get_session(backend), arg->ui);
}

void
cleanup(void)
{
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
#endif
  for (int i = 0; i < NUM_LOCK_IMAGES; i++)
  {
    destroysurface(&Lock.images[i]);
  }
  wlr_scene_node_destroy(Lock.scene);
	tray_destroy(tray);
	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	pango_font_description_free(pango_font_description);
	wlr_backend_destroy(backend);
	wlr_renderer_destroy(drw);
	wlr_allocator_destroy(alc);
	wlr_xcursor_manager_destroy(cursor_mgr);
	wlr_cursor_destroy(cursor);
	wlr_output_layout_destroy(output_layout);
	wlr_seat_destroy(seat);
	wl_display_destroy(dpy);

  if (background_image_surface)
    cairo_surface_destroy(background_image_surface);

	if (status_text)
		free(status_text);
}

void
cleanupkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_input_device *device = data;
	Keyboard *kb = device->data;

	wl_list_remove(&kb->link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

void
cleanupbar(Monitor *m)
{
  ClientOnBar *current;
  destroysurface(&m->bar.tray);
  wl_list_for_each(current, &m->bar.clients, link)
  {
    if (m == current->c->mon)
    {
      destroyclientonbar(current->c);
    }
  }

  for (int i = 0; i < LENGTH(layouts); i++)
  {
    destroysurface(m->bar.layout_signs + i);
  }

  for (int i = 0; i < NUM_BAR_TEXTS; i++)
  {
    if (m->bar.texts[i].data)
    {
      destroysurface(&m->bar.texts[i]);
      nullsurface(&m->bar.texts[i]);
    }
  }
  
  for (int i = 0; i < NUM_BAR_SCENES; i++)
  {
    if (m->bar.scenes[i])
    {
      wlr_scene_node_destroy(m->bar.scenes[i]);
      m->bar.scenes[i] = NULL;
    }
  }

  free(m->bar.layout_signs);
  wlr_scene_node_destroy(m->bar.scenes[BarBottomScene]);
  wlr_scene_node_destroy(m->bar.scenes[BarTopScene]);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	struct wlr_output *wlr_output = data;
	Monitor *m = wlr_output->data;
	int nmons, i = 0;

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);

	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	if ((nmons = wl_list_length(&mons)))
    do // don't switch to disabled mons
      selmon = wl_container_of(mons.prev, selmon, link);
    while (!selmon->wlr_output->enabled && i++ < nmons);

  free(m->bar.tags_info);

	focusclient(focustop(selmon), 1);
	
  /* Destroing nodes */
  if (m->background_image.data)
  {
    wlr_scene_node_destroy(&m->background_image.data->node);
    readonly_data_buffer_drop(m->background_image.buffer);
  }
  wlr_scene_node_destroy(m->background_scene);
  cleanupbar(m);
  
	closemon(m);
	free(m);
}

void
closemon(Monitor *m)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
				.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->tags);
	}
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *layersurface = wl_container_of(listener, layersurface, surface_commit);
	struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
	struct wlr_output *wlr_output = wlr_layer_surface->output;

	/* For some reason this layersurface have no monitor, this can be because
	 * its monitor has just been destroyed */
	if (!wlr_output || !(layersurface->mon = wlr_output->data))
		return;

	if (layers[wlr_layer_surface->current.layer] != layersurface->scene->parent) {
		wlr_scene_node_reparent(layersurface->scene,
				layers[wlr_layer_surface->current.layer]);
		wl_list_remove(&layersurface->link);
		wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->current.layer],
				&layersurface->link);
	}

	if (wlr_layer_surface->current.committed == 0
			&& layersurface->mapped == wlr_layer_surface->mapped)
		return;
	layersurface->mapped = wlr_layer_surface->mapped;

	arrangelayers(layersurface->mon);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	struct wlr_box box = {0};
	client_get_geometry(c, &box);

	if (c->mon && !wlr_box_empty(&box) && (box.width != c->geom.width - 2 * c->bw
			|| box.height != c->geom.height - 2 * c->bw))
		arrange(c->mon);

	/* mark a pending resize as completed */
	if (c->resize && (c->resize <= c->surface.xdg->current.configure_serial
			|| (c->surface.xdg->current.geometry.width == c->surface.xdg->pending.geometry.width
			&& c->surface.xdg->current.geometry.height == c->surface.xdg->pending.geometry.height)))
		c->resize = 0;
}

void
commitpointerconstraint(struct wl_listener *listener, void *data)
{
	assert(active_constraint->constraint->surface == data);
	checkconstraintregion();
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	wl_signal_add(&idle_inhibitor->events.destroy, &idle_inhibitor_destroy);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_input_device *device)
{
	Keyboard *kb = device->data = calloc(1, sizeof(*kb));
	kb->device = device;

	setkblayout(kb, &xkb_rules);

	wlr_keyboard_set_repeat_info(device->keyboard, repeat_rate, repeat_delay);

	/* Here we set up listeners for keyboard events. */
	LISTEN(&device->keyboard->events.modifiers, &kb->modifiers, keypressmod);
	LISTEN(&device->keyboard->events.key, &kb->key, keypress);
	LISTEN(&device->events.destroy, &kb->destroy, cleanupkeyboard);

	wlr_seat_set_keyboard(seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&keyboards, &kb->link);
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	Monitor *m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, alc, drw);

	/* Initialize monitor state using configured rules */
	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);
	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
      m->round = r->round;
			wlr_output_set_scale(wlr_output, r->scale);
			wlr_xcursor_manager_load(cursor_mgr, r->scale);
			m->lt[0] = m->lt[1] = r->lt;
			wlr_output_set_transform(wlr_output, r->rr);
			break;
		}
	}

	/* The mode is a tuple of (width, height, refresh rate), and each
	 * monitor supports only a specific set of modes. We just pick the
	 * monitor's preferred mode; a more sophisticated compositor would let
	 * the user configure it. */
	wlr_output_set_mode(wlr_output, wlr_output_preferred_mode(wlr_output));
	wlr_output_enable_adaptive_sync(wlr_output, 1);

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);

	/* Dbus signals from tray */
	LISTEN(&tray->events.applications_update, &m->bar.tray_update, updatetrayicons);
	LISTEN(&tray->events.message_update, &m->bar.status_update, updatestatusmessage);
	LISTEN(&tray->events.background_update, &m->bar.background_update, updatebacks);

	wlr_output_enable(wlr_output, 1);
	if (!wlr_output_commit(wlr_output))
		return;

	wl_list_insert(&mons, &m->link);
	printstatus();

	/* Adds this to the output layout in the order it was configured in.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	wlr_output_layout_add_auto(output_layout, wlr_output);

	/* Creating bar surfaces */
	initbarrendering(m);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct wlr_xdg_surface *xdg_surface = data;
	Client *c;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
  {
		struct wlr_box box;
		LayerSurface *l = toplevel_from_popup(xdg_surface->popup);
		xdg_surface->surface->data = wlr_scene_xdg_surface_create(
      xdg_surface->popup->parent->data, xdg_surface
    );

		if (wlr_surface_is_layer_surface(xdg_surface->popup->parent) && l
				&& l->layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
    {
      wlr_scene_node_reparent(xdg_surface->surface->data, layers[LyrTop]);
    }

		if (!l || !l->mon)
    {
      return;
    }
		box = l->type == LayerShell ? l->mon->m : l->mon->w;
		box.x -= l->geom.x;
		box.y -= l->geom.y;
		wlr_xdg_popup_unconstrain_from_box(xdg_surface->popup, &box);
		return;
	} else 
  {
    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE)
    {
      return;
    }
  }

	/* Allocate a Client for this surface */
	c = xdg_surface->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = xdg_surface;
	c->bw = borderpx;

	LISTEN(&xdg_surface->events.map, &c->map, mapnotify);
	LISTEN(&xdg_surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&xdg_surface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xdg_surface->toplevel->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xdg_surface->toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xdg_surface->toplevel->events.request_minimize, &c->set_hidden, hideclient);
	c->isfullscreen = 0;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *wlr_layer_surface = data;
	LayerSurface *layersurface;
	struct wlr_layer_surface_v1_state old_state;

	if (!wlr_layer_surface->output)
		wlr_layer_surface->output = selmon ? selmon->wlr_output : NULL;

	if (!wlr_layer_surface->output)
		wlr_layer_surface_v1_destroy(wlr_layer_surface);

	layersurface = ecalloc(1, sizeof(LayerSurface));
	layersurface->type = LayerShell;
	LISTEN(&wlr_layer_surface->surface->events.commit,
			&layersurface->surface_commit, commitlayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.destroy, &layersurface->destroy,
			destroylayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.map, &layersurface->map,
			maplayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.unmap, &layersurface->unmap,
			unmaplayersurfacenotify);

	layersurface->layer_surface = wlr_layer_surface;
	layersurface->mon = wlr_layer_surface->output->data;
	wlr_layer_surface->data = layersurface;

	layersurface->scene = wlr_layer_surface->surface->data =
			wlr_scene_subsurface_tree_create(layers[wlr_layer_surface->pending.layer],
			wlr_layer_surface->surface);
	layersurface->scene->data = layersurface;

	wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->pending.layer],
			&layersurface->link);

	/* Temporarily set the layer's current state to pending
	 * so that we can easily arrange it
	 */
	old_state = wlr_layer_surface->current;
	wlr_layer_surface->current = wlr_layer_surface->pending;
	layersurface->mapped = 1;
	arrangelayers(layersurface->mon);
	wlr_layer_surface->current = old_state;
}

void
createpointer(struct wlr_input_device *device)
{
	if (wlr_input_device_is_libinput(device)) {
		struct libinput_device *libinput_device =  (struct libinput_device*)
			wlr_libinput_get_device_handle(device);

		if (libinput_device_config_tap_get_finger_count(libinput_device)) {
			libinput_device_config_tap_set_enabled(libinput_device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(libinput_device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(libinput_device, drag_lock);
		}

		if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
			libinput_device_config_scroll_set_natural_scroll_enabled(libinput_device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(libinput_device))
			libinput_device_config_dwt_set_enabled(libinput_device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(libinput_device))
			libinput_device_config_left_handed_set(libinput_device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(libinput_device))
			libinput_device_config_middle_emulation_set_enabled(libinput_device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(libinput_device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method (libinput_device, scroll_method);
		
		 if (libinput_device_config_click_get_methods(libinput_device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
                        libinput_device_config_click_set_method (libinput_device, click_method);

		if (libinput_device_config_send_events_get_modes(libinput_device))
			libinput_device_config_send_events_set_mode(libinput_device, send_events_mode);

		if (libinput_device_config_accel_is_available(libinput_device)) {
			libinput_device_config_accel_set_profile(libinput_device, accel_profile);
			libinput_device_config_accel_set_speed(libinput_device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(cursor, device);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;
	PointerConstraint *constraint = ecalloc(1, sizeof(*constraint));
	Client *c = client_from_wlr_surface(wlr_constraint->surface), *sel = selclient();
	constraint->constraint = wlr_constraint;
	wlr_constraint->data = constraint;

	LISTEN(&wlr_constraint->events.set_region, &constraint->set_region,
			pointerconstraintsetregion);
	LISTEN(&wlr_constraint->events.destroy, &constraint->destroy,
			destroypointerconstraint);

	if (c == sel)
		cursorconstrain(wlr_constraint);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *wlr_constraint)
{
	PointerConstraint *constraint = wlr_constraint->data;

	if (active_constraint == constraint)
		return;

	wl_list_remove(&pointer_constraint_commit.link);
	if (active_constraint) {
		if (!wlr_constraint)
			cursorwarptoconstrainthint();

		wlr_pointer_constraint_v1_send_deactivated(active_constraint->constraint);
	}

  //wlr_cursor_set_surface(cursor, NULL, 0, 0);
	active_constraint = constraint;

	if (!wlr_constraint) {
		wl_list_init(&pointer_constraint_commit.link);
		return;
	}

	active_confine_requires_warp = 1;

	/* Stolen from sway/input/cursor.c:1435
	 *
	 * FIXME: Big hack, stolen from wlr_pointer_constraints_v1.c:121.
	 * This is necessary because the focus may be set before the surface
	 * has finished committing, which means that warping won't work properly,
	 * since this code will be run *after* the focus has been set.
	 * That is why we duplicate the code here.
	 */
	if (pixman_region32_not_empty(&wlr_constraint->current.region)) {
		pixman_region32_intersect(&wlr_constraint->region,
			&wlr_constraint->surface->input_region, &wlr_constraint->current.region);
	} else {
		pixman_region32_copy(&wlr_constraint->region,
			&wlr_constraint->surface->input_region);
	}

	checkconstraintregion();

	wlr_pointer_constraint_v1_send_activated(wlr_constraint);

	LISTEN(&wlr_constraint->surface->events.commit, &pointer_constraint_commit,
			commitpointerconstraint);
}

void
cursorwarptoconstrainthint(void)
{
	struct wlr_pointer_constraint_v1 *constraint = active_constraint->constraint;

	if (constraint->current.committed &
			WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT) {
		double lx, ly;
		double sx = lx = constraint->current.cursor_hint.x;
		double sy = ly = constraint->current.cursor_hint.y;

		Client *c = client_from_wlr_surface(constraint->surface);
		if (c) {
			lx -= c->geom.x;
			ly -= c->geom.y;
		}

		wlr_cursor_warp(cursor, NULL, lx, ly);

		/* Warp the pointer as well, so that on the next pointer rebase we don't
		 * send an unexpected synthetic motion event to clients. */
		wlr_seat_pointer_warp(seat, sx, sy);
	}
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
destroysurface(Surface *surface)
{
  wlr_scene_node_destroy(&surface->data->node);
  readonly_data_buffer_drop(surface->buffer);
  cairo_surface_destroy(surface->surface);
}

void
destroyclientonbar(Client *c)
{
  ClientOnBar *current, *found = NULL;
  Monitor *m = c->mon;

  wl_list_for_each(current, &m->bar.clients, link)
  {
    if (current->c == c)
    {
      found = current;
    }
  }

  if (found)
  {
    destroysurface(&found->surface);
    wl_list_remove(&found->link);
    free(found);
  }

  c->isclientonbarset = 0;
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* I've been testing and at this point the inhibitor has not yet been
	 * removed from the list, checking if it has at least one item. */
	checkidleinhibitor(data);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *layersurface = wl_container_of(listener, layersurface, destroy);

	wl_list_remove(&layersurface->link);
	wl_list_remove(&layersurface->destroy.link);
	wl_list_remove(&layersurface->map.link);
	wl_list_remove(&layersurface->unmap.link);
	wl_list_remove(&layersurface->surface_commit.link);
	wlr_scene_node_destroy(layersurface->scene);
	free(layersurface);
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is destroyed and should never be shown again. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	wl_list_remove(&c->set_hidden.link);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->set_hints.link);
		wl_list_remove(&c->activate.link);
	}
#endif
	free(c);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *constraint = wl_container_of(listener, constraint, destroy);
	wl_list_remove(&constraint->set_region.link);
	wl_list_remove(&constraint->destroy.link);

	if (active_constraint == constraint) {
		cursorwarptoconstrainthint();

		if (pointer_constraint_commit.link.next)
			wl_list_remove(&pointer_constraint_commit.link);

		wl_list_init(&pointer_constraint_commit.link);
		active_constraint = NULL;
	}

	free(constraint);
}

unsigned long
djb2hash(const char* str)
{
  unsigned long hash = 5381;
  int c;

  if (str == NULL || strlen(str) == 0)
    return 0;

  while (c = *str++)
    hash = ((hash << 5) + hash) + c;

  return hash;
}

void
togglecursor(const Arg *arg)
{
  if (cursor_is_hidden_in_games)
  {
    cursor_is_hidden_in_games = 0;
		wlr_xcursor_manager_set_cursor_image(cursor_mgr, (cursor_image = "left_ptr"), cursor);
  } else
  {
    cursor_is_hidden_in_games = 1;
		wlr_cursor_set_surface(cursor, NULL, 0, 0);
  }
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = selclient();
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
scrollclients(const Arg *arg)
{
	Monitor *m = selmon;
	Client *c = NULL;
	list_node *clients_on_mon = NULL, *end_node, *tmp;
	int clients_amount = 0;

	/*  Getting a list of clients on current monitor*/
	if (arg->i == 1)
	{
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || !(c->tags & m->tagset[m->seltags]) || c->ishidden || c->isfloating || c->isfullscreen)
				continue;

			end_node = list_append(&clients_on_mon, (void*)c);
			++clients_amount;
		}
	} else
	{
		wl_list_for_each_reverse(c, &clients, link) {
			if (c->mon != m || !(c->tags & m->tagset[m->seltags]) || c->ishidden || c->isfloating || c->isfullscreen)
				continue;

			end_node = list_append(&clients_on_mon, (void*)c);
			++clients_amount;
		}
	}

	if (clients_amount < 2)
		return;

	tmp = clients_on_mon->next;
	end_node->next = clients_on_mon;
	clients_on_mon->next = NULL;
	clients_on_mon = tmp;

	/* Remove clients from global list */
	for (list_node* current = clients_on_mon; current;)
	{
		c = (Client*)current->data;
		wl_list_remove(&c->link);
		current = current->next;
	}

	/* Clear tmp list and add clients in global list */
	for (list_node* current = clients_on_mon; current;)
	{
		c = (Client*)current->data;
		wl_list_insert(&clients, &c->link);

		tmp = current;
		current = current->next;

		free(tmp);
	}

	arrange(m);
}

void
setfullscreen(Client *c, int fullscreen)
{
	c->isfullscreen = fullscreen;
	if (!c->mon)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
		/* The xdg-protocol specifies:
		 *
		 * If the fullscreened surface is not opaque, the compositor must make
		 * sure that other screen content not part of the same surface tree (made
		 * up of subsurfaces, popups or similarly coupled surfaces) are not
		 * visible below the fullscreened surface.
		 *
		 * For brevity we set a black background for all clients
		 */
		if (!c->fullscreen_bg) {
			c->fullscreen_bg = wlr_scene_rect_create(c->scene,
				c->geom.width, c->geom.height, fullscreen_bg);
			wlr_scene_node_lower_to_bottom(&c->fullscreen_bg->node);
		}
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
		if (c->fullscreen_bg) {
			wlr_scene_node_destroy(&c->fullscreen_bg->node);
			c->fullscreen_bg = NULL;
		}
	}

  if (!c->isfullscreen)
    arrange(c->mon);

  wlr_scene_node_raise_to_top(c->scene);
	printstatus();
}

void
sethidden(Client *c, int hidden)
{
	c->ishidden = hidden;
  wlr_scene_node_set_enabled(c->scene, hidden);

	arrange(c->mon);
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void
dragicondestroy(struct wl_listener *listener, void *data)
{
	struct wlr_drag_icon *icon = data;
	wlr_scene_node_destroy(icon->data);
	// Focus enter isn't sent during drag, so refocus the focused node.
	focusclient(selclient(), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
focusclient(Client *c, int lift)
{
  ClientOnBar *current;
  Monitor *m = c ? c->mon : NULL;
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	struct wlr_keyboard *kb;
	int i;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(c->scene);

	if (c && client_surface(c) == old)
		return;

	/* Put the new client atop the focus stack and select its monitor */
	if (c) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;
		client_restack_surface(c);

		if (!exclusive_focus)
      for (i = 0; i < 4; i++)
        wlr_scene_rect_set_color(c->border[i], focuscolor);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (wlr_surface_is_layer_surface(old)) {
			struct wlr_layer_surface_v1 *wlr_layer_surface =
				wlr_layer_surface_v1_from_wlr_surface(old);

			if (wlr_layer_surface && ((LayerSurface *)wlr_layer_surface->data)->mapped && (
						wlr_layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
						wlr_layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
						))
				return;
		} else {
			Client *w;
			if ((w = client_from_wlr_surface(old)))
				for (i = 0; i < 4; i++)
					wlr_scene_rect_set_color(w->border[i], bordercolor);

			client_activate_surface(old, 0);
		}
	}

	printstatus();
	checkidleinhibitor(NULL);

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);

  /* Updating client on bar */
  wl_list_for_each(current, &m->bar.clients, link)
  {
    if (current->c == c)
    {
      wlr_scene_rect_set_size(
        m->bar.rects[BarFocusedClientRect],
        current->box.width,
        current->box.height
      );
      MOVENODE(m, &m->bar.rects[BarFocusedClientRect]->node, current->box.x, 748);
    }
  }
  wlr_scene_node_set_enabled(&m->bar.rects[BarFocusedClientRect]->node, true);
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons)
    do
      selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	focusclient(focustop(selmon), 1);
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = selclient();
	if (!sel || (sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue;  /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break;  /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue;  /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break;  /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink)
		if (VISIBLEON(c, m))
			return c;
	return NULL;
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(device);
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	int handled = 0;
	const Key *k;
	for (k = keys; k < END(keys); k++) {
		if (CLEANMASK(mods) == CLEANMASK(k->mod) &&
				sym == k->keysym && k->func) {
			k->func(&k->arg);
			handled = 1;
		}
	}
	return handled;
}

void
keypress(struct wl_listener *listener, void *data)
{
	int i;
	/* This event is raised when a key is pressed or released. */
	Keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_event_keyboard_key *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			kb->device->keyboard->xkb_state, keycode, &syms);

	int handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->device->keyboard);

	wlr_idle_notify_activity(idle, seat);

	/* If lock is set, blocking sending keyboard messages to clients  */
	if (Lock.set && event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
	{
		char key[8];
    lockstatechange(Entering); // If button pressed - allowing drawing

		/* On pressing Enter - checking */
		if (syms[0] == XKB_KEY_Return)
		{
			Lock.password[Lock.len] = '\0';

			if (!strcmp(crypt(Lock.password, codesalt), codetounlock))
				lockdeactivate();
			else
				lockstatechange(Error);

			Lock.password[0] = '\0';
			Lock.len = 0;
		} else /* On pressing backspace - removing last symbol */
			if (syms[0] == XKB_KEY_BackSpace)
			{
				if (Lock.len > 0)
				{
					Lock.len -= 1;
					Lock.password[Lock.len] = '\0';
				}

				if (Lock.len == 0)
					lockstatechange(Clear);
			} else
				if (xkb_keysym_to_utf8(*syms, key, 8) == 2)
				{
					if (Lock.len < 255)
					{
						Lock.password[Lock.len] = key[0];
						Lock.len += 1;
					}
				}
		return;
	}

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!input_inhibit_mgr->active_inhibitor
			&& event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (i = 0; i < nsyms; i++)
			handled = keybinding(mods, syms[i]) || handled;


	if (!handled) {
		/* Pass unhandled keycodes along to the client. */
		wlr_seat_set_keyboard(seat, kb->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	Keyboard *kb = wl_container_of(listener, kb, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(seat, kb->device);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
		&kb->device->keyboard->modifiers);
}

void
killclient(const Arg *arg)
{
	Client *sel = selclient();
	if (sel)
    client_send_close(sel);
}

void
maplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, map);
	wlr_surface_send_enter(l->layer_surface->surface, l->mon->wlr_output);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p, *c = wl_container_of(listener, c, map);
	int i;

  c->ishidden = false;
	/* Create scene tree for this client and its border */
	c->scene = &wlr_scene_tree_create(layers[LyrTile])->node;
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	if (client_surface(c)) {
		client_surface(c)->data = c->scene;
		/* Ideally we should do this in createnotify{,x11} but at that moment
		* wlr_xwayland_surface doesn't have wlr_surface yet
		*/
		LISTEN(&client_surface(c)->events.commit, &c->commit, commitnotify);
	}
	c->scene->data = c->scene_surface->data = c;

#ifdef XWAYLAND
	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		client_get_geometry(c, &c->geom);
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(c->scene, layers[LyrFloat]);
		wlr_scene_node_set_position(c->scene, c->geom.x + borderpx,
			c->geom.y + borderpx);
		return;
	}
#endif

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
		c->border[i]->node.data = c;
		wlr_scene_rect_set_color(c->border[i], bordercolor);
	}

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	client_get_geometry(c, &c->geom);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	if ((p = client_get_parent(c)) && client_is_mapped(p)) {
		c->isfloating = 1;
		wlr_scene_node_reparent(c->scene, layers[LyrFloat]);
		setmon(c, p->mon, p->tags);
	} else {
		applyrules(c);
	}

	if (c->isfullscreen)
		setfullscreen(c, 1);

	c->mon->un_map = 1;

	/* Updating list of applications on bar */
  updateclientonbar(c);
}

void
monocle(Monitor *m)
{
	unsigned int i, n = 0, h, mw, my, ty;
	unsigned int mh, mx, tx;
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
      resize(c, (struct wlr_box){
        .x = m->w.x + mw,
        .y = MAX(m->w.y, barheight) - barheight, // , barheight
        .width = m->w.width - mw,
        .height = MIN((m->w.height - ty),  m->w.height - barheight)
      }, 0);

      tx += c->geom.width - barheight;
	}
	focusclient(focustop(m), 1);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct wlr_event_pointer_motion_absolute *event = data;
	double lx, ly, dx, dy;
	wlr_cursor_absolute_to_layout_coords(cursor, event->device, event->x, event->y, &lx, &ly);

	dx = lx - cursor->x;
	dy = ly - cursor->y;

	motionnotify(event->time_msec, event->device, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	double sx = 0, sy = 0;
	double sx_confined, sy_confined;
  LayerSurface *l;
	struct wlr_surface *surface = NULL;
	struct wlr_drag_icon *icon;
	Client *c = NULL;
	struct wlr_pointer_constraint_v1 *constraint = NULL;

	// time is 0 in internal calls meant to restore pointer focus.
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint) {
			constraint = active_constraint->constraint;
			if (
          constraint->surface == surface
					&& wlr_region_confine(
            &active_confine, sx, sy, sx + dx, sy + dy, &sx_confined, &sy_confined
          )
      ) 
      {
				dx = sx_confined - sx;
				dy = sy_confined - sy;
			} else 
      {
        /* In a lot of games (like League of Legends and Phasmophobia there is an
         * active cursor, that must be moved. Here we moving it, but according to
         * available window border.*/
        if (!cursor_is_hidden_in_games)
        {
          Client *c = client_from_wlr_surface(constraint->surface);
          sx = CLAMP(cursor->x + dx, c->geom.x, c->geom.x + c->geom.width);
          sy = CLAMP(cursor->y + dy, c->geom.y, c->geom.y + c->geom.height);
          wlr_cursor_move(cursor, device, sx - cursor->x, sy - cursor->y);
        }
				return;
			}
		}
		wlr_cursor_move(cursor, device, dx, dy);

		wlr_idle_notify_activity(idle, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}

	if (seat->drag && (icon = seat->drag->icon))
		wlr_scene_node_set_position(icon->data, cursor->x + icon->surface->sx,
				cursor->y + icon->surface->sy);
	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		resize(grabc, (struct wlr_box){.x = cursor->x - grabcx, .y = cursor->y - grabcy,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurResize) {
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = cursor->x - grabc->geom.x, .height = cursor->y - grabc->geom.y}, 1);
		return;
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag) {
		if ((l = toplevel_from_wlr_layer_surface(
				 seat->pointer_state.focused_surface))) {
			surface = seat->pointer_state.focused_surface;
			sx = cursor->x - l->geom.x;
			sy = cursor->y - l->geom.y;
		}
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && (!cursor_image || strcmp(cursor_image, "left_ptr")))
		wlr_xcursor_manager_set_cursor_image(cursor_mgr, (cursor_image = "left_ptr"), cursor);

	pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_event_pointer_motion *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, event->device, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc))
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = arg->ui) {
	case CurMove:
		grabcx = cursor->x - grabc->geom.x;
		grabcy = cursor->y - grabc->geom.y;
		wlr_xcursor_manager_set_cursor_image(cursor_mgr, (cursor_image = "fleur"), cursor);
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_xcursor_manager_set_cursor_image(cursor_mgr,
				(cursor_image = "bottom_right_corner"), cursor);
		break;
	}
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration.  This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	/* First disable outputs we need to disable */
	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		if (!wlr_output->enabled || config_head->state.enabled)
			continue;
		wlr_output_enable(wlr_output, 0);
		if (test) {
			ok &= wlr_output_test(wlr_output);
			wlr_output_rollback(wlr_output);
		} else {
			ok &= wlr_output_commit(wlr_output);
		}
	}

	/* Then enable outputs that need to */
	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		if (!config_head->state.enabled)
			continue;

		wlr_output_enable(wlr_output, 1);
		if (config_head->state.mode)
			wlr_output_set_mode(wlr_output, config_head->state.mode);
		else
			wlr_output_set_custom_mode(wlr_output,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		/* Don't move monitors if position wouldn't change, this to avoid
		 * wlroots marking the output as manually configured */
		if (m->m.x != config_head->state.x || m->m.y != config_head->state.y)
			wlr_output_layout_move(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);
		wlr_output_set_transform(wlr_output, config_head->state.transform);
		wlr_output_set_scale(wlr_output, config_head->state.scale);

		if (test) {
			ok &= wlr_output_test(wlr_output);
			wlr_output_rollback(wlr_output);
		} else {
			int output_ok = 1;
			/* If it's a custom mode to avoid an assertion failed in wlr_output_commit()
			 * we test if that mode does not fail rather than just call wlr_output_commit().
			 * We do not test normal modes because (at least in my hardware (@sevz17))
			 * wlr_output_test() fails even if that mode can actually be set */
			if (!config_head->state.mode)
				ok &= (output_ok = wlr_output_test(wlr_output)
						&& wlr_output_commit(wlr_output));
			else
				ok &= wlr_output_commit(wlr_output);

			/* In custom modes we call wlr_output_test(), it it fails
			 * we need to rollback, and normal modes seems to does not cause
			 * assertions failed in wlr_output_commit() which rollback
			 * the output on failure */
			if (!output_ok)
				wlr_output_rollback(wlr_output);
		}
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
pointerconstraintsetregion(struct wl_listener *listener, void *data)
{
	PointerConstraint *constraint = wl_container_of(listener, constraint, set_region);
	active_confine_requires_warp = 1;
	constraint->constraint->surface->data = NULL;
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;
	int internal_call = !time;

	/* Use top level surface if nothing more specific given */
	if (sloppyfocus && !internal_call && c && !client_is_unmanaged(c))
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (internal_call) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
printstatus(void)
{
	Monitor *m = NULL;
	Client *c;
	unsigned int occ, urg, sel;

	wl_list_for_each(m, &mons, link) {
		occ = urg = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		if ((c = focustop(m))) {
			printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
			printf("%s fullscreen %u\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %u\n", m->wlr_output->name, c->isfloating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
		printf("%s tags %u %u %u %u\n", m->wlr_output->name, occ, m->tagset[m->seltags],
				sel, urg);
		printf("%s layout %s\n", m->wlr_output->name, m->lt[m->sellt]->symbol);
	}
}

void
quit(const Arg *arg)
{
  int i = 0;
  /* Stoping autorun apps */
  for (i = 0; i < autorun_amount; i++)
  {
    if (autorun_app[i] > 0)
    {
      kill(autorun_app[i], SIGTERM);
      waitpid(autorun_app[i], NULL, 0);
    }
  }
	wl_display_terminate(dpy);
}

void
quitsignal(int signo)
{
	quit(NULL);
}

void
initbarrendering(Monitor *m)
{
	/*
	 * This function renders a text line via pango/cario, which will be rendered in renderbar
	 * It must be called once at the start, to init metrics and cairo surfaces
	 */
	int tags_amount, position = 0;
	int symbol_width = barheight;
  struct wlr_fbox back_box = {0, 0, 0, 748};
	PangoLayout *layout;
	PangoFontMetrics *metrics;
	PangoContext *context;
	cairo_surface_t *surface;
	cairo_t *cairo;
	int text_height = barheight / 2 - barfontsize / 2;
  int layouts_amount = LENGTH(layouts);
  int max_layouts_size = 0;

	/* Tags init */
	tags_amount = sizeof(tags) / sizeof(tags[0]);

  m->bar.tags_info = ecalloc(tags_amount, sizeof(Tag));

	/* Getting background image */
  if (!background_image_surface)
    background_image_surface = load_image(backgroundimage);

  if (background_image_surface)
  {
    back_box.width = cairo_image_surface_get_width(background_image_surface);
    back_box.height = cairo_image_surface_get_height(background_image_surface);
    m->background_image.buffer = readonly_data_buffer_create(
        DRM_FORMAT_ARGB8888,
        cairo_image_surface_get_stride(background_image_surface),
        cairo_image_surface_get_width(background_image_surface),
        cairo_image_surface_get_height(background_image_surface),
        cairo_image_surface_get_data(background_image_surface)
    );
    m->background_scene = &wlr_scene_tree_create(layers[LyrBg])->node;

    m->background_image.data = wlr_scene_buffer_create(m->background_scene, (struct wlr_buffer*)m->background_image.buffer);
    wlr_scene_buffer_set_source_box(m->background_image.data, &back_box);
    wlr_scene_buffer_set_dest_size(m->background_image.data, m->m.width, m->m.height);
    m->background_image.data->node.data = NULL;
    MOVENODE(m, &m->background_image.data->node, 0, 748);
  }

	/*  Init surfaces and cairo for bar */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m->w.width, barheight);
	cairo = cairo_create(surface);

	/* Creating font description */
	pango_font_description = pango_font_description_from_string(barfontname);
	pango_font_description_set_absolute_size(pango_font_description, barfontsize * PANGO_SCALE);

	/* updating draw  */
	layout = pango_cairo_create_layout(cairo);
	pango_layout_set_font_description(layout, pango_font_description);
	cairo_set_source_rgba(cairo, barfontcolor[0], barfontcolor[1], barfontcolor[2], barfontcolor[3]);

	/* getting metrics */
	context = pango_layout_get_context(layout);
	metrics = pango_context_get_metrics(context, pango_font_description, pango_language_get_default());
	symbol_width = pango_font_metrics_get_approximate_char_width(metrics) / PANGO_SCALE;
	pango_font_metrics_unref(metrics);

	pango_font_symbol_width = symbol_width;

	/* Drawing tags list */
	tags_amount = LENGTH(tags);
	for (int i = 0; i < tags_amount; i++)
	{
		m->bar.tags_info[i].left = position;
		m->bar.tags_info[i].right = position += symbol_width * strlen(tags[i]) + barfontsize;
		cairo_move_to(cairo, m->bar.tags_info[i].left + barfontsize / 2, text_height);
		pango_layout_set_text(layout, tags[i], -1);
		pango_cairo_update_layout(cairo, layout);
		pango_cairo_show_layout (cairo, layout);
	}

	m->bar.tags_border = position;

	g_object_unref (layout);
  cairo_destroy(cairo);
  
  /* Zeroing every pointer */
  for (int i = 0; i < NUM_BAR_SCENES; i++)
  {
    m->bar.scenes[i] = 0;
  }
  for (int i = 0; i < NUM_BAR_RECTS; i++)
  {
    m->bar.rects[i] = 0;
  }
  for (int i = 0; i < NUM_BAR_TEXTS; i++)
  {
    nullsurface(&m->bar.texts[i]);
  }
  m->bar.status_text_hash = 0;

  /* Scenes creating */
  m->bar.scenes[BarBottomScene] = &wlr_scene_tree_create(layers[LyrBarBottom])->node;
  m->bar.scenes[BarTopScene] = &wlr_scene_tree_create(layers[LyrBarTop])->node;

  /* Tray bar pre-render */
  m->bar.tray.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, barheight*10, barheight);
  m->bar.tray.buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(m->bar.tray.surface),
      cairo_image_surface_get_width(m->bar.tray.surface),
      cairo_image_surface_get_height(m->bar.tray.surface),
      cairo_image_surface_get_data(m->bar.tray.surface)
  );
  m->bar.tray.data = wlr_scene_buffer_create(m->bar.scenes[BarTopScene], (struct wlr_buffer*)m->bar.tray.buffer);
  wlr_scene_node_set_enabled(&m->bar.tray.data->node, false);
  m->bar.applications_amount = 0;

  /* Text rendering */
  m->bar.texts[BarTagsText].surface = surface;
  m->bar.texts[BarTagsText].buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(surface),
      cairo_image_surface_get_width(surface),
      cairo_image_surface_get_height(surface),
      cairo_image_surface_get_data(surface)
  );
  m->bar.texts[BarTagsText].data = wlr_scene_buffer_create(m->bar.scenes[BarTopScene], (struct wlr_buffer*)m->bar.texts[BarTagsText].buffer);
  m->bar.texts[BarTagsText].data->node.data = NULL;
  MOVENODE(m, &m->bar.texts[BarTagsText].data->node, 0, 748);

  /* Layout sign rendering*/
  m->bar.layout_signs = ecalloc(layouts_amount, sizeof(Surface));
  for (int i = 0, current_width = 0; i < layouts_amount; i++)
  {
    current_width = (strlen(layouts[i].symbol) + 1) * pango_font_symbol_width;
    rendertextonsurface(m, &m->bar.layout_signs[i], layouts[i].symbol, current_width, barheight);
    MOVENODE(m, &m->bar.layout_signs[i].data->node, m->bar.tags_border, 748);

    max_layouts_size = MAX(current_width, max_layouts_size);

    if (i != 0)
    {
      wlr_scene_node_set_enabled(&m->bar.layout_signs[i].data->node, false);
    }
  }
	m->bar.layout_symbol_border = m->bar.tags_border + max_layouts_size;

  /* Rect renderings */
  // Base bar
  m->bar.rects[BarSubstraceRect] = wlr_scene_rect_create(m->bar.scenes[BarBottomScene], m->w.width, barheight, barbackcolor);
  m->bar.rects[BarSubstraceRect]->node.data = NULL;
  MOVENODE(m, &m->bar.rects[BarSubstraceRect]->node, 0, 748);
  
  // Undertag rect
  m->bar.rects[BarUndertagsRect] = wlr_scene_rect_create(
    m->bar.scenes[BarBottomScene],
    m->bar.layout_symbol_border,
    barheight,
    barbackcolor
  );
  m->bar.rects[BarUndertagsRect]->node.data = NULL;
  MOVENODE(m, &m->bar.rects[BarUndertagsRect]->node, 0, 748);

  // Status rect
  m->bar.rects[BarInfoRect] = wlr_scene_rect_create(m->bar.scenes[BarBottomScene], m->bar.status_border, barheight, barbackcolor);
  m->bar.rects[BarInfoRect]->node.data = NULL;
  MOVENODE(m, &m->bar.rects[BarInfoRect]->node, m->m.width - m->bar.status_border, 748);

  // Bar for selected tag
  m->bar.rects[BarSelectedTagRect] = wlr_scene_rect_create(m->bar.scenes[BarBottomScene], m->bar.tags_info[0].right, barheight, barcolor);
  m->bar.rects[BarSelectedTagRect]->node.data = NULL;
  MOVENODE(m, &m->bar.rects[BarSelectedTagRect]->node, 0, 748);

  // Bar for focused client on bar
  m->bar.rects[BarFocusedClientRect] = wlr_scene_rect_create(m->bar.scenes[BarBottomScene], 0, barheight, barcolor);
  m->bar.rects[BarFocusedClientRect]->node.data = NULL;
  wlr_scene_node_set_enabled(&m->bar.rects[BarFocusedClientRect]->node, false);

  /* Clients on tag rects */
	for (int i = 0, w = 0, h = 0, w_e; i < tags_amount; i++)
	{
    w = (int)( m->bar.tags_info[i].right - m->bar.tags_info[i].left ) * 0.8;
    w_e = (int)(m->bar.tags_info[i].right - m->bar.tags_info[i].left - w) / 2;
    h = (int)MAX(3, barheight / 4);

    m->bar.tags_info[i].has_clients_rect = wlr_scene_rect_create(m->bar.scenes[BarBottomScene], w, h, baractivecolor);
    m->bar.tags_info[i].has_clients_not_focused_rect = wlr_scene_rect_create(
      m->bar.scenes[BarBottomScene],
      w - 2,
      h - 2,
      barbackcolor
    );

    MOVENODE(m, &m->bar.tags_info[i].has_clients_rect->node, m->bar.tags_info[i].left + w_e, 748 );
    MOVENODE(
      m,
      &m->bar.tags_info[i].has_clients_not_focused_rect->node,
      m->bar.tags_info[i].left + w_e + 1,
      1
    );
    wlr_scene_node_set_enabled(&m->bar.tags_info[i].has_clients_rect->node, false);
    wlr_scene_node_set_enabled(&m->bar.tags_info[i].has_clients_not_focused_rect->node, false);
  }

  /* Status text rendering */
  setstatustext(m, edwl_version);

  wl_list_init(&m->bar.clients);
}

void
lockstatechange(enum LockCircleDrawState state)
{
  Lock.state = state;

  for (int i = 0; i < NUM_LOCK_IMAGES; i++)
  {
    wlr_scene_node_set_enabled(&Lock.images[i].data->node, false);
  }

  // TODO Check MM problem
  switch (state)
  {
    case Nothing:
      break;
    case Clear:
      wlr_scene_node_set_enabled(&Lock.images[LockImageClean].data->node, true);
      MOVENODE(selmon, &Lock.images[LockImageClean].data->node, ( selmon->m.width - Lock.width ) / 2, ( selmon->m.height  - Lock.height ) / 2);
      break;
    case Error:
      wlr_scene_node_set_enabled(&Lock.images[LockImageWrong].data->node, true);
      MOVENODE(selmon, &Lock.images[LockImageWrong].data->node, ( selmon->m.width - Lock.width ) / 2, ( selmon->m.height  - Lock.height ) / 2);
      break;
    case Entering:
      wlr_scene_node_set_enabled(&Lock.images[Lock.len % 2 == 0 ? LockImageEnter1 : LockImageEnter2].data->node, true);
      MOVENODE(selmon, &Lock.images[Lock.len % 2 == 0 ? LockImageEnter1 : LockImageEnter2].data->node, ( selmon->m.width - Lock.width ) / 2, ( selmon->m.height  - Lock.height ) / 2);
  }
}

void
lockactivate()
{
  Lock.set = 1;
  lockstatechange(Nothing);
  for (int i = LyrBarBottom; i < LyrLock; i++)
  {
    wlr_scene_node_set_enabled(layers[i], false);
  }
}

void
lockdeactivate()
{
  Lock.set = 0;
  lockstatechange(Disabled);
  for (int i = LyrBarBottom; i < LyrLock; i++)
  {
    wlr_scene_node_set_enabled(layers[i], true);
  }
}

void
rendertextonsurface(Monitor *m, Surface *surface, const char *text, int width, int height)
{
	PangoLayout *layout = NULL;
	cairo_t *cairo = NULL;
	int text_height = barheight / 2 - barfontsize / 2;

  if (surface->surface == NULL)
    surface->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  else
  {
    if (width != cairo_image_surface_get_width(surface->surface) || height != cairo_image_surface_get_height(surface->surface))
    {
      cairo_surface_destroy(surface->surface);
      surface->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height); 
    }
  }

	cairo = cairo_create(surface->surface);

  /* Clear */
  cairo_surface_flush(surface->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_rectangle(cairo, 0, 0, width, height);
	cairo_fill(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);

  if (text && strlen(text) > 0)
  {
    /* Setup */
    layout = pango_cairo_create_layout(cairo);
    pango_layout_set_font_description(layout, pango_font_description);
    cairo_set_source_rgba(cairo, barfontcolor[0], barfontcolor[1], barfontcolor[2], barfontcolor[3]);

    /* Rendering */
    cairo_move_to(cairo, barfontsize / 2, text_height);
    pango_layout_set_text(layout, text, -1);
    pango_cairo_update_layout(cairo, layout);
    pango_cairo_show_layout (cairo, layout);
  }

  /* Clearing old data */
  if (surface->data)
  {
    wlr_scene_node_destroy(&surface->data->node);
    readonly_data_buffer_drop(surface->buffer);
  }

  /* Move to buffer */
  surface->buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(surface->surface),
      cairo_image_surface_get_width(surface->surface),
      cairo_image_surface_get_height(surface->surface),
      cairo_image_surface_get_data(surface->surface)
  );

  /* Packing */
  surface->data = wlr_scene_buffer_create(m->bar.scenes[BarTopScene], (struct wlr_buffer*)surface->buffer);
  surface->data->node.data = NULL;
  wlr_scene_node_raise_to_top(&surface->data->node);

  if (layout)
    g_object_unref (layout);
  cairo_destroy(cairo);
}

void
rendertray(Monitor *m)
{
	cairo_t *cairo;

	if (tray && tray->surface)
	{
    cairo = cairo_create(m->bar.tray.surface);

		/* clear surface from previous use */
		cairo_surface_flush(m->bar.tray.surface);
		cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
		cairo_rectangle(cairo, 0, 0, 10*barheight, barheight);
		cairo_fill(cairo);

    if (tray->applications_amount)
    {
      /* clear prev buffer */
      wlr_scene_node_destroy(&m->bar.tray.data->node);
      readonly_data_buffer_drop(m->bar.tray.buffer);

      /* Drawing icons */
      cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
      cairo_set_source_surface(cairo, tray->surface, 0, 0);
      cairo_paint(cairo);

      /* Packing */
      m->bar.tray.buffer = readonly_data_buffer_create(
          DRM_FORMAT_ARGB8888,
          cairo_image_surface_get_stride(m->bar.tray.surface),
          cairo_image_surface_get_width(m->bar.tray.surface),
          cairo_image_surface_get_height(m->bar.tray.surface),
          cairo_image_surface_get_data(m->bar.tray.surface)
      );
      m->bar.tray.data = wlr_scene_buffer_create(m->bar.scenes[BarTopScene], (struct wlr_buffer*)m->bar.tray.buffer);
      MOVENODE(m, &m->bar.tray.data->node, m->m.width - tray->applications_amount * barheight, 748);
      wlr_scene_node_set_enabled(&m->bar.tray.data->node, true);
      wlr_scene_node_raise_to_top(&m->bar.tray.data->node);
    } 
    else
    {
      wlr_scene_node_set_enabled(&m->bar.tray.data->node, false);
    }

    /* Updating status bar position and clients on bar */
    m->bar.status_border = (m->bar.status_border - m->bar.applications_amount * barheight) + barheight * tray->applications_amount;
    MOVENODE(m, &m->bar.texts[BarStatusText].data->node, m->m.width - m->bar.status_border, 748);

    m->bar.applications_amount = tray->applications_amount;
    cairo_destroy(cairo);

    updateclientbounds(m);
	}
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	Client *c;
	int skip = 0;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Checking m->un_map for every client is not optimal but works */
	wl_list_for_each(c, &clients, link) 
  {
		if ((c->resize && m->un_map) || (c->type == XDGShell
				&& (c->surface.xdg->pending.geometry.width !=
				c->surface.xdg->current.geometry.width
				|| c->surface.xdg->pending.geometry.height !=
				c->surface.xdg->current.geometry.height)))
    {
			/* Lie */
			wlr_surface_send_frame_done(client_surface(c), &now);
			skip = 1;
		}
	}
	if (!skip && !wlr_scene_output_commit(m->scene_output))
		return;
	/* Let clients know a frame has been rendered */
	wlr_scene_output_send_frame_done(m->scene_output, &now);
	m->un_map = 0;
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox = interact ? &sgeom : &c->mon->w;
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(c->scene, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(c->scene_surface, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);
	if (c->fullscreen_bg)
		wlr_scene_rect_set_size(c->fullscreen_bg, c->geom.width, c->geom.height);

	/* wlroots makes this a no-op if size hasn't changed */
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
}

void
renderlockimages()
{
	/* Here we drawing circles that will be displayed on monitor while entering password */
	PangoLayout *layout;
	PangoFontMetrics *metrics;
	PangoContext *context;
	PangoFontDescription *font;
	cairo_surface_t *image;
	cairo_t *cairo;
	int symbol_width = barheight;
	int border_size = 2;

  Lock.scene = &wlr_scene_tree_create(layers[LyrLock])->node;

	font = pango_font_description_from_string(barfontname);
	pango_font_description_set_absolute_size(font, barfontsize * 2 * PANGO_SCALE);
	pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);

	Lock.images[LockImageEnter1].surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Lock.width, Lock.height);
	cairo = cairo_create(Lock.images[LockImageEnter1].surface);
	layout = pango_cairo_create_layout(cairo);
	pango_layout_set_font_description(layout, font);

	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);

	/** ==== Drawing circle #1 with a border ==== **/
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 1.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

	/* Drawing inner circle for #1 with a border */
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 1.0, 0.5, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);
  
  /* Writing text */
	cairo_move_to(cairo, Lock.width / 2 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 0.7);
	pango_layout_set_text(layout, "ENTERING", -1);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);
	cairo_move_to(cairo, Lock.width * 1.5 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);

	g_object_unref(layout);
	cairo_destroy(cairo);

  /* Packing */
  Lock.images[LockImageEnter1].buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(Lock.images[LockImageEnter1].surface),
      cairo_image_surface_get_width(Lock.images[LockImageEnter1].surface),
      cairo_image_surface_get_height(Lock.images[LockImageEnter1].surface),
      cairo_image_surface_get_data(Lock.images[LockImageEnter1].surface)
  );
  Lock.images[LockImageEnter1].data = wlr_scene_buffer_create(Lock.scene, (struct wlr_buffer*)Lock.images[LockImageEnter1].buffer);
  wlr_scene_node_set_enabled(&Lock.images[LockImageEnter1].data->node, false);

	/** ==== Drawing circle #2 with a border ==== **/
	Lock.images[LockImageEnter2].surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Lock.width, Lock.height);
	cairo = cairo_create(Lock.images[LockImageEnter2].surface);
	layout = pango_cairo_create_layout(cairo);
	pango_layout_set_font_description(layout, font);

	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 1.0, 0.5, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

	/* Drawing inner circle for #2 with a border */
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width  / 2, Lock.height / 2, Lock.width * 0.45, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 1.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

  /* Writing text */
	cairo_move_to(cairo, Lock.width / 2 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 0.7);
	pango_layout_set_text(layout, "ENTERING", -1);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);
	cairo_move_to(cairo, Lock.width * 1.5 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);

	g_object_unref(layout);
	cairo_destroy(cairo);

  /* Packing */
  Lock.images[LockImageEnter2].buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(Lock.images[LockImageEnter2].surface),
      cairo_image_surface_get_width(Lock.images[LockImageEnter2].surface),
      cairo_image_surface_get_height(Lock.images[LockImageEnter2].surface),
      cairo_image_surface_get_data(Lock.images[LockImageEnter2].surface)
  );
  Lock.images[LockImageEnter2].data = wlr_scene_buffer_create(Lock.scene, (struct wlr_buffer*)Lock.images[LockImageEnter2].buffer);
  wlr_scene_node_set_enabled(&Lock.images[LockImageEnter2].data->node, false);
	layout = pango_cairo_create_layout(cairo);
	pango_layout_set_font_description(layout, font);

	/** ==== Drawing clear circle with a border ==== **/
	Lock.images[LockImageClean].surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Lock.width, Lock.height);
	cairo = cairo_create(Lock.images[LockImageClean].surface);

	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 1.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

	/* Drawing inner circle for clear cirlce for with a border */
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 0.1, 0.1, 1.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

  /* Writing text */
	cairo_move_to(cairo, Lock.width / 2 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 0.7);
	pango_layout_set_text(layout, "CLEAR", -1);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);
	cairo_move_to(cairo, Lock.width * 1.5 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);

	g_object_unref(layout);
	cairo_destroy(cairo);

  /* Packing */
  Lock.images[LockImageClean].buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(Lock.images[LockImageClean].surface),
      cairo_image_surface_get_width(Lock.images[LockImageClean].surface),
      cairo_image_surface_get_height(Lock.images[LockImageClean].surface),
      cairo_image_surface_get_data(Lock.images[LockImageClean].surface)
  );
  Lock.images[LockImageClean].data = wlr_scene_buffer_create(Lock.scene, (struct wlr_buffer*)Lock.images[LockImageClean].buffer);
  wlr_scene_node_set_enabled(&Lock.images[LockImageClean].data->node, false);

	/** ==== Drawing error circle with a border ==== **/
	Lock.images[LockImageWrong].surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Lock.width, Lock.height);
	cairo = cairo_create(Lock.images[LockImageWrong].surface);
	layout = pango_cairo_create_layout(cairo);
	pango_layout_set_font_description(layout, font);

	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 1.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width / 2 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

	/* Drawing inner circle for error cirlce for with a border */
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45, 0.0, 2 * 3.14);
	cairo_fill(cairo);
	cairo_set_source_rgba(cairo, 1.0, 0.1, 0.1, 1.0);
	cairo_arc(cairo, Lock.width / 2, Lock.height / 2, Lock.width * 0.45 - border_size, 0.0, 2 * 3.14);
	cairo_fill(cairo);

  /* Writing text */
	cairo_move_to(cairo, Lock.width / 2 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 0.7);
	pango_layout_set_text(layout, "ERROR", -1);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);
	cairo_move_to(cairo, Lock.width * 1.5 - 4 * symbol_width + border_size, Lock.height / 2 - barheight / 2 + border_size);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout (cairo, layout);

	g_object_unref(layout);
	cairo_destroy(cairo);

  /* Packing */
  Lock.images[LockImageWrong].buffer = readonly_data_buffer_create(
      DRM_FORMAT_ARGB8888,
      cairo_image_surface_get_stride(Lock.images[LockImageWrong].surface),
      cairo_image_surface_get_width(Lock.images[LockImageWrong].surface),
      cairo_image_surface_get_height(Lock.images[LockImageWrong].surface),
      cairo_image_surface_get_data(Lock.images[LockImageWrong].surface)
  );
  Lock.images[LockImageWrong].data = wlr_scene_buffer_create(Lock.scene, (struct wlr_buffer*)Lock.images[LockImageWrong].buffer);
  wlr_scene_node_set_enabled(&Lock.images[LockImageWrong].data->node, false);

}

void
run(char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
  autostart();
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}
	/* If nobody is reading the status output, don't terminate */
	signal(SIGPIPE, SIG_IGN);
	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping.  still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_xcursor_manager_set_cursor_image(cursor_mgr, cursor_image, cursor);

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

void
screenlock(const Arg *)
{
	lockactivate(); // don't draw anything before users toches keyboard
}

Client *
selclient(void)
{
	Client *c = wl_container_of(fstack.next, c, flink);
	if (wl_list_empty(&fstack) || !VISIBLEON(c, selmon))
		return NULL;
	return c;
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal)
		return;
	cursor_image = NULL;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setfloating(Client *c, int floating)
{
	c->isfloating = floating;
	wlr_scene_node_reparent(c->scene, layers[c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
}

void
setkblayout(Keyboard *kb, const struct xkb_rule_names *newrule)
{
	/* Prepare an XKB keymap and assign it to the keyboard. */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, newrule,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(kb->device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

void
setlayout(const Arg *arg)
{
	if (!selmon)
		return;
  if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
    selmon->sellt ^= 1;
  if (arg && arg->v)
    selmon->lt[selmon->sellt] = (Layout *)arg->v;

  for (int i = 0; i < LENGTH(layouts); i++)
  {
    if (!strcmp(selmon->lt[selmon->sellt]->symbol, layouts[i].symbol))
    {
      selmon->layout_number = i;
      wlr_scene_node_set_enabled(&selmon->bar.layout_signs[i].data->node, true);
    }
    else
    {
      wlr_scene_node_set_enabled(&selmon->bar.layout_signs[i].data->node, false);
    }
  }

  arrange(selmon);
  printstatus();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
setmon(Client *c, Monitor *m, unsigned int newtags)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* TODO leave/enter is not optimal but works */
	if (oldmon) {
		wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
		arrange(oldmon);
	}
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		wlr_surface_send_enter(client_surface(c), m->wlr_output);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
	}
	focusclient(focustop(selmon), 1);

  if (m)
  {
    updateclientonbar(c);
  }
}

void
setstatustext(Monitor *m, const char *text)
{
  int text_width;
	int text_height = barheight / 2 - barfontsize / 2;
  unsigned int hash = djb2hash(text);

  if (m->bar.status_text_hash == hash)
    return;

  // TODO Adjust text length calculcation
  text_width = text ? (strlen(text) + 1) * pango_font_symbol_width : pango_font_symbol_width;
  rendertextonsurface(m, &m->bar.texts[BarStatusText], text, m->m.width / 2, barheight);
  m->bar.status_border = text_width + barheight * m->bar.applications_amount;

  MOVENODE(m, &m->bar.texts[BarStatusText].data->node, m->m.width - m->bar.status_border, 748);
  wlr_scene_rect_set_size(m->bar.rects[BarInfoRect], text_width, barheight);
  MOVENODE(m, &m->bar.rects[BarInfoRect]->node, m->m.width - text_width, 748);

  m->bar.status_text_hash = hash;

  /* Raise tray icons on top */
  wlr_scene_node_raise_to_top(&m->bar.tray.data->node);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void
setup(void)
{
	struct wl_event_loop *event_loop = NULL;

	/* Force line-buffered stdout */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();

	/* Set up signal handlers */
	sigchld(0);
	signal(SIGINT, quitsignal);
	signal(SIGTERM, quitsignal);
  signal(SIGSEGV, sigfaulthandler);

	/*  Running dbus system init */
	event_loop = wl_display_get_event_loop(dpy);
	tray = tray_init(barheight, event_loop);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. The NULL argument here optionally allows you
	 * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
	 * backend uses the renderer, for example, to fall back to software cursors
	 * if the backend does not support hardware cursors (some older GPUs
	 * don't). */
	if (!(backend = wlr_backend_autocreate(dpy)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	layers[LyrBg] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrBarBottom] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrBarTop] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrBottom] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrTile] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrFloat] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrTop] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrOverlay] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrNoFocus] = &wlr_scene_tree_create(&scene->node)->node;
	layers[LyrLock] = &wlr_scene_tree_create(&scene->node)->node;

	/* Create a renderer with the default implementation */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	wlr_renderer_init_wl_display(drw, dpy);

	/* Create a default allocator */
	if (!(alc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, drw);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_gamma_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create();
	wl_signal_add(&output_layout->events.change, &layout_change);
	wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists and the xdg-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);

	idle = wlr_idle_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &idle_inhibitor_create);

	layer_shell = wlr_layer_shell_v1_create(dpy);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_shell_surface);

	xdg_shell = wlr_xdg_shell_create(dpy);
	wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);

	input_inhibit_mgr = wlr_input_inhibit_manager_create(dpy);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wlr_xdg_decoration_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&keyboards);
	wl_signal_add(&backend->events.new_input, &new_input);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);
	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);
	wl_list_init(&pointer_constraint_commit.link);

	wlr_scene_set_presentation(scene, wlr_presentation_create(dpy, backend));
	/*  Setting up pango font settings  */
  pango_font_description = pango_font_description_from_string(barfontname);
  pango_font_description_set_absolute_size(pango_font_description, barfontsize * PANGO_SCALE);

  /* Setup lock */
  Lock.password[0] = '\0';
  Lock.state = Disabled;
  Lock.width = 300;
  Lock.height = 300;
  Lock.len = 0;
  Lock.set = 0;
  renderlockimages();
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	xwayland = wlr_xwayland_create(dpy, compositor, 1);
	if (xwayland) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		/*
		 * Create the XWayland cursor manager at scale 1, setting its default
		 * pointer to match the rest of dwl.
		 */
		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		fprintf(stderr, "failed to setup XWayland X server, continuing without it\n");
	}
#endif
}

void
sigchld(int unused)
{
	/* We should be able to remove this function in favor of a simple
	 *     signal(SIGCHLD, SIG_IGN);
	 * but the Xwayland implementation in wlroots currently prevents us from
	 * setting our own disposition for SIGCHLD.
	 */
	pid_t pid;

	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		EBARF("can't install SIGCHLD handler");
	while (0 < (pid = waitpid(-1, NULL, WNOHANG)))
  {
    pid_t *p, *lim;
    if (pid == child_pid)
      child_pid = -1;
    if (!(p = autorun_app))
      continue;

    lim = &p[autorun_amount];
    for (;p < lim; p++)
    {
      if (*p == pid)
      {
        *p = -1;
        break;
      }
    }
  }
}

void
sigfaulthandler(int sig)
{
  void *array[10];
  size_t size;

  size = backtrace(array, 10);
  fprintf(stderr, "Exception: %d\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwl: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;

	if (!drag->icon)
		return;

	drag->icon->data = wlr_scene_subsurface_tree_create(layers[LyrNoFocus], drag->icon->surface);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_signal_add(&drag->icon->events.destroy, &drag_icon_destroy);
}

void
tag(const Arg *arg)
{
	Client *sel = selclient();
	if (sel && arg->ui & TAGMASK) {
		sel->tags = arg->ui & TAGMASK;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}

  updateclientbounds(selmon);
	printstatus();
}

void
tagmon(const Arg *arg)
{
	Client *sel = selclient();
	if (sel)
  {
    destroyclientonbar(sel);
    setmon(sel, dirtomon(arg->i), 0);
  }
}

void
tile(Monitor *m)
{
	unsigned int i, n = 0, h, mw, my, ty;
	unsigned int mh, mx, tx;
  int double_gaps = 2 * gapsize;
  int fullscreenon = 0;
	Client *c;

	wl_list_for_each(c, &clients, link)
  {
    if (VISIBLEON(c, m) && !c->isfloating && !c->ishidden)
    {
      // If there is a fullscreen client - no need to arrange anything
      if (c->isfullscreen)
        return;
      n++;
    }
  }
	if (n == 0)
		return;

  i = 0;
  mx = tx = gapsize;
  if (m->round)
  {
    if (n > m->nmaster)
      mw = m->nmaster ? m->w.width * m->mfact : 0;
    else
      mw = m->w.width;
    
    wl_list_for_each(c, &clients, link) {
      if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen || c->ishidden)
        continue;

      if (i < m->nmaster) {
        resize(c, (struct wlr_box){
          .x = m->w.x + gapsize,
          .y = MAX(m->w.y - my, gapsize),
          .width = mw - double_gaps,
          .height = MIN((m->w.height - my) / (MIN(n, m->nmaster) - i), m->w.height - barheight)  - double_gaps
        }, 0);
        
        mx += c->geom.width + gapsize;
      } else {
        resize(c, (struct wlr_box){
          .x = m->w.x + mw + gapsize,
          .y = MAX(m->w.y - ty, barheight + gapsize) - barheight,
          .width = m->w.width - mw - double_gaps,
          .height = MIN((m->w.height - ty) / (n - i),  m->w.height - barheight) - double_gaps
        }, 0);

        tx += c->geom.width + gapsize;
      }
      i++;
    }
  }
  else
  {
    if (n > m->nmaster)
      mh = m->nmaster ? m->w.height * m->mfact : 0;
    else
      mh = m->w.height;

    wl_list_for_each(c, &clients, link) {
      if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen || c->ishidden)
        continue;
      if (i < m->nmaster) {
        resize(c, (struct wlr_box){
          .x = m->w.x + mx + gapsize,
          .y = m->w.y + gapsize,
          .width = MIN((m->w.width - mx) / (MIN(n, m->nmaster) - i), m->w.width) - double_gaps,
          .height = mh - double_gaps - barheight
        }, 0);
        
        mx += c->geom.width + gapsize;
      } else {
        resize(c, (struct wlr_box){
          .x = m->w.x + tx + gapsize,
          .y = MAX(m->w.y - mh, barheight) + gapsize - barheight,
          .width = MIN((m->w.width - tx) / (n - i),  m->w.width) - double_gaps,
          .height = m->w.height - mh - double_gaps
        }, 0);

        tx += c->geom.width + gapsize;
      }
      i++;
    }
  }
}

void
togglefloating(const Arg *arg)
{
	Client *sel = selclient();
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
    setfloating(sel, !sel->isfloating);
}

void
togglekblayout(const Arg *arg)
{
	Keyboard *kb;
	struct xkb_rule_names newrule = xkb_rules;

  if (LENGTH(kblayouts) <= 1)
    return;

	kblayout = (kblayout + 1) % LENGTH(kblayouts);
	newrule.layout = kblayouts[kblayout];
	wl_list_for_each(kb, &keyboards, link) {
		setkblayout(kb, &newrule);
	}
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;
	Client *sel = selclient();
	if (!sel)
		return;
	newtags = sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		sel->tags = newtags;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}

  updateclientbounds(selmon);
	printstatus();
}

void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0;

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}

  updateclientbounds(selmon);
	printstatus();
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *layersurface = wl_container_of(listener, layersurface, unmap);

	layersurface->mapped = 0;
	wlr_scene_node_set_enabled(layersurface->scene, 0);
	if (layersurface == exclusive_focus)
		exclusive_focus = NULL;
	if (layersurface->layer_surface->output
			&& (layersurface->mon = layersurface->layer_surface->output->data))
		arrangelayers(layersurface->mon);
	if (layersurface->layer_surface->surface ==
			seat->keyboard_state.focused_surface)
		focusclient(selclient(), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	if (c->mon)
    c->mon->un_map = 1;

	if (!client_is_unmanaged(c)) {
    destroyclientonbar(c);
    updateclientbounds(c->mon);
		wl_list_remove(&c->link);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	}


	wl_list_remove(&c->commit.link);
	wlr_scene_node_destroy(c->scene);
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
updateclientbounds(Monitor *m)
{
  ClientOnBar *current;
  Client *c = selclient();
  struct wlr_fbox crop_box = {0, 0, 0, barheight};
  int clients_amount = 0;
  int clients_zone_size = m->m.width - m->bar.layout_symbol_border - m->bar.status_border;
  int width_for_each_client = 0;
  int current_point = m->bar.layout_symbol_border;
	int tags_amount = LENGTH(tags);
  int clients_on_seltag = 0;
  
  for (int current_tag = 0, current_tag_id = 1; current_tag < tags_amount; current_tag++)
  {
    clients_amount = 0;
    wl_list_for_each(current, &m->bar.clients, link)
    {
      if (m == current->c->mon && current->c->tags & current_tag_id)
        ++clients_amount;
    }

    if (clients_amount)
    {
      current_point = m->bar.layout_symbol_border;
      crop_box.width = clients_zone_size / clients_amount;

      /* Updating clients-on-tag rectangles */
      wlr_scene_node_set_enabled(&m->bar.tags_info[current_tag].has_clients_rect->node, true);
      wlr_scene_node_set_enabled(
        &m->bar.tags_info[current_tag].has_clients_not_focused_rect->node,
        (current_tag_id & TAGMASK) != m->tagset[m->seltags]
      );

      /* Updating clients on bar */
      wl_list_for_each(current, &m->bar.clients, link)
      {
        if (m == current->c->mon && current->c->tags & current_tag_id)
        {
          ++clients_on_seltag;
          current->box.x = current_point;
          current->box.width = crop_box.width;
          current->is_enabled = current->c->tags & m->tagset[m->seltags];
          current->is_focused = c == current->c;

          wlr_scene_buffer_set_source_box(current->surface.data, &crop_box);
          wlr_scene_buffer_set_dest_size(current->surface.data, crop_box.width, barheight);
          MOVENODE(m, &current->surface.data->node, current_point, 748);
          wlr_scene_node_set_enabled(&current->surface.data->node, current->is_enabled);

          current_point += crop_box.width;

          if (c == current->c)
          {
            wlr_scene_rect_set_size(
              m->bar.rects[BarFocusedClientRect],
              current->box.width,
              current->box.height
            );
            MOVENODE(m, &m->bar.rects[BarFocusedClientRect]->node, current->box.x, 748);
            wlr_scene_node_set_enabled(&m->bar.rects[BarFocusedClientRect]->node, true);
          }
        }
      }
    }
    else
    {
      wlr_scene_node_set_enabled(&m->bar.tags_info[current_tag].has_clients_rect->node, false);
      wlr_scene_node_set_enabled(&m->bar.tags_info[current_tag].has_clients_not_focused_rect->node, false);
    }

    current_tag_id = current_tag_id << 1;
  }

  wlr_scene_node_set_enabled(&m->bar.rects[BarFocusedClientRect]->node, c != NULL && c->mon == selmon);
}

void
updateclientonbar(Client *c)
{
  ClientOnBar *current = NULL;
  Monitor *m = c->mon;
  const char *title = NULL;
  unsigned long title_hash;
  int client_zone_size = m->m.width - m->bar.layout_symbol_border - m->bar.status_border;

  wl_list_for_each(current, &m->bar.clients, link)
  {
    if (c == current->c)
    {
      title = client_get_title(c);
      title_hash = djb2hash(title);

      if (title_hash == current->title_hash)
        return;

      current->title_hash = title_hash;
      rendertextonsurface(m, &current->surface, title ? title : "broken", client_zone_size, barheight);
      updateclientbounds(m);
      return;
    }
  }

  current = ecalloc(1, sizeof(*current));
  current->box.x = m->bar.layout_symbol_border;
  current->box.y = 0;
  current->box.width = m->m.width - m->bar.layout_symbol_border - m->bar.status_border;
  current->box.height = barheight;
  title = client_get_title(c);
  current->title_hash = djb2hash(title);
  current->c = c;
  c->isclientonbarset = true;
  rendertextonsurface(m, &current->surface, title ? title : "broken", client_zone_size, barheight);
  wl_list_insert(&m->bar.clients, &current->link);

  updateclientbounds(m);
}

void
updatebacks(struct wl_listener *listener, void *data)
{
	Monitor *m;
	Tray *tray = (Tray*)data;
	cairo_surface_t *surface = NULL;
  struct wlr_fbox back_box = {0, 0, 0, 0};

	surface = load_image(tray->background_path);
  back_box.width = cairo_image_surface_get_width(surface);
  back_box.height = cairo_image_surface_get_height(surface);

	if (surface)
	{
    wl_list_for_each(m, &mons, link) {
      if (m->background_image.data)
      {
        wlr_scene_node_destroy(&m->background_image.data->node);
        readonly_data_buffer_drop(m->background_image.buffer);
      }

      /* Move to buffer */
      m->background_image.buffer = readonly_data_buffer_create(
          DRM_FORMAT_ARGB8888,
          cairo_image_surface_get_stride(surface),
          back_box.width,
          back_box.height,
          cairo_image_surface_get_data(surface)
      );
      m->background_image.data = wlr_scene_buffer_create(m->background_scene, (struct wlr_buffer*)m->background_image.buffer);
      wlr_scene_buffer_set_source_box(m->background_image.data, &back_box);
      wlr_scene_buffer_set_dest_size(m->background_image.data, m->m.width, m->m.height);
      MOVENODE(m, &m->background_image.data->node, 0, 0);
    }
	}

  if (background_image_surface)
  {
    cairo_surface_destroy(background_image_surface);
    background_image_surface = surface;
  }
}

void
updatestatusmessage(struct wl_listener *listener, void *data)
{
	Monitor *m;
	Tray *tray = (Tray*)data;

	wl_list_for_each(m, &mons, link) {
    setstatustext(m, tray->status_message);
    updateclientbounds(m);
	}
}

void
updatetrayicons(struct wl_listener *listener, void *data)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		rendertray(m);
	}
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc.  This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	Client *c;
	Monitor *m;
	sgeom = *wlr_output_layout_get_box(output_layout, NULL);
	wl_list_for_each(m, &mons, link) {
		struct wlr_output_configuration_head_v1 *config_head =
			wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* TODO: move clients off disabled monitors */
		/* TODO: move focus if selmon is disabled */

		/* Get the effective monitor geometry to use for surfaces */
		m->w = m->m = *wlr_output_layout_get_box(output_layout, m->wlr_output);
		m->w.y += barheight;
		m->w.height -= barheight;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);
	
		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);

		config_head->state.enabled = m->wlr_output->enabled;
		config_head->state.mode = m->wlr_output->current_mode;
		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;
	}
	if (selmon && selmon->wlr_output->enabled)
		wl_list_for_each(c, &clients, link)
			if (!c->mon && client_is_mapped(c))
				setmon(c, selmon, c->tags);


	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	const char* title = client_get_title(c);

	if (c == focustop(c->mon))
		printstatus();

	if (title)
	{
		if (!strncmp(title, devourcommand, strlen(devourcommand)))
		{
			c->isdevoured = c->ishidden = 1;
		} else
		{
			if (c->isdevoured)
			{
				c->isdevoured = c->ishidden = 0;
        sethidden(c, 0);
			}
		}
	}

  if (c->isclientonbarset)
  {
    updateclientonbar(c);
  }
}

void
hideclient(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hidden);
	sethidden(c, 1);
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = client_from_wlr_surface(event->surface);
	if (c && c != selclient()) {
		c->isurgent = 1;
		printstatus();
	}
}

void
view(const Arg *arg)
{
  int active_tag = 0;
  int tags_value;

	if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);

  tags_value = selmon->tagset[selmon->seltags] & TAGMASK;

  /* Change active tag highlight */
  while (!(tags_value & 0x1))
  {
    tags_value = tags_value >> 1;
    ++active_tag;
  }

  wlr_scene_rect_set_size(
    selmon->bar.rects[BarSelectedTagRect],
    selmon->bar.tags_info[active_tag].right - selmon->bar.tags_info[active_tag].left,
    barheight
  );
  MOVENODE(selmon, &selmon->bar.rects[BarSelectedTagRect]->node, selmon->bar.tags_info[active_tag].left, 748);
  updateclientbounds(selmon);

  printstatus();
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	struct wlr_input_device *device = &keyboard->input_device;
	createkeyboard(device);
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

struct wlr_scene_node *
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	const int *layer;
	int focus_order[] = { LyrOverlay, LyrTop, LyrFloat, LyrTile, LyrBottom, LyrBg };

	for (layer = focus_order; layer < END(focus_order); layer++) {
		if ((node = wlr_scene_node_at(layers[*layer], x, y, nx, ny))) {
			if (node->type == WLR_SCENE_NODE_SURFACE)
				surface = wlr_scene_surface_from_node(node)->surface;
			/* Walk the tree to find a node that knows the client */
			for (pnode = node; pnode && !c; pnode = pnode->parent)
				c = pnode->data;
			if (c && c->type == LayerShell) {
				c = NULL;
				l = pnode->data;
			}
		}
		if (surface)
			break;
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
	return node;
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = selclient();

	if (!sel || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon);
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated */
	if (c->type == X11Managed)
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	wlr_xwayland_surface_configure(c->surface.xwayland,
			event->x, event->y, event->width, event->height);
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xwayland_surface = data;
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->isfullscreen && VISIBLEON(c, c->mon))
			setfullscreen(c, 0);

	/* Allocate a Client for this surface */
	c = xwayland_surface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xwayland_surface;
	c->type = xwayland_surface->override_redirect ? X11Unmanaged : X11Managed;
	c->bw = borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xwayland_surface->events.map, &c->map, mapnotify);
	LISTEN(&xwayland_surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&xwayland_surface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xwayland_surface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xwayland_surface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xwayland_surface->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xwayland_surface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xwayland_surface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xwayland_surface->events.request_minimize, &c->set_hidden, hideclient);
}

Atom
getatom(xcb_connection_t *xc, const char *name)
{
	Atom atom = 0;
	xcb_intern_atom_reply_t *reply;
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(xc, 0, strlen(name), name);
	if ((reply = xcb_intern_atom_reply(xc, cookie, NULL)))
		atom = reply->atom;
	free(reply);

	return atom;
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	if (c != selclient()) {
		c->isurgent = c->surface.xwayland->hints_urgency;
		printstatus();
	}
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;
	xcb_connection_t *xc = xcb_connect(xwayland->display_name, NULL);
	int err = xcb_connection_has_error(xc);
	if (err) {
		fprintf(stderr, "xcb_connect to X server failed with code %d\n. Continuing with degraded functionality.\n", err);
		return;
	}

	/* Collect atoms we are interested in.  If getatom returns 0, we will
	 * not detect that window type. */
	netatom[NetWMWindowTypeDialog] = getatom(xc, "_NET_WM_WINDOW_TYPE_DIALOG");
	netatom[NetWMWindowTypeSplash] = getatom(xc, "_NET_WM_WINDOW_TYPE_SPLASH");
	netatom[NetWMWindowTypeToolbar] = getatom(xc, "_NET_WM_WINDOW_TYPE_TOOLBAR");
	netatom[NetWMWindowTypeUtility] = getatom(xc, "_NET_WM_WINDOW_TYPE_UTILITY");

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "left_ptr", 1)))
		wlr_xwayland_set_cursor(xwayland,
				xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
				xcursor->images[0]->width, xcursor->images[0]->height,
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);

	xcb_disconnect(xc);
}
#endif

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL;
	int c;
	Arg arg;

	while ((c = getopt(argc, argv, "s:hv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'v')
			die("edwl");
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	// Wayland requires XDG_RUNTIME_DIR for creating its communications
	// socket
	if (!getenv("XDG_RUNTIME_DIR"))
		BARF("XDG_RUNTIME_DIR must be set");
	setup();
	/* Run main application */
	run(startup_cmd);
	cleanup();

	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-s startup command]", argv[0]);
}
