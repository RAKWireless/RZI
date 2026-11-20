/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief RZI LoRaWAN FUOTA coordination API.
 */
#ifndef RZI_LORAWAN_FUOTA_H
#define RZI_LORAWAN_FUOTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

#include <rzi/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup rzi_fuota RZI FUOTA service
 *  @brief Backend-independent firmware-update coordination for ChirpStack.
 *
 *  LoRaWAN Clock Synchronization, Remote Multicast Setup and Fragmentation
 *  remain inside the selected backend. On USP the fragmentation descriptor
 *  selects the store. Descriptor 0 writes the MCUboot secondary slot. After
 *  the multicast window the service restores the class that was active when
 *  the window was accepted, then waits for a local StatusAns transmission or
 *  30 seconds before the test upgrade and reboot. Descriptor 0x304B4152
 *  (bytes 52 41 4B 30, ASCII RAK0) writes the fuota_delta partition and leaves
 *  the device running. Any other descriptor is rejected. The image is not
 *  checked here.
 *
 *  @since 0.1.0
 *  @version 0.1.0
 *  @{
 */

/** Observable FUOTA session states. */
enum rzi_fuota_state {
	/** Service is not active or the device is not joined. */
	RZI_FUOTA_STATE_IDLE,
	/** Joined and waiting for a ChirpStack or network-server session. */
	RZI_FUOTA_STATE_READY,
	/** A multicast fragment session is active. */
	RZI_FUOTA_STATE_TRANSFERRING,
	/** The backend reconstructed a complete firmware image. */
	RZI_FUOTA_STATE_COMPLETE,
	/** The latest session failed to reconstruct an image. */
	RZI_FUOTA_STATE_FAILED,
	/** Image installation or reboot is in progress. */
	RZI_FUOTA_STATE_APPLYING,
};

/** Snapshot returned by @ref rzi_fuota_get_status. */
struct rzi_fuota_status {
	/** Current coordination state. */
	enum rzi_fuota_state state;
	/** True when a reconstructed image can be read or applied. */
	bool image_ready;
	/** Detected or configured image size, or zero when unknown. */
	size_t image_size;
	/** Zero, or the negative RZI_ERR_* from the latest failure. */
	int last_error;
};

/**
 * @brief Asynchronous FUOTA callbacks.
 *
 * Callbacks execute in the LoRaWAN dispatcher thread. They must return
 * promptly and must not call @ref rzi_fuota_apply.
 *
 */
struct rzi_fuota_callbacks {
	/** Called after every observable state transition. */
	void (*state_changed)(enum rzi_fuota_state state, void *user_data);
	/** Called when a multicast fragment session starts. */
	void (*session_started)(void *user_data);
	/** Called with zero after a successful transfer, otherwise a negative RZI_ERR_*. */
	void (*complete)(int status, void *user_data);
	/** Opaque pointer passed to every callback. */
	void *user_data;
};

/**
 * @brief Register optional FUOTA callbacks.
 *
 * The table is copied. Register before @ref rzi_fuota_init to observe the
 * first READY transition.
 *
 * @param callbacks Callback table copied before this function returns.
 *
 * @retval 0 Callbacks stored.
 * @retval -RZI_ERR_INVALID callbacks is NULL or empty.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only.
 */
__must_check int rzi_fuota_register_callbacks(const struct rzi_fuota_callbacks *callbacks);

/**
 * @brief Initialize FUOTA coordination.
 *
 * Clock synchronization starts after the next successful LoRaWAN join. The
 * backend then accepts ChirpStack unicast setup and multicast fragments.
 *
 * @retval 0 Coordination initialized or already active.
 * @retval -RZI_ERR_NOT_SUPPORTED The selected backend does not advertise FUOTA.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only. rzi_lorawan_init() initializes this service when
 *       CONFIG_RZI_LORAWAN_FUOTA is enabled.
 */
__must_check int rzi_fuota_init(void);

/**
 * @brief Return the current FUOTA session snapshot.
 *
 * @param[out] status Caller-owned status object.
 *
 * @retval 0 Status stored.
 * @retval -RZI_ERR_INVALID status is NULL.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only.
 */
__must_check int rzi_fuota_get_status(struct rzi_fuota_status *status);

/**
 * @brief One accepted fragmentation round.
 *
 * storage_errno is zero when no reconstructed-byte write has failed.
 */
struct rzi_fuota_round {
	/** Generation assigned when this setup was accepted. Never zero. */
	uint32_t generation;
	/** True after a reconstructed-byte write has failed. */
	bool write_failed;
	/** First flash error, or zero when the writes succeeded. */
	int storage_errno;
};

/**
 * @brief Copy the accepted fragmentation round.
 *
 * @param[out] out Caller-owned round object.
 *
 * @retval 0 Round copied.
 * @retval -RZI_ERR_INVALID out is NULL.
 * @retval -RZI_ERR_NOT_FOUND No setup has been accepted for the current round.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only.
 */
__must_check int rzi_fuota_get_round(struct rzi_fuota_round *out);

/**
 * @brief Report whether an application uplink may be sent.
 *
 * Returns false while a multicast fragment window is open, while an upgrade
 * reboot is being requested, and while a fragmentation or multicast answer
 * is still queued.
 *
 * @retval true Application uplinks may be sent.
 * @retval false A FUOTA window or installation owns the radio.
 *
 * @note Thread context only.
 */
bool rzi_fuota_uplink_allowed(void);

/**
 * @brief Optionally override the reconstructed image size in bytes.
 *
 * The service prefers the size reported by the backend, then an MCUboot
 * image header. Call this only when both are unavailable, for example a
 * raw binary on a backend that does not export the fragment session size.
 *
 * @param size Image size in bytes.
 *
 * @retval 0 Size stored.
 * @retval -RZI_ERR_INVALID size is zero.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only.
 */
__must_check int rzi_fuota_set_expected_size(size_t size);

/**
 * @brief Read bytes from the reconstructed firmware image.
 *
 * @param offset Byte offset from the start of the image.
 * @param buffer Destination buffer.
 * @param size Number of bytes to copy.
 *
 * @retval 0 Bytes copied.
 * @retval -RZI_ERR_INVALID buffer is NULL or size is zero.
 * @retval -RZI_ERR_NOT_FOUND No reconstructed image is available.
 * @retval -RZI_ERR_NOT_SUPPORTED The backend cannot expose the stored image.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only.
 */
__must_check int rzi_fuota_read_image(size_t offset, void *buffer, size_t size);

/**
 * @brief Request a MCUboot test upgrade and reboot.
 *
 * On USP a descriptor 0 image is already in the secondary slot. This writes
 * the upgrade trailer and reboots. The service calls it after the multicast
 * window, once StatusAns has been sent or 30 seconds have elapsed. A delta
 * session does not call this; calling it then returns -RZI_ERR_NOT_SUPPORTED.
 * The function does
 * not return on a successful reboot.
 *
 * @retval -RZI_ERR_NOT_FOUND No reconstructed image is available.
 * @retval -RZI_ERR_NO_DATA The image size is unknown.
 * @retval -RZI_ERR_NOT_SUPPORTED No installer is compiled into this firmware.
 * @retval -RZI_ERR_IO Copying the image into the secondary slot failed.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @warning Must not be called from a FUOTA callback.
 * @note Thread context only.
 */
__must_check int rzi_fuota_apply(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* RZI_LORAWAN_FUOTA_H */
