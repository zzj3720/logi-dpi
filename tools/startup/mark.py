# Copies logi-dpi's MouseDPI sources here with the debug marks (marks.txt) and MOUSEDPI_TEST (more than one at once).
import sys, os
src = sys.argv[1]
d = src  # marked in place: run it in a scratch copy
s = open(os.path.join(src, 'hidpp_win.c')).read()
def rep(s, a, b):
    assert a in s, a
    return s.replace(a, b, 1)
s = rep(s, '#pragma comment(lib, "hid.lib")', '#include <stdlib.h>\nvoid mousedpi_mark(int32_t n);\n\n#pragma comment(lib, "hid.lib")')
s = rep(s, 'int32_t mousedpi_single(void) {\n', 'int32_t mousedpi_single(void) {\n    if (getenv("MOUSEDPI_TEST")) return 1;\n')
s = rep(s, 'static void show_held_window(void) {\n', 'static void show_held_window(void) {\n    mousedpi_mark(60);\n')
s += '''
void mousedpi_mark(int32_t n) {
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimePreciseAsFileTime(&now);
    ULONGLONG a = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    ULONGLONG b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    FILE *f = fopen("marks.txt", "a");
    if (f) { fprintf(f, "mark %d at %.1f ms\\n", n, (double)(b - a) / 10000.0); fclose(f); }
}
'''
open(os.path.join(d, 'hidpp_win.c'), 'w').write(s)
m = open(os.path.join(src, 'main.pion')).read()
m = rep(m, 'extern "C" fn c_setup_soon', 'extern "C" fn c_mark(n : Int) -> Unit = "mousedpi_mark"\n\nextern "C" fn c_setup_soon')
m = rep(m, 'fn main(world : World) -> Int {\n', 'fn main(world : World) -> Int {\n  c_mark(1)\n')
m = rep(m, '      let first = c_init_and_restore()\n', '      let first = c_init_and_restore()\n      c_mark(3)\n')
m = rep(m, '        dpi_ui::app(root, m, d, picture="mouse.png")\n', '        c_mark(4)\n        dpi_ui::app(root, m, d, picture="mouse.png")\n        c_mark(5)\n')
m = rep(m, '{ take(m, r) })', '{\n          c_mark(8)\n          take(m, r)\n        })')
open(os.path.join(d, 'main.pion'), 'w').write(m)
