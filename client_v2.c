#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE] = {0};
    char message[BUFFER_SIZE];

    // Ensure the client ID is passed as a command-line argument
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <client_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int client_id = atoi(argv[1]);

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Define server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    // Convert address from text to binary
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("Connected to the server.\n");

    // Message exchange loop
    while (1) {
        // Prepare the message to send
        snprintf(message, BUFFER_SIZE, "geia%d", client_id);
        send(sock, message, strlen(message), 0);
        printf("Message sent to server: %s\n", message);

        // Wait for the server's response
        ssize_t bytes_read = read(sock, buffer, BUFFER_SIZE);
        if (bytes_read <= 0) {
            printf("Server disconnected.\n");
            break;
        }

        buffer[bytes_read] = '\0'; // Null-terminate the received message
        printf("Message from server: %s\n", buffer);

        // Wait for 5 seconds before sending the next message
        sleep(5);
    }

    // Close the socket
    close(sock);

    return 0;
}
