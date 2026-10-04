/*
    LumiPaint - per-note colour control for ROLI LUMI Keys
    Copyright (C) 2026 Simon Bourdareau

    This program is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
    PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along with
    this program. If not, see <https://www.gnu.org/licenses/>.
*/

/*
    The smallest CLAP host that can open the editor, so CI can tell when it stops.

    What it checks is narrow and worth having: the module loads, the factory reports one
    plugin, init and activate succeed, the gui extension exists, the X11 API is
    accepted, and create / set_parent / set_size / show all return true against a real
    X11 window. Under Xvfb with llvmpipe that runs anywhere, no hardware and no DAW.

    What it does not check is everything after that - MIDI, the device, screen capture.
    Those need a keyboard and a desktop. This is the floor, not the ceiling.

    The parent window is ours rather than the plugin's: LumiPaint's gui extension
    refuses isFloating, as an embedded editor should, so a host that hands it nothing to
    sit inside gets a flat no and learns nothing.

    Build:
        cc -Iclap-src/include tests/linux_gui_smoke.c -o smoke -ldl -lX11
    Run:
        xvfb-run -s "-screen 0 1400x960x24" ./smoke build/LumiPaint.clap
*/

#include <clap/clap.h>
#include <X11/Xlib.h>

#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

static void hostRestart (const clap_host_t *h) { (void) h; }
static void hostProcess (const clap_host_t *h) { (void) h; }
static void hostCallback (const clap_host_t *h) { (void) h; }

static const void *hostExtension (const clap_host_t *h, const char *id)
{
    (void) h;
    (void) id;
    return NULL;
}

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); return 1; } while (0)

int main (int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/LumiPaint.clap";

    static clap_host_t host = { CLAP_VERSION_INIT, NULL,
                                "lumipaint-smoke", "LumiPaint", "-", "1.0",
                                hostExtension, hostRestart, hostProcess, hostCallback };

    Display *display = XOpenDisplay (NULL);

    if (display == NULL)
        FAIL ("no X display - run under xvfb-run\n");

    const int screen = DefaultScreen (display);
    Window parent = XCreateSimpleWindow (display, RootWindow (display, screen),
                                         0, 0, 1340, 900, 0, 0, 0x111111);
    XMapRaised (display, parent);
    XFlush (display);

    void *module = dlopen (path, RTLD_NOW);

    if (module == NULL)
        FAIL ("dlopen %s: %s\n", path, dlerror());

    const clap_plugin_entry_t *entry = dlsym (module, "clap_entry");

    if (entry == NULL)
        FAIL ("no clap_entry symbol\n");

    if (! entry->init (path))
        FAIL ("entry init\n");

    const clap_plugin_factory_t *factory = entry->get_factory (CLAP_PLUGIN_FACTORY_ID);

    if (factory == NULL)
        FAIL ("no plugin factory\n");

    const uint32_t count = factory->get_plugin_count (factory);

    if (count != 1)
        FAIL ("expected 1 plugin, got %u\n", count);

    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor (factory, 0);
    printf ("  id %s, name %s, version %s\n", desc->id, desc->name, desc->version);

    const clap_plugin_t *plugin = factory->create_plugin (factory, &host, desc->id);

    if (plugin == NULL || ! plugin->init (plugin))
        FAIL ("plugin init\n");

    if (! plugin->activate (plugin, 48000, 32, 512))
        FAIL ("activate\n");

    const clap_plugin_gui_t *gui = plugin->get_extension (plugin, CLAP_EXT_GUI);

    if (gui == NULL)
        FAIL ("no gui extension\n");

    if (! gui->is_api_supported (plugin, CLAP_WINDOW_API_X11, false))
        FAIL ("x11 embedded not supported\n");

    if (gui->is_api_supported (plugin, CLAP_WINDOW_API_X11, true))
        FAIL ("floating should be refused\n");

    if (! gui->create (plugin, CLAP_WINDOW_API_X11, false))
        FAIL ("gui create\n");

    uint32_t w = 0, h = 0;
    gui->get_size (plugin, &w, &h);
    printf ("  editor %ux%u\n", w, h);

    if (w < 100 || h < 100)
        FAIL ("implausible editor size\n");

    clap_window_t window;
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = parent;

    if (! gui->set_parent (plugin, &window))
        FAIL ("set_parent\n");

    if (! gui->set_size (plugin, w, h))
        FAIL ("set_size\n");

    if (! gui->show (plugin))
        FAIL ("show\n");

    /* Long enough for the repaint thread to have drawn frames, which is where a
       context or font problem would surface rather than at create time. */
    for (int i = 0; i < 100; ++i)
    {
        while (XPending (display) > 0)
        {
            XEvent event;
            XNextEvent (display, &event);
        }

        usleep (20000);
    }

    gui->hide (plugin);
    gui->destroy (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    entry->deinit();

    printf ("PASS: editor opened, parented and rendered\n");
    return 0;
}
