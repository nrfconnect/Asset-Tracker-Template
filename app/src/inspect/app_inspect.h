/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _APP_INSPECT_H_
#define _APP_INSPECT_H_

#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_inspect_provider {
	const char *name;
	const char *(*get_state_name)(void);
};

#if defined(CONFIG_APP_INSPECT_SHELL)
/* Macro to register an inspect provider for a module.
 * The provider will be used to print the state of the module in the inspect shell.
 */
#define APP_INSPECT_MODULE_REGISTER(_name, _get_state_name_fn)                                     \
	STRUCT_SECTION_ITERABLE(app_inspect_provider, _name##_inspect_provider) = {                \
		.name = #_name,                                                                    \
		.get_state_name = _get_state_name_fn,                                              \
	}

/* Macro to register an inspect provider and state names */
#define APP_INSPECT_MODULE_REGISTER_STATE(_name, _state_ctx_ptr, _states, _state_enum_t,           \
					  _to_string_fn)                                           \
	/* Create a function that translates the current state to a string */                      \
	static const char *_name##_state_name_get(void)                                            \
	{                                                                                          \
		const struct smf_state *current;                                                   \
                                                                                                   \
		if ((_state_ctx_ptr) == NULL) {                                                    \
			return "STATE_UNINITIALIZED";                                              \
		}                                                                                  \
                                                                                                   \
		current = smf_get_current_leaf_state(SMF_CTX(_state_ctx_ptr));                     \
                                                                                                   \
		for (size_t i = 0; i < ARRAY_SIZE(_states); i++) {                                 \
			if (current == &(_states)[i]) {                                            \
				return _to_string_fn((_state_enum_t)i);                            \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		return "STATE_UNKNOWN";                                                            \
	}                                                                                          \
	APP_INSPECT_MODULE_REGISTER(_name, _name##_state_name_get)
#else
/* If the inspect shell is not enabled, define empty macros to avoid compilation errors. */
#define APP_INSPECT_MODULE_REGISTER(_name, _get_state_name_fn)
#define APP_INSPECT_MODULE_REGISTER_STATE(_name, _state_ctx_ptr, _states, _state_enum_t,           \
					  _to_string_fn)
#endif

#ifdef __cplusplus
}
#endif

#endif /* _APP_INSPECT_H_ */
