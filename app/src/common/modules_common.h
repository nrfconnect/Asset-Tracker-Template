/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MODULES_COMMON_H_
#define _MODULES_COMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Set the initial state for a module.
 *
 *  @param _state_obj State machine object.
 *  @param _state State to set as initial.
 *
 *  @return see smf_set_initial().
 */
#define STATE_SET_INITIAL(_state)	smf_set_initial(SMF_CTX(&_state_obj), &states[_state])

/** @brief Set the state for a module.
 *
 *  @param _state_obj State machine object.
 *  @param _state State to set.
 *
 *  @return see smf_set_state().
 */
#define STATE_SET(_state)		smf_set_state(SMF_CTX(&_state_obj), &states[_state])

/** @brief Set the state for a module and handle the event.
 *
 *  @param _state_obj State machine object.
 *
 *  @return see smf_set_handled().
 */
#define STATE_EVENT_HANDLED(_state_obj)	smf_set_handled(SMF_CTX(&_state_obj))

/** @brief Run the state machine for a module.
 *
 *  @param _state_obj State machine object.
 *
 *  @return see smf_run_state().
 */
#define STATE_RUN(_state_obj)			smf_run_state(SMF_CTX(&_state_obj))

#ifdef __cplusplus
}
#endif

#endif /* _MODULES_COMMON_H_ */
