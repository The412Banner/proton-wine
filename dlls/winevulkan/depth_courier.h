/* SGSR2 depth-export moonshot — Gate 0 "courier probe" public interface.
 *
 * This header is intentionally free of any Vulkan / Android headers so it can
 * be included by the winevulkan UNIX ICD (vulkan.c) without pulling in the NDK
 * Vulkan universe (the courier keeps its own, decoupled from wine/vulkan.h).
 *
 * The whole probe is INERT unless the environment variable WINE_DEPTH_COURIER
 * is set to a non-zero value. When inert, depth_courier_maybe_start() does
 * nothing but a getenv() + an atomic test, so normal Wine users are unaffected.
 */

#ifndef __WINE_DEPTH_COURIER_H
#define __WINE_DEPTH_COURIER_H

/* host_get_instance_proc_addr must be the *raw host* vkGetInstanceProcAddr,
 * i.e. vk_funcs->p_vkGetInstanceProcAddr (dlsym'd out of libvulkan_wrapper.so
 * by win32u). It is passed as a void* so this header needs no Vulkan types.
 *
 * The call is a no-op unless WINE_DEPTH_COURIER is set. When enabled it runs
 * the capability probe exactly once, on a detached background thread, so it
 * never blocks or perturbs the guest's own vkCreateInstance/Device path. */
void depth_courier_maybe_start(void *host_get_instance_proc_addr);

#endif /* __WINE_DEPTH_COURIER_H */
