#include <libwebsockets.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static GstElement *webrtc = NULL;

struct per_session_data {
    char message[4096];
    size_t len;
};

// Forward declaration
static void on_answer_created(GstPromise *promise, gpointer user_data);

/* Called when GStreamer has a local ICE candidate to send */
static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex,
                             gchar *candidate, gpointer user_data)
{
    struct lws *wsi = (struct lws *)user_data;
    lwsl_user("[Receiver] Local ICE candidate:\n%s\n", candidate);

    gchar *msg = g_strdup_printf("candidate:%s", candidate);
    unsigned char *buf = malloc(LWS_PRE + strlen(msg) + 1);
    memset(buf, 0, LWS_PRE + strlen(msg) + 1);
    memcpy(&buf[LWS_PRE], msg, strlen(msg));

    if (lws_write(wsi, &buf[LWS_PRE], strlen(msg), LWS_WRITE_TEXT) < 0) {
        lwsl_err("[Receiver] Failed to send ICE candidate\n");
    }

    free(buf);
    g_free(msg);
}

/* Add a remote ICE candidate on the receiver side */
static void handle_remote_candidate(const char *candidate_sdp)
{
    lwsl_user("[Receiver] Adding remote ICE candidate:\n%s\n", candidate_sdp);
    // Typically parse out mline index, but let's assume 0
    g_signal_emit_by_name(webrtc, "add-ice-candidate", 0, candidate_sdp);
}

/* Called after we create an Answer in GStreamer */
static void on_answer_created(GstPromise *promise, gpointer user_data)
{
    struct lws *wsi = (struct lws *)user_data;
    gst_promise_wait(promise);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = NULL;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    if (!answer) {
        lwsl_err("[Receiver] Failed to create Answer\n");
        gst_promise_unref(promise);
        return;
    }

    // Set local desc
    g_signal_emit_by_name(webrtc, "set-local-description", answer, NULL);

    gchar *sdp_text = gst_sdp_message_as_text(answer->sdp);
    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);

    if (!sdp_text) {
        lwsl_err("[Receiver] Could not convert Answer to text\n");
        return;
    }

    lwsl_user("[Receiver] Created SDP Answer:\n%s\n", sdp_text);

    // Send "answer:..." back to server
    unsigned char *buf = malloc(LWS_PRE + strlen(sdp_text) + 8);
    memset(buf, 0, LWS_PRE + strlen(sdp_text) + 8);
    snprintf((char*)&buf[LWS_PRE], strlen(sdp_text) + 8, "answer:%s", sdp_text);

    if (lws_write(wsi, &buf[LWS_PRE], strlen((char*)&buf[LWS_PRE]), LWS_WRITE_TEXT) < 0) {
        lwsl_err("[Receiver] Failed to send SDP Answer\n");
    } else {
        lwsl_user("[Receiver] Sent SDP Answer to server\n");
    }

    free(buf);
    g_free(sdp_text);
}

/* LWS callback for the receiver */
static int
callback_signaling_client(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len)
{
    struct per_session_data *psd = (struct per_session_data *)user;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        lwsl_user("[Receiver] Connected to server\n");
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if (psd->len + len >= sizeof(psd->message)) {
            lwsl_err("[Receiver] Message too long\n");
            return -1;
        }
        memcpy(psd->message + psd->len, in, len);
        psd->len += len;
        psd->message[psd->len] = '\0';

        if (lws_is_final_fragment(wsi)) {
            lwsl_user("[Receiver] Complete msg from server:\n%s\n", psd->message);

            if (!strncmp(psd->message, "SERVER_OFFER:", 13)) {
                // it's an Offer
                const char *offer_text = psd->message + 13;
                lwsl_user("[Receiver] Got SDP Offer from server:\n%s\n", offer_text);

                GstSDPMessage *sdp = NULL;
                if (gst_sdp_message_new_from_text(offer_text, &sdp) != GST_SDP_OK) {
                    lwsl_err("[Receiver] Failed to parse Offer\n");
                } else {
                    GstWebRTCSessionDescription *offer =
                        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
                    // Set remote desc
                    g_signal_emit_by_name(webrtc, "set-remote-description", offer, NULL);
                    gst_webrtc_session_description_free(offer);

                    // create an Answer
                    GstPromise *promise =
                        gst_promise_new_with_change_func(on_answer_created, wsi, NULL);
                    g_signal_emit_by_name(webrtc, "create-answer", NULL, promise);
                }
            }
            else if (!strncmp(psd->message, "SERVER_ANSWER:", 14)) {
                // We're the receiver, typically we ignore the "SERVER_ANSWER"
                lwsl_user("[Receiver] Got SERVER_ANSWER from server, ignoring\n");
            }
            else if (!strncmp(psd->message, "candidate:", 10)) {
                // ICE candidate from the other side
                const char *cand = psd->message + 10;
                handle_remote_candidate(cand);
            }
            else {
                lwsl_user("[Receiver] Unknown server msg:\n%s\n", psd->message);
            }

            psd->len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        lwsl_err("[Receiver] Connection error\n");
        break;

    case LWS_CALLBACK_CLOSED:
        lwsl_user("[Receiver] WebSocket closed\n");
        break;

    default:
        break;
    }
    return 0;
}

int main()
{
    // Initialize GStreamer
    gst_init(NULL, NULL);

    // Logging
    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
    lwsl_user("[Receiver] Starting up\n");

    // LWS
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN; // client

    static struct lws_protocols protocols[] = {
        {
            "signaling-protocol",
            callback_signaling_client,
            sizeof(struct per_session_data),
            4096
        },
        {NULL, NULL, 0, 0}
    };
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        lwsl_err("[Receiver] Failed to create LWS context\n");
        return 1;
    }

    // Connect to the signaling server
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context;
    ccinfo.address = "localhost";  // same machine
    ccinfo.port = 8080;
    ccinfo.path = "/";
    ccinfo.protocol = "signaling-protocol";

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        lwsl_err("[Receiver] Failed to connect\n");
        lws_context_destroy(context);
        return 1;
    }

    // GStreamer pipeline: webrtcbin -> autovideosink
    GstElement *pipeline = gst_parse_launch(
    "webrtcbin name=webrtcbin "
    // no stun-server
    " ! videoconvert ! autovideosink",
    NULL
    );
    if (!pipeline) {
        lwsl_err("[Receiver] Failed to create pipeline\n");
        return 1;
    }

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    if (!webrtc) {
        lwsl_err("[Receiver] Failed to find webrtcbin\n");
        return 1;
    }

    // Hook up local ICE candidate signal
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
