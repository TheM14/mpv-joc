#!/usr/bin/env python3
"""Apply the ad_joc integration to an mpv source tree by anchors.

A unified diff breaks when upstream moves lines around; this edits the same
six places by their surrounding text instead, and is idempotent, so it can be
re-run on any mpv revision.  If an anchor is gone, the call fails before
touching anything: that is the moment a human has to look at what upstream
changed.

Usage: apply_edits.py <mpv-source-dir>
"""
import os
import sys

MESON_OPTION = ("option('joc-core-dir', type: 'string', value: '', "
                "description: 'Directory holding joc_core.lib: builds the E-AC-3 JOC decoder (ad_joc)')")

MESON_BLOCK = """# E-AC-3 JOC decoder (ad_joc): links the JustOneCacophony rendering core.
joc_core_dir = get_option('joc-core-dir')
if joc_core_dir != ''
    joc_core = cc.find_library('joc_core', dirs: [joc_core_dir], required: true)
    dependencies += joc_core
    sources += files('audio/decode/ad_joc.c', 'audio/decode/joc_eac3.c')
    joc_enabled = 1
else
    joc_enabled = 0
endif"""

WRAPPER_HELPER = """static bool decoder_name_listed(const char *list, const char *name)
{
    if (!list)
        return false;
    bstr sel = bstr0(list);
    while (sel.len) {
        bstr item;
        bstr_split_tok(sel, ",", &item, &sel);
        if (bstr_equals0(item, name))
            return true;
    }
    return false;
}"""

WRAPPER_BLOCK = """#if HAVE_JOC
        /* E-AC-3 JOC through the JustOneCacophony core: --ad=joc renders the
         * objects (binaural or a speaker layout) instead of letting FFmpeg
         * downmix.  Opt-in, so default playback is untouched. */
        if (driver == &ad_lavc && p->codec->codec &&
            strcmp(p->codec->codec, "eac3") == 0 &&
            decoder_name_listed(user_list, "joc"))
        {
            struct mp_decoder_list *joc_list =
                talloc_zero(NULL, struct mp_decoder_list);
            ad_joc.add_decoders(joc_list);
            if (joc_list->num_entries) {
                driver = &ad_joc;
                list = joc_list;
            } else {
                talloc_free(joc_list);
            }
        }
#endif"""


class Tree:
    def __init__(self, root):
        self.root = root
        self.orig = {}
        self.text = {}

    def read(self, rel):
        path = os.path.join(self.root, rel)
        # Universal newlines: the tool works on LF and writes LF back, which is
        # what the mpv tree uses; git normalises the rest.
        with open(path, "r", encoding="utf-8") as f:
            s = f.read()
        self.orig[rel] = s
        self.text[rel] = s
        return s

    def put(self, rel, s):
        self.text[rel] = s

    def write_all(self):
        for rel, s in self.text.items():
            if s != self.orig.get(rel):
                with open(os.path.join(self.root, rel), "w", encoding="utf-8", newline="\n") as f:
                    f.write(s)


def eol_of(s):
    return "\r\n" if "\r\n" in s[:2000] else "\n"


def add_after(tree, rel, anchor, block, what):
    s = tree.text[rel]
    if block.strip() in s:
        return
    i = s.find(anchor)
    if i < 0:
        raise SystemExit("%s: anchor for %s not found" % (rel, what))
    eol = eol_of(s)
    j = i + len(anchor)
    tree.put(rel, s[:j] + eol + block.replace("\n", eol) + s[j:])


def add_before(tree, rel, anchor, block, what):
    s = tree.text[rel]
    if block.strip() in s:
        return
    i = s.find(anchor)
    if i < 0:
        raise SystemExit("%s: anchor for %s not found" % (rel, what))
    eol = eol_of(s)
    tree.put(rel, s[:i] + block.replace("\n", eol) + eol + s[i:])


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    tree = Tree(root)

    for rel in ("meson.options", "meson.build", "filters/f_decoder_wrapper.c",
                "filters/f_decoder_wrapper.h", "options/options.c", "options/options.h"):
        tree.read(rel)

    add_after(tree, "meson.options",
              "option('gpl', type: 'boolean', value: true, description: 'GPL (version 2 or later) build')",
              MESON_OPTION, "gpl option")
    add_before(tree, "meson.build", "# Set config.h", MESON_BLOCK, "config.h marker")
    add_after(tree, "meson.build",
              "conf_data.set_quoted('DEFAULT_OPTICAL_DEVICE', optical_device)",
              "conf_data.set('HAVE_JOC', joc_enabled)", "conf_data")

    add_after(tree, "filters/f_decoder_wrapper.c", '#include "common/msg.h"',
              '#include "misc/bstr.h"', "bstr include")
    add_before(tree, "filters/f_decoder_wrapper.c",
               "static bool reinit_decoder(struct priv *p)", WRAPPER_HELPER, "reinit_decoder")
    s = tree.text["filters/f_decoder_wrapper.c"]
    i = s.find("static bool reinit_decoder(struct priv *p)")
    j = s.find("\n    if (!driver)", i)
    if j < 0:
        raise SystemExit("filters/f_decoder_wrapper.c: if (!driver) not found")
    if WRAPPER_BLOCK.strip() not in s:
        eol = eol_of(s)
        # Insert inside the audio branch: the last "    }" before if (!driver)
        # closes it, and the block belongs before that brace.
        k = s.rfind("\n    }", i, j)
        if k < 0:
            raise SystemExit("filters/f_decoder_wrapper.c: audio branch brace not found")
        pos = k + 1
        tree.put("filters/f_decoder_wrapper.c",
                 s[:pos] + WRAPPER_BLOCK.replace("\n", eol) + eol + s[pos:])

    add_after(tree, "filters/f_decoder_wrapper.h",
              "extern const struct mp_decoder_fns ad_dsd;",
              "extern const struct mp_decoder_fns ad_joc;", "ad_dsd extern")
    add_after(tree, "options/options.c",
              "extern const struct m_sub_options ad_lavc_conf;",
              "extern const struct m_sub_options ad_joc_conf;", "ad_lavc_conf extern")
    s = tree.text["options/options.c"]
    if '"ad-joc", OPT_SUBSTRUCT' not in s:
        eol = eol_of(s)
        anchor = '    {"ad-lavc", OPT_SUBSTRUCT(ad_lavc_params, ad_lavc_conf)},'
        i = s.find(anchor)
        if i < 0:
            raise SystemExit("options/options.c: ad-lavc option not found")
        block = "#if HAVE_JOC" + eol + '    {"ad-joc", OPT_SUBSTRUCT(ad_joc_params, ad_joc_conf)},' + eol + "#endif"
        tree.put("options/options.c", s[:i + len(anchor)] + eol + block + s[i + len(anchor):])
    add_after(tree, "options/options.h",
              "    struct ad_lavc_params *ad_lavc_params;",
              "    struct ad_joc_params *ad_joc_params;", "ad_lavc field")

    tree.write_all()
    print("apply_edits: mpv tree updated")


if __name__ == "__main__":
    main()
