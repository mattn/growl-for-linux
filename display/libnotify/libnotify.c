/* Copyright 2011 by Kohei Takahashi
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
#endif

#include "../../gol.h"

G_MODULE_EXPORT gboolean
display_init() {
  return TRUE;
}

G_MODULE_EXPORT void
display_term() {
}

G_MODULE_EXPORT gboolean
display_show(gpointer data) {
  return FALSE;
}

G_MODULE_EXPORT const gchar*
display_name() {
  return "libnotify";
}

G_MODULE_EXPORT const gchar*
display_description() {
  return
    "<span size=\"large\"><b>libnotify</b></span>\n"
    "<span>This is front-end of libnotify.</span>\n"
    "<span>For more detail, see <a href=\"https://launchpad.net/notify-osd\">here</a>.</span>\n";
}

// FIXME: Prepare thumbnail xpm image.
G_MODULE_EXPORT char**
display_thumbnail() {
  return NULL;
}

// vim:set et sw=2 ts=2 ai:
