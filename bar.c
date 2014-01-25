#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib-xcb.h>

#include "config.h"

// Here be dragons

#define MAX(a,b) ((a > b) ? a : b)

typedef struct fontset_item_t {
    xcb_font_t      xcb_ft;
    xcb_charinfo_t *table;
    XftFont        *xft_ft;
    int             ascent;
    int             descent;
    int             avg_height;
    unsigned short  char_max;
    unsigned short  char_min;
} fontset_item_t;

#define FONT_MAX 4

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

static Display          *dpy;
static xcb_connection_t *c;
static xcb_window_t     win;
static xcb_drawable_t   canvas;
static xcb_gcontext_t   draw_gc;
static xcb_gcontext_t   clear_gc;
static xcb_gcontext_t   underl_gc;
static int              bar_width = 0;
static int              x = 0;
static int              y = 0;
static int              bar_bottom = BAR_BOTTOM;
static int              force_docking = 0;
static int              fontset_count;
static fontset_item_t   fontset[FONT_MAX]; 
static fontset_item_t   *sel_font = NULL;
static XftColor         sel_fg;
static XftDraw          *xft_draw;

#define MAX_COLORS 12
#define MAX_WIDTHS (1 << 16)
static const unsigned   palette[MAX_COLORS] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9,BACKGROUND,FOREGROUND};
static XftColor         xft_palette[MAX_COLORS];
static wchar_t          xft_char[MAX_WIDTHS];
static char             xft_width[MAX_WIDTHS];

static inline void
xcb_set_bg (int i)
{
    xcb_change_gc (c, draw_gc  , XCB_GC_BACKGROUND, (const unsigned []){ palette[i] });
    xcb_change_gc (c, clear_gc , XCB_GC_FOREGROUND, (const unsigned []){ palette[i] });
}

static inline void
xcb_set_fg (int i)
{
    sel_fg = xft_palette[i];
    xcb_change_gc (c, draw_gc , XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

static inline void
xcb_set_ud (int i)
{
    xcb_change_gc (c, underl_gc, XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

static inline void
xcb_fill_rect (xcb_gcontext_t gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

static inline void
xcb_set_fontset (int i)
{
    if (sel_font != &fontset[i]) {
        sel_font = &fontset[i];
        if (sel_font->xcb_ft)
            xcb_change_gc (c, draw_gc , XCB_GC_FONT, (const uint32_t []){ sel_font->xcb_ft });
    }
}

static inline int
xft_char_width_slot (wchar_t ch)
{
    int slot = ch % MAX_WIDTHS;
    while (xft_char[slot] != 0 && xft_char[slot] != ch)
        slot = (slot + 1) % MAX_WIDTHS;
    return slot;
}

int
xft_char_width (wchar_t ch)
{
    int slot = xft_char_width_slot(ch);
    if (!xft_char[slot]) {
        XGlyphInfo gi;
        XftTextExtents32 (dpy, sel_font->xft_ft, &ch, 1, &gi);
        xft_char[slot] = ch;
        xft_width[slot] = gi.xOff;
        return gi.xOff;
    } else if (xft_char[slot] == ch) {
        return xft_width[slot];
    } else {
        return 0;
    }
}

int
draw_char (int x, int align, wchar_t ch)
{
    int ch_width;

    if (sel_font->xft_ft) {
        ch_width = xft_char_width(ch);
    } else {
        ch_width = (ch > sel_font->char_min && ch < sel_font->char_max) ?
            sel_font->table[ch - sel_font->char_min].character_width    :
            0;
    }

    /* Some fonts (such as anorexia) have the space char with the width set to 0 */
    if (ch_width == 0)
        ch_width = BAR_FONT_FALLBACK_WIDTH;

    switch (align) {
        case ALIGN_C:
            xcb_copy_area (c, canvas, canvas, draw_gc, bar_width / 2 - x / 2, 0, 
                    bar_width / 2 - (x + ch_width) / 2, 0, x, BAR_HEIGHT);
            x = bar_width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area (c, canvas, canvas, draw_gc, bar_width - x, 0, 
                    bar_width - x - ch_width, 0, x, BAR_HEIGHT);
            x = bar_width - ch_width; 
            break;
    }

    /* Draw the background first */
    xcb_fill_rect (clear_gc, x, 0, ch_width, BAR_HEIGHT);

    /* String baseline coordinates */
    int y = BAR_HEIGHT / 2 + sel_font->avg_height / 2 - sel_font->descent;
    if (sel_font->xft_ft) {
        XftDrawString32 (xft_draw, &sel_fg, sel_font->xft_ft, x, y, &ch, 1);
    } else {
        char c_ = ch > 127 ? ' ' : ch;
        xcb_image_text_8 (c, 1, canvas, draw_gc, x, y, &c_);
    }

    /* Draw the underline */
    if (BAR_UNDERLINE_HEIGHT) 
        xcb_fill_rect (underl_gc, x, BAR_UNDERLINE*(BAR_HEIGHT-BAR_UNDERLINE_HEIGHT), ch_width, BAR_UNDERLINE_HEIGHT);

    return ch_width;
}

int
utf8decode(char *s, wchar_t *u) {
    unsigned char c;
    int i, n, rtn;

    rtn = 1;
    c = *s;
    if(~c & 0x80) { /* 0xxxxxxx */
        *u = c;
        return rtn;
    } else if((c & 0xE0) == 0xC0) { /* 110xxxxx */
        *u = c & 0x1F;
        n = 1;
    } else if((c & 0xF0) == 0xE0) { /* 1110xxxx */
        *u = c & 0x0F;
        n = 2;
    } else if((c & 0xF8) == 0xF0) { /* 11110xxx */
        *u = c & 0x07;
        n = 3;
    } else {
        goto invalid;
    }

    for(i = n, ++s; i > 0; --i, ++rtn, ++s) {
        c = *s;
        if((c & 0xC0) != 0x80) /* 10xxxxxx */
            goto invalid;
        *u <<= 6;
        *u |= c & 0x3F;
    }

    if((n == 1 && *u < 0x80) ||
       (n == 2 && *u < 0x800) ||
       (n == 3 && *u < 0x10000) ||
       (*u >= 0xD800 && *u <= 0xDFFF)) {
        goto invalid;
    }

    return rtn;
invalid:
    *u = 0xFFFD;

    return rtn;
}

void
parse (char *text)
{
    char *p = text;

    int pos_x = 0;
    int align = 0;

    /* Create xft drawable */
    int screen = DefaultScreen (dpy);
    if (!(xft_draw = XftDrawCreate (dpy, canvas, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen)))) {
        fprintf(stderr, "Couldn't create xft drawable\n");
    }

    xcb_fill_rect (clear_gc, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        if (*p == '\0')
            return;
        if (*p == '\n')
            return;

        if (*p == '\\' && p++ && *p != '\\' && strchr ("fbulcr", *p)) {
                switch (*p++) {
                    case 'f': 
                        xcb_set_fg (isdigit(*p) ? (*p)-'0' : 11);
                        p++;
                        break;
                    case 'b': 
                        xcb_set_bg (isdigit(*p) ? (*p)-'0' : 10);
                        p++;
                        break;
                    case 'u': 
                        xcb_set_ud (isdigit(*p) ? (*p)-'0' : 10);
                        p++;
                        break;

                    case 'l': 
                        align = ALIGN_L; 
                        pos_x = 0; 
                        break;
                    case 'c': 
                        align = ALIGN_C; 
                        pos_x = 0; 
                        break;
                    case 'r': 
                        align = ALIGN_R; 
                        pos_x = 0; 
                        break;
                }
        } else { /* utf-8 -> utf-32 */
            wchar_t t;
            p += utf8decode(p, &t);

            /* The character is outside the main font charset, use the fallback */
            xcb_set_fontset (0);
            for (int i = 0; i < fontset_count; i++) {
                fontset_item_t *f = fontset + i;
                if ((f->xcb_ft && (t < f->char_min || t > f->char_max)) ||
                    (f->xft_ft && !XftCharExists(dpy, f->xft_ft, t))) continue;
                xcb_set_fontset (i);
                break;
            }

            pos_x += draw_char (pos_x, align, t);
        }
    }

    XftDrawDestroy (xft_draw);
}

int
font_load (const char **font_list)
{
    xcb_query_font_cookie_t queryreq;
    xcb_query_font_reply_t *font_info;
    xcb_void_cookie_t cookie;
    xcb_font_t font;
    int max_height;

    max_height = -1;

    for (int i = 0; i < FONT_MAX && font_list[i]; i++) {
        fontset[i].xcb_ft = 0;
        fontset[i].xft_ft = 0;
        font = xcb_generate_id (c);

        cookie = xcb_open_font_checked (c, font, strlen (font_list[i]), font_list[i]);
        if (!xcb_request_check (c, cookie)) {
            queryreq = xcb_query_font (c, font);
            font_info = xcb_query_font_reply (c, queryreq, NULL);

            fontset[i].xcb_ft  = font;
            fontset[i].table   = xcb_query_font_char_infos (font_info);
            fontset[i].ascent  = font_info->font_ascent;
            fontset[i].descent = font_info->font_descent;
            fontset[i].char_max= font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
            fontset[i].char_min= font_info->min_byte1 << 8 | font_info->min_char_or_byte2;
        } else if (fontset[i].xft_ft = XftFontOpenName (dpy, DefaultScreen(dpy), font_list[i])) {
            fontset[i].ascent  = fontset[i].xft_ft->ascent;
            fontset[i].descent = fontset[i].xft_ft->descent;
        } else {
            fprintf (stderr, "Could not load font %s\n", font_list[i]);
            return 1;
        }
        max_height = MAX(fontset[i].ascent + fontset[i].descent, max_height);
        fontset_count++;
    }

    /* To have an uniform alignment */
    for (int i = 0; i < fontset_count; i++)
        fontset[i].avg_height = max_height;

    return 0;
}

enum {
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_DESKTOP,
    NET_WM_STRUT_PARTIAL,
    NET_WM_STRUT,
    NET_WM_STATE,
    NET_WM_WINDOW_OPACITY,
    NET_WM_STATE_STICKY,
    NET_WM_STATE_ABOVE,
};

void
set_ewmh_atoms ()
{
    const char *atom_names[] = {
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_DESKTOP",
        "_NET_WM_STRUT_PARTIAL",
        "_NET_WM_STRUT",
        "_NET_WM_STATE",
        "_NET_WM_WINDOW_OPACITY",
        /* Leave those at the end since are batch-set */
        "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_ABOVE",
    };
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];
    xcb_intern_atom_reply_t *atom_reply;
    int strut[12] = {0};

    /* As suggested fetch all the cookies first (yum!) and then retrieve the
     * atoms to exploit the async'ness */
    for (int i = 0; i < atoms; i++)
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);

    for (int i = 0; i < atoms; i++) {
        atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if (!atom_reply)
            return;
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    /* Prepare the strut array */
    if (bar_bottom) {
        strut[3]  = BAR_HEIGHT;
        strut[11] = bar_width;
    } else {
        strut[2] = BAR_HEIGHT;
        strut[9] = bar_width;
    }

    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_WINDOW_OPACITY], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ (uint32_t)(BAR_OPACITY * 0xffffffff) } );
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
    xcb_change_property (c, XCB_PROP_MODE_APPEND,  win, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
}

void
init (void)
{
    xcb_screen_t *scr;
    xcb_window_t root;
    int y;

    /* Connect to X */
    if ((dpy = XOpenDisplay(0)) == NULL)
        fprintf (stderr, "Couldn't open display\n");

    if ((c = XGetXCBConnection(dpy)) == NULL) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (1);
    }

    /* Grab infos from the first screen */
    scr  = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    root = scr->root;

    /* where to place the window */
    y = (bar_bottom) ? (scr->height_in_pixels - BAR_HEIGHT) : 0;

    /* if bar_width wasn't set in argc, set it here. */
    if (!bar_width) {
        bar_width = (BAR_WIDTH < 0) ? (scr->width_in_pixels - BAR_OFFSET) : BAR_WIDTH;
    }

    /* Load the font */
    memset(xft_char, 0, sizeof(xft_char));
    if (font_load ((const char* []){ BAR_FONT, NULL }))
        exit (1);

    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, x, y, bar_width,
            BAR_HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, (const uint32_t []){ palette[10], XCB_EVENT_MASK_EXPOSURE });

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    xcb_change_window_attributes (c, win, XCB_CW_OVERRIDE_REDIRECT, (const uint32_t []){ force_docking });

    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, bar_width, BAR_HEIGHT);

    /* Create the gc for drawing */
    draw_gc = xcb_generate_id (c);
    xcb_create_gc (c, draw_gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ palette[11], palette[10] });

    clear_gc = xcb_generate_id (c);
    xcb_create_gc (c, clear_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    underl_gc = xcb_generate_id (c);
    xcb_create_gc (c, underl_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    /* Initialise the xft colors */
    int i;
    for (i = 0; i < MAX_COLORS; i++) {
        char color[8] = "#000000";
        snprintf(color, sizeof(color), "#%06X", palette[i]);
        if (!XftColorAllocName (dpy, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultColormap(dpy, DefaultScreen(dpy)), color, xft_palette + i)) {
            fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
        }
    }
    sel_fg = xft_palette[11];

    /* Make the bar visible */
    xcb_map_window (c, win);

    xcb_flush (c);
}

void
cleanup (void)
{
    int i;
    for (i = 0; i < fontset_count; i++) {
        if (fontset[i].xcb_ft)
            xcb_close_font (c, fontset[i].xcb_ft);
        else if (fontset[i].xft_ft)
            XftFontClose (dpy, fontset[i].xft_ft);
    }
    for (i = 0; i < MAX_COLORS; i++) {
        XftColorFree (dpy, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultColormap(dpy, DefaultScreen(dpy)), xft_palette + i);
    }
    if (canvas)
        xcb_free_pixmap (c, canvas);
    if (win)
        xcb_destroy_window (c, win);
    if (draw_gc)
        xcb_free_gc (c, draw_gc);
    if (clear_gc)
        xcb_free_gc (c, clear_gc);
    if (underl_gc)
        xcb_free_gc (c, underl_gc);
    if (c)
        xcb_disconnect (c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM) 
        exit (0);
}

int 
main (int argc, char **argv)
{
    char input[1024] = {0, };
    struct pollfd pollin[2] = { 
        { .fd = STDIN_FILENO, .events = POLLIN }, 
        { .fd = -1          , .events = POLLIN }, 
    };

    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;

    int permanent = 0;

    char *w;

    char ch;
    while ((ch = getopt (argc, argv, "p:h:b:f:w:x:y")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-p | -h] [-b]\n"
                        "\t-h Show this help\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-f Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (0);
            case 'p': permanent = 1; break;
            case 'b': bar_bottom = 1; break;
            case 'f': force_docking = 1; break;
            case 'w': bar_width = atoi(optarg); break;
            case 'x': x = atoi(optarg); break;
        }
    }

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor (c);

    // x, y here
    xcb_fill_rect (clear_gc, x, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        int redraw = 0;

        if (poll (pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      /* No more data... */
                if (permanent) pollin[0].fd = -1;   /* ...null the fd and continue polling :D */
                else           break;               /* ...bail out */
            }
            if (pollin[0].revents & POLLIN) { /* New input, process it */
                fgets (input, sizeof(input), stdin);
                parse (input);
                redraw = 1;
            }
            if (pollin[1].revents & POLLIN) { /* Xserver broadcasted an event */
                while ((ev = xcb_poll_for_event (c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                        case XCB_EXPOSE: 
                            if (expose_ev->count == 0) redraw = 1; 
                        break;
                    }

                    free (ev);
                }
            }
        }

        if (redraw) /* Copy our temporary pixmap onto the window */
            xcb_copy_area (c, canvas, win, draw_gc, 0, 0, 0, 0, bar_width, BAR_HEIGHT);

        xcb_flush (c);
    }

    return 0;
}
