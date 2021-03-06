/**
 * Handling of rooms within matrix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

#include "matrix-room.h"

/* stdlib */
#include <string.h>

/* libpurple */
#include "connection.h"
#include "debug.h"

#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-event.h"
#include "matrix-json.h"
#include "matrix-roommembers.h"
#include "matrix-statetable.h"


static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv);

static MatrixConnectionData *_get_connection_data_from_conversation(
        PurpleConversation *conv)
{
    return conv->account->gc->proto_data;
}

/******************************************************************************
 *
 * conversation data
 */

/*
 * identifiers for purple_conversation_get/set_data
 */

/* a MatrixRoomStateEventTable * - see below */
#define PURPLE_CONV_DATA_STATE "state"

/* a GList of MatrixRoomEvent * */
#define PURPLE_CONV_DATA_EVENT_QUEUE "queue"

/* PurpleUtilFetchUrlData * */
#define PURPLE_CONV_DATA_ACTIVE_SEND "active_send"

/* MatrixRoomMemberTable * - see below */
#define PURPLE_CONV_MEMBER_TABLE "member_table"

/* PURPLE_CONV_FLAG_* */
#define PURPLE_CONV_FLAGS "flags"
#define PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE 0x1


/**
 * Get the member table for a room
 */
static MatrixRoomMemberTable *matrix_room_get_member_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_MEMBER_TABLE);
}


/**
 * Get the state table for a room
 */
static MatrixRoomStateEventTable *matrix_room_get_state_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_STATE);
}


static guint _get_flags(PurpleConversation *conv)
{
    return GPOINTER_TO_UINT(purple_conversation_get_data(conv,
            PURPLE_CONV_FLAGS));
}


static void _set_flags(PurpleConversation *conv, guint flags)
{
    purple_conversation_set_data(conv, PURPLE_CONV_FLAGS,
            GUINT_TO_POINTER(flags));
}


/******************************************************************************
 *
 * room state handling
 */


/**
 * Update the name of the room in the buddy list and the chat window
 *
 * @param conv: conversation info
 */
static void _update_room_alias(PurpleConversation *conv)
{
    gchar *room_name;
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    PurpleChat *chat;
    guint flags;

    room_name = _get_room_name(conn, conv);

    /* update the buddy list entry */
    chat = purple_blist_find_chat(conv->account, conv->name);
    /* we know there should be a buddy list entry for this room */
    g_assert(chat != NULL);
    purple_blist_alias_chat(chat, room_name);

    /* explicitly update the conversation title. This will tend to happen
     * anyway, but possibly not until the conversation tab is next activated.
     */
    if (strcmp(room_name, purple_conversation_get_title(conv)))
        purple_conversation_set_title(conv, room_name);

    g_free(room_name);

    flags = _get_flags(conv);
    flags &= ~PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE;
    _set_flags(conv, flags);
}


static void _schedule_name_update(PurpleConversation *conv)
{
    guint flags = _get_flags(conv);
    flags |= PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE;
    _set_flags(conv, flags);
    purple_debug_info("matrixprpl", "scheduled deferred room name update\n");
}

/**
 * Called when there is a change to the member list. Tells the MemberTable
 * about it.
 */
static void _on_member_change(PurpleConversation *conv,
        const gchar *member_user_id, MatrixRoomEvent *new_state)
{
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);

    matrix_roommembers_update_member(member_table, member_user_id,
            new_state->content);
}


/**
 * Called when there is a state update.
 *
 * old_state may be NULL to indicate addition of a state
 * key.
 */
static void _on_state_update(const gchar *event_type,
        const gchar *state_key, MatrixRoomEvent *old_state,
        MatrixRoomEvent *new_state, gpointer user_data)
{
    PurpleConversation *conv = user_data;
    g_assert(new_state != NULL);

    if(strcmp(event_type, "m.room.member") == 0) {
        _on_member_change(conv, state_key, new_state);
        /* we schedule a room name update here regardless of whether we end up
         * changing any members, because even changes to invited members can
         * affect the room name.
         */
        _schedule_name_update(conv);
    }
    else if(strcmp(event_type, "m.room.alias") == 0 ||
            strcmp(event_type, "m.room.canonical_alias") == 0 ||
            strcmp(event_type, "m.room.name") == 0) {
        _schedule_name_update(conv);
    }
}

void matrix_room_handle_state_event(struct _PurpleConversation *conv,
        JsonObject *json_event_obj)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    matrix_statetable_update(state_table, json_event_obj,
            _on_state_update, conv);
}


static gint _compare_member_user_id(const MatrixRoomMember *m,
        const gchar *user_id)
{
    return g_strcmp0(matrix_roommember_get_user_id(m), user_id);
}

/**
 * figure out the best name for a room based on its members list
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name_from_members(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    GList *tmp, *members;
    const gchar *member1;
    gchar *res;
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);
    members = matrix_roommembers_get_active_members(member_table, TRUE);

    /* remove ourselves from the list */
    tmp = g_list_find_custom(members, conn->user_id,
            (GCompareFunc)_compare_member_user_id);
    if(tmp != NULL) {
        members = g_list_delete_link(members, tmp);
    }

    if(members == NULL) {
        /* nobody else here! */
        return NULL;
    }

    member1 = matrix_roommember_get_displayname(members->data);

    if(members->next == NULL) {
        /* one other person */
        res = g_strdup(member1);
    } else if(members->next->next == NULL) {
        /* two other people */
        const gchar *member2 = matrix_roommember_get_displayname(
                members->next->data);
        res = g_strdup_printf(_("%s and %s"), member1, member2);
    } else {
        int nmembers = g_list_length(members);
        res = g_strdup_printf(_("%s and %i others"), member1, nmembers);
    }

    g_list_free(members);
    return res;
}


/**
 * figure out the best name for a room
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    gchar *res;

    /* first try to pick a name based on the official name / alias */
    res = matrix_statetable_get_room_alias(state_table);
    if (res)
        return res;

    /* look for room members, and pick a name based on that */
    res = _get_room_name_from_members(conn, conv);
    if (res)
        return res;

    /* failing all else, just use the room id */
    return g_strdup(conv -> name);

}

/******************************************************************************
 *
 * event queue handling
 */
static void _send_queued_event(PurpleConversation *conv);

/**
 * Get the state table for a room
 */
static GList *_get_event_queue(PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE);
}

static void _event_send_complete(MatrixConnectionData *account, gpointer user_data,
      JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    JsonObject *response_object;
    const gchar *event_id;
    GList *event_queue;
    MatrixRoomEvent *event;

    response_object = matrix_json_node_get_object(json_root);
    event_id = matrix_json_object_get_string_member(response_object,
            "event_id");
    purple_debug_info("matrixprpl", "Successfully sent event id %s\n",
            event_id);

    event_queue = _get_event_queue(conv);
    event = event_queue -> data;
    matrix_event_free(event);
    event_queue = g_list_remove(event_queue, event);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    _send_queued_event(conv);
}


/**
 * Unable to send event to homeserver
 */
void _event_send_error(MatrixConnectionData *ma, gpointer user_data,
        const gchar *error_message)
{
    PurpleConversation *conv = user_data;
    matrix_api_error(ma, user_data, error_message);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**
 * homeserver gave non-200 on event send.
 */
void _event_send_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    matrix_api_bad_response(ma, user_data, http_response_code, json_root);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**************************** Image handling *********************************/
/* Data structure passed around during the image event handling */
struct SendImageData {
    PurpleConversation *conv;
    MatrixRoomEvent *event;
    int imgstore_id;
};

/**
 * Called back by matrix_api_upload_file after the image is uploaded.
 * We get a 'content_uri' identifying the uploaded file, and that's what
 * we put in the event.
 */
static void _image_upload_complete(MatrixConnectionData *ma,
      gpointer user_data, JsonNode *json_root)
{
    MatrixApiRequestData *fetch_data = NULL;
    struct SendImageData *sid = user_data;
    JsonObject *response_object = matrix_json_node_get_object(json_root);
    const gchar *content_uri;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sid->imgstore_id);

    content_uri = matrix_json_object_get_string_member(response_object,
            "content_uri");
    if (content_uri == NULL) {
        matrix_api_error(ma, sid->conv,
                "image_upload_complete: no content_uri");
        purple_imgstore_unref(image);
        return;
    }

    json_object_set_string_member(sid->event->content, "url", content_uri);

    fetch_data = matrix_api_send(ma, sid->conv->name, sid->event->event_type,
             sid->event->txn_id, sid->event->content, _event_send_complete,
             _event_send_error, _event_send_bad_response, sid->conv);
    purple_conversation_set_data(sid->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
                    fetch_data);
    purple_imgstore_unref(image);
}

static void _image_upload_bad_response(MatrixConnectionData *ma, gpointer user_data,
            int http_response_code, JsonNode *json_root)
{
    struct SendImageData *sid = user_data;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sid->imgstore_id);

    matrix_api_bad_response(ma, sid->conv, http_response_code, json_root);
    purple_imgstore_unref(image);
    purple_conversation_set_data(sid->conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    /* More clear up with the message? */
}

void _image_upload_error(MatrixConnectionData *ma, gpointer user_data,
            const gchar *error_message)
{
    struct SendImageData *sid = user_data;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sid->imgstore_id);

    matrix_api_error(ma, sid->conv, error_message);
    purple_imgstore_unref(image);
    purple_conversation_set_data(sid->conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    /* More clear up with the message? */
}

/**
 * Return a mimetype based on some info; this should get replaced
 * with a glib/gio/gcontent_type_guess call if we can include it,
 * all other plugins do this manually.
 */
static const char *type_guess(PurpleStoredImage *image)
{
    /* Copied off the code in libpurple's jabber module */
    const char *ext = purple_imgstore_get_extension(image);

    if (strcmp(ext, "png") == 0) {
      return "image/png";
    } else if (strcmp(ext, "gif") == 0) {
      return "image/gif";
    } else if (strcmp(ext, "jpg") == 0) {
      return "image/jpeg";
    } else if (strcmp(ext, "tif") == 0) {
      return "image/tif";
    } else {
      return "image/x-icon"; /* or something... */
    }
}

/**
 * Called back by _send_queued_event for an image.
 */
static void _send_image_hook(void *opaque, MatrixRoomEvent *event)
{
    MatrixApiRequestData *fetch_data;
    PurpleConversation *conv = opaque;
    PurpleConnection *pc = conv->account->gc;
    MatrixConnectionData *acct = purple_connection_get_protocol_data(pc);
    struct SendImageData *sid = event->hook_data;
    int imgstore_id = sid->imgstore_id;
    PurpleStoredImage *image = purple_imgstore_find_by_id(imgstore_id);
    size_t imgsize;
    const char *filename;
    const char *ctype;
    gconstpointer imgdata;

    if (!image)
        return;

    imgsize = purple_imgstore_get_size(image);
    filename = purple_imgstore_get_filename(image);
    imgdata = purple_imgstore_get_data(image);
    ctype = type_guess(image);

    purple_debug_info("matrixprpl", "%s: image id %d for %s (type: %s)\n",
            __func__,
            sid->imgstore_id, filename, ctype);

    sid->event = event;
    json_object_set_string_member(event->content, "body", filename);

    fetch_data = matrix_api_upload_file(acct, ctype, imgdata, imgsize,
                           _image_upload_complete,
                           _image_upload_error,
                           _image_upload_bad_response,sid);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            fetch_data);
}


/**
 * send the next queued event, provided the connection isn't shutting down.
 *
 * Updates PURPLE_CONV_DATA_ACTIVE_SEND either way.
 */
static void _send_queued_event(PurpleConversation *conv)
{
    MatrixApiRequestData *fetch_data = NULL;
    MatrixConnectionData *acct;
    MatrixRoomEvent *event;
    PurpleConnection *pc = conv->account->gc;
    GList *queue;

    acct = purple_connection_get_protocol_data(pc);
    queue = _get_event_queue(conv);

    if(queue == NULL) {
        /* nothing else to send */
    } else if(pc -> wants_to_die) {
        /* don't make any more requests if the connection is closing */
        purple_debug_info("matrixprpl", "Not sending new events on dying"
                " connection");
    } else {
        event = queue -> data;
        g_assert(event != NULL);
        if (event->hook)
            return event->hook(conv, event);

        purple_debug_info("matrixprpl", "Sending %s with txn id %s\n",
                event->event_type, event->txn_id);

        fetch_data = matrix_api_send(acct, conv->name, event->event_type,
                event->txn_id, event->content, _event_send_complete,
                _event_send_error, _event_send_bad_response, conv);
    }

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            fetch_data);
}


static void _enqueue_event(PurpleConversation *conv, const gchar *event_type,
        JsonObject *event_content,
        EventSendHook hook, void *hook_data)
{
    MatrixRoomEvent *event;
    GList *event_queue;
    MatrixApiRequestData *active_send;

    event = matrix_event_new(event_type, event_content);
    event->txn_id = g_strdup_printf("%"G_GINT64_FORMAT"%"G_GUINT32_FORMAT,
            g_get_monotonic_time(), g_random_int());
    event->hook = hook;
    event->hook_data = hook_data;

    event_queue = _get_event_queue(conv);
    event_queue = g_list_append(event_queue, event);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    purple_debug_info("matrixprpl", "Enqueued %s with txn id %s\n",
            event_type, event->txn_id);

    active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);
    if(active_send != NULL) {
        purple_debug_info("matrixprpl", "Event send is already in progress\n");
    } else {
        _send_queued_event(conv);
    }
}


/**
 * If there is an event send in progress, cancel it
 */
static void _cancel_event_send(PurpleConversation *conv)
{
    MatrixApiRequestData *active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);

    if(active_send == NULL)
        return;

    purple_debug_info("matrixprpl", "Cancelling event send");
    matrix_api_cancel(active_send);

    g_assert(purple_conversation_get_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND)
            == NULL);
}

/*****************************************************************************/

void matrix_room_handle_timeline_event(PurpleConversation *conv,
       JsonObject *json_event_obj)
{
    const gchar *event_type, *sender_id, *transaction_id;
    gint64 timestamp;
    JsonObject *json_content_obj;
    JsonObject *json_unsigned_obj;
    const gchar *room_id, *msg_body, *msg_type;
    gchar *tmp_body = NULL;
    PurpleMessageFlags flags;

    const gchar *sender_display_name;
    MatrixRoomMember *sender = NULL;

    room_id = conv->name;

    event_type = matrix_json_object_get_string_member(
            json_event_obj, "type");
    sender_id = matrix_json_object_get_string_member(json_event_obj, "sender");
    timestamp = matrix_json_object_get_int_member(json_event_obj,
                "origin_server_ts");
    json_content_obj = matrix_json_object_get_object_member(
            json_event_obj, "content");

    if(event_type == NULL) {
        purple_debug_warning("matrixprpl", "event missing type field");
        return;
    }

    if(strcmp(event_type, "m.room.message") != 0) {
        purple_debug_info("matrixprpl", "ignoring unknown room event %s\n",
                        event_type);
        return;
    }

    msg_body = matrix_json_object_get_string_member(json_content_obj, "body");
    if(msg_body == NULL) {
        purple_debug_warning("matrixprpl", "no body in message event\n");
        return;
    }

    msg_type = matrix_json_object_get_string_member(json_content_obj, "msgtype");
    if(msg_type == NULL) {
        purple_debug_warning("matrixprpl", "no msgtype in message event\n");
        return;
    }

    json_unsigned_obj = matrix_json_object_get_object_member(json_event_obj,
            "unsigned");
    transaction_id = matrix_json_object_get_string_member(json_unsigned_obj,
            "transaction_id");

    /* if it has a transaction id, it's an echo of a message we sent.
     * We shouldn't really just ignore it, but I'm not sure how to update a sent
     * message.
     */
    if(transaction_id != NULL) {
        purple_debug_info("matrixprpl", "got remote echo %s in %s\n", msg_body,
                room_id);
        return;
    }

    if(sender_id != NULL) {
        MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
        sender = matrix_roommembers_lookup_member(table, sender_id);
    }
    if (sender != NULL) {
        sender_display_name = matrix_roommember_get_displayname(sender);
    } else {
        sender_display_name = "<unknown>";
    }

    if (!strcmp(msg_type, "m.emote")) {
        tmp_body = g_strdup_printf("/me %s", msg_body);
    }
    flags = PURPLE_MESSAGE_RECV;

    purple_debug_info("matrixprpl", "got message from %s in %s\n", sender_id,
            room_id);
    serv_got_chat_in(conv->account->gc, g_str_hash(room_id),
            sender_display_name, flags, tmp_body ? tmp_body : msg_body,
            timestamp / 1000);
    g_free(tmp_body);
}


PurpleConversation *matrix_room_create_conversation(
        PurpleConnection *pc, const gchar *room_id)
{
    PurpleConversation *conv;
    MatrixRoomStateEventTable *state_table;
    MatrixRoomMemberTable *member_table;

    purple_debug_info("matrixprpl", "New room %s\n", room_id);

    /* tell purple we have joined this chat */
    conv = serv_got_joined_chat(pc, g_str_hash(room_id), room_id);

    /* set our data on it */
    state_table = matrix_statetable_new();
    member_table = matrix_roommembers_new_table();
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE,
            member_table);

    return conv;
}


/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(PurpleConversation *conv)
{
    MatrixConnectionData *conn;
    MatrixRoomStateEventTable *state_table;
    GList *event_queue;
    MatrixRoomMemberTable *member_table;

    conn = _get_connection_data_from_conversation(conv);

    _cancel_event_send(conv);
    matrix_api_leave_room(conn, conv->name, NULL, NULL, NULL, NULL);

    /* At this point, we have no confirmation that the 'leave' request will
     * be successful (nor that it has even started), so it's questionable
     * whether we can/should actually free all of the room state.
     *
     * On the other hand, we don't have any mechanism for telling purple that
     * we haven't really left the room, and if the leave request does fail,
     * we'll set the error flag on the connection, which will eventually
     * result in pidgin flagging the connection as failed; things will
     * hopefully then get resynced when the user reconnects.
     */

    state_table = matrix_room_get_state_table(conv);
    matrix_statetable_destroy(state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, NULL);

    member_table = matrix_room_get_member_table(conv);
    matrix_roommembers_free_table(member_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE, NULL);

    event_queue = _get_event_queue(conv);
    if(event_queue != NULL) {
        g_list_free_full(event_queue, (GDestroyNotify)matrix_event_free);
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    }
}


/* *****************************************************************************
 *
 * Tracking of member additions/removals.
 *
 * We don't tell libpurple about new arrivals immediately, because that is
 * inefficient and takes ages on a big room like Matrix HQ. Instead, the
 * MatrixRoomMemberTable builds up a list of changes, and we then go through
 * those changes after processing all of the state changes in a /sync.
 *
 * This introduces a complexity in that we need to track what we've told purple
 * the displayname of the user is (for instance, member1 leaves a channel,
 * meaning that there is no longer a clash of displaynames, so member2
 * can be renamed: we need to know what we previously told libpurple member2 was
 * called). We do this by setting the member's opaque data to the name we gave
 * to libpurple.
 */


static void _on_member_deleted(MatrixRoomMember *member)
{
    gchar *displayname = matrix_roommember_get_opaque_data(member);
    g_free(displayname);
    matrix_roommember_set_opaque_data(member, NULL, NULL);
}


/**
 * Tell libpurple about newly-arrived members
 */
static void _handle_new_members(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GList *names = NULL, *flags = NULL;
    GSList *members;

    members = matrix_roommembers_get_new_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        const gchar *displayname;
        GSList *tmp;

        displayname = matrix_roommember_get_opaque_data(member);
        g_assert(displayname == NULL);

        displayname = matrix_roommember_get_displayname(member);
        matrix_roommember_set_opaque_data(member, g_strdup(displayname),
                _on_member_deleted);

        names = g_list_prepend(names, (gpointer)displayname);
        flags = g_list_prepend(flags, GINT_TO_POINTER(0));

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }

    if(names) {
        purple_conv_chat_add_users(chat, names, NULL, flags, announce_arrivals);
        g_list_free(names);
        g_list_free(flags);
    }
}


/**
 * Tell libpurple about renamed members
 */
void _handle_renamed_members(PurpleConversation *conv)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GSList *members;

    members = matrix_roommembers_get_renamed_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        gchar *current_displayname;
        const gchar *new_displayname;
        GSList *tmp;

        current_displayname = matrix_roommember_get_opaque_data(member);
        g_assert(current_displayname != NULL);

        new_displayname = matrix_roommember_get_displayname(member);

        purple_conv_chat_rename_user(chat, current_displayname,
                new_displayname);

        matrix_roommember_set_opaque_data(member, g_strdup(new_displayname),
                _on_member_deleted);
        g_free(current_displayname);

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }
}


/**
 * Tell libpurple about departed members
 */
void _handle_left_members(PurpleConversation *conv)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GSList *members;

    members = matrix_roommembers_get_left_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        gchar *current_displayname;
        GSList *tmp;

        current_displayname = matrix_roommember_get_opaque_data(member);
        g_assert(current_displayname != NULL);
        purple_conv_chat_remove_user(chat, current_displayname, NULL);

        g_free(current_displayname);
        matrix_roommember_set_opaque_data(member, NULL, NULL);

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }
}


static void _update_user_list(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    _handle_new_members(conv, announce_arrivals);
    _handle_renamed_members(conv);
    _handle_left_members(conv);
}



/**
 * Get the userid of a member of a room, given their displayname
 *
 * @returns a string, which will be freed by the caller, or null if not known
 */
gchar *matrix_room_displayname_to_userid(struct _PurpleConversation *conv,
        const gchar *who)
{
    /* TODO: make this more efficient */
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GList *members, *ptr;
    gchar *result = NULL;

    members = matrix_roommembers_get_active_members(table, TRUE);

    for(ptr = members; ptr != NULL; ptr = ptr->next) {
        MatrixRoomMember *member = ptr->data;
        const gchar *displayname = matrix_roommember_get_opaque_data(member);
        if(g_strcmp0(displayname, who) == 0) {
            result = g_strdup(matrix_roommember_get_user_id(member));
            break;
        }
    }

    g_list_free(members);
    return result;
}

/* ************************************************************************** */

void matrix_room_complete_state_update(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    _update_user_list(conv, announce_arrivals);
    if(_get_flags(conv) & PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE)
        _update_room_alias(conv);
}


static const gchar *_get_my_display_name(PurpleConversation *conv)
{
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    MatrixRoomMemberTable *member_table =
            matrix_room_get_member_table(conv);
    MatrixRoomMember *me;

    me = matrix_roommembers_lookup_member(member_table, conn->user_id);
    if(me == NULL)
        return NULL;
    else
        return matrix_roommember_get_displayname(me);
}

/**
 * Send an image message in a room
 */
void matrix_room_send_image(PurpleConversation *conv, int imgstore_id,
        const gchar *message)
{
    JsonObject *content;
    struct SendImageData *sid;

    if (!imgstore_id)
        return;
    /* This is the hook_data on the event, it gets free'd by the event
     * code when the event is free'd
     */
    sid = g_new0(struct SendImageData, 1);

    /* We can't send this event until we've uploaded the image because
     * the event contents including the file ID that we get back from
     * the upload process.
     * Our hook gets called back when we're ready to send the event,
     * then we do the upload.
     */
    content = json_object_new();
    json_object_set_string_member(content, "msgtype", "m.image");

    sid->imgstore_id = imgstore_id;
    sid->conv = conv;
    purple_debug_info("matrixprpl", "%s: image id=%d\n", __func__, imgstore_id);
    _enqueue_event(conv, "m.room.message", content, _send_image_hook, sid);
    json_object_unref(content);
    purple_conversation_write(conv, _get_my_display_name(conv),
            message, PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_IMAGES,
            g_get_real_time()/1000/1000);
}

/**
 * Send a message in a room
 */
void matrix_room_send_message(PurpleConversation *conv, const gchar *message)
{
    JsonObject *content;
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    const char *type_string = "m.text";
    const gchar *message_to_send = message;
    const char *image_start, *image_end;
    GData *image_attribs;

    /* Matrix doesn't have messages that have both images and text in, so
     * we have to split this message if it has an image.
     */
    if (purple_markup_find_tag("img", message,
                               &image_start,
                               &image_end,
                               &image_attribs)) {
        int imgstore_id = atoi(g_datalist_get_data(&image_attribs, "id"));
        gchar *image_message;
        purple_imgstore_ref_by_id(imgstore_id);

        if (image_start != message) {
            gchar *prefix = g_strndup(message, image_start - message);
            matrix_room_send_message(conv, prefix);
            g_free(prefix);
        }

        image_message = g_strndup(image_start, 1+(image_end-image_start));
        matrix_room_send_image(conv, imgstore_id, image_message);
        g_datalist_clear(&image_attribs);
        g_free(image_message);

        /* Anything after the image? */
        if (image_end[1]) {
            matrix_room_send_message(conv, image_end + 1);
        }
        return;
    }

    if (!strncmp(message, "/me ", 4)) {
        type_string = "m.emote";
        message_to_send = message + 4;
    }

    content = json_object_new();
    json_object_set_string_member(content, "msgtype", type_string);
    json_object_set_string_member(content, "body", message_to_send);

    _enqueue_event(conv, "m.room.message", content, NULL, NULL);
    json_object_unref(content);

    purple_conv_chat_write(chat, _get_my_display_name(conv),
            message, PURPLE_MESSAGE_SEND, g_get_real_time()/1000/1000);
}
