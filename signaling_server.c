#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// We'll keep a single Offer + single Answer for simplicity
static char sdp_offer[4096] = {0};
static char sdp_answer[4096] = {0};

// We'll track versions so we don't keep re-sending the same Offer/Answer
static unsigned long offer_version = 0;
static unsigned long answer_version = 0;
static unsigned long global_version_counter = 1;

// Per-connection data
struct per_session_data {
    char message[4096];
    size_t len;

    unsigned long seen_offer_version;
    unsigned long seen_answer_version;
};

static void maybe_send_offer_and_answer(struct lws *wsi);

// The server callback
static int
callback_signaling(struct lws *wsi, enum lws_callback_reasons reason,
                   void *user, void *in, size_t len)
{
    struct per_session_data *psd = (struct per_session_data *)user;

    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
        lwsl_user("[Signaling] New client connected\n");
        // Mark that this connection hasn't seen any versions yet
        psd->seen_offer_version = 0;
        psd->seen_answer_version = 0;

        // If we already have an Offer/Answer, schedule a write
        if (strlen(sdp_offer) > 0 || strlen(sdp_answer) > 0) {
            lws_callback_on_writable(wsi);
        }
        break;

    case LWS_CALLBACK_RECEIVE: {
        if (len >= sizeof(psd->message)) {
            lwsl_err("[Signaling] Message too long\n");
            return -1;
        }
        memcpy(psd->message, in, len);
        psd->message[len] = '\0';
        psd->len = len;

        // Distinguish "candidate:", "answer:", or an SDP Offer
        if (!strncmp(psd->message, "candidate:", 10)) {
            // ICE candidate from sender or receiver
            lwsl_user("[Signaling] Received ICE candidate:\n%s\n", psd->message);
            //  re-broadcast to all
            lws_callback_on_writable_all_protocol(lws_get_context(wsi),
                                                  lws_get_protocol(wsi));
        }
        else if (!strncmp(psd->message, "answer:", 7)) {
            // It's an Answer
            const char *answer_text = psd->message + 7;
            if (strcmp(sdp_answer, answer_text) == 0) {
                lwsl_user("[Signaling] Same Answer as before, ignoring\n");
            } else {
                lwsl_user("[Signaling] Storing NEW SDP Answer:\n%s\n", answer_text);
                strncpy(sdp_answer, answer_text, sizeof(sdp_answer) - 1);
                sdp_answer[sizeof(sdp_answer) - 1] = '\0';

                answer_version = ++global_version_counter;
                lws_callback_on_writable_all_protocol(lws_get_context(wsi),
                                                      lws_get_protocol(wsi));
            }
        }
        else if (strstr(psd->message, "v=0")) {
            //  treat anything containing "v=0" as an SDP Offer
            if (strcmp(sdp_offer, psd->message) == 0) {
                lwsl_user("[Signaling] Same Offer as before, ignoring\n");
            } else {
                lwsl_user("[Signaling] Storing NEW SDP Offer:\n%s\n", psd->message);
                strncpy(sdp_offer, psd->message, sizeof(sdp_offer) - 1);
                sdp_offer[sizeof(sdp_offer) - 1] = '\0';

                offer_version = ++global_version_counter;
                lws_callback_on_writable_all_protocol(lws_get_context(wsi),
                                                      lws_get_protocol(wsi));
            }
        }
        else {
            lwsl_user("[Signaling] Unknown message:\n%s\n", psd->message);
        }
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE:
        // Possibly send new Offer/Answer to this connection
        maybe_send_offer_and_answer(wsi);

        // If the last message was an ICE candidate, forward it
        if (!strncmp(psd->message, "candidate:", 10)) {
            unsigned char buf[LWS_PRE + 4096];
            memset(buf, 0, sizeof(buf));
            snprintf((char*)&buf[LWS_PRE], sizeof(buf) - LWS_PRE,
                     "%s", psd->message);

            lwsl_user("[Signaling] Forwarding ICE candidate to this client\n");
            lws_write(wsi, &buf[LWS_PRE],
                      strlen((char*)&buf[LWS_PRE]),
                      LWS_WRITE_TEXT);
        }
        break;

    default:
        break;
    }
    return 0;
}

// Helper: send Offer/Answer only if there's a new version
static void maybe_send_offer_and_answer(struct lws *wsi)
{
    struct per_session_data *psd =
        (struct per_session_data *)lws_wsi_user(wsi);

    // If there's a new Offer that this client hasn't seen
    if (strlen(sdp_offer) > 0 && psd->seen_offer_version < offer_version) {
        unsigned char buffer[LWS_PRE + 4096];
        memset(buffer, 0, sizeof(buffer));
        snprintf((char*)&buffer[LWS_PRE], sizeof(buffer) - LWS_PRE,
                 "SERVER_OFFER:%s", sdp_offer);

        lwsl_user("[Signaling] Sending NEW SDP Offer to this client\n");
        if (lws_write(wsi, &buffer[LWS_PRE],
                      strlen((char*)&buffer[LWS_PRE]),
                      LWS_WRITE_TEXT) >= 0) {
            psd->seen_offer_version = offer_version;
        }
    }

    // If there's a new Answer
    if (strlen(sdp_answer) > 0 && psd->seen_answer_version < answer_version) {
        unsigned char buffer[LWS_PRE + 4096];
        memset(buffer, 0, sizeof(buffer));
        snprintf((char*)&buffer[LWS_PRE], sizeof(buffer) - LWS_PRE,
                 "SERVER_ANSWER:%s", sdp_answer);

        lwsl_user("[Signaling] Sending NEW SDP Answer to this client\n");
        if (lws_write(wsi, &buffer[LWS_PRE],
                      strlen((char*)&buffer[LWS_PRE]),
                      LWS_WRITE_TEXT) >= 0) {
            psd->seen_answer_version = answer_version;
        }
    }
}

int main(void)
{
    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
    lwsl_user("[Signaling] Starting signaling server...\n");

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = 8080;

    static struct lws_protocols protocols[] = {
        {
            "signaling-protocol",
            callback_signaling,
            sizeof(struct per_session_data),
            4096
        },
        {NULL, NULL, 0, 0}
    };
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        lwsl_err("[Signaling] Failed to create WebSocket context\n");
        return 1;
    }

    lwsl_user("[Signaling] Server running on ws://localhost:8080\n");

    while (1) {
        lwsl_user("[Signaling] Waiting for events...\n");
        lws_service(context, 1000);
    }

    lws_context_destroy(context);
    return 0;
}
