/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"

#include "audio_input.h"
#include "client.h"
#include "common/cursor.h"
#include "common/display.h"
#include "common/recording.h"
#include "dvc.h"
#include "error.h"
#include "keyboard.h"
#include "rdp.h"
#include "rdp_bitmap.h"
#include "rdp_cliprdr.h"
#include "rdp_disp.h"
#include "rdp_fs.h"
#include "rdp_print_job.h"
#include "rdp_gdi.h"
#include "rdp_glyph.h"
#include "rdp_pointer.h"
#include "rdp_rail.h"
#include "rdp_stream.h"
#if 0
#include "rdp_svc.h"
#endif

#ifdef ENABLE_COMMON_SSH
#include "common-ssh/sftp.h"
#include "common-ssh/ssh.h"
#include "common-ssh/user.h"
#endif

#include <freerdp/addin.h>
#include <freerdp/cache/bitmap.h>
#include <freerdp/cache/brush.h>
#include <freerdp/cache/glyph.h>
#include <freerdp/cache/offscreen.h>
#include <freerdp/cache/palette.h>
#include <freerdp/cache/pointer.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/rail.h>
#include <freerdp/version.h>
#include <guacamole/audio.h>
#include <guacamole/client.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/timestamp.h>
#include <winpr/wtypes.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/**
 * Called whenever a channel connects via the PubSub event system within
 * FreeRDP.
 *
 * @param context
 *     The rdpContext associated with the active RDP session.
 *
 * @param e
 *     Event-specific arguments, mainly the name of the channel, and a
 *     reference to the associated plugin loaded for that channel by FreeRDP.
 */
static void guac_rdp_channel_connected(rdpContext* context,
        ChannelConnectedEventArgs* e) {

    guac_client* client = ((rdp_freerdp_context*) context)->client;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_settings* settings = rdp_client->settings;

    if (settings->resize_method == GUAC_RESIZE_DISPLAY_UPDATE) {
        /* Store reference to the display update plugin once it's connected */
        if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {

            DispClientContext* disp = (DispClientContext*) e->pInterface;

            /* Init module with current display size */
            guac_rdp_disp_set_size(rdp_client->disp, rdp_client->settings,
                    context->instance, guac_rdp_get_width(context->instance),
                    guac_rdp_get_height(context->instance));

            /* Store connected channel */
            guac_rdp_disp_connect(rdp_client->disp, disp);
            guac_client_log(client, GUAC_LOG_DEBUG,
                    "Display update channel connected.");

        }
    }

}

BOOL rdp_freerdp_pre_connect(freerdp* instance) {

    rdpContext* context = instance->context;
    rdpChannels* channels = context->channels;
    rdpGraphics* graphics = context->graphics;

    guac_client* client = ((rdp_freerdp_context*) context)->client;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_settings* settings = rdp_client->settings;

    guac_rdp_dvc_list* dvc_list = guac_rdp_dvc_list_alloc();

    /* Init FreeRDP add-in provider */
    freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);

    /* Subscribe to and handle channel connected events */
    PubSub_SubscribeChannelConnected(context->pubSub,
            (pChannelConnectedEventHandler) guac_rdp_channel_connected);

    /* Load "disp" plugin for display update */
    if (settings->resize_method == GUAC_RESIZE_DISPLAY_UPDATE)
        guac_rdp_disp_load_plugin(instance->context, dvc_list);

    /* Load "AUDIO_INPUT" plugin for audio input*/
    if (settings->enable_audio_input) {
        rdp_client->audio_input = guac_rdp_audio_buffer_alloc();
        guac_rdp_audio_load_plugin(instance->context, dvc_list);
    }

    /* Load clipboard plugin */
    if (freerdp_channels_load_plugin(channels, instance->settings,
                "cliprdr", NULL))
        guac_client_log(client, GUAC_LOG_WARNING,
                "Failed to load cliprdr plugin. Clipboard will not work.");

    /* If RDPSND/RDPDR required, load them */
    if (settings->printing_enabled
        || settings->drive_enabled
        || settings->audio_enabled) {

        /* Load RDPDR plugin */
        if (freerdp_channels_load_plugin(channels, instance->settings,
                    "guacdr", client))
            guac_client_log(client, GUAC_LOG_WARNING,
                    "Failed to load guacdr plugin. Drive redirection and "
                    "printing will not work. Sound MAY not work.");

        /* Load RDPSND plugin */
        if (freerdp_channels_load_plugin(channels, instance->settings,
                    "guacsnd", client))
            guac_client_log(client, GUAC_LOG_WARNING,
                    "Failed to load guacsnd alongside guacdr plugin. Sound "
                    "will not work. Drive redirection and printing MAY not "
                    "work.");

    }

    /* Load RAIL plugin if RemoteApp in use */
    if (settings->remote_app != NULL) {

        /* Attempt to load rail */
        if (freerdp_channels_load_plugin(channels, instance->settings,
                    "rail", instance->settings))
            guac_client_log(client, GUAC_LOG_WARNING,
                    "Failed to load rail plugin. RemoteApp will not work.");

    }

#if 0
    /* Load SVC plugin instances for all static channels */
    if (settings->svc_names != NULL) {

        char** current = settings->svc_names;
        do {

            guac_rdp_svc* svc = guac_rdp_alloc_svc(client, *current);

            /* Attempt to load guacsvc plugin for new static channel */
            if (freerdp_channels_load_plugin(channels, instance->settings,
                        "guacsvc", svc)) {
                guac_client_log(client, GUAC_LOG_WARNING,
                        "Cannot create static channel \"%s\": failed to load guacsvc plugin.",
                        svc->name);
                guac_rdp_free_svc(svc);
            }

            /* Store and log on success */
            else {
                guac_rdp_add_svc(client, svc);
                guac_client_log(client, GUAC_LOG_INFO, "Created static channel \"%s\"...",
                        svc->name);
            }

        } while (*(++current) != NULL);

    }
#endif

    /* Load DRDYNVC plugin if required */
    if (guac_rdp_load_drdynvc(instance->context, dvc_list))
        guac_client_log(client, GUAC_LOG_WARNING,
                "Failed to load drdynvc plugin. Display update and audio "
                "input support will be disabled.");

    /* Dynamic virtual channel list is no longer needed */
    guac_rdp_dvc_list_free(dvc_list);

    /* Init FreeRDP cache */
    instance->context->cache = cache_new(instance->settings);

    /* Init FreeRDP internal GDI implementation */
    if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
        return FALSE;

    /* Set up bitmap handling */
    rdpBitmap bitmap = *graphics->Bitmap_Prototype;
    bitmap.size = sizeof(guac_rdp_bitmap);
    bitmap.New = guac_rdp_bitmap_new;
    bitmap.Free = guac_rdp_bitmap_free;
    bitmap.Paint = guac_rdp_bitmap_paint;
    bitmap.SetSurface = guac_rdp_bitmap_setsurface;
    graphics_register_bitmap(graphics, &bitmap);

    /* Set up glyph handling */
    rdpGlyph glyph = *graphics->Glyph_Prototype;
    glyph.size = sizeof(guac_rdp_glyph);
    glyph.New = guac_rdp_glyph_new;
    glyph.Free = guac_rdp_glyph_free;
    glyph.Draw = guac_rdp_glyph_draw;
    glyph.BeginDraw = guac_rdp_glyph_begindraw;
    glyph.EndDraw = guac_rdp_glyph_enddraw;
    graphics_register_glyph(graphics, &glyph);

    /* Set up pointer handling */
    rdpPointer pointer = *graphics->Pointer_Prototype;
    pointer.size = sizeof(guac_rdp_pointer);
    pointer.New = guac_rdp_pointer_new;
    pointer.Free = guac_rdp_pointer_free;
    pointer.Set = guac_rdp_pointer_set;
    pointer.SetNull = guac_rdp_pointer_set_null;
    pointer.SetDefault = guac_rdp_pointer_set_default;
    graphics_register_pointer(graphics, &pointer);

    /* Set up GDI */
    instance->update->DesktopResize = guac_rdp_gdi_desktop_resize;
    instance->update->EndPaint = guac_rdp_gdi_end_paint;
    instance->update->SetBounds = guac_rdp_gdi_set_bounds;

    rdpPrimaryUpdate* primary = instance->update->primary;
    primary->DstBlt = guac_rdp_gdi_dstblt;
    primary->PatBlt = guac_rdp_gdi_patblt;
    primary->ScrBlt = guac_rdp_gdi_scrblt;
    primary->MemBlt = guac_rdp_gdi_memblt;
    primary->OpaqueRect = guac_rdp_gdi_opaquerect;

    pointer_cache_register_callbacks(instance->update);
    glyph_cache_register_callbacks(instance->update);
    brush_cache_register_callbacks(instance->update);
    bitmap_cache_register_callbacks(instance->update);
    offscreen_cache_register_callbacks(instance->update);
    palette_cache_register_callbacks(instance->update);

    return TRUE;

}

/**
 * Callback invoked by FreeRDP when authentication is required but a username
 * and password has not already been given. In the case of Guacamole, this
 * function always succeeds but does not populate the usename or password. The
 * username/password must be given within the connection parameters.
 *
 * @param instance
 *     The FreeRDP instance associated with the RDP session requesting
 *     credentials.
 *
 * @param username
 *     Pointer to a string which will receive the user's username.
 *
 * @param password
 *     Pointer to a string which will receive the user's password.
 *
 * @param domain
 *     Pointer to a string which will receive the domain associated with the
 *     user's account.
 *
 * @return
 *     Always TRUE.
 */
static BOOL rdp_freerdp_authenticate(freerdp* instance, char** username,
        char** password, char** domain) {

    rdpContext* context = instance->context;
    guac_client* client = ((rdp_freerdp_context*) context)->client;

    /* Warn if connection is likely to fail due to lack of credentials */
    guac_client_log(client, GUAC_LOG_INFO,
            "Authentication requested but username or password not given");
    return TRUE;

}

/**
 * Callback invoked by FreeRDP when the SSL/TLS certificate of the RDP server
 * needs to be verified. If this ever happens, this function implementation
 * will always fail unless the connection has been configured to ignore
 * certificate validity.
 *
 * @param instance
 *     The FreeRDP instance associated with the RDP session whose SSL/TLS
 *     certificate needs to be verified.
 *
 * @param subject
 *     The subject to whom the certificate was issued.
 *
 * @param issuer
 *     The authority that issued the certificate,
 *
 * @param fingerprint
 *     The cryptographic fingerprint of the certificate.
 *
 * @return
 *     TRUE if the certificate passes verification, FALSE otherwise.
 */
static DWORD rdp_freerdp_verify_certificate(freerdp* instance,
        const char* common_name, const char* subject, const char* issuer,
        const char* fingerprint, BOOL host_mismatch) {

    rdpContext* context = instance->context;
    guac_client* client = ((rdp_freerdp_context*) context)->client;
    guac_rdp_client* rdp_client =
        (guac_rdp_client*) client->data;

    /* Bypass validation if ignore_certificate given */
    if (rdp_client->settings->ignore_certificate) {
        guac_client_log(client, GUAC_LOG_INFO, "Certificate validation bypassed");
        return 2; /* Accept only for this session */
    }

    guac_client_log(client, GUAC_LOG_INFO, "Certificate validation failed");
    return 0; /* Reject certificate */

}

/**
 * Waits for messages from the RDP server for the given number of milliseconds.
 *
 * @param client
 *     The client associated with the current RDP session.
 *
 * @param timeout_msecs
 *     The maximum amount of time to wait, in milliseconds.
 *
 * @return
 *     A positive value if messages are ready, zero if the specified timeout
 *     period elapsed, or a negative value if an error occurs.
 */
static int rdp_guac_client_wait_for_messages(guac_client* client,
        int timeout_msecs) {

    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    freerdp* rdp_inst = rdp_client->rdp_inst;
    rdpChannels* channels = rdp_inst->context->channels;

    int result;
    int index;

    /* List of all file descriptors which we may read data from */
    void* read_fds[GUAC_RDP_MAX_FILE_DESCRIPTORS];
    int read_count = 0;

    /* List of all file descriptors which data may be written to. These will
     * ultimately be ignored, but FreeRDP requires that both read and write
     * file descriptors be retrieved simultaneously. */
    void* write_fds[GUAC_RDP_MAX_FILE_DESCRIPTORS];
    int write_count = 0;

    struct pollfd fds[GUAC_RDP_MAX_FILE_DESCRIPTORS];

    /* Get RDP file descriptors */
    if (!freerdp_get_fds(rdp_inst, read_fds, &read_count,
                write_fds, &write_count)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR,
                "Unable to read RDP file descriptors.");
        return -1;
    }

    /* Get RDP channel file descriptors */
    if (!freerdp_channels_get_fds(channels, rdp_inst, read_fds, &read_count,
                write_fds, &write_count)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR,
                "Unable to read RDP channel file descriptors.");
        return -1;
    }

    /* If no file descriptors, error */
    if (read_count == 0) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR,
                "No file descriptors associated with RDP connection.");
        return -1;
    }

    /* Populate poll() array of read file descriptors */
    for (index = 0; index < read_count; index++) {

        struct pollfd* current = &fds[index];

        /* Init poll() array element with RDP file descriptor */
        current->fd      = (int)(long) (read_fds[index]);
        current->events  = POLLIN;
        current->revents = 0;

    }

    /* Wait until data can be read from RDP file descriptors */
    result = poll(fds, read_count, timeout_msecs);
    if (result < 0) {

        /* If error ignorable, pretend timout occurred */
        if (errno == EAGAIN
            || errno == EWOULDBLOCK
            || errno == EINPROGRESS
            || errno == EINTR)
            return 0;

        /* Otherwise, return as error */
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_UNAVAILABLE,
                "Error waiting for file descriptor.");
        return -1;

    }

    /* Return wait result */
    return result;

}

/**
 * Connects to an RDP server as described by the guac_rdp_settings structure
 * associated with the given client, allocating and freeing all objects
 * directly related to the RDP connection. It is expected that all objects
 * which are independent of FreeRDP's state (the clipboard, display update
 * management, etc.) will already be allocated and associated with the
 * guac_rdp_client associated with the given guac_client. This function blocks
 * for the duration of the RDP session, returning only after the session has
 * completely disconnected.
 *
 * @param client
 *     The guac_client associated with the RDP settings describing the
 *     connection that should be established.
 *
 * @return
 *     Zero if the connection successfully terminated and a reconnect is
 *     desired, non-zero if an error occurs or the connection was disconnected
 *     and a reconnect is NOT desired.
 */
static int guac_rdp_handle_connection(guac_client* client) {

    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_settings* settings = rdp_client->settings;

    /* Init random number generator */
    srandom(time(NULL));

    /* Set up screen recording, if requested */
    if (settings->recording_path != NULL) {
        rdp_client->recording = guac_common_recording_create(client,
                settings->recording_path,
                settings->recording_name,
                settings->create_recording_path,
                !settings->recording_exclude_output,
                !settings->recording_exclude_mouse,
                settings->recording_include_keys);
    }

    /* Create display */
    rdp_client->display = guac_common_display_alloc(client,
            rdp_client->settings->width,
            rdp_client->settings->height);

    rdp_client->current_surface = rdp_client->display->default_surface;

    rdp_client->requested_clipboard_format = CF_TEXT;
    rdp_client->available_svc = guac_common_list_alloc();

    /* Init client */
    freerdp* rdp_inst = freerdp_new();
    rdp_inst->PreConnect = rdp_freerdp_pre_connect;
    rdp_inst->Authenticate = rdp_freerdp_authenticate;
    rdp_inst->VerifyCertificate = rdp_freerdp_verify_certificate;
    rdp_inst->ReceiveChannelData = freerdp_channels_data;

    /* Allocate FreeRDP context */
    rdp_inst->ContextSize = sizeof(rdp_freerdp_context);

    freerdp_context_new(rdp_inst);
    ((rdp_freerdp_context*) rdp_inst->context)->client = client;

    /* Load keymap into client */
    rdp_client->keyboard = guac_rdp_keyboard_alloc(client,
            settings->server_layout);

    /* Set default pointer */
    guac_common_cursor_set_pointer(rdp_client->display->cursor);

    /* Push desired settings to FreeRDP */
    guac_rdp_push_settings(client, settings, rdp_inst);

    /* Connect to RDP server */
    if (!freerdp_connect(rdp_inst)) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_NOT_FOUND,
                "Error connecting to RDP server");
        return 1;
    }

    /* Connection complete */
    rdp_client->rdp_inst = rdp_inst;
    rdpChannels* channels = rdp_inst->context->channels;

    guac_timestamp last_frame_end = guac_timestamp_current();

    /* Signal that reconnect has been completed */
    guac_rdp_disp_reconnect_complete(rdp_client->disp);

    /* Handle messages from RDP server while client is running */
    while (client->state == GUAC_CLIENT_RUNNING
            && !guac_rdp_disp_reconnect_needed(rdp_client->disp)) {

        /* Update remote display size */
        pthread_mutex_lock(&(rdp_client->rdp_lock));
        guac_rdp_disp_update_size(rdp_client->disp, settings, rdp_inst);
        pthread_mutex_unlock(&(rdp_client->rdp_lock));

        /* Wait for data and construct a reasonable frame */
        int wait_result = rdp_guac_client_wait_for_messages(client,
                GUAC_RDP_FRAME_START_TIMEOUT);
        if (wait_result > 0) {

            int processing_lag = guac_client_get_processing_lag(client);
            guac_timestamp frame_start = guac_timestamp_current();

            /* Read server messages until frame is built */
            do {

                guac_timestamp frame_end;
                int frame_remaining;

                pthread_mutex_lock(&(rdp_client->rdp_lock));

                /* Check the libfreerdp fds */
                if (!freerdp_check_fds(rdp_inst)
                        || !freerdp_channels_check_fds(channels, rdp_inst)) {

                    /* Flag connection failure */
                    wait_result = -1;
                    pthread_mutex_unlock(&(rdp_client->rdp_lock));
                    break;

                }

                /* Check for channel events */
#if 0
                wMessage* event = freerdp_channels_pop_event(channels);
                if (event) {

                    /* Handle channel events (clipboard and RAIL) */
                    if (GetMessageClass(event->id) == CliprdrChannel_Class)
                        guac_rdp_process_cliprdr_event(client, event);
                    else if (GetMessageClass(event->id) == RailChannel_Class)
                        guac_rdp_process_rail_event(client, event);

                    freerdp_event_free(event);

                }
#endif

                pthread_mutex_unlock(&(rdp_client->rdp_lock));

                /* Calculate time remaining in frame */
                frame_end = guac_timestamp_current();
                frame_remaining = frame_start + GUAC_RDP_FRAME_DURATION
                                - frame_end;

                /* Calculate time that client needs to catch up */
                int time_elapsed = frame_end - last_frame_end;
                int required_wait = processing_lag - time_elapsed;

                /* Increase the duration of this frame if client is lagging */
                if (required_wait > GUAC_RDP_FRAME_TIMEOUT)
                    wait_result = rdp_guac_client_wait_for_messages(client,
                            required_wait);

                /* Wait again if frame remaining */
                else if (frame_remaining > 0)
                    wait_result = rdp_guac_client_wait_for_messages(client,
                            GUAC_RDP_FRAME_TIMEOUT);
                else
                    break;

            } while (wait_result > 0);

            /* Record end of frame, excluding server-side rendering time (we
             * assume server-side rendering time will be consistent between any
             * two subsequent frames, and that this time should thus be
             * excluded from the required wait period of the next frame). */
            last_frame_end = frame_start;

        }

        /* Test whether the RDP server is closing the connection */
        pthread_mutex_lock(&(rdp_client->rdp_lock));
        int connection_closing = freerdp_shall_disconnect(rdp_inst);
        pthread_mutex_unlock(&(rdp_client->rdp_lock));

        /* Close connection cleanly if server is disconnecting */
        if (connection_closing)
            guac_rdp_client_abort(client);

        /* If a low-level connection error occurred, fail */
        else if (wait_result < 0)
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_UNAVAILABLE,
                    "Connection closed.");

        /* Flush frame only if successful */
        else {
            guac_common_display_flush(rdp_client->display);
            guac_client_end_frame(client);
            guac_socket_flush(client->socket);
        }

    }

    /* Clean up print job, if active */
    if (rdp_client->active_job != NULL) {
        guac_rdp_print_job_kill(rdp_client->active_job);
        guac_rdp_print_job_free(rdp_client->active_job);
    }

    pthread_mutex_lock(&(rdp_client->rdp_lock));

    /* Disconnect client and channels */
    freerdp_disconnect(rdp_inst);

    /* Clean up RDP client context */
    cache_free(rdp_inst->context->cache);
    freerdp_context_free(rdp_inst);

    /* Clean up RDP client */
    freerdp_free(rdp_inst);
    rdp_client->rdp_inst = NULL;

    /* Free SVC list */
    guac_common_list_free(rdp_client->available_svc);

    /* Free RDP keyboard state */
    guac_rdp_keyboard_free(rdp_client->keyboard);

    /* Free display */
    guac_common_display_free(rdp_client->display);

    pthread_mutex_unlock(&(rdp_client->rdp_lock));

    /* Client is now disconnected */
    guac_client_log(client, GUAC_LOG_INFO, "Internal RDP client disconnected");

    return 0;

}

void* guac_rdp_client_thread(void* data) {

    guac_client* client = (guac_client*) data;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_settings* settings = rdp_client->settings;

    /* If audio enabled, choose an encoder */
    if (settings->audio_enabled) {

        rdp_client->audio = guac_audio_stream_alloc(client, NULL,
                GUAC_RDP_AUDIO_RATE,
                GUAC_RDP_AUDIO_CHANNELS,
                GUAC_RDP_AUDIO_BPS);

        /* Warn if no audio encoding is available */
        if (rdp_client->audio == NULL)
            guac_client_log(client, GUAC_LOG_INFO,
                    "No available audio encoding. Sound disabled.");

    } /* end if audio enabled */

    /* Load filesystem if drive enabled */
    if (settings->drive_enabled) {

        /* Allocate actual emulated filesystem */
        rdp_client->filesystem =
            guac_rdp_fs_alloc(client, settings->drive_path,
                    settings->create_drive_path);

        /* Expose filesystem to owner */
        guac_client_for_owner(client, guac_rdp_fs_expose,
                rdp_client->filesystem);

    }

#ifdef ENABLE_COMMON_SSH
    /* Connect via SSH if SFTP is enabled */
    if (settings->enable_sftp) {

        /* Abort if username is missing */
        if (settings->sftp_username == NULL) {
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR,
                    "A username or SFTP-specific username is required if "
                    "SFTP is enabled.");
            return NULL;
        }

        guac_client_log(client, GUAC_LOG_DEBUG,
                "Connecting via SSH for SFTP filesystem access.");

        rdp_client->sftp_user =
            guac_common_ssh_create_user(settings->sftp_username);

        /* Import private key, if given */
        if (settings->sftp_private_key != NULL) {

            guac_client_log(client, GUAC_LOG_DEBUG,
                    "Authenticating with private key.");

            /* Abort if private key cannot be read */
            if (guac_common_ssh_user_import_key(rdp_client->sftp_user,
                        settings->sftp_private_key,
                        settings->sftp_passphrase)) {
                guac_client_abort(client, GUAC_PROTOCOL_STATUS_SERVER_ERROR,
                        "Private key unreadable.");
                return NULL;
            }

        }

        /* Otherwise, use specified password */
        else {

            guac_client_log(client, GUAC_LOG_DEBUG,
                    "Authenticating with password.");

            guac_common_ssh_user_set_password(rdp_client->sftp_user,
                    settings->sftp_password);

        }

        /* Attempt SSH connection */
        rdp_client->sftp_session =
            guac_common_ssh_create_session(client, settings->sftp_hostname,
                    settings->sftp_port, rdp_client->sftp_user, settings->sftp_server_alive_interval,
                    settings->sftp_host_key, NULL);

        /* Fail if SSH connection does not succeed */
        if (rdp_client->sftp_session == NULL) {
            /* Already aborted within guac_common_ssh_create_session() */
            return NULL;
        }

        /* Load and expose filesystem */
        rdp_client->sftp_filesystem =
            guac_common_ssh_create_sftp_filesystem(rdp_client->sftp_session,
                    settings->sftp_root_directory, NULL);

        /* Expose filesystem to connection owner */
        guac_client_for_owner(client,
                guac_common_ssh_expose_sftp_filesystem,
                rdp_client->sftp_filesystem);

        /* Abort if SFTP connection fails */
        if (rdp_client->sftp_filesystem == NULL) {
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_UNAVAILABLE,
                    "SFTP connection failed.");
            return NULL;
        }

        guac_client_log(client, GUAC_LOG_DEBUG,
                "SFTP connection succeeded.");

    }
#endif

    /* Continue handling connections until error or client disconnect */
    while (client->state == GUAC_CLIENT_RUNNING) {
        if (guac_rdp_handle_connection(client))
            break;
    }

    return NULL;

}

