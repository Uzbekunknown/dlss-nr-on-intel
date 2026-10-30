"""Generate the menu's four-language string tables for both C and Python, from one source.

Why generated rather than hand-written twice. The menu needs its labels in C, because it draws
inside the game's process with no interpreter; the tools need them in Python, because `nr_config`
writes the INI and the wizard renders the panel. Hand-writing both is how the knob *ranges*
already drifted once between three copies in this project.

**All four languages come from one file.** `dist-tools/windows-wizard.ps1` holds the setup window's
own strings, and it is now the single source for the overlay's too: the wizard's keys are
`nr_knobs.py`'s names, so the tables join without a mapping and a knob added there brings its
label to both. Chinese used to be a hand-written column in this file, which is exactly how the two
UIs drifted apart - the wizard had already dropped `Profile.neutral` while this generator still
emitted one, and the two disagreed about what a profile was. The wizard carries Chinese now, so
the split is gone.

Where the keys come from:
    Knob.<name>, Hint.<name>   the wizard's per-knob label and tooltip.
    everything else            the wizard's own keys, by name.

The menu's own keys - "compare", "menu key" - have no equivalent in a setup window with no
overlay, so they stay hand-written here for all four languages. See OWN below. They are the menu's
labels, not knob explanations: the explanations are the wizard's `Hint.*`, which every language
now carries, and which is what the overlay's tooltip shows.

The outputs are checked in. Re-run this when the wizard's strings change:

    py tools/gen_menu_strings.py
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WIZARD = REPO / "dist-tools" / "windows-wizard.ps1"
HEADER = REPO / "src" / "layer" / "nr_menu_strings.h"
MODULE = REPO / "src" / "layer" / "nr_menu_strings.py"

# The knobs, in `nr_knobs.py`'s order - which is the order the menu shows them.
KNOBS = ("render_scale", "min_extent", "profile", "intensity", "detail_strength",
         "colour_strength", "temporal", "hold", "release", "cut_limit", "weights")

# The wizard's key for each of the menu's non-knob strings. `None` means the wizard has no such
# key and the value comes from OWN below. The names are the wizard's, so a rename there fails
# loudly here rather than silently falling back to English.
EXTRA = {
    # our key:        (wizard key,      English fallback)
    "title":          ("Title",        "DLSS-NR on Intel - settings"),
    "saved":          ("Done.save",    "saved"),
    "reset":          ("ResetSettings", "reset to defaults"),
    "hint":           (None,           "Up/Down choose  Left/Right change  L language  "
                                       "Home NR on/off  S save  R reset  INS close  "
                                       "the game also sees these keys"),
    "toggle_nr":      (None,           "neural rendering"),
    "compare":        (None,           "compare"),
    "compare_split":  (None,           "split"),
    "compare_zoom":   (None,           "zoom"),
    "language":       (None,           "language"),
    "shortcut":       (None,           "menu key"),
    "on":             (None,           "on"),
    "off":            (None,           "off"),
    # The layer's own pacing, which the wizard words on its own controls page. These three are
    # the MENU's rows for the same settings, and the menu has no wizard key to join to, so they
    # are OWN. They were hand-written in `nr_layer.c` as `zh ? ... : ...`, which left Russian
    # and Spanish showing English - the language enum has four values and the ternary had two.
    "async":          (None,           "Asynchronous: the network runs ahead"),
    "async_hint":     (None,           "More frames per second, one behind the result. "
                                       "Takes effect at the next game start."),
    "gpu_image":      (None,           "Experimental: keep the frame on the GPU"),
    "gpu_image_hint": (None,           "Requires asynchronous mode off and every frame processed, "
                                       "or the effect is skipped in this mode. "
                                       "Takes effect at the next game start."),
    "every":          (None,           "Network runs on"),
    "every_hint":     (None,           "The frames between repeat the last result, so the game "
                                       "runs faster and the picture changes only when the network "
                                       "produces one. Takes effect at the next game start."),
    "every_frame":    (None,           "every frame"),
    # The cadence's other options, in two plural forms because Russian needs them: 2-4 takes
    # `every_nth_few` (каждые 2 кадра) and 5 and up takes `every_nth_many` (каждые 5 кадров).
    # English, Spanish and Chinese do not inflect, so their two forms are the same string and
    # only Russian is visibly different. The count picks the key in nr_layer.c; the `{}` inside
    # it takes the number, so a language with a different word order needs no C.
    "every_nth_few":  (None,           "every {} frames"),
    "every_nth_many": (None,           "every {} frames"),
    # What a choice knob's VALUES read as, rather than the raw name the daemon resolves. The
    # panel used to show `ht` / `cc_small_fp8` on the Network row while the setup window beside it
    # said "A: HT, the model this project reproduces" - and the row's own hint explained both.
    # The key is `Choice.<knob>.<value>`, the same spelling the window's table uses, so the two UIs
    # read one translation and cannot drift. A value with no key falls back to the raw name, which
    # is never wrong, only terse.
    "choice_weights_ht":          ("Choice.weights.ht",          "A: HT, the model this project reproduces"),
    "choice_weights_cc_small_fp8": ("Choice.weights.cc_small_fp8", "B: CC small FP8, the faster model"),
    # Why the GPU image path is not running while its row says it is on: the daemon's own
    # `fallback_reason` for this case, said in the panel's language. One key per case.
    "gpu_blocked_both": (None,         "Detail and colour strength are both not 1, which GPU "
                                       "composition does not accept: the picture is still "
                                       "processed, on the CPU path. Set both to 1 to use the GPU "
                                       "path."),
    "gpu_blocked_detail": (None,       "Detail strength is not 1, which GPU composition does not "
                                       "accept: the picture is still processed, on the CPU path. "
                                       "Set it to 1 to use the GPU path."),
    "gpu_blocked_colour": (None,       "Colour strength is not 1, which GPU composition does not "
                                       "accept: the picture is still processed, on the CPU path. "
                                       "Set it to 1 to use the GPU path."),
}

# Compare modes, in the order the key cycles them. OptiScaler's own names are Off / Side by side /
# Wipe; the wipe is the one worth playing in, because it cuts one frame and resamples nothing.
COMPARE_MODES = ("off", "wipe", "side by side")

# The menu's own keys, which the wizard does not have: it is a setup window with no overlay, so
# there is no "compare" or "menu key" in it to translate. These are the ONLY strings not taken
# from the wizard, and they are the menu's labels rather than knob explanations - the
# explanations are the wizard's `Hint.*`, which every language now carries.
OWN = {
    "en": {
        "hint":          "Up/Down choose  Left/Right change  L language  "
                         "Home NR on/off  S save  R reset  INS close  "
                         "the game also sees these keys",
        "toggle_nr":     "neural rendering",
        "compare":       "compare",
        "compare_split": "split",
        "compare_zoom":  "zoom",
        "language":      "language",
        "shortcut":      "menu key",
        "on":            "on",
        "off":           "off",
        "async":         "Asynchronous: the network runs ahead",
        "async_hint":    "More frames per second, one behind the result. "
                         "Takes effect at the next game start.",
        "gpu_image":     "Experimental: keep the frame on the GPU",
        "gpu_image_hint": "Requires asynchronous mode off and every frame processed, "
                          "or the effect is skipped in this mode. "
                          "Takes effect at the next game start.",
        "every":         "Network runs on",
        "every_hint":    "The frames between repeat the last result, so the game runs faster "
                         "and the picture changes only when the network produces one. "
                         "Takes effect at the next game start.",
        "every_frame":   "every frame",
        "every_nth_few":  "every {} frames",
        "every_nth_many": "every {} frames",
        "choice_weights_ht": "A: HT, the model this project reproduces",
        "choice_weights_cc_small_fp8": "B: CC small FP8, the faster model",
        "gpu_blocked_both": "Detail and colour strength are both not 1, which GPU composition does "
                            "not accept: the picture is still processed, on the CPU path. Set both "
                            "to 1 to use the GPU path.",
        "gpu_blocked_detail": "Detail strength is not 1, which GPU composition does not accept: the "
                              "picture is still processed, on the CPU path. Set it to 1 to use the "
                              "GPU path.",
        "gpu_blocked_colour": "Colour strength is not 1, which GPU composition does not accept: the "
                              "picture is still processed, on the CPU path. Set it to 1 to use the "
                              "GPU path.",
    },
    "zh": {
        "hint":          "\u4e0a/\u4e0b \u9009\u62e9  \u5de6/\u53f3 \u8c03\u6574  L \u8bed\u8a00  "
                         "Home \u5f00\u5173  S \u4fdd\u5b58  R \u91cd\u7f6e  INS \u5173\u95ed  "
                         "\u6e38\u620f\u4e5f\u4f1a\u6536\u5230\u8fd9\u4e9b\u6309\u952e",
        "toggle_nr":     "\u795e\u7ecf\u6e32\u67d3",
        "compare":       "\u753b\u9762\u5bf9\u6bd4",
        "compare_split": "\u5206\u5272\u4f4d\u7f6e",
        "compare_zoom":  "\u653e\u5927",
        "language":      "\u8bed\u8a00",
        "shortcut":      "\u83dc\u5355\u952e",
        "on":            "\u5f00",
        "off":           "\u5173",
        "async":         "\u5f02\u6b65\uff1a\u7f51\u7edc\u5728\u6e38\u620f\u7ed8\u5236\u4e0b\u4e00\u5e27\u65f6\u5de5\u4f5c",
        "async_hint":    "\u6bcf\u79d2\u5e27\u6570\u66f4\u591a\uff0c\u800c\u5c4f\u5e55\u4e0a\u663e\u793a\u7684\u6bd4\u5904\u7406\u7ed3\u679c\u843d\u540e\u4e00\u5e27\u3002"
                         "\u4e0b\u6b21\u6e38\u620f\u542f\u52a8\u751f\u6548\u3002",
        "gpu_image":     "\u5b9e\u9a8c\u6027\uff1a\u5e27\u4fdd\u7559\u5728 GPU \u4e0a",
        "gpu_image_hint": "\u5e27\u4fdd\u7559\u5728 GPU \u4e0a\u3002\u8981\u6c42\u5f02\u6b65\u5173\u95ed\u4e14\u6bcf\u5e27\u5904\u7406\uff0c"
                          "\u5426\u5219\u8be5\u6a21\u5f0f\u4e0b\u4f1a\u8df3\u8fc7\u6548\u679c\u3002\u4e0b\u6b21\u6e38\u620f\u542f\u52a8\u751f\u6548\u3002",
        "every":         "\u7f51\u7edc\u8fd0\u884c\u4e8e",
        "every_hint":    "\u5176\u95f4\u5404\u5e27\u91cd\u590d\u4e0a\u4e00\u6b21\u7ed3\u679c\uff1a\u6e38\u620f\u8dd1\u5f97\u66f4\u5feb\uff0c"
                         "\u753b\u9762\u53ea\u5728\u7f51\u7edc\u51fa\u5e27\u65f6\u53d8\u5316\u3002\u4e0b\u6b21\u6e38\u620f\u542f\u52a8\u751f\u6548\u3002",
        "every_frame":   "\u6bcf\u5e27",
        "every_nth_few":  "\u6bcf {} \u5e27",
        "every_nth_many": "\u6bcf {} \u5e27",
        "choice_weights_ht": "A\uff1aHT\uff08\u672c\u9879\u76ee\u590d\u73b0\u7684\u6a21\u578b\uff09",
        "choice_weights_cc_small_fp8": "B\uff1aCC small FP8\uff08\u66f4\u5feb\u7684\u6a21\u578b\uff09",
        "gpu_blocked_both": "\u7ec6\u8282\u5f3a\u5ea6\u548c\u8272\u5f69\u5f3a\u5ea6\u90fd\u4e0d\u4e3a 1\uff0cGPU \u5408\u6210\u4e0d\u63a5\u53d7\u8fd9\u5bf9\u53d6\u503c\uff1a"
                            "\u753b\u9762\u4ecd\u5728\u5904\u7406\uff0c\u4f46\u8d70\u7684\u662f CPU \u8def\u5f84\u3002"
                            "\u628a\u4e24\u9879\u90fd\u8c03\u56de 1 \u518d\u7528 GPU \u8def\u5f84\u3002",
        "gpu_blocked_detail": "\u7ec6\u8282\u5f3a\u5ea6\u4e0d\u4e3a 1\uff0cGPU \u5408\u6210\u4e0d\u63a5\u53d7\uff1a"
                              "\u753b\u9762\u4ecd\u5728\u5904\u7406\uff0c\u4f46\u8d70\u7684\u662f CPU \u8def\u5f84\u3002"
                              "\u628a\u7ec6\u8282\u5f3a\u5ea6\u8c03\u56de 1 \u518d\u7528 GPU \u8def\u5f84\u3002",
        "gpu_blocked_colour": "\u8272\u5f69\u5f3a\u5ea6\u4e0d\u4e3a 1\uff0cGPU \u5408\u6210\u4e0d\u63a5\u53d7\uff1a"
                              "\u753b\u9762\u4ecd\u5728\u5904\u7406\uff0c\u4f46\u8d70\u7684\u662f CPU \u8def\u5f84\u3002"
                              "\u628a\u8272\u5f69\u5f3a\u5ea6\u8c03\u56de 1 \u518d\u7528 GPU \u8def\u5f84\u3002",
    },
    "ru": {
        "hint":          "\u0412\u0432\u0435\u0440\u0445/\u0412\u043d\u0438\u0437 \u0432\u044b\u0431\u043e\u0440  "
                         "\u0412\u043b\u0435\u0432\u043e/\u0412\u043f\u0440\u0430\u0432\u043e \u0438\u0437\u043c\u0435\u043d\u0438\u0442\u044c  "
                         "L \u044f\u0437\u044b\u043a  Home \u0432\u043a\u043b/\u0432\u044b\u043a\u043b  "
                         "S \u0441\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c  R \u0441\u0431\u0440\u043e\u0441  INS \u0437\u0430\u043a\u0440\u044b\u0442\u044c  "
                         "\u0438\u0433\u0440\u0430 \u0442\u043e\u0436\u0435 \u0432\u0438\u0434\u0438\u0442 \u044d\u0442\u0438 \u043a\u043b\u0430\u0432\u0438\u0448\u0438",
        "toggle_nr":     "\u043d\u0435\u0439\u0440\u043e\u043d\u043d\u044b\u0439 \u0440\u0435\u043d\u0434\u0435\u0440\u0438\u043d\u0433",
        "compare":       "\u0441\u0440\u0430\u0432\u043d\u0435\u043d\u0438\u0435",
        "compare_split": "\u0433\u0440\u0430\u043d\u0438\u0446\u0430",
        "compare_zoom":  "\u0443\u0432\u0435\u043b\u0438\u0447\u0435\u043d\u0438\u0435",
        "language":      "\u044f\u0437\u044b\u043a",
        "shortcut":      "\u043a\u043b\u0430\u0432\u0438\u0448\u0430 \u043c\u0435\u043d\u044e",
        "on":            "\u0432\u043a\u043b",
        "off":           "\u0432\u044b\u043a\u043b",
        "async":         "\u0410\u0441\u0438\u043d\u0445\u0440\u043e\u043d\u043d\u043e: \u0441\u0435\u0442\u044c \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442 "
                         "\u043f\u043e\u043a\u0430 \u0438\u0433\u0440\u0430 \u0440\u0438\u0441\u0443\u0435\u0442 \u0441\u043b\u0435\u0434\u0443\u044e\u0449\u0438\u0439 \u043a\u0430\u0434\u0440",
        "async_hint":    "\u0411\u043e\u043b\u044c\u0448\u0435 \u043a\u0430\u0434\u0440\u043e\u0432 \u0432 \u0441\u0435\u043a\u0443\u043d\u0434\u0443, "
                         "\u043d\u043e \u043d\u0430 \u044d\u043a\u0440\u0430\u043d\u0435 \u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442 "
                         "\u043d\u0430 \u043e\u0434\u0438\u043d \u043a\u0430\u0434\u0440 \u043f\u043e\u0437\u0436\u0435. "
                         "\u041f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u0441\u044f \u043f\u0440\u0438 \u0441\u043b\u0435\u0434\u0443\u044e\u0449\u0435\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0435 \u0438\u0433\u0440\u044b.",
        "gpu_image":     "\u042d\u043a\u0441\u043f\u0435\u0440\u0438\u043c\u0435\u043d\u0442: \u043e\u0441\u0442\u0430\u0432\u043b\u044f\u0442\u044c \u043a\u0430\u0434\u0440 \u043d\u0430 GPU",
        "gpu_image_hint": "\u041a\u0430\u0434\u0440 \u043e\u0441\u0442\u0430\u0451\u0442\u0441\u044f \u043d\u0430 GPU. "
                          "\u0422\u0440\u0435\u0431\u0443\u0435\u0442 \u0432\u044b\u043a\u043b\u044e\u0447\u0435\u043d\u043d\u043e\u0433\u043e "
                          "\u0430\u0441\u0438\u043d\u0445\u0440\u043e\u043d\u043d\u043e\u0433\u043e \u0440\u0435\u0436\u0438\u043c\u0430 "
                          "\u0438 \u043e\u0431\u0440\u0430\u0431\u043e\u0442\u043a\u0438 \u043a\u0430\u0436\u0434\u043e\u0433\u043e \u043a\u0430\u0434\u0440\u0430, "
                          "\u0438\u043d\u0430\u0447\u0435 \u0432 \u044d\u0442\u043e\u043c \u0440\u0435\u0436\u0438\u043c\u0435 "
                          "\u044d\u0444\u0444\u0435\u043a\u0442 \u043f\u0440\u043e\u043f\u0443\u0441\u043a\u0430\u0435\u0442\u0441\u044f. "
                          "\u041f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u0441\u044f \u043f\u0440\u0438 \u0441\u043b\u0435\u0434\u0443\u044e\u0449\u0435\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0435 \u0438\u0433\u0440\u044b.",
        "every":         "\u0421\u0435\u0442\u044c \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442 \u043d\u0430",
        "every_hint":    "\u041a\u0430\u0434\u0440\u044b \u043c\u0435\u0436\u0434\u0443 \u043d\u0438\u043c\u0438 "
                         "\u043f\u043e\u0432\u0442\u043e\u0440\u044f\u044e\u0442 \u043f\u0440\u0435\u0434\u044b\u0434\u0443\u0449\u0438\u0439 "
                         "\u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442: \u0438\u0433\u0440\u0430 \u0438\u0434\u0451\u0442 "
                         "\u0431\u044b\u0441\u0442\u0440\u0435\u0435, \u0430 \u043a\u0430\u0440\u0442\u0438\u043d\u043a\u0430 "
                         "\u043c\u0435\u043d\u044f\u0435\u0442\u0441\u044f \u0442\u043e\u043b\u044c\u043a\u043e \u043a\u043e\u0433\u0434\u0430 "
                         "\u0441\u0435\u0442\u044c \u0432\u044b\u0434\u0430\u0451\u0442 \u043a\u0430\u0434\u0440. "
                          "\u041f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u0441\u044f \u043f\u0440\u0438 \u0441\u043b\u0435\u0434\u0443\u044e\u0449\u0435\u043c \u0437\u0430\u043f\u0443\u0441\u043a\u0435 \u0438\u0433\u0440\u044b.",
        "every_frame":   "\u043a\u0430\u0436\u0434\u044b\u0439 \u043a\u0430\u0434\u0440",
        # 2-4 takes кадра and 5 and up takes кадров: Russian inflects, and one `{}` cannot choose.
        "every_nth_few":  "\u043a\u0430\u0436\u0434\u044b\u0435 {} \u043a\u0430\u0434\u0440\u0430",
        "every_nth_many": "\u043a\u0430\u0436\u0434\u044b\u0435 {} \u043a\u0430\u0434\u0440\u043e\u0432",
        "choice_weights_ht": "A: HT, \u043c\u043e\u0434\u0435\u043b\u044c, \u043a\u043e\u0442\u043e\u0440\u0443\u044e \u0432\u043e\u0441\u043f\u0440\u043e\u0438\u0437\u0432\u043e\u0434\u0438\u0442 \u043f\u0440\u043e\u0435\u043a\u0442",
        "choice_weights_cc_small_fp8": "B: CC small FP8, \u0431\u044b\u0441\u0442\u0440\u0430\u044f \u043c\u043e\u0434\u0435\u043b\u044c",
        "gpu_blocked_both": "\u0421\u0438\u043b\u0430 \u0434\u0435\u0442\u0430\u043b\u0435\u0439 \u0438 \u0441\u0438\u043b\u0430 \u0446\u0432\u0435\u0442\u0430 "
                            "\u043e\u0431\u0435 \u043d\u0435 \u0440\u0430\u0432\u043d\u044b 1, \u0430 GPU-\u043a\u043e\u043c\u043f\u043e\u0437\u0438\u0446\u0438\u044f "
                            "\u0442\u0430\u043a\u0438\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u044f \u043d\u0435 \u043f\u0440\u0438\u043d\u0438\u043c\u0430\u0435\u0442: "
                            "\u043a\u0430\u0434\u0440\u044b \u0432\u0441\u0451 \u0440\u0430\u0432\u043d\u043e \u043e\u0431\u0440\u0430\u0431\u0430\u0442\u044b\u0432\u0430\u044e\u0442\u0441\u044f, "
                            "\u043d\u043e \u043d\u0430 CPU. \u041f\u043e\u0441\u0442\u0430\u0432\u044c\u0442\u0435 \u043e\u0431\u0430 \u0432 1, \u0447\u0442\u043e\u0431\u044b "
                            "\u0438\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c GPU.",
        "gpu_blocked_detail": "\u0421\u0438\u043b\u0430 \u0434\u0435\u0442\u0430\u043b\u0435\u0439 \u043d\u0435 \u0440\u0430\u0432\u043d\u0430 1, \u0430 "
                              "GPU-\u043a\u043e\u043c\u043f\u043e\u0437\u0438\u0446\u0438\u044f \u0442\u0430\u043a\u043e\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 "
                              "\u043d\u0435 \u043f\u0440\u0438\u043d\u0438\u043c\u0430\u0435\u0442: \u043a\u0430\u0434\u0440\u044b \u0432\u0441\u0451 \u0440\u0430\u0432\u043d\u043e "
                              "\u043e\u0431\u0440\u0430\u0431\u0430\u0442\u044b\u0432\u0430\u044e\u0442\u0441\u044f, \u043d\u043e \u043d\u0430 CPU. "
                              "\u041f\u043e\u0441\u0442\u0430\u0432\u044c\u0442\u0435 1, \u0447\u0442\u043e\u0431\u044b \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c GPU.",
        "gpu_blocked_colour": "\u0421\u0438\u043b\u0430 \u0446\u0432\u0435\u0442\u0430 \u043d\u0435 \u0440\u0430\u0432\u043d\u0430 1, \u0430 "
                              "GPU-\u043a\u043e\u043c\u043f\u043e\u0437\u0438\u0446\u0438\u044f \u0442\u0430\u043a\u043e\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 "
                              "\u043d\u0435 \u043f\u0440\u0438\u043d\u0438\u043c\u0430\u0435\u0442: \u043a\u0430\u0434\u0440\u044b \u0432\u0441\u0451 \u0440\u0430\u0432\u043d\u043e "
                              "\u043e\u0431\u0440\u0430\u0431\u0430\u0442\u044b\u0432\u0430\u044e\u0442\u0441\u044f, \u043d\u043e \u043d\u0430 CPU. "
                              "\u041f\u043e\u0441\u0442\u0430\u0432\u044c\u0442\u0435 1, \u0447\u0442\u043e\u0431\u044b \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c GPU.",
    },
    "es": {
        "hint":          "Arriba/Abajo elegir  Izq/Der cambiar  L idioma  Home activar  "
                         "S guardar  R restablecer  INS cerrar  "
                         "el juego tambi\u00e9n recibe estas teclas",
        "toggle_nr":     "renderizado neuronal",
        "compare":       "comparaci\u00f3n",
        "compare_split": "divisi\u00f3n",
        "compare_zoom":  "aumento",
        "language":      "idioma",
        "shortcut":      "tecla del men\u00fa",
        "on":            "activado",
        "off":           "desactivado",
        "async":         "As\u00edncrono: la red trabaja mientras el juego dibuja el siguiente fotograma",
        "async_hint":    "M\u00e1s fotogramas por segundo, pero lo que se ve va un fotograma "
                         "procesado por detr\u00e1s. Se aplica en el pr\u00f3ximo inicio del juego.",
        "gpu_image":     "Experimental: mantener el fotograma en la GPU",
        "gpu_image_hint": "El fotograma se queda en la GPU. Requiere el modo as\u00edncrono "
                          "desactivado y el procesamiento de cada fotograma, o el efecto se "
                          "omite en este modo. Se aplica en el pr\u00f3ximo inicio del juego.",
        "every":         "La red se ejecuta en",
        "every_hint":    "Los fotogramas intermedios repiten el resultado anterior: el juego va "
                         "m\u00e1s r\u00e1pido y la imagen solo cambia cuando la red produce uno. "
                         "Se aplica en el pr\u00f3ximo inicio del juego.",
        "every_frame":   "cada fotograma",
        "every_nth_few":  "cada {} fotogramas",
        "every_nth_many": "cada {} fotogramas",
        "choice_weights_ht": "A: HT, el modelo que reproduce el proyecto",
        "choice_weights_cc_small_fp8": "B: CC small FP8, el modelo r\u00e1pido",
        "gpu_blocked_both": "Ni la fuerza del detalle ni la del color son 1, y la composici\u00f3n por "
                            "GPU no acepta esa pareja: la imagen se sigue procesando, en la ruta "
                            "de CPU. Pon las dos en 1 para usar la ruta de GPU.",
        "gpu_blocked_detail": "La fuerza del detalle no es 1, y la composici\u00f3n por GPU no acepta "
                              "ese valor: la imagen se sigue procesando, en la ruta de CPU. Ponla "
                              "en 1 para usar la ruta de GPU.",
        "gpu_blocked_colour": "La fuerza del color no es 1, y la composici\u00f3n por GPU no acepta "
                              "ese valor: la imagen se sigue procesando, en la ruta de CPU. Ponla "
                              "en 1 para usar la ruta de GPU.",
    },
}

# The compare-mode names. The wizard has no compare feature, so these stay here for all four
# languages; OptiScaler's own order is Off / Side by side / Wipe, which this keeps.
COMPARE = {
    "en": ("Off", "Wipe", "Side by side"),
    "zh": ("\u5173\u95ed", "\u64e6\u9664", "\u5e76\u6392"),
    "ru": ("\u0412\u044b\u043a\u043b", "\u0428\u0442\u043e\u0440\u043a\u0430", "\u0420\u044f\u0434\u043e\u043c"),
    "es": ("Apagado", "Cortina", "Lado a lado"),
}


def wizard_tables():
    """The wizard's `en`, `ru` and `es` hashtables, by line range into the file.

    The keys are matched with or without quotes, because the wizard uses both: a key with a dot in
    it is written `'Knob.render_scale'` and a plain one is written `Title`. Matching only the
    quoted form silently dropped every plain key, which showed up as "ru is missing title" - so the
    pattern takes both and the check below catches it if the wizard changes shape again.
    """
    lines = WIZARD.read_text(encoding="utf-8", errors="replace").splitlines()
    starts = {}
    for i, line in enumerate(lines):
        m = re.match(r"\s*(en|ru|es|zh)\s*=\s*@\{\s*$", line)
        if m:
            starts[m.group(1)] = i

    tables = {}
    pair = re.compile(r"(?:'([\w.\-]+)'|([A-Za-z_][\w.\-]*))\s*=\s*'((?:[^']|'')*)'")
    for lang, i in starts.items():
        depth, body = 0, []
        for line in lines[i:]:
            depth += line.count("{") - line.count("}")
            body.append(line)
            if depth <= 0 and len(body) > 1:
                break
        found = {}
        for quoted, plain, value in pair.findall("\n".join(body)):
            found[quoted or plain] = value.replace("''", "'")
        tables[lang] = found
    return tables


def build():
    tables = wizard_tables()
    if sorted(tables) != ["en", "es", "ru", "zh"]:
        raise SystemExit(f"expected en/es/ru/zh in the wizard, got {sorted(tables)}")

    # Labels: the wizard's for every language it carries, and OWN's for the keys it has no
    # equivalent for. English is the fallback only when a language names nothing at all.
    labels = {}
    for lang, table in tables.items():
        got = {}
        for knob in KNOBS:
            if f"Knob.{knob}" in table:
                got[knob] = table[f"Knob.{knob}"]
        for our, (wizard_key, fallback) in EXTRA.items():
            if wizard_key and wizard_key in table:
                got[our] = table[wizard_key]
            elif our in OWN.get(lang, {}):
                got[our] = OWN[lang][our]      # a key the wizard has no equivalent for
            elif lang == "en":
                got[our] = fallback
        labels[lang] = got

    # Hints: the wizard's `Hint.*` for every language it carries. A hint is what a tooltip shows,
    # and this is the array the overlay's question-mark reads - so it has to be complete for all
    # four, which is what the check below enforces.
    hints = {}
    for lang, table in tables.items():
        hints[lang] = {k: table[f"Hint.{k}"] for k in KNOBS if f"Hint.{k}" in table}

    # Every language must carry every key, or one of them shows a blank in a menu nobody can read.
    keys = set(labels["en"])
    for lang in ("zh", "ru", "es"):
        missing = sorted(keys - set(labels[lang]))
        if missing:
            raise SystemExit(f"{lang} is missing labels: {missing}")

    # And every language must have a hint for every knob: a knob with no explanation is a
    # question mark that shows nothing.
    for lang in ("zh", "ru", "es"):
        missing = sorted(set(KNOBS) - set(hints[lang]))
        if missing:
            raise SystemExit(f"{lang} is missing hints: {missing}")

    for lang in ("zh", "ru", "es"):
        if len(COMPARE[lang]) != len(COMPARE_MODES):
            raise SystemExit(f"{lang} has {len(COMPARE[lang])} compare modes, expected "
                             f"{len(COMPARE_MODES)}")

    write_header(labels, hints, COMPARE)
    write_module(labels, hints, COMPARE)
    print(f"  {HEADER.relative_to(REPO)}")
    print(f"  {MODULE.relative_to(REPO)}")
    print(f"  {len(keys)} keys x 4 languages, {len(KNOBS)} hints each, "
          f"{len(COMPARE_MODES)} compare modes")


def c_string(text):
    """A C string literal, UTF-8 with the bytes escaped as octal.

    Escaped rather than written raw because the layer compiles with /utf-8 on MSVC and a bare
    multibyte literal in a .c file is fine there but is not fine for every compiler that builds
    this tree, and the escape form is unambiguous in all of them.
    """
    out = []
    for byte in text.encode("utf-8"):
        if byte == 0x22:
            out.append('\\"')
        elif byte == 0x5C:
            out.append("\\\\")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


def write_header(labels, hints, compare):
    langs = ("en", "zh", "ru", "es")
    keys = list(labels["en"])
    lines = [
        "/* The menu's strings, in four languages. GENERATED - do not edit by hand.",
        " *",
        " * Written by `tools/gen_menu_strings.py` from two sources:",
        " *   en, ru, es  `dist-tools/windows-wizard.ps1`, which already ships them for every knob.",
        " *               Extracted rather than retyped, because the wizard's keys are",
        " *               `nr_knobs.py`'s names - so the tables join without a mapping and a knob",
        " *               added there brings its label here.",
        " *   zh          written in the generator. The wizard has no Chinese, so this is the one",
        " *               column that is new work.",
        " *",
        " * Strings are UTF-8 bytes, octal-escaped, so the literal means the same thing whatever",
        " * codepage the compiler reads the file in. The menu draws them through the atlas, which",
        " * is built from these codepoints at start-up - see nr_menu_font.c.",
        " */",
        "#ifndef NR_MENU_STRINGS_H",
        "#define NR_MENU_STRINGS_H",
        "",
        "#define NR_MENU_LANG_COUNT 4",
        "#define NR_MENU_KEY_COUNT " + str(len(keys)),
        "#define NR_MENU_COMPARE_COUNT " + str(len(compare["en"])),
        "/* The hints are one per KNOB, which is fewer than the keys: a key like `title` or `on` is",
        " * a string the menu shows, not a knob it explains. Declaring them with the key count left",
        " * the tail of each row zero-initialised, and the atlas builder walked into those NULLs -",
        " * which cost the menu its Chinese and Russian glyphs and faulted on the first real game. */",
        "#define NR_MENU_KNOB_COUNT " + str(len(KNOBS)),
        "enum { NR_LANG_EN = 0, NR_LANG_ZH, NR_LANG_RU, NR_LANG_ES };",
        "",
        "/* C linkage, like nr_menu.h and nr_menu_bridge.h.",
        " *",
        " * The tables are defined in a .c file and the menu's own UI is C++. Without this guard a",
        " * C++ caller looks for a mangled name (`?nr_menu_text@@YAPEBDHH@Z`) that nothing defines, so",
        " * the link fails only once something in C++ calls into this table - which is what happened",
        " * to the first fixture that did, after these strings had been used from C alone. */",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
        "enum {",
    ]
    for i, k in enumerate(keys):
        lines.append(f"    NR_STR_{k.upper()} = {i},")
    lines += [
        "};",
        "",
        "/* [language][key] */",
        "extern const char *const nr_menu_strings[NR_MENU_LANG_COUNT][NR_MENU_KEY_COUNT];",
        "",
        "/* [language][mode] - the compare mode names, cycled by the compare key. */",
        "extern const char *const nr_menu_compare_names[NR_MENU_LANG_COUNT][NR_MENU_COMPARE_COUNT];",
        "",
        "/* [language][knob] - the one-line hint for a knob, indexed as nr_knobs.py lists them. */",
        "extern const char *const nr_menu_hints[NR_MENU_LANG_COUNT][NR_MENU_KNOB_COUNT];",
        "",
        "/* Implemented in nr_menu_strings_lookup.c, which is hand-written: these are logic, not",
        " * data, and would be lost on the next run of the generator. */",
        "",
        "/* The string for one key in one language. Falls back to English, then to an empty string,",
        " * so a hole in a table is a word in the wrong language rather than a blank row. */",
        "const char *nr_menu_text(int language, int key);",
        "",
        "/* Which key index a knob name maps to, or -1. The names are nr_knobs.py's, so the knob",
        " * list joins to this table without a mapping that could itself drift. */",
        "int nr_menu_key_for(const char *knob_name);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif /* NR_MENU_STRINGS_H */",
        "",
    ]
    (REPO / "src" / "layer" / "nr_menu_strings.h").write_text("\n".join(lines),
                                                              encoding="utf-8", newline="\n")

    body = ['/* GENERATED by tools/gen_menu_strings.py - do not edit by hand. See the header for',
            ' * where each language comes from. */',
            '#include "nr_menu_strings.h"',
            '',
            'const char *const nr_menu_strings[NR_MENU_LANG_COUNT][NR_MENU_KEY_COUNT] = {']
    for lang in langs:
        body.append(f"    /* {lang} */ {{")
        for k in keys:
            body.append(f"        {c_string(labels[lang][k])},")
        body.append("    },")
    body += ['};', '', 'const char *const nr_menu_compare_names[NR_MENU_LANG_COUNT]'
                       '[NR_MENU_COMPARE_COUNT] = {']
    for lang in langs:
        names = ", ".join(c_string(n) for n in compare[lang])
        body.append(f"    /* {lang} */ {{ {names} }},")
    body += ['};', '', 'const char *const nr_menu_hints[NR_MENU_LANG_COUNT][NR_MENU_KNOB_COUNT] = {']
    for lang in langs:
        body.append(f"    /* {lang} */ {{")
        for k in KNOBS:
            body.append(f"        {c_string(hints[lang].get(k, ''))},")
        body.append("    },")
    body += ['};', '']
    (REPO / "src" / "layer" / "nr_menu_strings.c").write_text("\n".join(body),
                                                              encoding="utf-8", newline="\n")
    print(f"  src/layer/nr_menu_strings.c")


def write_module(labels, hints, compare):
    langs = ("en", "zh", "ru", "es")
    lines = [
        '"""The menu\'s strings, in four languages. GENERATED - do not edit by hand.',
        "",
        "Written by `tools/gen_menu_strings.py`; see the generated header for where each language",
        "comes from. Kept in step with the C table by construction: both are written from the same",
        "run, so the menu and the INI cannot disagree about a label.",
        '"""',
        "",
        "LANGUAGES = ('en', 'zh', 'ru', 'es')",
        "",
        "LABELS = {",
    ]
    for lang in langs:
        lines.append(f"    {lang!r}: {{")
        for k, v in labels[lang].items():
            lines.append(f"        {k!r}: {v!r},")
        lines.append("    },")
    lines += ["}", "", "HINTS = {"]
    for lang in langs:
        lines.append(f"    {lang!r}: {{")
        for k, v in hints[lang].items():
            lines.append(f"        {k!r}: {v!r},")
        lines.append("    },")
    lines += ["}", "", "COMPARE = {"]
    for lang in langs:
        lines.append(f"    {lang!r}: {list(compare[lang])!r},")
    lines += ["}", "", "COMPARE_MODES = ('off', 'wipe', 'side_by_side')", ""]
    MODULE.write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    build()
