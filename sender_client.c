#include <libwebsockets.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static GstElement *webrtc = NULL;

// We store partial incoming messages here
struct client_session_data {
    char message[2048];
    size_t len;
};

// Forward declarations
static void on_offer_created(GstPromise *promise, gpointer wsi);

/* ICE candidate from the local (sender) side */
static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex,
                             gchar *candidate, gpointer user_data)
{
    struct lws *wsi = (struct lws *)user_data;
    lwsl_user("Sender: Got local ICE candidate:\n%s\n", candidate);

    gchar *msg = g_strdup_printf("candidate:%s", candidate);
    unsigned char *buf = malloc(LWS_PRE + strlen(msg) + 1);
    memset(buf, 0, LWS_PRE + strlen(msg) + 1);
    memcpy(&buf[LWS_PRE], msg, strlen(msg));

    if (lws_write(wsi, &buf[LWS_PRE], strlen(msg), LWS_WRITE_TEXT) < 0) {
        lwsl_err("Sender: Failed to send ICE candidate\n");
    }

    free(buf);
    g_free(msg);
}

/* Add a remote ICE candidate on this side (sender) */
static void handle_remote_candidate(const char *candidate_sdp)
{
    lwsl_user("Sender: Adding remote ICE candidate:\n%s\n", candidate_sdp);
    g_signal_emit_by_name(webrtc, "add-ice-candidate", 0, candidate_sdp);
}

/* create SDP Offer */
static void on_negotiation_needed(GstElement *webrtcbin, gpointer wsi)
{
    lwsl_user("Sender: on_negotiation_needed\n");
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, wsi, NULL);
    g_signal_emit_by_name(webrtcbin, "create-offer", NULL, promise);
}

/* Called after create-offer finishes */
static void on_offer_created(GstPromise *promise, gpointer user_data)
{
    struct lws *wsi = (struct lws *)user_data;
    gst_promise_wait(promise);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    if (!offer) {
        lwsl_err("Sender: Failed to create Offer\n");
        gst_promise_unref(promise);
        return;
    }

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    lwsl_user("Sender: Created SDP Offer:\n%s\n", sdp_text);

    // Set local desc
    g_signal_emit_by_name(webrtc, "set-local-description", offer, NULL);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    // Send Offer to server
    unsigned char *buf = malloc(LWS_PRE + strlen(sdp_text) + 1);
    memset(buf, 0, LWS_PRE + strlen(sdp_text) + 1);
    memcpy(&buf[LWS_PRE], sdp_text, strlen(sdp_text));

    if (lws_write(wsi, &buf[LWS_PRE], strlen(sdp_text), LWS_WRITE_TEXT) < 0) {
        lwsl_err("Sender: Failed to send SDP Offer\n");
    } else {
        lwsl_user("Sender: Sent SDP Offer to server\n");
    }

    free(buf);
    g_free(sdp_text);
}

/* LWS callback for the sender */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    struct client_session_data *csd = (struct client_session_data *)user;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        lwsl_user("Sender: WebSocket connection established\n");
        // We'll let "on-negotiation-needed" be called automatically by webrtcbin
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if (csd->len + len >= sizeof(csd->message)) {
            lwsl_err("Sender: Received too-long msg\n");
            return -1;
        }
        memcpy(csd->message + csd->len, in, len);
        csd->len += len;
        csd->message[csd->len] = '\0';

        if (lws_is_final_fragment(wsi)) {
            lwsl_user("Sender: Complete message:\n%s\n", csd->message);

            if (!strncmp(csd->message, "candidate:", 10)) {
                // We got an ICE candidate from the server (originating from the receiver)
                const char *cand = csd->message + 10;
                handle_remote_candidate(cand);
            }
            else if (!strncmp(csd->message, "SERVER_ANSWER:", 14)) {
                // the Answer
                const char *answer_sdp = csd->message + 14;
                lwsl_user("Sender: Got SDP Answer:\n%s\n", answer_sdp);

                GstSDPMessage *sdp = NULL;
                if (gst_sdp_message_new_from_text(answer_sdp, &sdp) != GST_SDP_OK) {
                    lwsl_err("Sender: Failed to parse SDP Answer\n");
                } else {
                    GstWebRTCSessionDescription *answer =
                        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
                    g_signal_emit_by_name(webrtc, "set-remote-description", answer, NULL);
                    gst_webrtc_session_description_free(answer);
                }
            }
            else if (!strncmp(csd->message, "SERVER_OFFER:", 13)) {
                // it's the sender, so we ignore a re-sent Offer
                lwsl_user("Sender: Got SERVER_OFFER from server, ignoring (we are the sender)\n");
            }
            else {
                lwsl_user("Sender: Unknown message:\n%s\n", csd->message);
            }

            csd->len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        lwsl_err("Sender: Connection error\n");
        break;

    case LWS_CALLBACK_CLOSED:
        lwsl_user("Sender: WebSocket closed\n");
        break;

    default:
        break;
    }
    return 0;
}

int main()
{
    gst_init(NULL, NULL);
    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);

    lwsl_user("Sender: Starting up...\n");

    // LWS context
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN; // we're a client

    static struct lws_protocols protocols[] = {
        {
            "signaling-protocol",
            websocket_callback,
            sizeof(struct client_session_data),
            2048
        },
        {NULL, NULL, 0, 0}
    };
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        lwsl_err("Sender: Failed to create LWS context\n");
        return 1;
    }

    // Connect
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context;
    ccinfo.address = "localhost";
    ccinfo.port = 8080;
    ccinfo.path = "/";
    ccinfo.protocol = "signaling-protocol";

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        lwsl_err("Sender: Failed to connect to server\n");
        lws_context_destroy(context);
        return 1;
    }

    // GStreamer pipeline for a test video → webrtcbin
    GstElement *pipeline = gst_parse_launch(
  "videotestsrc ! video/x-raw,width=640,height=480 ! videoconvert ! queue ! "
  "vp8enc ! rtpvp8pay ! "
  "webrtcbin name=webrtcbin "
  // no stun-server
  ,
  NULL
);
    if (!pipeline) {
        lwsl_err("Sender: Failed to create GStreamer pipeline\n");
        return 1;
    }

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    if (!webrtc) {
        lwsl_err("Sender: Failed to find webrtcbin\n");
        return 1;
    }

    // Connect signals
    g_signal_connect(webrtc, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), wsi);

    g_signal_connect(webrtc, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), wsi);

    // Start pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Main loop
    while (1) {
        lws_service(context, 1000);
    }

    // Cleanup
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    lws_context_destroy(context);
    return 0;
}
