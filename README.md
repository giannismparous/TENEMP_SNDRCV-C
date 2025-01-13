# WebRTC GStreamer Signaling and Clients

This project demonstrates a simple WebRTC setup with GStreamer and a custom signaling server. It includes:

1. A **signaling_server** based on `libwebsockets`.
2. A **sender_client** that uses GStreamer WebRTC.
3. A **receiver_client** that uses GStreamer WebRTC.

## Requirements

You will need the following packages on Ubuntu:

- **libwebsockets-dev** – for building the signaling server and linking against libwebsockets.
- **GStreamer** and its development packages – for GStreamer WebRTC functionality:
  - `gstreamer-1.0`
  - `gstreamer-webrtc-1.0`
  - `gstreamer-sdp-1.0`
  - `gstreamer-plugins-base1.0-dev`
  - `gstreamer-plugins-good1.0-dev`
  - `gstreamer-plugins-bad1.0-dev`
  - `gstreamer-plugins-ugly1.0-dev`
  - `gstreamer-libav1.0-dev`
  - `libgstreamer-plugins-base1.0-dev`

### Installing the Packages
```
sudo apt-get update
sudo apt-get install libwebsockets-dev \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-doc \
    gstreamer1.0-tools \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-alsa \
    gstreamer-plugins-base1.0-dev \
    gstreamer-plugins-good1.0-dev \
    gstreamer-plugins-bad1.0-dev \
    gstreamer-plugins-ugly1.0-dev \
    gstreamer-libav1.0-dev
```


Compilation:

```
gcc signaling_server.c -o signaling_server -lwebsockets
gcc -D GST_USE_UNSTABLE_API sender_client.c -o sender_client \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0) \
    -lwebsockets
gcc -D GST_USE_UNSTABLE_API receiver_client.c -o receiver_client \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0) \
    -lwebsockets
```
Order of Execution

Start the 1. signaling server, 2. the sender 3.receiver.

Terminal 1: Start signaling server
```
./signaling_server
```
Terminal 2: Start sender client
```
GST_DEBUG=webrtc*:6,ice*:6,3 ./sender_client
```
Terminal 3: Start receiver client
```
GST_DEBUG=webrtc*:6,ice*:6,3 ./receiver_client
```
