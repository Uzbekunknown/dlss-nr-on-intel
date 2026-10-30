/* The in-game menu's glue, as one Windows-only translation unit.
 *
 * **Why this file exists.** The menu's composite pass, its key reading, its row building and its
 * mouse all need `struct device_data` and `struct swapchain_data`, and they are the part of the
 * layer that is Windows by construction: the ImGui panel needs a font from the Windows font
 * directory, keys through `GetAsyncKeyState`, and the STORAGE usage its blend pass writes through.
 * None of that exists off Windows.
 *
 * Keeping them inside `nr_layer.c` meant either 88 generated no-op declarations or a layer that
 * always carries a menu it can never show. The maintainer's answer was better: move the glue into
 * one file, built only on Windows, and let `nr_layer.c` reach it through three hooks that are
 * empty elsewhere. Linux then compiles none of the menu and `nr_layer.c` comes back to the size it
 * has on `newnr`.
 *
 * **The hooks.** Three, matching the three moments the layer needs the menu:
 *
 *   nr_menu_layer_note_storage  a swapchain was created - remember whether it took STORAGE
 *   nr_menu_layer_present       a frame is about to present - build, read keys, draw
 *   nr_menu_layer_destroy       the device is going - release the bridge, panel, pool and fence
 *
 * The names are these, and the comment used to say create_swapchain and destroy_device, which
 * is what the code does NOT call them. A header that names functions nobody can find is worse than
 * no comment: it reads as documentation of an API that does not exist.
 *
 * Off Windows all three are `static inline` no-ops in this header, so `nr_layer.c` calls them with
 * no `#ifdef` of its own and there is exactly one place that decides whether a menu exists.
 */
#ifndef NR_MENU_LAYER_H
#define NR_MENU_LAYER_H

#include <vulkan/vulkan.h>

struct device_data;
struct swapchain_data;

#ifdef _WIN32

/* The swapchain accepted STORAGE, so the menu may write into its images. Called from the
 * swapchain-creation path once the flags are settled; a surface that refused STORAGE leaves
 * `storage_ok` at 0 and the menu is then never built. */
void nr_menu_layer_note_storage(struct device_data *data, struct swapchain_data *chain);

/* Build the menu on the first present that can carry it, read its keys, and draw it.
 *
 * Returns 1 when it drew into the frame, 0 otherwise. Never fails a present: a menu that cannot be
 * built sets a reason and the frame goes out without one. */
int nr_menu_layer_present(struct device_data *data, VkQueue queue,
                          const VkPresentInfoKHR *info);

/* Release what the menu owns. Safe to call on a device that never built one. */
void nr_menu_layer_destroy(struct device_data *data);

/* ------------------------------------------------ what `nr_layer.c` lends this file
 *
 * **These four have to be declared, and the reason is a real crash.** Without a prototype C assumes
 * a function returns `int`, so `nr_layer_trigger_path()` - a `const char *` - came back as a
 * 32-bit value with the top half of the address gone. The probe printed `0000000069E2E7E1` on a
 * 64-bit process, and `access()` on that address faulted the game a few frames in. It looked like
 * an unrelated crash because the truncation happens silently at the call.
 *
 * `nr_layer.c` exports them and includes this header, so a change to one of these signatures is a
 * compile error on both sides rather than a value that quietly loses its top half.
 */
void nr_layer_menu_globals(VkInstance *instance, PFN_vkGetInstanceProcAddr *next_proc);
struct swapchain_data *nr_layer_find_swapchain(VkSwapchainKHR swapchain);
void nr_layer_set_trigger_file(const char *path, int on);
const char *nr_layer_trigger_path(void);

#else /* !_WIN32 */

/* Nothing to do, and nothing to include: off Windows the menu does not exist, so these are the
 * whole of the layer's contact with it. The `(void)` casts are what keep a build with `-Wunused`
 * quiet about the parameters a real implementation would use. */
static inline void nr_menu_layer_note_storage(struct device_data *data, struct swapchain_data *chain)
{ (void)data; (void)chain; }
static inline int nr_menu_layer_present(struct device_data *data, VkQueue queue,
                                        const VkPresentInfoKHR *info)
{ (void)data; (void)queue; (void)info; return 0; }
static inline void nr_menu_layer_destroy(struct device_data *data) { (void)data; }

#endif /* _WIN32 */

#endif /* NR_MENU_LAYER_H */
